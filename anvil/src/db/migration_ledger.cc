// versioned-write-exempt: every write here carries the OWNER in its filter,
// which is optimistic concurrency with the lease in the place a version number
// would otherwise be. db/versioned.h is for documents an application edits from
// two tabs; this row has exactly one legitimate writer at a time and the filter
// is what enforces it — a fenced runner sees zero matched and stops.

#include "anvil/db/migration_ledger.h"

#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/find_one_and_update.hpp>

#include "anvil/core/uuid.h"
#include "anvil/db/repository.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

// Short keys, for the same reason every other stored document in this library
// uses them: the key is stored per row.
constexpr std::string_view kDone = "done";
constexpr std::string_view kCursor = "cur";
constexpr std::string_view kCursorAt = "cur_at";
constexpr std::string_view kDocuments = "docs";
constexpr std::string_view kBatches = "batches";
constexpr std::string_view kAttempts = "attempts";
constexpr std::string_view kError = "err";
constexpr std::string_view kOwner = "owner";
constexpr std::string_view kOwnerLabel = "owner_label";
constexpr std::string_view kLease = "lease";
constexpr std::string_view kStartedAt = "started_at";
constexpr std::string_view kUpdatedAt = "updated_at";
constexpr std::string_view kFinishedAt = "finished_at";

constexpr int kDuplicateKeyCode = 11000;

// "Free" is a lease in the past, not a null owner.
//
// One equality and one range makes the claim filter upsertable — MongoDB derives
// the new document from the query's equality conditions, and a filter spelled as
// `$or` over three ways of being free derives nothing and reads as three rules
// where there is one.
[[nodiscard]] TimeMs released_lease() noexcept { return TimeMs{std::chrono::milliseconds{0}}; }

[[nodiscard]] std::string_view text_of(const bsoncxx::document::element& field) noexcept {
    if (!field || field.type() != bsoncxx::type::k_string) { return {}; }
    const bsoncxx::stdx::string_view value = field.get_string().value;
    return std::string_view{value.data(), value.size()};
}

[[nodiscard]] std::int64_t number_of(const bsoncxx::document::element& field) noexcept {
    if (!field) { return 0; }
    if (field.type() == bsoncxx::type::k_int32) { return field.get_int32().value; }
    if (field.type() == bsoncxx::type::k_int64) { return field.get_int64().value; }
    return 0;
}

[[nodiscard]] std::optional<TimeMs> time_of(const bsoncxx::document::element& field) noexcept {
    if (!field || field.type() != bsoncxx::type::k_date) { return std::nullopt; }
    const TimeMs at{field.get_date().value};
    if (at == released_lease()) { return std::nullopt; }
    return at;
}

[[nodiscard]] LedgerEntry decode(const bsoncxx::document::view& doc) {
    LedgerEntry entry{};

    const bsoncxx::document::element cursor = doc[codec::key_of(kCursor)];
    if (cursor && cursor.type() != bsoncxx::type::k_null) {
        entry.cursor = bsoncxx::types::bson_value::value{cursor.get_value()};
    }
    entry.last_error = std::string{text_of(doc[codec::key_of(kError)])};

    const bsoncxx::document::element owner = doc[codec::key_of(kOwner)];
    if (owner && owner.type() == bsoncxx::type::k_binary) {
        const bsoncxx::types::b_binary bytes = owner.get_binary();
        if (bytes.size == Uuid{}.size()) {
            Uuid id{};
            for (std::size_t i = 0; i < id.size(); ++i) { id[i] = bytes.bytes[i]; }
            entry.owner = id;
        }
    }
    entry.owner_label = std::string{text_of(doc[codec::key_of(kOwnerLabel)])};
    entry.lease_expires_at = time_of(doc[codec::key_of(kLease)]);
    entry.cursor_at = time_of(doc[codec::key_of(kCursorAt)]);
    entry.started_at = time_of(doc[codec::key_of(kStartedAt)]);
    entry.finished_at = time_of(doc[codec::key_of(kFinishedAt)]);
    entry.documents = number_of(doc[codec::key_of(kDocuments)]);
    entry.batches = number_of(doc[codec::key_of(kBatches)]);
    entry.attempts = number_of(doc[codec::key_of(kAttempts)]);

    const bsoncxx::document::element done = doc[codec::key_of(kDone)];
    entry.done = done && done.type() == bsoncxx::type::k_bool && done.get_bool().value;
    return entry;
}

