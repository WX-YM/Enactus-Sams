// Phase 8 — the gate for the failures a green suite cannot see.
//
// SCOPED DELIBERATELY: this is not a benchmark suite and it measures no
// latencies. It exists because a `release-check` run of an application built on
// anvil produced two numbers that 1,088 passing tests could not have produced —
// 8,485 refused audit batches against a live, busy mongod, and 393,116 aborted
// transaction attempts — and neither is a timing property, so neither is the
// flaky kind docs/16 rules out.
//
// Five assertions, and each one is a property only load can falsify:
//
//   1. No flush is refused while the database is answering — asserted on BOTH
//      sinks. That is the claim the audit sink's comment made and the load run
//      falsified, and the 8,485 refusals it falsified it with were audit
//      batches, so a gate that drove only the analytics sink would be measuring
//      the one that was built with the correction.
//   2. A full queue SHEDS rather than grows. An unbounded queue converts a flood
//      into an OOM kill (docs/00-architecture.md §3).
//   3. The registry's resident cost stays inside the ceiling stated in its own
//      header. Under a sanitizer the process's RSS measures the sanitizer, so
//      what is asserted is the arena the registry actually allocated — which is
//      the number the ceiling is about.
//   4. The two counters that are zero in a healthy deployment are zero here, and
//      the ones that should have moved have moved.
//
// Labelled `load`, and excluded from every default preset: it saturates pools on
// purpose, which is antisocial to run beside a suite that assumes it can post a
// task.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>

#include <exception>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/ingest.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/analytics/repository.h"
#include "anvil/analytics/sessions.h"
#include "anvil/audit/service.h"
#include "anvil/core/thread_pools.h"
#include "anvil/crypto/random.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "events.h"
#include "testapp/audit_actions.h"
#include "metrics.h"

namespace anvil::analytics {
namespace {

using anvil::testfixture::scratch_names;

constexpr std::string_view kEvents = "analytics_events";
constexpr std::string_view kSessions = "analytics_sessions";
constexpr std::string_view kRollups = "analytics_rollups";
constexpr std::string_view kAuditLog = "audit_log";

constexpr auto kDenied = audit::AuditAction::of(testapp::Action::AccessDenied);
constexpr auto kPermissionChanged =
    audit::AuditAction::of(testapp::Action::StaffPermissionChanged);
inline constexpr AnalyticsCollections kCollections{kEvents, kSessions, kRollups};

// Enough to cross the batch threshold many times over without turning the gate
// into a five-minute job. The point is to exceed kBatchRows repeatedly, not to
// find a throughput number.
constexpr int kProducers = 8;
constexpr int kPerProducer = 2000;

// This binary sizes its OWN pools, and does not use tests/app_fixture.h's.
//
// That fixture is deliberately tiny — a queue of one, so that shedding is
// observable — which is exactly right for the suites asserting that a full queue
// sheds, and exactly wrong here. The first assertion below is "no batch is
// refused WHILE THE DATABASE IS ANSWERING", and against a one-thread pool with
// four batches of headroom that assertion measures the fixture rather than the
// sink: sixteen thousand events in under a second outrun four batches whatever
// the sink does.
//
// Sized as a deployment would size it, from docs/00-architecture.md §3:
// analytics_pool is small and fixed, and its queue holds BATCHES rather than
// rows, so a couple of hundred is tens of thousands of rows of headroom.
//
// Pools::init throws on a second call, and nothing else in this binary calls it.
[[nodiscard]] bool load_pools_ready() {
    struct PoolGuard final {
        bool ready;

        PoolGuard() : ready{false} {
            try {
                Pools::init(PoolSizes{.db_threads = 8,
                                      .db_queue = 256,
                                      .cpu_threads = 2,
                                      .cpu_queue = 16,
                                      .hash_threads = 1,
                                      .hash_queue = 8,
                                      .audit_threads = 2,
                                      .audit_queue = 128,
                                      .analytics_threads = 2,
                                      .analytics_queue = 256});
                ready = true;
            } catch (const std::exception&) {
                ready = false;
            }
        }

        ~PoolGuard() {
            if (ready) { Pools::shutdown(); }
        }

        PoolGuard(const PoolGuard&) = delete;
        PoolGuard& operator=(const PoolGuard&) = delete;
    };

    static const PoolGuard guard{};
    return guard.ready;
}

class LoadGate : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        if (!load_pools_ready()) { GTEST_SKIP() << "thread pools unavailable"; }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kEvents);
        anvil::testfixture::clear_collection(**client_, kSessions);
        anvil::testfixture::clear_collection(**client_, kAuditLog);

