// Phase 8 — the property the rollup's correctness rests on.
//
// A rollup that $incs is not re-runnable. Every queue in this system is
// at-least-once (CLAUDE.md §6), so a rollup that added to what it found would
// double-count the first time a worker was reclaimed after a lease expiry — and
// the resulting number is wrong in a way nothing reports and nobody can
// reconstruct. Running it twice and asserting ONE document with IDENTICAL values
// is the only assertion that proves the design.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "anvil/analytics/query.h"
#include "anvil/analytics/repository.h"
#include "anvil/analytics/rollup.h"
#include "anvil/core/uuid.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "events.h"

namespace anvil::analytics {
namespace {

using anvil::testfixture::scratch_names;

constexpr std::string_view kEvents = "analytics_events";
constexpr std::string_view kSessions = "analytics_sessions";
constexpr std::string_view kRollups = "analytics_rollups";

// events and sessions live in the second database and rollups in the first.
// Resolving per collection is the whole reason these take DatabaseNames: a job
// that assumed one database would read an empty collection and write into one
// nothing queries, and every count would silently be zero.
inline constexpr AnalyticsCollections kCollections{kEvents, kSessions, kRollups};

[[nodiscard]] EventCode code_of(testapp::Event event) noexcept {
    return static_cast<EventCode>(event);
}

class RollupDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kEvents);
        anvil::testfixture::clear_collection(**client_, kRollups);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kEvents)};
    }

    [[nodiscard]] static EventRepository events() {
        return EventRepository{database(), kEvents};
    }

    [[nodiscard]] static RollupJob job(Granularity granularity = Granularity::Hour) {
        return RollupJob{scratch_names(), kCollections, granularity};
    }

    [[nodiscard]] std::int64_t stored_rollups() {
        return db()[std::string{scratch_names().for_collection(kRollups)}]
            [std::string{kRollups}]
                .count_documents(bsoncxx::builder::basic::make_document());
    }

    // Writes rows directly, so a case can put an event at an instant of its
    // choosing rather than waiting for one.
    void write(testapp::Surface surface, db::TimeMs at, std::uint32_t repeats,
               std::uint8_t session_byte) {
        DimensionValues dimensions = no_dimensions();
        dimensions[0] = static_cast<std::uint8_t>(surface);
        VisitorId session{};
        session[0] = session_byte;
        const EventRow row{at,
                           Event{code_of(testapp::Event::SignupCompleted), dimensions, session,
                                 std::nullopt},
                           repeats};
        const std::vector<EventRow> batch{row};
        ASSERT_TRUE(events()
                        .append_many(db(), batch, db::now_ms(), std::chrono::hours{24 * 30})
                        .ok());
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(RollupDb, RunningTheSameRollupTwiceProducesOneIdenticalDocument) {
    const db::TimeMs now = db::now_ms();
    const db::TimeMs bucket = bucket_start(now - std::chrono::hours{2}, Granularity::Hour);

    write(testapp::Surface::Web, bucket + std::chrono::minutes{1}, 3, 1);
    write(testapp::Surface::Web, bucket + std::chrono::minutes{2}, 4, 2);
    write(testapp::Surface::Ios, bucket + std::chrono::minutes{3}, 1, 1);

    const RollupJob rollup = job();
    const Result<BucketReport> first = rollup.roll_bucket(db(), bucket, now);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().rows_read, 3);
    // Two dimension combinations: web and ios.
    EXPECT_EQ(first.value().rows_written, 2);
    EXPECT_EQ(stored_rollups(), 2);

    const AnalyticsQuery reader{scratch_names(), kCollections};
    const TimeRange range{bucket, bucket + std::chrono::hours{1}};
    const Result<std::vector<Bucket>> before = reader.counts_over_time(
        db(), code_of(testapp::Event::SignupCompleted), range, Granularity::Hour);
    ASSERT_TRUE(before.ok());
    ASSERT_EQ(before.value().size(), 1U);
    // The coalescer's REPEAT COUNTS, not one per row: 3 + 4 + 1.
    EXPECT_EQ(before.value()[0].count, 8);

    // The second run. A rollup that $inc'd would now report sixteen.
    const Result<BucketReport> second = rollup.roll_bucket(db(), bucket, now);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(stored_rollups(), 2);

    const Result<std::vector<Bucket>> after = reader.counts_over_time(
        db(), code_of(testapp::Event::SignupCompleted), range, Granularity::Hour);
    ASSERT_TRUE(after.ok());
    ASSERT_EQ(after.value().size(), 1U);
    EXPECT_EQ(after.value()[0].count, before.value()[0].count);
    EXPECT_EQ(after.value()[0].sessions, before.value()[0].sessions);
}

