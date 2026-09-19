// Concurrency suite. Runs under TSan via `ctest --preset tsan`.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "anvil/core/thread_pools.h"
#include "anvil/core/types.h"
#include "anvil/http/trace_context.h"

#include "app_fixture.h"

namespace anvil {

TEST(BoundedThreadPool, RunsEveryAcceptedTask) {
    BoundedThreadPool pool{"test", 4, 1024};
    std::atomic<int> done{0};

    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(pool.try_post([&done] { done.fetch_add(1, std::memory_order_relaxed); }));
    }
    pool.shutdown();
    EXPECT_EQ(done.load(), 500);
}

TEST(BoundedThreadPool, ShedsInsteadOfGrowing) {
    // A full queue must refuse work so the caller can return 503. Queueing
    // unboundedly converts a login flood into an OOM kill.
    BoundedThreadPool pool{"test", 1, 4};
    std::atomic<bool> release{false};

    ASSERT_TRUE(pool.try_post([&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }));

    int accepted = 0;
    int refused = 0;
    for (int i = 0; i < 64; ++i) {
        if (pool.try_post([] {})) { ++accepted; } else { ++refused; }
    }

    EXPECT_GT(refused, 0) << "a bounded queue must refuse work when full";
    EXPECT_LE(accepted, 4);

    release.store(true, std::memory_order_release);
    pool.shutdown();
}

TEST(BoundedThreadPool, GuardedTaskContainsExceptions) {
    // An exception escaping a pool task calls std::terminate and takes the
    // process down. This test passing at all is the assertion.
    BoundedThreadPool pool{"test", 2, 16};
    std::atomic<int> after{0};

    ASSERT_TRUE(pool.try_post(guarded("test", [] { throw std::runtime_error("boom"); })));
    ASSERT_TRUE(pool.try_post(guarded("test", [] { throw 42; })));
    ASSERT_TRUE(pool.try_post(guarded("test", [&after] {
        after.fetch_add(1, std::memory_order_relaxed);
    })));

    pool.shutdown();
    EXPECT_EQ(after.load(), 1) << "the pool must keep serving after a task throws";
}

// --- the trace context, across the one boundary an application cannot reach ---
//
// These are in the CONCURRENCY binary rather than beside the parser because what
// they assert is a property of threads: the storage is thread-local, the capture
// happens on one thread and the install on another, and the case that a worker
// does not inherit the last task's context is only expressible with a worker
// that has run a previous task. TSan runs this suite, which is the point.

namespace {

[[nodiscard]] http::TraceContext sample_trace(std::uint8_t seed) noexcept {
    http::TraceContext ctx{};
    ctx.trace_id.fill(seed);
    ctx.span_id.fill(seed);
    ctx.flags = http::kTraceFlagSampled;
    return ctx;
}

}  // namespace

TEST(TracePropagation, APooledTaskObservesThePostingThreadsContext) {
    // The whole reason the capture is in `guarded()` and not at a call site: the
    // application cannot wrap a task posted by anvil, so a context threaded by
    // hand would cross every boundary but the ones inside this library.
    BoundedThreadPool pool{"test", 2, 16};
    const http::TraceContext posted = sample_trace(0x7a);

    std::array<std::uint8_t, 16> seen{};
    std::atomic<bool> ran{false};
    {
        const http::TraceScope scope{posted};
        ASSERT_TRUE(pool.try_post(guarded("test", [&seen, &ran] {
            seen = http::current_trace().trace_id;
            ran.store(true, std::memory_order_release);
        })));
    }

    pool.shutdown();
    ASSERT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(seen, posted.trace_id);

    // And the posting thread is where it was. A scope that leaked would put a
    // finished request's id on every later log line this thread writes.
    EXPECT_FALSE(http::current_trace().present());
}

// The two cases below read the worker thread through an UNGUARDED post, and
// that is the only spelling of them that can fail.
//
// The first draft posted the observer through `guarded()` as well, and it passed
// against a build with the restore deliberately deleted — because `guarded()`
// INSTALLS unconditionally, so the second task overwrote the stale value with
// its own absent one before reading it. It was asserting that an assignment
// happens, which is not the property. What the restore is for is every OTHER
// thing that runs on a pool thread: a task posted without the wrapper, and the
// pool's own bookkeeping between tasks.

TEST(TracePropagation, AnUnguardedTaskDoesNotInheritTheLastOnesContext) {
    // One thread, so the two tasks are guaranteed to be the same worker.
    BoundedThreadPool pool{"test", 1, 16};

    {
        const http::TraceScope scope{sample_trace(0x31)};
        ASSERT_TRUE(pool.try_post(guarded("test", [] {})));
    }

    std::atomic<bool> inherited{true};
    ASSERT_TRUE(pool.try_post([&inherited] {
        inherited.store(http::current_trace().present(), std::memory_order_release);
    }));

    pool.shutdown();
    EXPECT_FALSE(inherited.load(std::memory_order_acquire))
        << "a worker kept the previous task's trace, so the next log line on this "
           "thread names a request that has already finished";
}

TEST(TracePropagation, AThrowingTaskStillRestores) {
    // The catch path is where a restore gets forgotten, and a restore that only
    // runs on the happy path is worse than none: the one task that fails is
    // exactly the one whose id the next thing on that thread would be logged
    // under.
    BoundedThreadPool pool{"test", 1, 16};

    {
        const http::TraceScope scope{sample_trace(0x5c)};
        ASSERT_TRUE(pool.try_post(
            guarded("test", [] { throw std::runtime_error("boom"); })));
    }

    std::atomic<bool> inherited{true};
    ASSERT_TRUE(pool.try_post([&inherited] {
        inherited.store(http::current_trace().present(), std::memory_order_release);
    }));

    pool.shutdown();
    EXPECT_FALSE(inherited.load(std::memory_order_acquire));
}

