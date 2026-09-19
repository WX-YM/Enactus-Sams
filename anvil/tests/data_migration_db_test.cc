// Phase 9 — what the runner promises, asserted against a live cluster.
//
// Four properties, and each is one the design rests on rather than one the code
// happens to have:
//
//   a step resumed from a recorded `_id` re-applies the overlapping batch and
//   converges — that is the design and not a rough edge;
//   a step that reached the end records its terminal state and a second
//   invocation does NOTHING at all;
//   a failed step blocks every step after it, because N+1 was written by
//   somebody who assumed N ran;
//   --dry-run walks the same cursor, calls the same step, reports the same
//   counts and writes nothing — asserted by comparing the collection before and
//   after, never by reading the runner's own report.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/core/uuid.h"
#include "anvil/db/data_migrations.h"
#include "anvil/db/migrate_cli.h"
#include "anvil/db/mongo_pool.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/indexes.h"
#include "testapp/migrations.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

constexpr std::string_view kUsers = "users";
constexpr std::string_view kSections = "sections";
constexpr std::string_view kBackfill = "2024_06_backfill_display_name";
constexpr std::string_view kDropTitle = "2024_07_drop_legacy_section_title";

// A step that always refuses, for the one property that needs a failure.
StepOutcome always_fails(StepContext&, std::span<const bsoncxx::document::view>) noexcept {
    return StepOutcome::Failed;
}

// A step over a collection whose `_id` is NOT a uuid, declared as IdRange. It is
// the mistake the two cursor shapes exist to catch, and it is caught by the
// runner rather than by a reviewer.
StepOutcome never_runs(StepContext&, std::span<const bsoncxx::document::view>) noexcept {
    return StepOutcome::Ok;
}

// Applies the reference transform to the first batch and refuses the second.
//
// That is how the crash is simulated: the run STOPS where a crash would stop it,
// at a batch boundary with the earlier batches already written, rather than by
// back-dating a ledger row into a state no crash produces.
//
// A file-scope counter is safe here because gtest_discover_tests runs one case
// per process.
std::size_t g_batches_seen = 0;

StepOutcome stops_after_the_first_batch(
    StepContext& context, std::span<const bsoncxx::document::view> batch) noexcept {
    if (g_batches_seen > 0) { return StepOutcome::Failed; }
    ++g_batches_seen;
    return testapp::backfill_display_name(context, batch);
}

class DataMigrationDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kUsers);
        anvil::testfixture::clear_collection(**client_, kSections);
        ledger_collection().delete_many(make_document());
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] mongocxx::collection ledger_collection() {
        return (**client_)[anvil::testfixture::scratch_database()]
                          [std::string{kMigrationLedgerCollection}];
    }

    [[nodiscard]] mongocxx::collection collection(std::string_view name) {
        return (**client_)[std::string{
            anvil::testfixture::scratch_names().for_collection(name)}][std::string{name}];
    }

    // A user with the two fields the reference step derives from, plus the two
    // the catalogue puts a unique index on — a row with neither collides with
    // the next one on `{em: null}`, which is the partial-index lesson the
    // reference indexes.h already records.
    void insert_users(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const Uuid id = uuid::generate_v7();
            const std::string handle = "user" + std::to_string(i);
            collection(kUsers).insert_one(
                make_document(kvp("_id", codec::uuid_bin(id)),
                              kvp("un", handle),
                              kvp("em", handle + "@example.test"),
                              kvp("und", handle),
                              kvp("emd", handle + "@example.test")));
        }
    }

    // Two documents per key under a compound `_id`, which is the shape the
    // second cursor exists for.
    void insert_sections(std::size_t count, bool with_legacy_title) {
        for (std::size_t i = 0; i < count; ++i) {
            for (int state = 0; state < 2; ++state) {
                bsoncxx::builder::basic::document doc;
                doc.append(kvp("_id", make_document(kvp("k", "section" + std::to_string(i)),
                                                    kvp("s", bsoncxx::types::b_int32{state}))));
                if (with_legacy_title) { doc.append(kvp("title", "legacy")); }
                collection(kSections).insert_one(doc.view());
            }
        }
    }

    [[nodiscard]] RunOptions options(bool dry_run) const {
        return RunOptions{{}, "test-runner", std::chrono::seconds{30}, uuid::generate_v4(),
                          dry_run};
    }

    [[nodiscard]] MigrationDeps deps(std::span<const MigrationStep> steps) const {
        return MigrationDeps{anvil::testfixture::scratch_names(),
                             anvil::testfixture::test_uri(),
                             testapp::kIndexes,
                             testapp::kRetiredIndexes,
                             steps,
                             testapp::kCollectionOptions,
                             testapp::kSchemaVersion};
    }

    // Every document in a collection, as JSON in `_id` order: the only way to
    // assert a dry run wrote nothing that does not go through the report the dry
    // run itself produced.
    [[nodiscard]] std::vector<std::string> snapshot(std::string_view name) {
        mongocxx::options::find sorted{};
        sorted.sort(make_document(kvp("_id", bsoncxx::types::b_int32{1})));
        std::vector<std::string> rows;
        for (const bsoncxx::document::view& doc : collection(name).find({}, sorted)) {
            rows.push_back(bsoncxx::to_json(doc));
        }
        return rows;
    }

    [[nodiscard]] std::int64_t with_display_name() {
        return collection(kUsers).count_documents(
            make_document(kvp("display_name", make_document(kvp("$exists", true)))));
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(DataMigrationDb, AStepWalksEveryDocumentInBatchesAndRecordsItsTerminalState) {
    // More documents than one batch holds, so the cursor is exercised rather
    // than merely present: 500 is the reference step's batch size.
    insert_users(12);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), options(false));

    EXPECT_EQ(report.outcome, RunOutcome::Applied);
    ASSERT_EQ(report.steps.size(), 1U);
    EXPECT_EQ(report.steps.front().state, StepState::Applied);
    EXPECT_EQ(report.steps.front().documents_scanned, 12);
    EXPECT_EQ(report.steps.front().documents_written, 12);
    // Twelve documents in batches of five: three reads that returned something.
    EXPECT_EQ(report.steps.front().batches, 3);
    EXPECT_EQ(with_display_name(), 12);
}

TEST_F(DataMigrationDb, ASecondInvocationOfAFinishedStepDoesNothingAtAll) {
    insert_users(4);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    ASSERT_EQ(run_data_migrations(db(), deps(steps), options(false)).outcome,
              RunOutcome::Applied);

    const DataMigrationReport again = run_data_migrations(db(), deps(steps), options(false));
    EXPECT_EQ(again.outcome, RunOutcome::Applied);
    EXPECT_EQ(again.steps.front().state, StepState::AlreadyDone);
    // Nothing read, nothing written, and — because a finished step is never
    // claimed — no second attempt on the ledger either.
    EXPECT_EQ(again.steps.front().batches, 0);
    EXPECT_EQ(again.steps.front().documents_written, 0);

    const auto row = ledger_collection().find_one(make_document(kvp("_id", kBackfill)));
    ASSERT_TRUE(row);
    EXPECT_EQ(row->view()["attempts"].get_int64().value, 1);
}

