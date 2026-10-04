// Phase 8 — the metrics seam, asserted from outside anvil.
//
// Half of this seam is a BUILD rather than a test: metric_table_is_well_formed
// is static_asserted over tests/testapp/metrics.h, so a table whose label space
// exceeds the ceiling never links. What is left for a test is the set of
// malformed tables the check must REFUSE — every one of them a mistake that
// would otherwise ship as a scrape nothing can read, or as resident memory
// nobody budgeted.
//
// They are static_asserts rather than EXPECT_FALSE because that is where an
// application meets them, and they are written from OUTSIDE anvil for the reason
// CLAUDE.md §1 gives: a seam that cannot be satisfied from outside the library
// fails in tests/testapp, which is the only place it can fail cheaply.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/analytics/metric_spec.h"
#include "anvil/analytics/openmetrics.h"

#include "metrics.h"

namespace anvil::analytics {
namespace {

inline constexpr std::array<std::string_view, 2> kTwoValues{"a", "b"};
inline constexpr std::array<LabelSpec, 1> kOneLabel{{{"which", kTwoValues}}};
inline constexpr std::array<std::int64_t, 2> kRising{10, 20};
inline constexpr std::array<std::int64_t, 2> kFlat{10, 10};

inline constexpr std::array<MetricSpec, 1> kEmptyName{
    {{"", "help", {}, {}, MetricKind::Counter, MetricUnit::None}}};
static_assert(!metric_table_is_well_formed(kEmptyName));

inline constexpr std::array<MetricSpec, 1> kEmptyHelp{
    {{"widgets", "", {}, {}, MetricKind::Counter, MetricUnit::None}}};
static_assert(!metric_table_is_well_formed(kEmptyHelp));

inline constexpr std::array<MetricSpec, 1> kUngrammatical{
    {{"widgets-made", "help", {}, {}, MetricKind::Counter, MetricUnit::None}}};
static_assert(!metric_table_is_well_formed(kUngrammatical));

// The writer appends `_total`, so a name carrying it emits `_total_total` — a
// different series from the one every dashboard names.
inline constexpr std::array<MetricSpec, 1> kCounterWithTotal{
    {{"widgets_total", "help", {}, {}, MetricKind::Counter, MetricUnit::None}}};
static_assert(!metric_table_is_well_formed(kCounterWithTotal));

inline constexpr std::array<MetricSpec, 1> kUnitMismatch{
    {{"widget_latency", "help", {}, kRising, MetricKind::Histogram,
      MetricUnit::Microseconds}}};
static_assert(!metric_table_is_well_formed(kUnitMismatch));

inline constexpr std::array<MetricSpec, 1> kFlatBuckets{
    {{"widget_latency_microseconds", "help", {}, kFlat, MetricKind::Histogram,
      MetricUnit::Microseconds}}};
static_assert(!metric_table_is_well_formed(kFlatBuckets));

inline constexpr std::array<MetricSpec, 1> kHistogramWithoutBuckets{
    {{"widget_latency_microseconds", "help", {}, {}, MetricKind::Histogram,
      MetricUnit::Microseconds}}};
static_assert(!metric_table_is_well_formed(kHistogramWithoutBuckets));

// The prefix is enforced in BOTH directions, which is what keeps the two tables
// from ever colliding and lets a reader of a scrape tell which side a series
// came from.
inline constexpr std::array<MetricSpec, 1> kStolenPrefix{
    {{"anvil_widgets", "help", {}, {}, MetricKind::Counter, MetricUnit::None}}};
static_assert(!metric_table_is_well_formed(kStolenPrefix));
static_assert(internal_metric_table_is_well_formed(kStolenPrefix));

inline constexpr std::array<MetricSpec, 2> kDuplicateName{
    {{"widgets", "help", {}, {}, MetricKind::Counter, MetricUnit::None},
     {"widgets", "other", kOneLabel, {}, MetricKind::Gauge, MetricUnit::None}}};
static_assert(!metric_table_is_well_formed(kDuplicateName));

// Sixty-five cells against a ceiling of sixty-four, as a BUILD failure rather
// than as an OOM at 3am. This is the assertion the whole cardinality design
// exists to make cheap.
inline constexpr std::array<std::string_view, 65> kSixtyFive{
    "v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",  "v7",  "v8",  "v9",  "v10", "v11", "v12",
    "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25",
    "v26", "v27", "v28", "v29", "v30", "v31", "v32", "v33", "v34", "v35", "v36", "v37", "v38",
    "v39", "v40", "v41", "v42", "v43", "v44", "v45", "v46", "v47", "v48", "v49", "v50", "v51",
    "v52", "v53", "v54", "v55", "v56", "v57", "v58", "v59", "v60", "v61", "v62", "v63", "v64"};
inline constexpr std::array<LabelSpec, 1> kWideLabel{{{"which", kSixtyFive}}};
inline constexpr std::array<MetricSpec, 1> kTooWide{
    {{"widgets", "help", kWideLabel, {}, MetricKind::Counter, MetricUnit::None}}};
static_assert(!metric_table_is_well_formed(kTooWide));

// A histogram series is buckets + 3 cells, so a ceiling that counted SERIES
// would not bound anything.
static_assert(cell_count(testapp::kMetrics[2]) == 8);
static_assert(cell_count(testapp::kMetrics[0]) == 6);

TEST(MetricTable, TheReferenceApplicationsTableIsAccepted) {
    // The positive case, so a check that refuses everything cannot pass the
    // negatives above by accident.
    EXPECT_TRUE(metric_table_is_well_formed(testapp::kMetrics));
    EXPECT_FALSE(internal_metric_table_is_well_formed(testapp::kMetrics));
}

TEST(MetricTable, CellCountsAreTheCeilingsArithmetic) {
    // A histogram series is buckets + 3 cells — one per boundary, an overflow, a
    // _sum and a _count — so a ceiling that counted SERIES would not bound
    // anything, which is the whole job of having one.
    EXPECT_EQ(cell_count(testapp::kMetrics[0]), 6U);
    EXPECT_EQ(cell_count(testapp::kMetrics[1]), 3U);
    EXPECT_EQ(cell_count(testapp::kMetrics[2]), 8U);
    EXPECT_EQ(table_cell_count(testapp::kMetrics), 17U);
    EXPECT_LE(table_cell_count(testapp::kMetrics), kMaxCells);
}

// --- the registry -----------------------------------------------------------

class MetricsTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_unique<Registry>(kInternalMetrics, testapp::kMetrics);
    }