        crypto::Key256 pepper;
        crypto::random_bytes(pepper.mutable_span());
        install_visitor_pepper(std::move(pepper));

        registry_ = std::make_shared<Registry>(kInternalMetrics, testapp::kMetrics);
        install_registry(registry_);
    }

    void TearDown() override {
        uninstall_registry();
        registry_.reset();
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] std::uint64_t audit_drops() const {
        std::uint64_t total = 0;
        for (std::size_t cls = 0; cls < kAuditClassValues.size(); ++cls) {
            for (std::size_t stage = 0; stage < kAuditStageValues.size(); ++stage) {
                total += registry_->value_at(metric_of(Internal::AuditRowsDropped),
                                             (cls * kAuditStageValues.size()) + stage, 0);
            }
        }
        return total;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
    std::shared_ptr<Registry>              registry_;
};

TEST_F(LoadGate, NoFlushIsRefusedWhileTheDatabaseIsAnswering) {
    EventSink sink{scratch_names(), kCollections, testapp::kEvents, IngestConfig{}};

    std::atomic<int> recorded{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&sink, &recorded, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                DimensionValues dimensions = no_dimensions();
                dimensions[0] = static_cast<std::uint8_t>(i % 3);
                dimensions[1] = static_cast<std::uint8_t>(i % 2);
                const std::string address =
                    "203.0.113." + std::string{static_cast<char>('0' + (p % 10))};
                const Offer offered{static_cast<EventCode>(testapp::Event::PageViewed),
                                    dimensions, http::pack_address(address), std::nullopt,
                                    true};
                if (sink.offer(offered) == EventSink::Outcome::Recorded) {
                    recorded.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& producer : producers) { producer.join(); }

    // Drain whatever is still held, on this thread.
    ASSERT_TRUE(sink.flush_now(db()).ok());

    // The claim the audit sink's comment made and a load run falsified. A
    // refusal here means analytics_pool's own queue filled while mongod was
    // answering, which is the case that comment excluded.
    EXPECT_EQ(sink.refused_flushes(), 0U) << "analytics_pool shed while the database answered";
    // And none was SKIPPED either. A refusal arms a latch that suppresses the
    // next attempt while the queue stays full, so a gate asserting only the
    // first number would pass on a sink that refused once and then quietly
    // stopped trying.
    EXPECT_EQ(sink.deferred_flushes(), 0U);
    EXPECT_EQ(sink.dropped_conversions(), 0U);
    EXPECT_GT(recorded.load(), 0);

    // Give the pooled flushes a bounded moment to land before counting rows.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    const std::int64_t expected = recorded.load();
    std::int64_t stored = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        stored = db()[std::string{scratch_names().for_collection(kEvents)}]
                     [std::string{kEvents}]
                         .count_documents(bsoncxx::builder::basic::make_document());
        if (stored > 0 && Pools::analytics().queue_depth() == 0) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    // Fewer ROWS than events, because the coalescer folds — which is the point
    // of it. What must hold is that rows were written at all and that nothing
    // was dropped.
    EXPECT_GT(stored, 0);
    EXPECT_LE(stored, expected);
    EXPECT_EQ(sink.dropped_behaviour(), 0U);
}

TEST_F(LoadGate, NoAuditFlushIsRefusedWhileTheDatabaseIsAnswering) {
    // The measurement this gate exists for — 8,485 refused batches against a
    // live, busy mongod — was taken on the AUDIT sink, and until this case
    // nothing here drove it. The assertion above measures the analytics sink,
    // which shipped with the correction the audit sink was missing, so on its
    // own it proves the wrong half.
    audit::AuditService sink{std::string{scratch_names().for_collection(kAuditLog)},
                             kAuditLog, testapp::kAuditActions, kDenied};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&sink, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                // Changes, which do not coalesce, so the pressure on the pool is
                // the real thing rather than the coalescer's.
                std::array<std::uint8_t, 16> ip{};
                ip[14] = static_cast<std::uint8_t>(p);
                ip[15] = static_cast<std::uint8_t>(i);
                sink.write_async(audit::AuditEntry{.actor = uuid::generate_v4(),
                                                   .subject = uuid::generate_v4(),
                                                   .from_state = 0,
                                                   .to_state = 1,
                                                   .ip = ip,
                                                   .action = kPermissionChanged,
                                                   .code = ErrorCode::Ok,
                                                   .succeeded = true,
                                                   .stealthed = false});
            }
        });
    }
    for (std::thread& producer : producers) { producer.join(); }

    ASSERT_TRUE(sink.flush_now(db()).ok());

    EXPECT_EQ(sink.refused_flushes(), 0U) << "audit_pool shed while the database answered";
    EXPECT_EQ(sink.deferred_flushes(), 0U);
    // Changes are the class that must never be lost, and a refused flush no
    // longer loses them even when one happens.
    EXPECT_EQ(sink.dropped_changes(), 0U);
    EXPECT_EQ(sink.dropped_traffic(), 0U);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    std::int64_t stored = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        stored = db()[std::string{scratch_names().for_collection(kAuditLog)}]
                     [std::string{kAuditLog}]
                         .count_documents(bsoncxx::builder::basic::make_document());
        if (stored >= kProducers * kPerProducer && Pools::audit().queue_depth() == 0) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    // Every row, because nothing here is compressible and nothing was dropped.
    EXPECT_EQ(stored, static_cast<std::int64_t>(kProducers) * kPerProducer);
}

