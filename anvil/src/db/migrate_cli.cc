#include "anvil/db/migrate_cli.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include "anvil/core/uuid.h"
#include "anvil/db/migration_ledger.h"
#include "anvil/db/mongo_pool.h"

namespace anvil::db {
namespace {

// stdout through <cstdio> rather than <iostream>.
//
// migrate_cli.cc is compiled into anvil::platform, so an <iostream> include here
// would put that header's static initialiser into every application that links
// the library — for a CLI most of them will never invoke.
void say(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
}

[[nodiscard]] bool matches(const char* argument, std::string_view flag) noexcept {
    return std::strlen(argument) == flag.size() &&
           std::memcmp(argument, flag.data(), flag.size()) == 0;
}

[[nodiscard]] std::string_view state_name(StepState state) noexcept {
    switch (state) {
        case StepState::Skipped: return "skipped";
        case StepState::AlreadyDone: return "already done";
        case StepState::Applied: return "applied";
        case StepState::Failed: return "FAILED";
        case StepState::Held: return "HELD";
        case StepState::Blocked: return "blocked";
        case StepState::Refused: return "REFUSED";
    }
    return "unknown";
}

// One process's identity on a ledger row, for the operator who has to decide
// which host to go and look at.
[[nodiscard]] std::string runner_label() {
    std::array<char, 64> host{};
    if (::gethostname(host.data(), host.size() - 1) != 0) { host[0] = '?'; }
    return std::string{host.data()} + ":" + std::to_string(::getpid());
}

void print_step(const StepReport& step) {
    std::fprintf(stdout, "  %-40.*s %-12.*s  scanned %lld  written %lld  batches %lld\n",
                 static_cast<int>(step.name.size()), step.name.data(),
                 static_cast<int>(state_name(step.state).size()), state_name(step.state).data(),
                 static_cast<long long>(step.documents_scanned),
                 static_cast<long long>(step.documents_written),
                 static_cast<long long>(step.batches));
    if (!step.detail.empty()) {
        std::fprintf(stdout, "      %s\n", step.detail.c_str());
    }
}

void print_findings(std::string_view heading, const std::vector<IndexFinding>& findings) {
    if (findings.empty()) { return; }
    std::fprintf(stdout, "\n%.*s\n", static_cast<int>(heading.size()), heading.data());
    for (const IndexFinding& finding : findings) {
        std::fprintf(stdout, "  %s.%s  %s\n", finding.database.c_str(),
                     finding.collection.c_str(), finding.name.c_str());
    }
}

[[nodiscard]] int run_status(mongocxx::client& client, const MigrationDeps& deps) {
    for (std::size_t i = 0; i < deps.databases.names.size(); ++i) {
        const std::string_view name = deps.databases.names[i];
        std::fprintf(stdout, "schema version  %-24.*s %d (catalogue declares %d)\n",
                     static_cast<int>(name.size()), name.data(),
                     applied_schema_version(client, name), deps.schema_version);
    }

    MigrationLedger ledger{std::string{ledger_database(deps.databases)}, uuid::generate_v4(),
                           runner_label()};
    say("\nsteps");
    for (const MigrationStep& step : deps.steps) {
        const Result<std::optional<LedgerEntry>> row = ledger.read(client, step.name);
        if (!row) {
            std::fprintf(stdout, "  %-40.*s  the ledger could not be read\n",
                         static_cast<int>(step.name.size()), step.name.data());
            return kMigrateInconsistent;
        }
        if (!row.value().has_value()) {
            std::fprintf(stdout, "  %-40.*s  never run\n",
                         static_cast<int>(step.name.size()), step.name.data());
            continue;
        }
        const LedgerEntry& entry = *row.value();
        std::fprintf(stdout,
                     "  %-40.*s  %s  documents %lld  batches %lld  attempts %lld%s\n",
                     static_cast<int>(step.name.size()), step.name.data(),
                     entry.done ? "done      " : "unfinished",
                     static_cast<long long>(entry.documents),
                     static_cast<long long>(entry.batches),
                     static_cast<long long>(entry.attempts),
                     entry.owner.has_value() ? "  HELD" : "");
        if (entry.owner.has_value()) {
            std::fprintf(stdout, "      held by %s\n", entry.owner_label.c_str());
        }
        if (!entry.last_error.empty()) {
            std::fprintf(stdout, "      last error: %s\n", entry.last_error.c_str());
        }
    }

    // Reported and never acted on. An operator deciding what to do about a
    // hand-made index needs to be reading it at a moment of their choosing.
    print_findings("retired indexes this cluster still carries", 
                   verify_retired(client, deps.databases, deps.retired));
    print_findings("indexes nobody declared",
                   report_undeclared(client, deps.databases, deps.indexes));
    return kMigrateApplied;
}

[[nodiscard]] int run_unlock(mongocxx::client& client, const MigrationDeps& deps,
                             std::string_view step) {
    MigrationLedger ledger{std::string{ledger_database(deps.databases)}, uuid::generate_v4(),
                           runner_label()};

    // Printed BEFORE anything is cleared. This is the one operation whose whole
    // purpose is to override the safety net, so the thing being overridden is
    // named first and the operator gets to change their mind.
    const Result<std::optional<LedgerEntry>> row = ledger.read(client, step);
    if (!row || !row.value().has_value()) {
        std::fprintf(stderr, "no ledger row for step %.*s\n", static_cast<int>(step.size()),
                     step.data());
        return kMigrateInconsistent;
    }
    const LedgerEntry& entry = *row.value();
    if (!entry.owner.has_value()) {
        std::fprintf(stdout, "step %.*s is not held; nothing to unlock\n",
                     static_cast<int>(step.size()), step.data());
        return kMigrateApplied;
    }
    std::fprintf(stdout, "step %.*s is held by %s\n", static_cast<int>(step.size()), step.data(),
                 entry.owner_label.c_str());

    if (const Status released = ledger.release(client, step); !released) {
        std::fputs("the lease could not be released\n", stderr);
        return kMigrateInconsistent;
    }
    say("lease released");
    return kMigrateApplied;
}

// Collection options, then the index catalogue. In that order because
// createIndexes creates a missing collection implicitly, and one created that
// way is not clustered, not capped and not a timeseries.
[[nodiscard]] int apply_schema(mongocxx::client& client, const MigrationDeps& deps) {
    const CollectionOptionsReport options = apply_collection_options(
        client, deps.databases, deps.collection_options, OptionsPhase::Create);
    for (const OptionsViolation& violation : options.violations) {
        std::fprintf(stderr, "collection %s: %s\n", violation.collection.c_str(),
                     violation.reason.c_str());
    }
    if (!options.violations.empty()) { return kMigrateInconsistent; }

    const MigrationReport indexes = apply_migrations(client, deps.databases, deps.indexes,
                                                     deps.schema_version, deps.retired);
    std::fprintf(stdout, "collections created %zu; indexes applied %zu; schema version %d -> %d\n",
                 options.created, indexes.indexes_applied, indexes.previous_version,
                 indexes.current_version);
    return kMigrateApplied;
}

}  // namespace

MigrateArgs parse_migrate_args(int argc, const char* const* argv) noexcept {
    MigrateArgs args{};
    args.mode = MigrateMode::Apply;

    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        if (matches(argument, "--dry-run")) {
            args.dry_run = true;
        } else if (matches(argument, "--indexes-only")) {
            args.mode = MigrateMode::IndexesOnly;
        } else if (matches(argument, "--status")) {
            args.mode = MigrateMode::Status;
        } else if (matches(argument, "--only")) {
            if (i + 1 >= argc) {
                args.mode = MigrateMode::Invalid;
                args.rejected = "--only";
                return args;
            }
            args.only = argv[++i];
        } else if (matches(argument, "--unlock")) {
            if (i + 1 >= argc) {
                args.mode = MigrateMode::Invalid;
                args.rejected = "--unlock";
                return args;
            }
            args.mode = MigrateMode::Unlock;
            args.unlock = argv[++i];
        } else {
            // Including --help, which is not an error but reaches the same
            // usage text. Anything unrecognised is refused rather than ignored:
            // a misspelled --dry-run that was silently dropped is a run against
            // production nobody meant to make.
            args.mode = MigrateMode::Invalid;
            args.rejected = argument;
            return args;
        }
    }
    return args;
}

