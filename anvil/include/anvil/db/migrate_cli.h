#pragma once

// anvil ships no main() — but it ships the BODY of one.
//
// Five applications spelling --dry-run five ways is an operator running the
// wrong one against production, at the moment they are least able to check. So
// the flags, the output and the exit codes are the library's, and an
// application's migrate binary is a main() that resolves its configuration and
// calls this.
//
// It is split in three so the parts that need no cluster can be tested without
// one: parse_migrate_args reads the command line, migrate_run does the work
// against a client somebody else opened, and migrate_main is the thin wrapper
// that opens the pool. The split is also what lets a test drive the runner
// inside a binary whose MongoPool is already initialised — init throws on its
// second call.

#include <cstdint>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/db/data_migrations.h"

namespace anvil::db {

enum class MigrateMode : std::uint8_t {
    // Collection options, then indexes, then the data steps, then validators.
    Apply = 0,
    // Everything but the data steps. The deploy step that has to happen before
    // the new code starts, without the pass that walks every document.
    IndexesOnly = 1,
    // Reads and prints; changes nothing.
    Status = 2,
    // Clears one step's lease. Loud, separate, and it prints the current owner
    // and expiry before it acts, because it is the one flag whose whole purpose
    // is to override the safety net.
    Unlock = 3,
    // The command line was not understood.
    Invalid = 4,
};

struct MigrateArgs final {
    std::string_view only;      // --only <name>
    std::string_view unlock;    // --unlock <name>
    // What was not understood, for the usage message. Never echoed back into
    // anything but stderr.
    std::string_view rejected;
    MigrateMode      mode;
    bool             dry_run;
};

// The exit codes, which are what a deploy script branches on.
//
// They are the RunOutcome values, asserted below rather than transcribed: a
// mapping written twice is a mapping that can disagree, and the disagreement
// would be a deploy script treating a failure as success.
inline constexpr int kMigrateApplied = 0;
inline constexpr int kMigrateStepFailed = 1;
inline constexpr int kMigrateHeld = 2;
// The run could not be attempted as described — the catalogue does not match the
// cluster, or the invocation was not understood.
inline constexpr int kMigrateInconsistent = 3;

static_assert(static_cast<int>(RunOutcome::Applied) == kMigrateApplied);
static_assert(static_cast<int>(RunOutcome::StepFailed) == kMigrateStepFailed);
static_assert(static_cast<int>(RunOutcome::Held) == kMigrateHeld);
static_assert(static_cast<int>(RunOutcome::Inconsistent) == kMigrateInconsistent);

// Never throws, never allocates, and holds views into `argv` — which outlives
// main by construction.
[[nodiscard]] MigrateArgs parse_migrate_args(int argc, const char* const* argv) noexcept;

void print_migrate_usage();

// The work, against a client the caller opened. Prints its report to stdout and
// returns the exit code.
[[nodiscard]] int migrate_run(mongocxx::client& client, const MigrationDeps& deps,
                              const MigrateArgs& args);

// The whole of a migrate binary's main(), pool included.
[[nodiscard]] int migrate_main(int argc, const char* const* argv, const MigrationDeps& deps);

}  // namespace anvil::db