TEST_F(LoadGate, AFullQueueShedsRatherThanGrows) {
    // An unbounded queue converts a flood into an OOM kill, so "full" means shed
    // and the depth may never exceed the bound — however many tasks are thrown
    // at it and from however many threads.
    const std::size_t capacity = Pools::analytics().queue_capacity();
    struct Shared final {
        std::atomic<bool> release{false};
        std::atomic<int>  occupied{0};
        std::atomic<int>  finished{0};
        std::atomic<int>  refused{0};
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

    std::vector<std::thread> floods;
    floods.reserve(4);
    for (int t = 0; t < 4; ++t) {
        floods.emplace_back([shared] {
            for (int i = 0; i < 5000; ++i) {
                if (!Pools::analytics().try_post([] {})) {
                    shared->refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& flood : floods) { flood.join(); }

    EXPECT_GT(shared->refused.load(), 0) << "the queue never shed, so this proves nothing";
    EXPECT_LE(Pools::analytics().queue_depth(), capacity);

    shared->release.store(true, std::memory_order_release);
    while (shared->finished.load(std::memory_order_acquire) < static_cast<int>(blockers)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

TEST_F(LoadGate, TheRegistryStaysInsideTheCeilingItsOwnHeaderStates) {
    // bytes = SUM over cells (gauge ? 1 : shards) x 64 <= kMaxCells x kMaxShards
    // x 64 = 1 MiB. Under a sanitizer the process's RSS measures the sanitizer,
    // so what is asserted is the arena the registry actually allocated — which
    // is the number the ceiling is about.
    EXPECT_LE(registry_->arena_bytes(), kMaxCells * Registry::kMaxShards * 64U);
    EXPECT_LE(registry_->shard_count(), Registry::kMaxShards);

    // And the whole of it is one allocation made at construction: the cell count
    // is fixed by the two tables and nothing here grows it under load.
    const std::size_t before = registry_->cell_capacity();
    for (int i = 0; i < 100000; ++i) {
        count(Internal::AuthzCacheHits, AuthzTier::Local);
        observe(Internal::MongoPoolWaitMicroseconds, i % 2000);
    }
    EXPECT_EQ(registry_->cell_capacity(), before);
}

TEST_F(LoadGate, TheCountersThatAreZeroInAHealthyDeploymentAreZero) {
    EventSink sink{scratch_names(), kCollections, testapp::kEvents, IngestConfig{}};
    for (int i = 0; i < 4000; ++i) {
        DimensionValues dimensions = no_dimensions();
        dimensions[0] = static_cast<std::uint8_t>(i % 3);
        ASSERT_NE(sink.offer(Offer{static_cast<EventCode>(testapp::Event::SignupCompleted),
                                   dimensions, http::pack_address("198.51.100.4"),
                                   std::nullopt, true}),
                  EventSink::Outcome::Dropped);
    }
    ASSERT_TRUE(sink.flush_now(db()).ok());

    // Both of these are zero in a healthy deployment and neither is visible from
    // outside: a dropped audit row produces no error response, and a correctly
    // retried transaction produces none either.
    EXPECT_EQ(audit_drops(), 0U);
    EXPECT_EQ(registry_->value_at(metric_of(Internal::TransactionsAborted),
                                  static_cast<std::size_t>(TxnOutcome::Failed), 0),
              0U);

    // And the one that SHOULD have moved did: every acquire on this path timed
    // its wait, so the histogram's count is non-zero even where every wait was
    // a fraction of a microsecond.
    const MetricId wait = metric_of(Internal::MongoPoolWaitMicroseconds);
    const std::size_t count_slot = kMongoWaitBucketsUs.size() + 2;
    EXPECT_GT(registry_->value_at(wait, 0, count_slot), 0U);
}

}  // namespace
}  // namespace anvil::analytics
