#pragma once

// Five thread pools, sized independently, each with a bounded queue.
//
// A single shared pool couples every workload's latency: one burst of image
// uploads occupies every thread and every database query — including login —
// queues behind seconds of CPU work (docs/00-architecture.md §3).
//
//   db_pool        all mongocxx calls on a request path; the driver is
//                  synchronous
//   cpu_pool       libvips work; sized from hardware concurrency
//   hash_pool      Argon2id ONLY; sized from a MEMORY budget, not a core count.
//                  Argon2id at 64 MiB x 32 threads is 2 GiB of RSS reachable by
//                  one attacker, so the pool size IS the memory cap.
//   audit_pool     the audit sink's flushes, and nothing else.
//   analytics_pool the event sink's flushes and the rollup's batches.
//
// audit_pool is the fourth for a reason worth stating plainly.
// Batching audit writes stopped the sink from saturating db_pool; it did nothing
// about db_pool starving the sink. A batch posted to the queue every request
// uses is refused precisely when that queue is full, which is precisely the
// flood the rows describe — so the forensic record went blank for the one
// traffic pattern 03-access-control.md §4.4 nominates as the intrusion
// detector. Capacity that a request path cannot spend is the only fix.
//
// It is SMALL. Its work is one insert_many per second per 256 rows, so a couple
// of threads is a wide margin, and every thread here draws a mongocxx client
// that MONGO_POOL_MAX must cover (anvil/config/config.cc).
//
// analytics_pool is the fifth, and reusing audit_pool for it would NOT have been
// symmetry — it would have re-created exactly the problem audit_pool was made to
// solve. audit_pool exists so a saturated request path cannot starve the record
// of what saturated it; a second writer on it starves that record again, and the
// second writer is the one that floods, because an analytics event fires on
// requests that are not interesting enough to audit (docs/17-analytics.md §10).
//
// Queues are bounded and full means shed with 503, never queue unboundedly:
// an unbounded queue converts a login flood into an OOM kill.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace anvil {

class BoundedThreadPool final {
public:
    BoundedThreadPool(std::string name, std::size_t threads, std::size_t max_queue);
    ~BoundedThreadPool();

    // Returns false when the queue is full — the caller must then shed with 503
    // rather than block. Never blocks a Trantor event-loop thread.
    [[nodiscard]] bool try_post(std::function<void()> task);

    void shutdown() noexcept;

    [[nodiscard]] std::size_t queue_depth() const;
    [[nodiscard]] std::size_t queue_capacity() const noexcept { return max_queue_; }
    // Advisory admission control. try_post is still the authority — this only
    // lets a caller shed BEFORE doing expensive preparatory work, which on the
    // login path means before touching the database at all.
    [[nodiscard]] bool saturated() const { return queue_depth() >= max_queue_; }
    [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    BoundedThreadPool(const BoundedThreadPool&) = delete;
    BoundedThreadPool& operator=(const BoundedThreadPool&) = delete;

private:
    void run() noexcept;

    // Declaration order is construction order: everything the worker threads
    // touch must be fully constructed before workers_ spawns them.
    const std::string               name_;
    const std::size_t               max_queue_;
    mutable std::mutex              mutex_;
    std::condition_variable         cv_;
    std::deque<std::function<void()>> queue_;
    std::atomic<bool>               stopping_;
    std::vector<std::thread>        workers_;   // LAST — spawned after the rest
};

struct PoolSizes final {
    std::size_t db_threads;
    std::size_t db_queue;
    std::size_t cpu_threads;
    std::size_t cpu_queue;
    std::size_t hash_threads;   // == argon2_memory_budget_mib / 64
    std::size_t hash_queue;
    // Small and fixed. Its queue holds BATCHES of up to 256 rows, not rows, so a
    // bound of a few dozen is thousands of rows of headroom in front of a sink
    // that flushes every second.
    std::size_t audit_threads;
    std::size_t audit_queue;
    // Small and fixed, like audit's, and for the same reason: its queue holds
    // BATCHES, not rows. Separate from audit's because the two must not be able
    // to starve each other — see the header comment.
    std::size_t analytics_threads;
    std::size_t analytics_queue;
};

class Pools final {
public:
    static void init(const PoolSizes& sizes);
    static void shutdown() noexcept;

    // Whether init() has run. For a caller that must not throw — the queue-depth
    // gauge sampler runs on a scrape, and a scrape in a process that never
    // started its pools should report nothing rather than fail.
    [[nodiscard]] static bool ready() noexcept;

    [[nodiscard]] static BoundedThreadPool& db();
    [[nodiscard]] static BoundedThreadPool& cpu();
    [[nodiscard]] static BoundedThreadPool& hash();
    [[nodiscard]] static BoundedThreadPool& audit();
    [[nodiscard]] static BoundedThreadPool& analytics();
};

// Wraps a task body in try/catch. An exception escaping a pool task calls
// std::terminate and takes the process down. Every task posted to
// any pool goes through this.
[[nodiscard]] std::function<void()> guarded(std::string_view pool_name,
                                            std::function<void()> body);

}  // namespace anvil