TEST_F(DataMigrationDb, AStepResumedFromARecordedCursorReAppliesTheOverlapAndConverges) {
    insert_users(10);
    const std::array<MigrationStep, 1> interrupted{
        {{kBackfill, kUsers, &stops_after_the_first_batch, 4, 1, Cursor::IdRange}}};
    const std::array<MigrationStep, 1> whole{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 4, 1, Cursor::IdRange}}};

    // Stop where a crash would stop: one batch applied and recorded, the rest
    // untouched.
    const DataMigrationReport stopped =
        run_data_migrations(db(), deps(interrupted), options(false));
    ASSERT_EQ(stopped.outcome, RunOutcome::StepFailed);
    ASSERT_EQ(with_display_name(), 4);

    // Now the window the design actually pays for. A process that dies between
    // applying a batch and recording it leaves the writes on disk and the cursor
    // where it was, so the next run re-reads documents it has already
    // transformed. Rewinding the cursor is exactly that state, and it is the one
    // state a back-dated row could not produce.
    ledger_collection().update_one(
        make_document(kvp("_id", kBackfill)),
        make_document(kvp("$set", make_document(kvp("cur", bsoncxx::types::b_null{}),
                                                kvp("docs", bsoncxx::types::b_int64{0}),
                                                kvp("batches", bsoncxx::types::b_int64{0})))));

    const DataMigrationReport resumed = run_data_migrations(db(), deps(whole), options(false));
    EXPECT_EQ(resumed.outcome, RunOutcome::Applied);
    // It walks all ten, four of which it has already done. Re-applying them is a
    // no-op because the step writes what the document should BE, so the overlap
    // is the design rather than a rough edge.
    EXPECT_EQ(resumed.steps.front().documents_scanned, 10);
    EXPECT_EQ(with_display_name(), 10);
    const std::vector<std::string> after_resuming = snapshot(kUsers);

    // And the result is IDENTICAL to the run that was never interrupted, which
    // is the only assertion that proves idempotence rather than describing it.
    collection(kUsers).update_many(
        make_document(), make_document(kvp("$unset", make_document(kvp("display_name", "")))));
    ledger_collection().delete_many(make_document());

    ASSERT_EQ(run_data_migrations(db(), deps(whole), options(false)).outcome,
              RunOutcome::Applied);
    EXPECT_EQ(snapshot(kUsers), after_resuming);
}

TEST_F(DataMigrationDb, ADryRunWalksTheSameCursorAndWritesNothing) {
    insert_users(7);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 3, 1, Cursor::IdRange}}};

    const std::vector<std::string> before = snapshot(kUsers);

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), options(true));
    EXPECT_EQ(report.outcome, RunOutcome::Applied);
    // The same cursor, the same step, the same counts.
    EXPECT_EQ(report.steps.front().documents_scanned, 7);
    EXPECT_EQ(report.steps.front().documents_written, 7);
    EXPECT_EQ(report.steps.front().batches, 3);

    // Asserted by comparing the collection, never by reading the runner's own
    // report: a dry run that reported nothing written while writing would pass
    // any assertion made against its report.
    EXPECT_EQ(snapshot(kUsers), before);
    EXPECT_EQ(with_display_name(), 0);

    // And it wrote no ledger row either. A dry run that took the lock could
    // block the real one, and a dry run that recorded a cursor would make the
    // real one skip what it had only pretended to do.
    EXPECT_EQ(ledger_collection().count_documents(make_document()), 0);
}

TEST_F(DataMigrationDb, TheCompoundIdCursorWalksSectionsWithoutAUuidInSight) {
    // The reason the reference table carries two steps. `sections` keys on
    // {k, s}, so every `$gt` here compares embedded documents rather than
    // 16 bytes of binary.
    insert_sections(6, true);
    const std::array<MigrationStep, 1> steps{{{kDropTitle, kSections,
                                               &testapp::drop_legacy_section_title, 4, 1,
                                               Cursor::WholeCollection}}};

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), options(false));
    EXPECT_EQ(report.outcome, RunOutcome::Applied);
    EXPECT_EQ(report.steps.front().documents_scanned, 12);
    EXPECT_EQ(report.steps.front().documents_written, 12);
    EXPECT_EQ(collection(kSections).count_documents(
                  make_document(kvp("title", make_document(kvp("$exists", true))))),
              0);

    // A batch that needs no change asks for no write at all, rather than one
    // that matches and does nothing.
    ledger_collection().delete_many(make_document());
    const DataMigrationReport clean = run_data_migrations(db(), deps(steps), options(false));
    EXPECT_EQ(clean.steps.front().documents_scanned, 12);
    EXPECT_EQ(clean.steps.front().documents_written, 0);
}