    [[nodiscard]] static MetricId checkout() {
        return application_metric(static_cast<std::size_t>(testapp::Metric::CheckoutCompleted));
    }
    [[nodiscard]] static MetricId carts() {
        return application_metric(static_cast<std::size_t>(testapp::Metric::CartsOpen));
    }
    [[nodiscard]] static MetricId latency() {
        return application_metric(static_cast<std::size_t>(testapp::Metric::CheckoutLatency));
    }

    std::unique_ptr<Registry> registry_;
};

TEST_F(MetricsTest, TheArenaIsOneAllocationSizedFromTheTables) {
    // The bound stated in counters.h, read back rather than believed. A
    // cardinality explosion is supposed to be arithmetic somebody can check.
    EXPECT_LE(registry_->arena_bytes(), kMaxCells * Registry::kMaxShards * 64U);
    EXPECT_LE(registry_->shard_count(), Registry::kMaxShards);
    EXPECT_EQ(registry_->metric_count(), kInternalMetrics.size() + testapp::kMetrics.size());
}

TEST_F(MetricsTest, ACounterAccumulatesPerSeries) {
    registry_->increment(checkout(), testapp::Plan::Pro, testapp::Outcome::Succeeded);
    registry_->increment(checkout(), testapp::Plan::Pro, testapp::Outcome::Succeeded);
    registry_->add(checkout(), testapp::Plan::Free, testapp::Outcome::Failed, 5);

    // series = plan * |outcome| + outcome.
    EXPECT_EQ(registry_->value_at(checkout(), (1 * 2) + 0, 0), 2U);
    EXPECT_EQ(registry_->value_at(checkout(), (0 * 2) + 1, 0), 5U);
    EXPECT_EQ(registry_->value_at(checkout(), (0 * 2) + 0, 0), 0U);
}

TEST_F(MetricsTest, ALabelIndexPastTheValueSpaceWritesNothing) {
    // The one thing between the public API and a write past the end of the
    // arena. Refusing is the only safe direction: a metrics call must never be
    // the reason a process dies.
    registry_->increment(checkout(), LabelIndex{99}, testapp::Outcome::Failed);
    registry_->increment(checkout(), testapp::Plan::Free, LabelIndex{99});
    for (std::size_t series = 0; series < 6; ++series) {
        EXPECT_EQ(registry_->value_at(checkout(), series, 0), 0U) << "series " << series;
    }
}

