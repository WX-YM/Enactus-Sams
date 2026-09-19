// Phase 8 — the no-allocation claim, counted rather than believed.
//
// docs/17-analytics.md §2 is a statement about COST: an increment must cost no
// allocation, no lock and no contended cache line, because a counter that is
// cheap at ten requests per second and expensive at ten thousand switches itself
// off exactly when it is being read. The allocation half of that is the half a
// refactor can break silently, so it is counted here — in the binary that
// replaces global operator new — rather than asserted in prose.
//

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "alloc_counter.h"

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/analytics/openmetrics.h"
#include "anvil/analytics/snapshot.h"

#include "metrics.h"

namespace anvil::analytics {
namespace {

using anvil::testing::AllocationCounter;

[[nodiscard]] MetricId checkout() {
    return application_metric(static_cast<std::size_t>(testapp::Metric::CheckoutCompleted));
}
[[nodiscard]] MetricId carts() {
    return application_metric(static_cast<std::size_t>(testapp::Metric::CartsOpen));
}
[[nodiscard]] MetricId latency() {
    return application_metric(static_cast<std::size_t>(testapp::Metric::CheckoutLatency));
}

TEST(MetricsAllocation, EveryIncrementPathAllocatesNothing) {
    Registry registry{kInternalMetrics, testapp::kMetrics};
    // First touch assigns this thread its shard; doing it before the count
    // starts measures the increment rather than the one-time assignment.
    registry.increment(checkout(), testapp::Plan::Free, testapp::Outcome::Succeeded);

    const AllocationCounter counted;
    for (int i = 0; i < 10000; ++i) {
        registry.increment(checkout(), testapp::Plan::Pro, testapp::Outcome::Succeeded);
        registry.add(checkout(), testapp::Plan::Team, testapp::Outcome::Failed, 3);
        registry.set(carts(), testapp::Plan::Free, static_cast<std::uint64_t>(i));
        registry.observe(latency(), i);
        count(Internal::AuthzCacheHits, AuthzTier::Local);
        sample(Internal::PoolQueueDepth, PoolLabel::Db, 2);
        observe(Internal::MongoPoolWaitMicroseconds, 90);
    }
    EXPECT_EQ(counted.count(), 0U);
}

TEST(MetricsAllocation, ASnapshotAllocatesOnlyWhenItIsBuilt) {
    Registry registry{kInternalMetrics, testapp::kMetrics};
    Snapshot snapshot{registry};
    snapshot.collect();

    // Summing the shards on READ is what lets a pull scrape need no periodic
    // task at all, and it is only worth that if the read itself is free.
    const AllocationCounter counted;
    for (int i = 0; i < 100; ++i) { snapshot.collect(); }
    EXPECT_EQ(counted.count(), 0U);
}

TEST(MetricsAllocation, AScrapeIntoAReservedBufferAllocatesOnce) {
    Registry registry{kInternalMetrics, testapp::kMetrics};
    Snapshot snapshot{registry};
    snapshot.collect();

    std::string body;
    body.reserve(estimate_openmetrics_bytes(snapshot));
    const std::size_t reserved = body.capacity();

    const AllocationCounter counted;
    append_openmetrics(body, snapshot);

    // One: the scratch buffer holding `<name>_bucket`, reserved once for the
    // longest name in the table. Anything more would mean the caller's reserve()
    // was short, which costs a reallocation and a copy of the whole body.
    EXPECT_LE(counted.count(), 1U);
    EXPECT_EQ(body.capacity(), reserved) << "estimate_openmetrics_bytes under-reserved";
    EXPECT_FALSE(body.empty());
}

}  // namespace
}  // namespace anvil::analytics