void print_migrate_usage() {
    say("usage: migrate [--dry-run] [--only <step>] [--indexes-only] [--status]");
    say("               [--unlock <step>]");
    say("");
    say("  --dry-run       walk every step and write nothing at all");
    say("  --only <step>   run one step by name");
    say("  --indexes-only  collection options and indexes, no data steps");
    say("  --status        print the ledger and what the cluster carries");
    say("  --unlock <step> clear one step's lease, whoever holds it");
    say("");
    say("exit: 0 applied or nothing to do, 1 a step failed, 2 held by another");
    say("      runner, 3 the run could not be attempted as described");
}

int migrate_run(mongocxx::client& client, const MigrationDeps& deps, const MigrateArgs& args) {
    if (args.mode == MigrateMode::Invalid) {
        std::fprintf(stderr, "unrecognised argument: %.*s\n",
                     static_cast<int>(args.rejected.size()), args.rejected.data());
        print_migrate_usage();
        return kMigrateInconsistent;
    }
    if (args.mode == MigrateMode::Status) { return run_status(client, deps); }
    if (args.mode == MigrateMode::Unlock) { return run_unlock(client, deps, args.unlock); }

    // A dry run writes nothing, and that includes the schema: creating a
    // collection or building an index is exactly the kind of thing an operator
    // runs --dry-run to find out about before doing.
    if (!args.dry_run) {
        if (const int applied = apply_schema(client, deps); applied != kMigrateApplied) {
            return applied;
        }
    }
    if (args.mode == MigrateMode::IndexesOnly) { return kMigrateApplied; }

    // A NAMED local, because RunOptions::label is a view and this call outlives
    // the full expression that builds the options. ASan caught the temporary as
    // a heap-use-after-free the first time this ran, which is the failure mode
    // ENGINEERING_RULES.md §2.2 names as the single most likely crash in code built on this
    // library.
    const std::string label = runner_label();
    const RunOptions options{args.only, label, kDefaultLeaseSeconds, uuid::generate_v4(),
                             args.dry_run};
    const DataMigrationReport report = run_data_migrations(client, deps, options);

    say(args.dry_run ? "\nsteps (dry run — nothing was written)" : "\nsteps");
    for (const StepReport& step : report.steps) { print_step(step); }

    // Validators land AFTER the data steps, because adding one to a collection
    // that already holds documents rejects the writes that would have made them
    // conform. A run that did not get through its steps does not reach here.
    if (!args.dry_run && report.outcome == RunOutcome::Applied) {
        const CollectionOptionsReport validated = apply_collection_options(
            client, deps.databases, deps.collection_options, OptionsPhase::Validate);
        for (const OptionsViolation& violation : validated.violations) {
            std::fprintf(stderr, "collection %s: %s\n", violation.collection.c_str(),
                         violation.reason.c_str());
        }
        if (!validated.violations.empty()) { return kMigrateInconsistent; }
        std::fprintf(stdout, "validators applied %zu\n", validated.validators_applied);
    }

    return static_cast<int>(report.outcome);
}

int migrate_main(int argc, const char* const* argv, const MigrationDeps& deps) {
    const MigrateArgs args = parse_migrate_args(argc, argv);
    if (args.mode == MigrateMode::Invalid) {
        std::fprintf(stderr, "unrecognised argument: %.*s\n",
                     static_cast<int>(args.rejected.size()), args.rejected.data());
        print_migrate_usage();
        return kMigrateInconsistent;
    }

    try {
        // One client. A migration is a single-threaded process that exits, so a
        // pool of one is the whole requirement — and MongoPool is still what
        // owns the driver instance, because the instance must be constructed
        // once and outlive every client drawn from it.
        MongoPool::init(deps.mongodb_uri, 1);
        auto client = MongoPool::instance().acquire();
        return migrate_run(*client, deps, args);
    } catch (const std::exception&) {
        // No driver text. A migrate binary runs in a deploy log that is kept,
        // and a connection string in an exception message is a connection string
        // in that log.
        std::fputs("the migration could not reach the cluster as configured\n", stderr);
        return kMigrateInconsistent;
    }
}

}  // namespace anvil::db