TEST_F(DataMigrationDb, AnIdRangeStepOverACompoundIdIsRefusedRatherThanWalkingAPrefix) {
    insert_sections(2, false);
    const std::array<MigrationStep, 1> steps{
        {{"mislabelled_cursor", kSections, &never_runs, 4, 1, Cursor::IdRange}}};

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), options(false));
    // Exit 3: the catalogue does not describe this cluster. The alternative is a
    // run that walks a prefix and reports that it finished.
    EXPECT_EQ(report.outcome, RunOutcome::Inconsistent);
    EXPECT_EQ(report.steps.front().state, StepState::Refused);
}

TEST_F(DataMigrationDb, AStepWhosePreconditionIsUnmetIsRefusedBeforeAnythingIsClaimed) {
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5,
          testapp::kSchemaVersion + 1, Cursor::IdRange}}};

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), options(false));
    EXPECT_EQ(report.outcome, RunOutcome::Inconsistent);
    EXPECT_EQ(report.steps.front().state, StepState::Refused);
    // Nothing was attempted, so nothing is half-applied and no lease was taken.
    EXPECT_EQ(ledger_collection().count_documents(make_document()), 0);
}

TEST_F(DataMigrationDb, AFailedStepBlocksEveryStepAfterIt) {
    insert_users(3);
    insert_sections(2, true);
    const std::array<MigrationStep, 2> steps{
        {{"failing_step", kUsers, &always_fails, 5, 1, Cursor::IdRange},
         {kDropTitle, kSections, &testapp::drop_legacy_section_title, 5, 1,
          Cursor::WholeCollection}}};

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), options(false));
    EXPECT_EQ(report.outcome, RunOutcome::StepFailed);
    ASSERT_EQ(report.steps.size(), 2U);
    EXPECT_EQ(report.steps[0].state, StepState::Failed);
    // Steps are ordered and N+1 was written by somebody who assumed N ran, so
    // continuing past a failure produces a state no step's precondition
    // describes.
    EXPECT_EQ(report.steps[1].state, StepState::Blocked);
    EXPECT_EQ(collection(kSections).count_documents(
                  make_document(kvp("title", make_document(kvp("$exists", true))))),
              4);

    // The failure is on the row, and the lease is released so the next runner
    // does not have to wait one out to retry.
    const auto row = ledger_collection().find_one(make_document(kvp("_id", "failing_step")));
    ASSERT_TRUE(row);
    EXPECT_FALSE(row->view()["done"].get_bool().value);
    EXPECT_FALSE(std::string_view{row->view()["err"].get_string().value.data()}.empty());
}

TEST_F(DataMigrationDb, ADataRunLeavesTheSchemaVersionExactlyWhereItFoundIt) {
    // The regression easiest to ship and hardest to notice. The two ledgers
    // answer questions with different shapes and the integer is not shared.
    const std::int32_t before =
        applied_schema_version(db(), anvil::testfixture::scratch_database());

    insert_users(2);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};
    ASSERT_EQ(run_data_migrations(db(), deps(steps), options(false)).outcome,
              RunOutcome::Applied);

    EXPECT_EQ(applied_schema_version(db(), anvil::testfixture::scratch_database()), before);
}

TEST_F(DataMigrationDb, OnlyRunsOneStepAndLeavesTheRestUntouched) {
    insert_users(3);
    insert_sections(2, true);
    const std::array<MigrationStep, 2> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange},
         {kDropTitle, kSections, &testapp::drop_legacy_section_title, 5, 1,
          Cursor::WholeCollection}}};

    RunOptions only = options(false);
    only.only = kDropTitle;

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), only);
    EXPECT_EQ(report.outcome, RunOutcome::Applied);
    EXPECT_EQ(report.steps[0].state, StepState::Skipped);
    EXPECT_EQ(report.steps[1].state, StepState::Applied);
    EXPECT_EQ(with_display_name(), 0);
}

TEST_F(DataMigrationDb, AStepAnotherRunnerHoldsIsReportedHeldAndNotWaitedOn) {
    insert_users(2);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    MigrationLedger other{anvil::testfixture::scratch_database(), uuid::generate_v4(),
                          "the-other-runner"};
    ASSERT_TRUE(other.claim(db(), kBackfill, std::chrono::hours{1}));

    const DataMigrationReport report = run_data_migrations(db(), deps(steps), options(false));
    // Exit 2, immediately. A second operator watching a migration hang is how
    // two of them end up force-killing the one that was working.
    EXPECT_EQ(report.outcome, RunOutcome::Held);
    EXPECT_EQ(report.steps.front().state, StepState::Held);
    EXPECT_EQ(with_display_name(), 0);
}

