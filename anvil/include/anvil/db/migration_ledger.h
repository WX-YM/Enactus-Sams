#pragma once

// Where a data migration records what it has done, and what it is doing now.
//
// anvil's OWN storage, beside anvil_schema_meta: the mechanism's bookkeeping, so
// an application neither declares it in config::kCollections nor can collide
// with it. Access is `_id` equality only, so it needs no index and appears in no
// query catalogue.
//
// --- two ledgers, two questions ---------------------------------------------
//
// anvil_schema_meta answers "do this database's indexes exist?" with one integer
// per database, written with $max. This answers "has this step finished, and
// where did it get to?" with one document per step. The integer is deliberately
// NOT shared: a version is a high-water mark, and a step is done, or not done,
// or somewhere in the middle — and a counter cannot represent the third, which
// is the state this exists to describe (docs/18-data-migrations.md §2).
//
// --- what the lock is, and what it is not -----------------------------------
//
// The claim is one find_one_and_update against an expiring lease: never
// check-then-act (ENGINEERING_RULES.md §6). It is in MongoDB and not Redis because the
// ledger is here, a lock held in a store docs/10-timer-jobs.md §1 says is not
// the system of record can disagree with the thing it locks, and a migration
// must still run when Redis is down — it is the tool you reach for during an
// incident.
//
// THE LOCK IS NOT WHAT MAKES A STEP CORRECT. IDEMPOTENCE IS. A lease can expire
// against a process that is alive but stalled — a paused VM, a partition, a host
// that lost its disk for ninety seconds — so the lock stops the wasted work and
// the write conflicts and is never relied on for correctness. Every write here
// carries the owner in its FILTER, so a runner that has been fenced learns it
// from a zero matched count and stops rather than trusting a branch.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <bsoncxx/types/bson_value/value.hpp>
#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"

namespace anvil::db {

inline constexpr std::string_view kMigrationLedgerCollection = "anvil_migration_ledger";

// How long a claim is good for without a renewal.
//
// It must exceed the worst-case time one BATCH takes, because the renewal
// happens once per batch: the runner renews, reads, transforms and writes, and a
// lease shorter than that cycle is a runner that fences itself while working.
// Sixty seconds against a default batch of a few hundred documents is a wide
// margin; a step doing expensive per-document work should raise both.
inline constexpr std::chrono::seconds kDefaultLeaseSeconds{60};

// One step's row, decoded.
//
// Not a hot struct — one of these is read per step per invocation of a process
// that exits — so it is ordered for reading rather than for packing.
struct LedgerEntry final {
    // The `_id` of the last document the step has certainly been applied to, or
    // nullopt before the first batch. The resume point, and the reason
    // pagination here is a range and never skip(n).
    std::optional<bsoncxx::types::bson_value::value> cursor;
    // Empty when the last attempt did not fail. Carries no driver text.
    std::string                                      last_error;
    // Who holds the lease, and what they are: a UUID for equality and a label
    // for the operator staring at --status wondering which host to go and look
    // at.
    std::optional<Uuid>                              owner;
    std::string                                      owner_label;
    std::optional<TimeMs>                            lease_expires_at;
    // Only meaningful for a UUIDv7 `_id`, where the cursor encodes the moment
    // the document was created. It is the one progress figure an operator can
    // read without knowing the collection.
    std::optional<TimeMs>                            cursor_at;
    std::optional<TimeMs>                            started_at;
    std::optional<TimeMs>                            finished_at;
    std::int64_t                                     documents;
    std::int64_t                                     batches;
    std::int64_t                                     attempts;
    bool                                             done;
};

// What one batch adds to the row. The runner holds the running totals — it read
// them back from the claim — and writes them, so this is a $set and never a
// delta from what the server happens to hold.
struct LedgerProgress final {
    std::optional<bsoncxx::types::bson_value::value> cursor;
    std::optional<TimeMs>                            cursor_at;
    std::int64_t                                     documents;
    std::int64_t                                     batches;
};

class MigrationLedger final {
public:
    // `runner` identifies this process for the lifetime of the run; `label` is
    // what an operator reads. Both are stored on the row while the lease is
    // held.
    MigrationLedger(std::string database, Uuid runner, std::string label) noexcept;

    [[nodiscard]] std::string_view database() const noexcept { return database_; }
    [[nodiscard]] Uuid runner() const noexcept { return runner_; }

    // The claim. One find_one_and_update against an expiring lease, upserting
    // the row on the first run.
    //
    //   the entry           the lease is ours and the row is as it now stands
    //   Conflict            another runner holds it. The caller reports HELD and
    //                       exits 2 rather than waiting — a second operator
    //                       watching a migration "hang" is how two of them end
    //                       up force-killing the one that was working.
    [[nodiscard]] Result<LedgerEntry> claim(mongocxx::client& client, std::string_view step,
                                            std::chrono::seconds lease);

    // Renews the lease and records the batch, in ONE operation filtered on
    // ownership.
    //
    // Called at the TOP of each batch, before anything is read or written, so a
    // fenced runner discovers it before it writes rather than after. Returns
    // Conflict when the owner has changed, and a caller that sees it must write
    // nothing further.
    [[nodiscard]] Status record(mongocxx::client& client, std::string_view step,
                                const LedgerProgress& progress, std::chrono::seconds lease);

    // Terminal. Both release the lease rather than letting it expire, so a
    // successor takes over in milliseconds instead of waiting out the TTL.
    [[nodiscard]] Status finish(mongocxx::client& client, std::string_view step,
                                const LedgerProgress& progress);
    [[nodiscard]] Status record_failure(mongocxx::client& client, std::string_view step,
                                        const LedgerProgress& progress, std::string_view reason);

    [[nodiscard]] Result<std::optional<LedgerEntry>> read(mongocxx::client& client,
                                                          std::string_view step);

    // --unlock. Clears the lease whoever holds it, which is why the CLI prints
    // the current owner and expiry first: it is the one operation whose whole
    // purpose is to override the safety net.
    [[nodiscard]] Status release(mongocxx::client& client, std::string_view step);

private:
    std::string database_;
    std::string label_;
    Uuid        runner_;
};

}  // namespace anvil::db