TEST(TracePropagation, TwoThreadsDoNotShareOne) {
    // Thread-local and not a map keyed by thread id, so this is the assertion
    // that the storage class is what the header says it is. A shared slot would
    // hand one request's id to a concurrent one, which is the same defect as the
    // missing restore and is invisible under a single-threaded test.
    constexpr std::size_t kThreads = 8;
    std::atomic<int> mismatched{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([i, &mismatched] {
            const http::TraceContext mine = sample_trace(static_cast<std::uint8_t>(i + 1U));
            const http::TraceScope scope{mine};
            for (int spin = 0; spin < 200; ++spin) {
                if (http::current_trace().trace_id != mine.trace_id) {
                    mismatched.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }
        });
    }
    for (std::thread& thread : threads) { thread.join(); }

    EXPECT_EQ(mismatched.load(), 0);
}

TEST(BoundedThreadPool, DrainsOnShutdown) {
    // A task accepted and then dropped at shutdown is a lost job.
    BoundedThreadPool pool{"test", 2, 256};
    std::atomic<int> done{0};

    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(pool.try_post([&done] {
            std::this_thread::sleep_for(std::chrono::microseconds{50});
            done.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    pool.shutdown();
    EXPECT_EQ(done.load(), 200);
}

TEST(BoundedThreadPool, RefusesAfterShutdown) {
    BoundedThreadPool pool{"test", 2, 16};
    pool.shutdown();
    EXPECT_FALSE(pool.try_post([] {}));
    pool.shutdown();  // idempotent
}

TEST(BoundedThreadPool, SurvivesConcurrentProducers) {
    BoundedThreadPool pool{"test", 4, 4096};
    std::atomic<int> done{0};
    std::atomic<int> posted{0};

    std::vector<std::thread> producers;
    producers.reserve(8);
    for (int t = 0; t < 8; ++t) {
        producers.emplace_back([&pool, &done, &posted] {
            for (int i = 0; i < 200; ++i) {
                if (pool.try_post([&done] { done.fetch_add(1, std::memory_order_relaxed); })) {
                    posted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& producer : producers) { producer.join(); }

    pool.shutdown();
    EXPECT_EQ(done.load(), posted.load());
}

// The one property audit_pool exists for: audit writes must make no progress
// harder for a saturated request path.
//
// Batching stopped the sink from saturating db_pool and did nothing about
// --- deferred: audit-pool isolation ----------------------------------------
//
// The original suite ended with a case asserting that a SATURATED db_pool costs
// the audit sink nothing — that a batch posted to the audit pool is still
// accepted while the request pool refuses every task. That property is the whole
// reason audit_pool is a fourth pool rather than a share of the third
// (docs/00-architecture.md §3), and it is worth asserting.
//
// It needs an AuditService to post through, which arrives in Phase 3. The case
// moves there rather than being weakened into one that posts empty lambdas — a
// test that no longer names the thing it is about stops being read as being about
// it.

// --- the fifth pool is separate, and this is why ---------------------------

TEST(PoolIsolation, ASaturatedAnalyticsPoolDoesNotDelayAnAuditFlush) {
    if (!testfixture::pools_ready()) { GTEST_SKIP() << "thread pools unavailable"; }

    // The property analytics_pool exists for. audit_pool was made a pool of its
    // own so a saturated request path could not starve the record of what
    // saturated it; putting the event sink on it would starve that record again,
    // and the event sink is the one that floods — an analytics event fires on
    // requests that are not interesting enough to audit
    // (docs/17-analytics.md §10).
    //
    // Every task below captures a shared_ptr BY VALUE, never the test's stack
    // (ENGINEERING_RULES.md §3.3). A blocker outliving the test body and reading a
    // reference to a dead frame is the exact use-after-free the rule is about,
    // and ASan reports it as one.
    struct Shared final {
        std::atomic<bool> release{false};
        std::atomic<int>  occupied{0};
        std::atomic<int>  finished{0};
        std::atomic<bool> audited{false};
    };
    const auto shared = std::make_shared<Shared>();

    const std::size_t blockers = Pools::analytics().thread_count();
    for (std::size_t i = 0; i < blockers; ++i) {
        ASSERT_TRUE(Pools::analytics().try_post([shared] {
            shared->occupied.fetch_add(1, std::memory_order_relaxed);
            while (!shared->release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            shared->finished.fetch_add(1, std::memory_order_release);
        }));
    }
    while (shared->occupied.load(std::memory_order_relaxed) < static_cast<int>(blockers)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    bool refused = false;
    for (std::size_t i = 0; i < Pools::analytics().queue_capacity() + 1; ++i) {
        if (!Pools::analytics().try_post([] {})) {
            refused = true;
            break;
        }
    }
    EXPECT_TRUE(refused) << "analytics_pool never shed, so this proves nothing";

    // An audit flush, posted while the analytics pool is refusing everything.
    // Not a timing assertion: the question is whether the audit pool ACCEPTS and
    // RUNS the task at all, which a shared pool would not.
    EXPECT_TRUE(Pools::audit().try_post(
        [shared] { shared->audited.store(true, std::memory_order_release); }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!shared->audited.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(shared->audited.load(std::memory_order_acquire));

    // Drained before the test returns. The pools outlive every test in this
    // binary, so a blocker still spinning here is a blocker that spins for the
    // rest of the run.
    shared->release.store(true, std::memory_order_release);
    while (shared->finished.load(std::memory_order_acquire) < static_cast<int>(blockers)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

}  // namespace anvil

