// Phase 9 — the migration-step seam, asserted from OUTSIDE anvil.
//
// tests/testapp/migrations.h is the reference consumer's table and its
// static_assert is the shape check: a malformed table is a build failure, never
// a 500 at three in the morning (CLAUDE.md §1). What is left for a runtime test
// is the half a static_assert cannot show — that the predicate REJECTS, which is
// the direction nobody notices has stopped working.

#include <gtest/gtest.h>

#include <array>
#include <span>

#include <bsoncxx/document/view.hpp>

#include "anvil/db/migration_step.h"

#include "testapp/migrations.h"

namespace anvil::db {
namespace {

StepOutcome noop_step(StepContext&, std::span<const bsoncxx::document::view>) noexcept {
    return StepOutcome::Ok;
}

[[nodiscard]] MigrationStep valid_step() noexcept {
    return MigrationStep{"a_step", "users", &noop_step, 100, 0, Cursor::IdRange};
}

TEST(MigrationSeam, TheReferenceTableIsWellFormed) {
    static_assert(step_table_is_well_formed(testapp::kSteps));
    EXPECT_TRUE(steps_are_present(testapp::kSteps));
}

TEST(MigrationSeam, TheReferenceTableCoversBothIdShapes) {
    // The reason there are two steps rather than one. A cursor proved against a
    // 16-byte UUID is a cursor that has never seen a compound `_id`.
    bool saw_id_range = false;
    bool saw_whole_collection = false;
    for (const MigrationStep& step : testapp::kSteps) {
        saw_id_range = saw_id_range || step.cursor == Cursor::IdRange;
        saw_whole_collection = saw_whole_collection || step.cursor == Cursor::WholeCollection;
    }
    EXPECT_TRUE(saw_id_range);
    EXPECT_TRUE(saw_whole_collection);
}

TEST(MigrationSeam, ANameThatIsNotATypeableKeyIsRejected) {
    // The name is a stored `_id` and it is what an operator types back into
    // `--only` while recovering a half-applied migration.
    EXPECT_TRUE(step_name_is_well_formed("2024_06_backfill"));
    EXPECT_TRUE(step_name_is_well_formed("a"));

    EXPECT_FALSE(step_name_is_well_formed(""));
    EXPECT_FALSE(step_name_is_well_formed("Backfill"));
    EXPECT_FALSE(step_name_is_well_formed("back fill"));
    EXPECT_FALSE(step_name_is_well_formed("back-fill"));
    EXPECT_FALSE(step_name_is_well_formed("_leading"));
    EXPECT_FALSE(step_name_is_well_formed("trailing_"));
    EXPECT_FALSE(step_name_is_well_formed("double__underscore"));

    std::array<char, kMaxStepNameLength + 1> too_long{};
    too_long.fill('a');
    EXPECT_FALSE(step_name_is_well_formed(std::string_view{too_long.data(), too_long.size()}));
}

TEST(MigrationSeam, ATableWithTwoStepsOfOneNameIsRejected) {
    // Both would read the other's progress as their own: the name IS the
    // ledger's `_id`.
    const std::array<MigrationStep, 2> duplicated{{valid_step(), valid_step()}};
    EXPECT_FALSE(step_table_is_well_formed(duplicated));
}

TEST(MigrationSeam, ACollectionTheApplicationDoesNotDeclareIsRejected) {
    // It would otherwise resolve to database index 0 — the first one declared —
    // and walk the wrong collection while reporting that it finished.
    MigrationStep step = valid_step();
    step.collection = "not_a_declared_collection";
    const std::array<MigrationStep, 1> table{{step}};
    EXPECT_FALSE(step_table_is_well_formed(table));
}

TEST(MigrationSeam, AZeroBatchSizeIsRejected) {
    MigrationStep step = valid_step();
    step.batch_size = 0;
    const std::array<MigrationStep, 1> table{{step}};
    EXPECT_FALSE(step_table_is_well_formed(table));
}

TEST(MigrationSeam, ANegativeSchemaPreconditionIsRejected) {
    MigrationStep step = valid_step();
    step.min_schema_version = -1;
    const std::array<MigrationStep, 1> table{{step}};
    EXPECT_FALSE(step_table_is_well_formed(table));
}

TEST(MigrationSeam, ANullBodyIsARuntimeCheckAndNotACompileTimeOne) {
    // step_table_is_well_formed deliberately does not compare a function pointer
    // against nullptr: it is not foldable in a constant expression on every
    // compiler, and a well_formed() that does not compile is worse than one that
    // checks less. The check lives here instead, and the runner refuses.
    MigrationStep step = valid_step();
    step.apply = nullptr;
    const std::array<MigrationStep, 1> table{{step}};
    EXPECT_TRUE(step_table_is_well_formed(table));
    EXPECT_FALSE(steps_are_present(table));
}

}  // namespace
}  // namespace anvil::db