// --- the CLI body, against a live cluster ----------------------------------
//
// The exit code is the contract — a deploy script branches on it — so it is
// asserted against the cluster rather than inferred from the runner's report.

TEST_F(DataMigrationDb, IndexesOnlyAppliesTheSchemaAndRunsNoStep) {
    insert_users(3);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    MigrateArgs args{};
    args.mode = MigrateMode::IndexesOnly;

    EXPECT_EQ(migrate_run(db(), deps(steps), args), kMigrateApplied);
    // The deploy step that has to happen before the new code starts, without the
    // pass that walks every document.
    EXPECT_EQ(with_display_name(), 0);
    EXPECT_EQ(ledger_collection().count_documents(make_document()), 0);
}

TEST_F(DataMigrationDb, TheExitCodeIsTwoWhenAnotherRunnerHoldsAStep) {
    insert_users(2);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    MigrationLedger other{anvil::testfixture::scratch_database(), uuid::generate_v4(),
                          "the-other-runner"};
    ASSERT_TRUE(other.claim(db(), kBackfill, std::chrono::hours{1}));

    MigrateArgs args{};
    args.mode = MigrateMode::Apply;
    EXPECT_EQ(migrate_run(db(), deps(steps), args), kMigrateHeld);
}

TEST_F(DataMigrationDb, StatusReadsAndChangesNothing) {
    insert_users(2);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    MigrateArgs args{};
    args.mode = MigrateMode::Status;

    EXPECT_EQ(migrate_run(db(), deps(steps), args), kMigrateApplied);
    EXPECT_EQ(with_display_name(), 0);
    EXPECT_EQ(ledger_collection().count_documents(make_document()), 0);
}

TEST_F(DataMigrationDb, UnlockNamesTheHolderAndThenClearsTheLease) {
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    MigrationLedger other{anvil::testfixture::scratch_database(), uuid::generate_v4(),
                          "the-stuck-runner"};
    ASSERT_TRUE(other.claim(db(), kBackfill, std::chrono::hours{1}));

    MigrateArgs args{};
    args.mode = MigrateMode::Unlock;
    args.unlock = kBackfill;
    EXPECT_EQ(migrate_run(db(), deps(steps), args), kMigrateApplied);

    // And the step is claimable again, which is the only thing --unlock is for.
    MigrationLedger next{anvil::testfixture::scratch_database(), uuid::generate_v4(), "next"};
    EXPECT_TRUE(next.claim(db(), kBackfill, std::chrono::seconds{30}));
}

TEST_F(DataMigrationDb, UnlockingAStepThatWasNeverRunIsExitThree) {
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    MigrateArgs args{};
    args.mode = MigrateMode::Unlock;
    args.unlock = kBackfill;
    EXPECT_EQ(migrate_run(db(), deps(steps), args), kMigrateInconsistent);
}

TEST_F(DataMigrationDb, ADryRunThroughTheCliTouchesNeitherTheSchemaNorTheDocuments) {
    insert_users(3);
    const std::array<MigrationStep, 1> steps{
        {{kBackfill, kUsers, &testapp::backfill_display_name, 5, 1, Cursor::IdRange}}};

    const std::vector<std::string> before = snapshot(kUsers);

    MigrateArgs args{};
    args.mode = MigrateMode::Apply;
    args.dry_run = true;

    EXPECT_EQ(migrate_run(db(), deps(steps), args), kMigrateApplied);
    EXPECT_EQ(snapshot(kUsers), before);
    // Creating a collection or building an index is exactly the kind of thing an
    // operator runs --dry-run to find out about before doing, so a dry run does
    // not do it either.
    EXPECT_EQ(ledger_collection().count_documents(make_document()), 0);
}

}  // namespace
}  // namespace anvil::db
