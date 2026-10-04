#pragma once

// Every query the entries module issues, and nothing else.
//
// The shapes, so an application can declare the indexes behind them
// (docs/20-entries.md §6 has the rows to copy):
//
//   listing     {sc, live, o, _id}   every list, both stages, both directions
//   slug        {k, slug}  UNIQUE, partial on slug existing
//   by id       {_id}                the primary key, filtered with k as well
//
// A method taking a client_session& runs inside the caller's transaction and
// lets a transient error through for with_transaction to retry
// (tools/check-db-discipline.sh, rule 6).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <bsoncxx/document/value.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/repository.h"
#include "anvil/entries/document.h"
#include "anvil/entries/registry.h"

namespace anvil::entries {

// `k` for a root kind; `k/<parent id>` for a child. Built here and nowhere else,
// so a listing and the document it lists cannot spell one scope two ways.
[[nodiscard]] std::string scope_of(const KindSpec& kind, const std::optional<Uuid>& parent);

class EntryRepository final : public repo::RepositoryBase {
public:
    EntryRepository(std::string database, std::string_view collection,
                    std::span<const KindSpec> kinds) noexcept
        : RepositoryBase{std::move(database), collection}, kinds_{kinds} {}

    // --- reads ------------------------------------------------------------------

    // nullptr-free: an id that exists under another kind is NotFound here, so an
    // id is never a way to read across kinds.
    [[nodiscard]] Result<std::optional<EntryDocument>> find(mongocxx::client& client,
                                                            const KindSpec& kind, const Uuid& id,
                                                            Stage stage) const;
    [[nodiscard]] Result<std::optional<EntryDocument>> find(mongocxx::client& client,
                                                            mongocxx::client_session& session,
                                                            const KindSpec& kind, const Uuid& id,
                                                            Stage stage) const;

    // Published only when `stage` says so: an unpublished entry's slug answers
    // NotFound to a reader, exactly as an unknown one does.
    [[nodiscard]] Result<std::optional<EntryDocument>> find_by_slug(mongocxx::client& client,
                                                                    const KindSpec& kind,
                                                                    std::string_view slug,
                                                                    Stage stage) const;

    [[nodiscard]] Result<EntryPage> list(mongocxx::client& client, const KindSpec& kind,
                                         std::string_view scope, const EntryQuery& query) const;

    // The highest position in a scope, or -1 when it is empty. One index seek.
    [[nodiscard]] Result<std::int64_t> last_position(mongocxx::client& client,
                                                     std::string_view scope) const;

    // Entries in a scope, live or not. Counted with a limit of `ceiling`, so the
    // capacity check costs at most that many index keys however large the
    // collection has grown.
    [[nodiscard]] Result<std::int64_t> count_scope(mongocxx::client& client,
                                                   std::string_view scope,
                                                   std::int64_t ceiling) const;

    // Every id in a scope, in a transaction's snapshot, for reorder().
    [[nodiscard]] Result<std::vector<Uuid>> scope_ids(mongocxx::client& client,
                                                      mongocxx::client_session& session,
                                                      std::string_view scope,
                                                      std::int64_t ceiling) const;

    // --- writes -----------------------------------------------------------------

    // The whole document, built by the service. Conflict when the slug is taken.
    [[nodiscard]] Status insert(mongocxx::client& client, mongocxx::client_session& session,
                                const bsoncxx::document::view& document) const;

    // `set_fields` under the expected version; the version it now holds, or
    // VersionMismatch. Conflict when a slug in `set_fields` is taken.
    [[nodiscard]] Result<std::int64_t> update(mongocxx::client& client,
                                              mongocxx::client_session& session,
                                              const KindSpec& kind, const Uuid& id,
                                              std::int64_t expected_version,
                                              const bsoncxx::document::view& set_fields) const;

    // Sets and clears bits in one atomic update and reports whether the entry
    // exists. Unversioned by design: two toggles of two different flags
    // commute, and a toggle that made an editor's open form stale would make
    // pinning something a race against whoever is typing.
    [[nodiscard]] Result<bool> set_flags(mongocxx::client& client, const KindSpec& kind,
                                         const Uuid& id, FlagSet set, FlagSet clear) const;

    [[nodiscard]] Status set_position(mongocxx::client& client, mongocxx::client_session& session,
                                      const KindSpec& kind, const Uuid& id,
                                      std::int64_t position) const;

    // +1 only while the count is below `capacity`, so the bound on a parent's
    // children is exact under any concurrency; -1 unconditionally. NotFound when
    // the parent is gone, Conflict when it is full.
    [[nodiscard]] Status adjust_children(mongocxx::client& client,
                                         mongocxx::client_session& session,
                                         const KindSpec& parent_kind, const Uuid& parent,
                                         std::int64_t delta, std::uint32_t capacity) const;

    // Under the expected version. False when nothing matched.
    [[nodiscard]] Result<bool> remove(mongocxx::client& client, mongocxx::client_session& session,
                                      const KindSpec& kind, const Uuid& id,
                                      std::int64_t expected_version) const;

    // The once-per-kind seed claim. True when this call made it, false when some
    // earlier boot on any instance already had.
    [[nodiscard]] Result<bool> claim_seed(mongocxx::client& client,
                                          mongocxx::client_session& session,
                                          std::string_view kind) const;

private:
    std::span<const KindSpec> kinds_;
};

}  // namespace anvil::entries