TEST_F(RollupDb, DistinctSessionsAreCountedRatherThanDerivedFromTheEventCount) {
    const db::TimeMs now = db::now_ms();
    const db::TimeMs bucket = bucket_start(now - std::chrono::hours{2}, Granularity::Hour);

    // Two visitors, five rows, nineteen events.
    write(testapp::Surface::Web, bucket + std::chrono::minutes{1}, 10, 1);
    write(testapp::Surface::Web, bucket + std::chrono::minutes{2}, 4, 1);
    write(testapp::Surface::Web, bucket + std::chrono::minutes{3}, 5, 2);

    ASSERT_TRUE(job().roll_bucket(db(), bucket, now).ok());

    const AnalyticsQuery reader{scratch_names(), kCollections};
    const Result<std::vector<Bucket>> series = reader.counts_over_time(
        db(), code_of(testapp::Event::SignupCompleted),
        TimeRange{bucket, bucket + std::chrono::hours{1}}, Granularity::Hour);
    ASSERT_TRUE(series.ok());
    ASSERT_EQ(series.value().size(), 1U);
    EXPECT_EQ(series.value()[0].count, 19);
    // Two, and it cannot be derived from nineteen afterwards — which is why the
    // rollup counts it at rollup time rather than leaving it to the reader.
    EXPECT_EQ(series.value()[0].sessions, 2);
}

TEST_F(RollupDb, AWindowLargerThanOnePageProducesTheSameTotals) {
    const db::TimeMs now = db::now_ms();
    const db::TimeMs bucket = bucket_start(now - std::chrono::hours{2}, Granularity::Hour);

    // More rows than one page of the walk, so the cursor is exercised rather
    // than bypassed. A rollup interrupted mid-window and resumed from its
    // boundary has to produce the same totals as one that ran straight through,
    // and the paging IS that resumption.
    constexpr int kRows = 40;
    for (int i = 0; i < kRows; ++i) {
        write(static_cast<testapp::Surface>(i % 3),
              bucket + std::chrono::seconds{i}, 1, static_cast<std::uint8_t>(i % 7));
    }

    const Result<BucketReport> rolled = job().roll_bucket(db(), bucket, now);
    ASSERT_TRUE(rolled.ok());
    EXPECT_EQ(rolled.value().rows_read, kRows);

    const AnalyticsQuery reader{scratch_names(), kCollections};
    const Result<std::vector<Bucket>> series = reader.counts_over_time(
        db(), code_of(testapp::Event::SignupCompleted),
        TimeRange{bucket, bucket + std::chrono::hours{1}}, Granularity::Hour);
    ASSERT_TRUE(series.ok());
    ASSERT_EQ(series.value().size(), 1U);
    EXPECT_EQ(series.value()[0].count, kRows);

    // Twenty-one and not seven, and that is the STATED approximation rather than
    // a defect: the rollup writes one document per dimension combination, and an
    // unnarrowed series sums them. Seven sessions appear under each of three
    // surfaces, so the sum counts each of them three times.
    //
    // Making it exact would mean storing the visitor SETS in every rollup
    // document, which is precisely the personal data §12 keeps out of this
    // collection on purpose. The narrowed query below is the exact number for
    // one combination, which is the question a dashboard actually asks.
    EXPECT_EQ(series.value()[0].sessions, 21);

    DimensionValues web = no_dimensions();
    web[0] = static_cast<std::uint8_t>(testapp::Surface::Web);
    const Result<std::vector<Bucket>> only_web = reader.counts_over_time_for(
        db(), code_of(testapp::Event::SignupCompleted), web,
        TimeRange{bucket, bucket + std::chrono::hours{1}}, Granularity::Hour);
    ASSERT_TRUE(only_web.ok());
    ASSERT_EQ(only_web.value().size(), 1U);
    EXPECT_EQ(only_web.value()[0].sessions, 7);
}

