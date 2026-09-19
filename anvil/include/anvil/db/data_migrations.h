#pragma once

// The runner: ordered, forward-only, resumable by an `_id` range.
//
// anvil owns the cursor, the batching, the ledger and the lock; a step owns only
// the transform (anvil/db/migration_step.h). This is the half that makes
// --dry-run a property of the library rather than a promise each step makes
// individually — the step accumulates the writes it wants and the runner decides
// whether to execute them.
//
// --- why this is a process that exits ---------------------------------------
//
// An index build holds its collection for the duration and every db_pool thread
// queues behind it; a data migration does that AND writes every document in the
// collection. Run at boot in a rolling deploy it runs once per instance, and the
// second instance starts its pass while the first is still going. So migrate is
// a process, run once, from one place — not a boot step, not a job on the queue,
// and not a route (docs/00-architecture.md §7).
//
// --- there is no down migration ---------------------------------------------
//
// Not omitted, refused. A down migration is code that has run exactly zero times
// in production and is expected to work under the one condition where everything
// else has already gone wrong, against data the forward step has already
// changed. The recovery path for a bad migration is a forward step that corrects
// it, written and tested like any other.

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/types.h"
#include "anvil/db/collection_options.h"
#include "anvil/db/collections.h"
#include "anvil/db/migration_ledger.h"
#include "anvil/db/migration_step.h"
#include "anvil/db/migrations.h"

namespace anvil::db {

// Everything one invocation needs, in one bundle, because one invocation applies
// all of it: the collection options, the indexes, then the documents.
//
// The step table is a std::span and NOT a third application configuration
// header. <anvil_app_jobs.h> exists because a size is derived from that table
// inside anvil's own compilation — the "a size must be a size" exception — and
// migrations derive no such constant (docs/01-seams.md §13).
struct MigrationDeps final {
    DatabaseNames                          databases;
    // The connection string, resolved by the application from its own
    // environment. It is here rather than read from MONGODB_URI inside the CLI
    // because DatabaseNames above it is already deployment configuration the
    // application resolved: two independent readers of one environment is two
    // places for a migration to be pointed at a different cluster than the
    // service it is migrating for.
    std::string_view                       mongodb_uri;
    std::span<const IndexSpec>             indexes;
    std::span<const RetiredIndex>          retired;
    std::span<const MigrationStep>         steps;
    std::span<const CollectionOptionsSpec> collection_options;
    std::int32_t                           schema_version;
};

enum class StepState : std::uint8_t {
    // --only named a different step.
    Skipped = 0,
    // The ledger says it finished. A second invocation does nothing at all,
    // which is the property that makes running migrate twice safe.
    AlreadyDone = 1,
    Applied = 2,
    Failed = 3,
    // Another runner holds the lease. Reported rather than waited on.
    Held = 4,
    // An earlier step failed or is held. Steps are ordered and N+1 was written
    // by somebody who assumed N ran, so continuing past a failure produces a
    // state no step's precondition describes.
    Blocked = 5,
    // The cluster does not satisfy the step's precondition — its schema version
    // has not been applied, or its collection's `_id` is not the shape its
    // cursor declared.
    Refused = 6,
};

struct StepReport final {
    std::string_view name;
    // Why it failed or was refused, in words an operator can act on. Never
    // driver text, never a document (ENGINEERING_RULES.md §5).
    std::string      detail;
    std::int64_t     documents_scanned;
    std::int64_t     documents_written;
    std::int64_t     batches;
    StepState        state;
};

// The values ARE the process exit codes, so a deploy script branches on them and
// the mapping cannot drift between the runner and the CLI.
enum class RunOutcome : std::uint8_t {
    Applied = 0,       // applied, or nothing to do
    StepFailed = 1,
    Held = 2,          // another runner has it
    Inconsistent = 3,  // the catalogue does not describe this cluster
};

struct RunOptions final {
    // Empty runs every step in order. Naming one runs only that step: an
    // operator override for recovering a half-applied migration.
    std::string_view     only;
    // Identifies this process on the ledger row. `label` is what --status and
    // --unlock print, so it should say which host to go and look at.
    //
    // A VIEW, so its storage must outlive the call. Binding it to a function
    // that returns a std::string by value is a dangling read the moment the
    // full expression ends — which is how this was first written, and what ASan
    // reported from the first run of the CLI's own test.
    std::string_view     label;
    std::chrono::seconds lease;
    Uuid                 runner;
    // Walks the same cursor, calls the same step, reports the same counts, and
    // writes NOTHING — not the documents, not the ledger, and not the lease. A
    // dry run that took the lock could block the real one, and a dry run that
    // wrote a cursor would make the real one skip what it had only pretended to
    // do.
    bool                 dry_run;
};

struct DataMigrationReport final {
    std::vector<StepReport> steps;
    RunOutcome              outcome;
};

// Which database holds the ledger: the FIRST one declared.
//
// One ledger for the deployment, because a step is named once. That is the
// opposite of anvil_schema_meta, which is written in EACH database so that a
// secondary database dropped whole leaves no stale marker claiming its indexes
// exist — a step's row describes work, not a database's state.
[[nodiscard]] inline std::string_view ledger_database(const DatabaseNames& databases) noexcept {
    return databases.names[0];
}

[[nodiscard]] DataMigrationReport run_data_migrations(mongocxx::client& client,
                                                      const MigrationDeps& deps,
                                                      const RunOptions& options);

}  // namespace anvil::db