// The progress fields, appended into whichever $set is being built. Written as
// what the row SHOULD say rather than as a delta: the runner read the totals
// back from its own claim, so it knows them, and a $inc from two overlapping
// runners is the one shape that corrupts silently.
void append_progress(bsoncxx::builder::basic::document& set, const LedgerProgress& progress) {
    set.append(kvp(codec::key_of(kDocuments), bsoncxx::types::b_int64{progress.documents}));
    set.append(kvp(codec::key_of(kBatches), bsoncxx::types::b_int64{progress.batches}));
    if (progress.cursor.has_value()) {
        set.append(kvp(codec::key_of(kCursor), progress.cursor->view()));
    }
    if (progress.cursor_at.has_value()) {
        set.append(kvp(codec::key_of(kCursorAt), codec::time_date(*progress.cursor_at)));
    }
}

}  // namespace

MigrationLedger::MigrationLedger(std::string database, Uuid runner, std::string label) noexcept
    : database_{std::move(database)}, label_{std::move(label)}, runner_{runner} {}

Result<LedgerEntry> MigrationLedger::claim(mongocxx::client& client, std::string_view step,
                                           std::chrono::seconds lease) {
    return repo::guarded([&]() -> Result<LedgerEntry> {
        const TimeMs now = now_ms();
        const TimeMs expires = now + std::chrono::duration_cast<std::chrono::milliseconds>(lease);

        mongocxx::options::find_one_and_update options{};
        options.upsert(true);
        options.return_document(mongocxx::options::return_document::k_after);

        try {
            const auto claimed =
                client[database_][std::string{kMigrationLedgerCollection}].find_one_and_update(
                    make_document(kvp("_id", codec::key_of(step)),
                                  kvp(codec::key_of(kLease),
                                      [now](sub_document sub) {
                                          sub.append(kvp("$lte", codec::time_date(now)));
                                      })),
                    make_document(
                        kvp("$set",
                            [&](sub_document sub) {
                                sub.append(kvp(codec::key_of(kOwner), codec::uuid_bin(runner_)));
                                sub.append(kvp(codec::key_of(kOwnerLabel),
                                               codec::key_of(label_)));
                                sub.append(kvp(codec::key_of(kLease), codec::time_date(expires)));
                                sub.append(kvp(codec::key_of(kUpdatedAt), codec::time_date(now)));
                            }),
                        // The one $inc in this subsystem, and it is anvil's own
                        // bookkeeping rather than a step's: it happens INSIDE
                        // the atomic claim, so exactly one runner performs it
                        // per successful claim. The ban a step lives under is
                        // about overlapping batch writes, which this is not.
                        kvp("$inc",
                            [](sub_document sub) {
                                sub.append(kvp(codec::key_of(kAttempts),
                                               bsoncxx::types::b_int64{1}));
                            }),
                        kvp("$setOnInsert",
                            [now](sub_document sub) {
                                sub.append(kvp(codec::key_of(kDone),
                                               bsoncxx::types::b_bool{false}));
                                sub.append(kvp(codec::key_of(kCursor), bsoncxx::types::b_null{}));
                                sub.append(kvp(codec::key_of(kDocuments),
                                               bsoncxx::types::b_int64{0}));
                                sub.append(kvp(codec::key_of(kBatches),
                                               bsoncxx::types::b_int64{0}));
                                sub.append(kvp(codec::key_of(kError), ""));
                                sub.append(kvp(codec::key_of(kStartedAt), codec::time_date(now)));
                            })),
                    options);
            if (!claimed) { return fail(ErrorCode::Conflict, "lease"); }
            return decode(claimed->view());
        } catch (const mongocxx::operation_exception& e) {
            // The row exists and is held, so the filter matched nothing and the
            // upsert tried to insert a second document under the same `_id`.
            // That IS the contention signal, and it arrives from one atomic
            // operation rather than from a read followed by a decision.
            if (e.code().value() == kDuplicateKeyCode) {
                return fail(ErrorCode::Conflict, "lease");
            }
            throw;
        }
    });
}

Status MigrationLedger::record(mongocxx::client& client, std::string_view step,
                               const LedgerProgress& progress, std::chrono::seconds lease) {
    return repo::guarded([&]() -> Status {
        const TimeMs now = now_ms();
        const TimeMs expires = now + std::chrono::duration_cast<std::chrono::milliseconds>(lease);

        bsoncxx::builder::basic::document set;
        set.append(kvp(codec::key_of(kLease), codec::time_date(expires)));
        set.append(kvp(codec::key_of(kUpdatedAt), codec::time_date(now)));
        append_progress(set, progress);

        const auto updated =
            client[database_][std::string{kMigrationLedgerCollection}].find_one_and_update(
                make_document(kvp("_id", codec::key_of(step)),
                              kvp(codec::key_of(kOwner), codec::uuid_bin(runner_))),
                make_document(kvp("$set", set.extract())));
        // Zero matched means the owner changed: this runner has been fenced and
        // must write nothing further. There is no separate check to race with,
        // because the renewal and the detection are one operation.
        if (!updated) { return fail(ErrorCode::Conflict, "lease"); }
        return ok();
    });
}