TEST_F(MetricsTest, AGaugeIsSetNotAdded) {
    registry_->set(carts(), testapp::Plan::Team, 17);
    EXPECT_EQ(registry_->value_at(carts(), 2, 0), 17U);
    registry_->set(carts(), testapp::Plan::Team, 4);
    EXPECT_EQ(registry_->value_at(carts(), 2, 0), 4U);

    // Adding to a gauge is the confusion that makes every capacity number in a
    // dashboard wrong in the direction an operator acts on, so it is refused
    // rather than silently accepted.
    registry_->add(carts(), testapp::Plan::Team, 100);
    EXPECT_EQ(registry_->value_at(carts(), 2, 0), 4U);

    // And a counter is not a gauge.
    registry_->set(checkout(), testapp::Plan::Free, 9);
    EXPECT_EQ(registry_->value_at(checkout(), 0, 0), 0U);
}

TEST_F(MetricsTest, AnObservationTouchesOneBucketPlusSumAndCount) {
    registry_->observe(latency(), 500);        // bucket 0 (<= 1000)
    registry_->observe(latency(), 60000);      // bucket 3 (<= 250000)
    registry_->observe(latency(), 9'000'000);  // overflow

    EXPECT_EQ(registry_->value_at(latency(), 0, 0), 1U);
    EXPECT_EQ(registry_->value_at(latency(), 0, 1), 0U);
    EXPECT_EQ(registry_->value_at(latency(), 0, 2), 0U);
    EXPECT_EQ(registry_->value_at(latency(), 0, 3), 1U);
    EXPECT_EQ(registry_->value_at(latency(), 0, 4), 0U);
    EXPECT_EQ(registry_->value_at(latency(), 0, 5), 1U);   // overflow
    EXPECT_EQ(registry_->value_at(latency(), 0, 6), 9'060'500U);
    EXPECT_EQ(registry_->value_at(latency(), 0, 7), 3U);
}

// --- the snapshot and the writer --------------------------------------------

class ScrapeTest : public MetricsTest {
protected:
    void SetUp() override {
        MetricsTest::SetUp();
        clear_gauge_samplers();
    }
    void TearDown() override { clear_gauge_samplers(); }

    [[nodiscard]] std::string scrape() {
        Snapshot snapshot{*registry_};
        snapshot.collect();
        std::string out;
        out.reserve(estimate_openmetrics_bytes(snapshot));
        append_openmetrics(out, snapshot);
        return out;
    }
};

TEST_F(ScrapeTest, TwoScrapesOfAnUnchangedRegistryAreByteIdentical) {
    registry_->increment(checkout(), testapp::Plan::Pro, testapp::Outcome::Succeeded);
    registry_->observe(latency(), 1234);
    registry_->set(carts(), testapp::Plan::Free, 3);

    const std::string first = scrape();
    const std::string second = scrape();
    // Not tidiness: it is what lets an operator diff two scrapes and see only
    // what moved.
    EXPECT_EQ(first, second);
}

TEST_F(ScrapeTest, TheDocumentEndsWithEofAndNamesEveryFamily) {
    const std::string body = scrape();
    ASSERT_GE(body.size(), 6U);
    EXPECT_EQ(body.substr(body.size() - 6), "# EOF\n");

    EXPECT_NE(body.find("# TYPE anvil_authz_cache_hits counter\n"), std::string::npos);
    EXPECT_NE(body.find("# TYPE anvil_pool_queue_depth gauge\n"), std::string::npos);
    EXPECT_NE(body.find("# TYPE anvil_mongo_pool_wait_microseconds histogram\n"),
              std::string::npos);
    EXPECT_NE(body.find("# UNIT anvil_mongo_pool_wait_microseconds microseconds\n"),
              std::string::npos);

    // The family name carries no `_total`; the SAMPLE does.
    EXPECT_EQ(body.find("# TYPE checkout_completed_total"), std::string::npos);
    EXPECT_NE(body.find("checkout_completed_total{plan=\"free\",outcome=\"succeeded\"} 0\n"),
              std::string::npos);
    // A gauge sample carries no suffix at all.
    EXPECT_NE(body.find("carts_open{plan=\"free\"} 0\n"), std::string::npos);
}

TEST_F(ScrapeTest, HistogramBucketsAreCumulativeAndEndAtInf) {
    registry_->observe(latency(), 500);
    registry_->observe(latency(), 60000);
    const std::string body = scrape();

    EXPECT_NE(body.find("checkout_latency_microseconds_bucket{le=\"1000\"} 1\n"),
              std::string::npos);
    EXPECT_NE(body.find("checkout_latency_microseconds_bucket{le=\"10000\"} 1\n"),
              std::string::npos);
    EXPECT_NE(body.find("checkout_latency_microseconds_bucket{le=\"250000\"} 2\n"),
              std::string::npos);
    EXPECT_NE(body.find("checkout_latency_microseconds_bucket{le=\"+Inf\"} 2\n"),
              std::string::npos);
    EXPECT_NE(body.find("checkout_latency_microseconds_sum 60500\n"), std::string::npos);
    EXPECT_NE(body.find("checkout_latency_microseconds_count 2\n"), std::string::npos);
}