TEST_F(RollupDb, AnOpenBucketIsNotRolled) {
    const db::TimeMs now = db::now_ms();
    // A row inserted late into an already-rolled bucket is a row the rollup will
    // not see, so a bucket is computed only once `now` is past its end plus the
    // grace margin.
    write(testapp::Surface::Web, now, 1, 1);

    const RollupJob rollup = job();
    EXPECT_LE(rollup.first_open_bucket(now), bucket_start(now, Granularity::Hour));

    const Result<RollupReport> report =
        rollup.run(db(), bucket_start(now, Granularity::Hour), now);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().buckets_rolled, 0);
    EXPECT_EQ(stored_rollups(), 0);
}

TEST_F(RollupDb, ARunAdvancesItsMarkerOnlyPastBucketsItCompleted) {
    const db::TimeMs now = db::now_ms();
    const db::TimeMs from = bucket_start(now - std::chrono::hours{3}, Granularity::Hour);

    write(testapp::Surface::Web, from + std::chrono::minutes{5}, 2, 1);
    write(testapp::Surface::Web, from + std::chrono::hours{1} + std::chrono::minutes{5}, 3, 1);

    const RollupJob rollup = job();
    const Result<RollupReport> report = rollup.run(db(), from, now);
    ASSERT_TRUE(report.ok());
    // Three closed hours between `from` and now, so three buckets were
    // considered and the marker sits at the end of the last one.
    EXPECT_GE(report.value().buckets_rolled, 2);
    EXPECT_EQ(report.value().rows_read, 2);
    EXPECT_LE(report.value().through, rollup.first_open_bucket(now));

    // The marker is what a caller persists between runs, so a second run from it
    // must find nothing left to do and must not rewrite what the first wrote.
    const std::int64_t written = stored_rollups();
    const Result<RollupReport> again = rollup.run(db(), report.value().through, now);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().rows_read, 0);
    EXPECT_EQ(stored_rollups(), written);
}

TEST_F(RollupDb, ASeriesNarrowedToOneDimensionCombinationReadsOnlyThatOne) {
    const db::TimeMs now = db::now_ms();
    const db::TimeMs bucket = bucket_start(now - std::chrono::hours{2}, Granularity::Hour);

    write(testapp::Surface::Web, bucket + std::chrono::minutes{1}, 7, 1);
    write(testapp::Surface::Ios, bucket + std::chrono::minutes{2}, 2, 2);
    ASSERT_TRUE(job().roll_bucket(db(), bucket, now).ok());

    const AnalyticsQuery reader{scratch_names(), kCollections};
    const TimeRange range{bucket, bucket + std::chrono::hours{1}};

    DimensionValues web = no_dimensions();
    web[0] = static_cast<std::uint8_t>(testapp::Surface::Web);
    const Result<std::vector<Bucket>> only_web = reader.counts_over_time_for(
        db(), code_of(testapp::Event::SignupCompleted), web, range, Granularity::Hour);
    ASSERT_TRUE(only_web.ok());
    ASSERT_EQ(only_web.value().size(), 1U);
    EXPECT_EQ(only_web.value()[0].count, 7);

    // And the unnarrowed series is their sum, folded across the documents the
    // rollup wrote one per dimension combination.
    const Result<std::vector<Bucket>> everything = reader.counts_over_time(
        db(), code_of(testapp::Event::SignupCompleted), range, Granularity::Hour);
    ASSERT_TRUE(everything.ok());
    ASSERT_EQ(everything.value().size(), 1U);
    EXPECT_EQ(everything.value()[0].count, 9);
}

}  // namespace
}  // namespace anvil::analytics