Status MigrationLedger::finish(mongocxx::client& client, std::string_view step,
                               const LedgerProgress& progress) {
    return repo::guarded([&]() -> Status {
        const TimeMs now = now_ms();

        bsoncxx::builder::basic::document set;
        set.append(kvp(codec::key_of(kDone), bsoncxx::types::b_bool{true}));
        set.append(kvp(codec::key_of(kFinishedAt), codec::time_date(now)));
        set.append(kvp(codec::key_of(kUpdatedAt), codec::time_date(now)));
        set.append(kvp(codec::key_of(kError), ""));
        // Released rather than left to lapse, so nothing waits out a lease
        // behind a step that is finished.
        set.append(kvp(codec::key_of(kOwner), bsoncxx::types::b_null{}));
        set.append(kvp(codec::key_of(kOwnerLabel), ""));
        set.append(kvp(codec::key_of(kLease), codec::time_date(released_lease())));
        append_progress(set, progress);

        const auto updated =
            client[database_][std::string{kMigrationLedgerCollection}].find_one_and_update(
                make_document(kvp("_id", codec::key_of(step)),
                              kvp(codec::key_of(kOwner), codec::uuid_bin(runner_))),
                make_document(kvp("$set", set.extract())));
        if (!updated) { return fail(ErrorCode::Conflict, "lease"); }
        return ok();
    });
}

Status MigrationLedger::record_failure(mongocxx::client& client, std::string_view step,
                                       const LedgerProgress& progress,
                                       std::string_view reason) {
    return repo::guarded([&]() -> Status {
        const TimeMs now = now_ms();

        bsoncxx::builder::basic::document set;
        // `done` is deliberately untouched: a failed step is not finished, and
        // every step after it is blocked because N+1 was written by somebody who
        // assumed N ran.
        set.append(kvp(codec::key_of(kError), codec::key_of(reason)));
        set.append(kvp(codec::key_of(kUpdatedAt), codec::time_date(now)));
        set.append(kvp(codec::key_of(kOwner), bsoncxx::types::b_null{}));
        set.append(kvp(codec::key_of(kOwnerLabel), ""));
        set.append(kvp(codec::key_of(kLease), codec::time_date(released_lease())));
        append_progress(set, progress);

        const auto updated =
            client[database_][std::string{kMigrationLedgerCollection}].find_one_and_update(
                make_document(kvp("_id", codec::key_of(step)),
                              kvp(codec::key_of(kOwner), codec::uuid_bin(runner_))),
                make_document(kvp("$set", set.extract())));
        if (!updated) { return fail(ErrorCode::Conflict, "lease"); }
        return ok();
    });
}

Result<std::optional<LedgerEntry>> MigrationLedger::read(mongocxx::client& client,
                                                         std::string_view step) {
    return repo::guarded([&]() -> Result<std::optional<LedgerEntry>> {
        const auto found =
            client[database_][std::string{kMigrationLedgerCollection}].find_one(
                make_document(kvp("_id", codec::key_of(step))));
        if (!found) { return std::optional<LedgerEntry>{}; }
        return std::optional<LedgerEntry>{decode(found->view())};
    });
}

Status MigrationLedger::release(mongocxx::client& client, std::string_view step) {
    return repo::guarded([&]() -> Status {
        // Deliberately NOT filtered on the owner. This is the override, and an
        // override that only works when you are already the holder overrides
        // nothing.
        const auto updated =
            client[database_][std::string{kMigrationLedgerCollection}].find_one_and_update(
                make_document(kvp("_id", codec::key_of(step))),
                make_document(kvp("$set", [](sub_document sub) {
                    sub.append(kvp(codec::key_of(kOwner), bsoncxx::types::b_null{}));
                    sub.append(kvp(codec::key_of(kOwnerLabel), ""));
                    sub.append(kvp(codec::key_of(kLease),
                                   codec::time_date(released_lease())));
                })));
        if (!updated) { return fail(ErrorCode::NotFound, "step"); }
        return ok();
    });
}

}  // namespace anvil::db
