// Phase 9 — the flags, because five applications spelling --dry-run five ways is
// an operator running the wrong one against production.
//
// The parser is the half that needs no cluster, and it is the half where a
// mistake is silent: a misspelled flag that was ignored rather than refused is a
// run against production nobody meant to make.

#include <gtest/gtest.h>

#include <array>

#include "anvil/db/migrate_cli.h"

namespace anvil::db {
namespace {

template <std::size_t N>
[[nodiscard]] MigrateArgs parse(const std::array<const char*, N>& argv) {
    return parse_migrate_args(static_cast<int>(N), argv.data());
}

TEST(MigrateCli, NoArgumentsAppliesEverything) {
    const std::array<const char*, 1> argv{{"migrate"}};
    const MigrateArgs args = parse(argv);
    EXPECT_EQ(args.mode, MigrateMode::Apply);
    EXPECT_FALSE(args.dry_run);
    EXPECT_TRUE(args.only.empty());
}

TEST(MigrateCli, EachModeHasExactlyOneSpelling) {
    const std::array<const char*, 2> dry{{"migrate", "--dry-run"}};
    EXPECT_TRUE(parse(dry).dry_run);
    EXPECT_EQ(parse(dry).mode, MigrateMode::Apply);

    const std::array<const char*, 2> indexes{{"migrate", "--indexes-only"}};
    EXPECT_EQ(parse(indexes).mode, MigrateMode::IndexesOnly);

    const std::array<const char*, 2> status{{"migrate", "--status"}};
    EXPECT_EQ(parse(status).mode, MigrateMode::Status);

    const std::array<const char*, 3> unlock{{"migrate", "--unlock", "a_step"}};
    EXPECT_EQ(parse(unlock).mode, MigrateMode::Unlock);
    EXPECT_EQ(parse(unlock).unlock, "a_step");

    const std::array<const char*, 3> only{{"migrate", "--only", "a_step"}};
    EXPECT_EQ(parse(only).mode, MigrateMode::Apply);
    EXPECT_EQ(parse(only).only, "a_step");
}

TEST(MigrateCli, FlagsCombine) {
    const std::array<const char*, 4> argv{{"migrate", "--dry-run", "--only", "a_step"}};
    const MigrateArgs args = parse(argv);
    EXPECT_EQ(args.mode, MigrateMode::Apply);
    EXPECT_TRUE(args.dry_run);
    EXPECT_EQ(args.only, "a_step");
}

TEST(MigrateCli, AnUnrecognisedArgumentIsRefusedAndNamed) {
    // Refused rather than ignored, and this is the case that matters: a
    // misspelled --dry-run that was silently dropped is a live run somebody
    // believed was a rehearsal.
    const std::array<const char*, 2> typo{{"migrate", "--dryrun"}};
    const MigrateArgs args = parse(typo);
    EXPECT_EQ(args.mode, MigrateMode::Invalid);
    EXPECT_EQ(args.rejected, "--dryrun");

    // A prefix of a real flag is not a real flag. Matching on length as well as
    // content is what stops `--dry` from being accepted as `--dry-run`.
    const std::array<const char*, 2> prefix{{"migrate", "--dry"}};
    EXPECT_EQ(parse(prefix).mode, MigrateMode::Invalid);

    const std::array<const char*, 2> positional{{"migrate", "a_step"}};
    EXPECT_EQ(parse(positional).mode, MigrateMode::Invalid);
}

TEST(MigrateCli, AFlagMissingItsValueIsRefusedRatherThanReadingPastTheEnd) {
    const std::array<const char*, 2> only{{"migrate", "--only"}};
    EXPECT_EQ(parse(only).mode, MigrateMode::Invalid);
    EXPECT_EQ(parse(only).rejected, "--only");

    const std::array<const char*, 2> unlock{{"migrate", "--unlock"}};
    EXPECT_EQ(parse(unlock).mode, MigrateMode::Invalid);
    EXPECT_EQ(parse(unlock).rejected, "--unlock");
}

TEST(MigrateCli, HelpReachesTheUsageTextByTheSameRoute) {
    // Not an error, and it does not need to be a special case: anything the
    // parser does not know reaches the usage text, which is what --help wants.
    const std::array<const char*, 2> argv{{"migrate", "--help"}};
    EXPECT_EQ(parse(argv).mode, MigrateMode::Invalid);
}

TEST(MigrateCli, TheExitCodesAreTheRunOutcomeAndNotACopyOfIt) {
    // A mapping written twice is a mapping that can disagree, and the
    // disagreement would be a deploy script treating a failure as success. The
    // header static_asserts it; this is the same claim where a reader looking
    // for the contract will find it.
    EXPECT_EQ(static_cast<int>(RunOutcome::Applied), kMigrateApplied);
    EXPECT_EQ(static_cast<int>(RunOutcome::StepFailed), kMigrateStepFailed);
    EXPECT_EQ(static_cast<int>(RunOutcome::Held), kMigrateHeld);
    EXPECT_EQ(static_cast<int>(RunOutcome::Inconsistent), kMigrateInconsistent);
}

}  // namespace
}  // namespace anvil::db