TEST(OpenMetricsEscaping, BackslashNewlineAndQuoteAreEscaped) {
    // An unescaped quote splits a sample into two the collector reads as
    // malformed, and an unescaped newline ends the line early — both corrupt
    // every series AFTER the one carrying it, not just this one.
    std::string out;
    append_openmetrics_escaped(out, "a\\b\nc\"d");
    EXPECT_EQ(out, "a\\\\b\\nc\\\"d");
}

TEST(ScrapePolicyTest, AnEmptyCidrSetAllowsNobody) {
    ScrapePolicy policy;
    EXPECT_EQ(policy.cidr_count(), 0U);
    // A deployment that has not set METRICS_SCRAPE_CIDRS has not decided who may
    // scrape, and the failing direction of that is a 404 rather than a live map
    // of where the system is weak served to whoever asks.
    EXPECT_FALSE(policy.allows(http::pack_address("127.0.0.1")));

    ASSERT_TRUE(policy.parse_cidrs("10.0.0.0/8, ::1"));
    EXPECT_TRUE(policy.allows(http::pack_address("10.4.1.9")));
    EXPECT_FALSE(policy.allows(http::pack_address("11.4.1.9")));
    EXPECT_TRUE(policy.allows(http::pack_address("::1")));
}

TEST(GaugeSamplers, RunBeforeTheCellsAreRead) {
    clear_gauge_samplers();
    Registry registry{kInternalMetrics, testapp::kMetrics};
    const MetricId carts =
        application_metric(static_cast<std::size_t>(testapp::Metric::CartsOpen));

    install_gauge_sampler([carts](Registry& target) {
        target.set(carts, testapp::Plan::Pro, 42);
    });

    Snapshot snapshot{registry};
    snapshot.collect();
    const std::size_t index =
        kInternalMetrics.size() + static_cast<std::size_t>(testapp::Metric::CartsOpen);
    EXPECT_EQ(snapshot.value_of(index, 1, 0), 42U);
    clear_gauge_samplers();
}

// --- the one table anvil populates ------------------------------------------

TEST(InternalMetricTable, EveryNameDescribesAMechanismInThisRepository) {
    // The test docs/17 §3 states for the exception to CLAUDE.md §1: a name here
    // would not have to change if the application changed.
    // `checkout_completed_total` would, and that is the line.
    EXPECT_TRUE(internal_metric_table_is_well_formed(kInternalMetrics));
    EXPECT_FALSE(metric_table_is_well_formed(kInternalMetrics));
    EXPECT_TRUE(metric_tables_fit_together(kInternalMetrics, testapp::kMetrics));

    for (const MetricSpec& spec : kInternalMetrics) {
        EXPECT_EQ(spec.name.substr(0, 6), "anvil_") << spec.name;
        EXPECT_FALSE(spec.help.empty()) << spec.name;
    }
}

TEST(InternalMetricTable, TheTtlLabelIsDerivedFromTheApplicationsCollections) {
    // The one label here whose width an application chooses. Deriving it keeps
    // the value space constexpr — so the series count stays a compile-time
    // number — while letting anvil count rows in collections it does not name.
    static_assert(kTtlCollectionCount > 0,
                  "the reference application declares lifetime-bounded collections");

    std::size_t declared = 0;
    for (const db::CollectionSpec& spec : config::kCollections) {
        if (spec.expiry_field.empty()) { continue; }
        ++declared;
        bool found = false;
        for (const std::string_view name : kTtlCollectionValues) {
            found = found || name == spec.name;
        }
        EXPECT_TRUE(found) << spec.name << " is lifetime-bounded but is not a series";
    }
    EXPECT_EQ(declared, kTtlCollectionCount);

    // A collection with no lifetime must NOT be a series: anvil_ttl_collection_
    // rows reporting a number for a collection whose rows never expire is a
    // number an operator would read as a leak.
    for (const std::string_view name : kTtlCollectionValues) {
        EXPECT_FALSE(db::lifetime_expiry_field(name).empty()) << name;
    }
}

}  // namespace
}  // namespace anvil::analytics
