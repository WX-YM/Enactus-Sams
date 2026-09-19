#pragma once

// A bounded pool, held full on purpose.
//
// Two suites need the same three moves: occupy the single worker, fill the queue
// behind it, and let both go again. Both are about a sink whose flush the pool
// REFUSES — AuditService in tests/audit_service_db_test.cc and EventSink in
// tests/analytics_db_test.cc — and that refusal is only reachable while the
// queue is genuinely full. A second copy of this would be a second place for the
// blocker to be subtly wrong in, which matters because a blocker that fails to
// block turns a case asserting a refusal into a case asserting nothing.
//
// It needs tests/app_fixture.h's pools: one worker and a queue of four, sized
// that way precisely so shedding is observable (see pools_ready()).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "anvil/core/thread_pools.h"

namespace anvil::testfixture {

// Held by every task posted here, so a task that outlives the test body still
// touches live memory rather than the fixture's.
struct Gate final {
    std::atomic<bool> release{false};
    std::atomic<bool> occupied{false};
    std::atomic<bool> drained{false};
};

// A pool held full for the lifetime of one case. `pool` is a reference returned
// by Pools::audit() or Pools::analytics(); it outlives every test.
class PoolGate final {
public:
    explicit PoolGate(BoundedThreadPool& pool)
        : pool_{pool}, gate_{std::make_shared<Gate>()} {}

    ~PoolGate() { gate_->release.store(true, std::memory_order_release); }

    PoolGate(const PoolGate&) = delete;
    PoolGate& operator=(const PoolGate&) = delete;

    // Occupies the single worker until release. ASSERT_* only expands inside a
    // void function, which is why these are void and not bool.
    void block() {
        const std::shared_ptr<Gate> gate = gate_;
        ASSERT_TRUE(pool_.try_post([gate] {
            gate->occupied.store(true, std::memory_order_release);
            while (!gate->release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }));
        while (!gate_->occupied.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    // Fills the queue behind the blocked worker, so the next try_post is refused
    // while the database is answering — which is the case the audit sink's old
    // comment reasoned could not happen and a load run produced 8,485 times.
    void fill_queue() {
        const std::shared_ptr<Gate> gate = gate_;
        for (std::size_t i = 0; i < pool_.queue_capacity(); ++i) {
            ASSERT_TRUE(pool_.try_post([gate] { (void)gate; }));
        }
        ASSERT_FALSE(pool_.try_post([] {})) << "the queue is not full";
    }

    // Releases the worker and waits for a sentinel posted BEHIND whatever the
    // test queued. One worker means FIFO, so the sentinel running is proof that
    // everything before it has finished.
    //
    // Released before the sentinel is posted, not after: a test that filled the
    // queue on purpose has nowhere to put one until the worker starts taking
    // them again.
    //
    // Callable more than once in a case. A test that lets the pool drain, posts
    // another batch and then has to wait for THAT one needs a second sentinel,
    // and a flag left set by the first would make the second return before the
    // task it is waiting for had run.
    void drain() {
        const std::shared_ptr<Gate> gate = gate_;
        gate_->drained.store(false, std::memory_order_release);
        gate_->release.store(true, std::memory_order_release);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!pool_.try_post(
            [gate] { gate->drained.store(true, std::memory_order_release); })) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline)
                << pool_.name() << " never made room for the sentinel";
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }

        while (!gate_->drained.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        ASSERT_TRUE(gate_->drained.load(std::memory_order_acquire))
            << pool_.name() << " never drained";
    }

private:
    BoundedThreadPool&    pool_;
    std::shared_ptr<Gate> gate_;
};

}  // namespace anvil::testfixture
