#pragma once

// The audit collection: append, append_many, and exactly one read.
//
// There is no update and no delete on this class, and their ABSENCE is the
// mechanism. The collection is append-only because nothing here can write to a
// row that already exists — a guard inside a handler would be one `if` away from
// removal, and a method that does not exist cannot be called by mistake.
//
// Retention is a TTL index, which is deliberately NOT a lifetime: a row kept for
// four hundred days is history, and filtering reads on `at > now` would return
// nothing at all. That is exactly why the application's collection table leaves
// this one's expiry field empty (anvil/db/collection_spec.h).

#include <span>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/audit/record.h"
#include "anvil/core/result.h"
#include "anvil/db/repository.h"

namespace anvil::audit {

class AuditRepository final : public repo::RepositoryBase {
public:
    AuditRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // Append-only. Never fails a request: the caller posts this to db_pool and
    // discards the Status, because a request that succeeded must not be turned
    // into a 500 by an audit write, and a request that was denied cannot become
    // more denied.
    [[nodiscard]] Status append(mongocxx::client& client, const AuditEntry& entry,
                                db::TimeMs at) const;

    // One insert_many for a whole batch, UNORDERED so a single rejected row does
    // not discard the rest of the batch behind it.
    //
    // This is the shape the audit path actually needs. One insert per row means
    // one pool task and one round trip per audited event, and audited events
    // include every denial — so a burst against a stealth route queues a task
    // per request and pushes the pool past its bound, at which point rows are
    // dropped. Dropping forensic rows precisely during an attack is the one time
    // they matter.
    [[nodiscard]] Status append_many(mongocxx::client& client,
                                     std::span<const AuditRow> rows) const;

    // One page, newest first. The ONLY read on this class.
    //
    // Every filter shape is written to ride an index whose trailing keys are
    // `(at, _id)`, so the ordering comes from the walk rather than from an
    // in-memory sort of four hundred days of rows.
    [[nodiscard]] Result<AuditPage> listing(mongocxx::client& client,
                                            const AuditQuery& query) const;
};

}  // namespace anvil::audit
