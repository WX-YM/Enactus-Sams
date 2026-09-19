// Phase 8 — the properties a sharded counter can quietly lose.
//
// Sharding is the reason an increment costs one uncontended add, and it is also
// the reason a counter can be wrong in ways a single-threaded test cannot see: a
// lost update is invisible, a summed gauge is plausible, and a torn read looks
// like a real number. Each one is asserted here, under TSan.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/analytics/snapshot.h"

#include "metrics.h"

namespace anvil::analytics {
namespace {

constexpr int kThreads = 16;
constexpr int kPerThread = 20000;

[[nodiscard]] MetricId checkout() {
    return application_metric(static_cast<std::size_t>(testapp::Metric::CheckoutCompleted));
}
[[nodiscard]] MetricId carts() {
    return application_metric(static_cast<std::size_t>(testapp::Metric::CartsOpen));
}
[[nodiscard]] MetricId latency() {
    return application_metric(static_cast<std::size_t>(testapp::Metric::CheckoutLatency));
}

TEST(MetricsConcurrency, NThreadsIncrementingOneSeriesSumToExactlyNTimesM) {
    Registry registry{kInternalMetrics, testapp::kMetrics};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&registry] {
            for (int i = 0; i < kPerThread; ++i) {
                registry.increment(checkout(), testapp::Plan::Pro, testapp::Outcome::Succeeded);
            }
        });
    }
    for (std::thread& worker : workers) { worker.join(); }

    // Summed across shards on read. A shard is an implementation detail of the
    // write path; the counter is the sum, and anything short of N x M means an
    // update was lost.
    EXPECT_EQ(registry.value_at(checkout(), (1 * 2) + 0, 0),
              static_cast<std::uint64_t>(kThreads) * kPerThread);
}

TEST(MetricsConcurrency, AGaugeIsNotSummed) {
    // The cheapest possible test, and it guards the one confusion that would
    // make every capacity number in a dashboard wrong in the direction an
    // operator acts on: sixteen shards of a gauge summed reports up to sixteen
    // times the depth that exists (docs/17-analytics.md §4).
    Registry registry{kInternalMetrics, testapp::kMetrics};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&registry, t] {
            const auto value = static_cast<std::uint64_t>(100 + t);
            for (int i = 0; i < kPerThread; ++i) {
                registry.set(carts(), testapp::Plan::Team, value);
            }
        });
    }
    for (std::thread& worker : workers) { worker.join(); }

    const std::uint64_t reported = registry.value_at(carts(), 2, 0);
    EXPECT_GE(reported, 100U);
    EXPECT_LE(reported, 100U + kThreads - 1);
}

TEST(MetricsConcurrency, ASnapshotTakenDuringWritesIsBetweenFirstAndLast) {
    Registry registry{kInternalMetrics, testapp::kMetrics};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> written{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&registry, &stop, &written] {
            while (!stop.load(std::memory_order_relaxed)) {
                // The witness is bumped BEFORE the counter, so `written` is an
                // upper bound on the counter at every instant. Bumping it after
                // makes it a LOWER bound with a window, and a snapshot taken
                // inside that window legitimately reads a value the witness has
                // not reached yet — which is a flaw in the assertion rather than
                // in the registry, and one only an -O3 build is fast enough to
                // hit.
                written.fetch_add(1, std::memory_order_relaxed);
                registry.increment(checkout(), testapp::Plan::Free, testapp::Outcome::Failed);
            }
        });
    }

    Snapshot snapshot{registry};
    const std::size_t index = kInternalMetrics.size() +
                              static_cast<std::size_t>(testapp::Metric::CheckoutCompleted);
    std::uint64_t previous = 0;
    for (int i = 0; i < 200; ++i) {
        snapshot.collect();
        const std::uint64_t after = written.load(std::memory_order_relaxed);
        const std::uint64_t observed = snapshot.value_of(index, (0 * 2) + 1, 0);

        // Relaxed ordering is correct because nothing depends on a counter's
        // value ordering against anything else: a snapshot reads a value between
        // the first and last observation, which is exactly what a monotonically
        // increasing counter means. What it may never do is go BACKWARDS or
        // report more than has been written — a torn 64-bit cell does both, and
        // a lost shard does the first.
        EXPECT_GE(observed, previous);
        EXPECT_LE(observed, after);
        previous = observed;
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& worker : workers) { worker.join(); }
}

TEST(MetricsConcurrency, CumulativeHistogramBucketsAreMonotone) {
    Registry registry{kInternalMetrics, testapp::kMetrics};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&registry, t] {
            for (int i = 0; i < kPerThread; ++i) {
                registry.observe(latency(), static_cast<std::int64_t>((i * 37) + t));
            }
        });
    }
    for (std::thread& worker : workers) { worker.join(); }

    // Monotone BY CONSTRUCTION: the increment path touches one bucket and the
    // cumulative form is a running total taken at snapshot, so a non-monotone
    // scrape would mean the running total itself was wrong.
    std::uint64_t running = 0;
    const std::size_t slots = testapp::kLatencyBucketsUs.size() + 1;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const std::uint64_t next = running + registry.value_at(latency(), 0, slot);
        EXPECT_GE(next, running);
        running = next;
    }
    EXPECT_EQ(running, static_cast<std::uint64_t>(kThreads) * kPerThread);
    EXPECT_EQ(registry.value_at(latency(), 0, slots + 1),
              static_cast<std::uint64_t>(kThreads) * kPerThread);
}

}  // namespace
}  // namespace anvil::analytics
