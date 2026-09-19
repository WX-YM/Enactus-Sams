#include "anvil/core/thread_pools.h"

#include <openssl/crypto.h>

#include <memory>
#include <stdexcept>
#include <utility>

#include <trantor/utils/Logger.h>

#include "anvil/analytics/gauges.h"
#include "anvil/http/trace_context.h"

namespace anvil {
namespace {

std::unique_ptr<BoundedThreadPool> g_db;
std::unique_ptr<BoundedThreadPool> g_cpu;
std::unique_ptr<BoundedThreadPool> g_hash;
std::unique_ptr<BoundedThreadPool> g_audit;
std::unique_ptr<BoundedThreadPool> g_analytics;

BoundedThreadPool& require(const std::unique_ptr<BoundedThreadPool>& pool, const char* which) {
    if (!pool) { throw std::logic_error(std::string{"Pools::init not called for "} + which); }
    return *pool;
}

}  // namespace

BoundedThreadPool::BoundedThreadPool(std::string name, std::size_t threads,
                                     std::size_t max_queue)
    : name_{std::move(name)},
      max_queue_{max_queue},
      mutex_{},
      cv_{},
      queue_{},
      stopping_{false},
      workers_{} {
    // Emit one line BEFORE spawning any worker.
    //
    // Trantor's logger initialises its output function lazily, and that
    // initialisation is not thread-safe: two threads whose first-ever log call
    // overlaps race on it (confirmed by TSan). guarded() logs from whichever
    // worker catches an exception, so without this the first two concurrent
    // task failures would hit the race — in production, not just in tests.
    // Forcing the initialisation here, single-threaded, closes it.
    LOG_INFO << "thread pool " << name_ << " starting: " << threads
             << " threads, queue cap " << max_queue_;

    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { run(); });
    }
}

BoundedThreadPool::~BoundedThreadPool() { shutdown(); }

bool BoundedThreadPool::try_post(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        if (stopping_.load(std::memory_order_relaxed)) { return false; }
        if (queue_.size() >= max_queue_) { return false; }   // shed, never grow
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void BoundedThreadPool::shutdown() noexcept {
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        if (stopping_.exchange(true, std::memory_order_relaxed)) { return; }
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) { worker.join(); }
    }
    workers_.clear();
}

std::size_t BoundedThreadPool::queue_depth() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    return queue_.size();
}

void BoundedThreadPool::run() noexcept {
    // OpenSSL allocates per-thread DRBG and error state the first time a thread
    // touches RAND_bytes or a digest — which every hash_pool and db_pool worker
    // does. That state is released by OPENSSL_thread_stop, NOT by thread exit,
    // so a worker must say so on the way out. Without it the state lives until
    // the process ends, which is a genuine leak for any pool that ever recycles
    // a thread and is reported as one by LeakSanitizer on every run.
    struct OpensslThreadState final {
        OpensslThreadState() = default;
        ~OpensslThreadState() { OPENSSL_thread_stop(); }
        OpensslThreadState(const OpensslThreadState&) = delete;
        OpensslThreadState& operator=(const OpensslThreadState&) = delete;
    } const openssl_state;

    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            cv_.wait(lock, [this] {
                return stopping_.load(std::memory_order_relaxed) || !queue_.empty();
            });
            // Drain on shutdown: an in-flight job that is dropped is a lost job.
            if (queue_.empty()) { return; }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();   // already wrapped by guarded()
    }
}

void Pools::init(const PoolSizes& sizes) {
    if (g_db) { throw std::logic_error("Pools::init called twice"); }
    g_db = std::make_unique<BoundedThreadPool>("db", sizes.db_threads, sizes.db_queue);
    g_cpu = std::make_unique<BoundedThreadPool>("cpu", sizes.cpu_threads, sizes.cpu_queue);
    g_hash = std::make_unique<BoundedThreadPool>("hash", sizes.hash_threads, sizes.hash_queue);
    g_audit =
        std::make_unique<BoundedThreadPool>("audit", sizes.audit_threads, sizes.audit_queue);
    g_analytics = std::make_unique<BoundedThreadPool>("analytics", sizes.analytics_threads,
                                                      sizes.analytics_queue);

    // Installed HERE rather than left to the application, so a deployment cannot
    // have bounded queues and no view of how full they are. Every one of these
    // queues sheds when it is full (docs/00-architecture.md §3), and a shed with
    // no depth series behind it is a 503 with no explanation.
    analytics::install_pool_gauge_sampler();
}

bool Pools::ready() noexcept { return static_cast<bool>(g_db); }

void Pools::shutdown() noexcept {
    // Reverse of init: stop accepting CPU and hash work before the DB pool the
    // rest depends on.
    if (g_hash) { g_hash->shutdown(); }
    if (g_cpu) { g_cpu->shutdown(); }
    if (g_db) { g_db->shutdown(); }
    // Analytics before audit: a row this pool is holding is a measurement, and a
    // row the audit pool is holding is the only record that somebody did
    // something. When the two compete for the last moments of a shutdown, the
    // forensic record wins.
    if (g_analytics) { g_analytics->shutdown(); }
    // LAST. AuditService::stop() writes its final batch on the calling thread
    // rather than through this pool, but a flush already in flight when the
    // signal arrived is a batch of rows that exists nowhere else, and shutdown()
    // drains what it accepted.
    if (g_audit) { g_audit->shutdown(); }
}

BoundedThreadPool& Pools::db() { return require(g_db, "db"); }
BoundedThreadPool& Pools::cpu() { return require(g_cpu, "cpu"); }
BoundedThreadPool& Pools::hash() { return require(g_hash, "hash"); }
BoundedThreadPool& Pools::audit() { return require(g_audit, "audit"); }
BoundedThreadPool& Pools::analytics() { return require(g_analytics, "analytics"); }

std::function<void()> guarded(std::string_view pool_name, std::function<void()> body) {
    // The trace is sampled HERE, on the posting thread, because here is the only
    // place that still knows which request this work belongs to. By the time the
    // body runs it is on a pool thread that has no request of its own, and a
    // context read there would be whatever the previous task left behind.
    //
    // 25 bytes copied into the closure, which is already allocating for the body
    // and the name. `TraceScope` restores on the way out including on the catch
    // path below — a worker thread runs many tasks, and one that carries no
    // trace must not inherit the last one that did (http/trace_context.h).
    return [name = std::string{pool_name}, trace = http::current_trace(),
            body = std::move(body)]() noexcept {
        const http::TraceScope scope{trace};
        try {
            body();
        } catch (const std::exception& e) {
            LOG_ERROR << "task threw in pool " << name << ": " << e.what();
        } catch (...) {
            LOG_ERROR << "task threw a non-std exception in pool " << name;
        }
    };
}

}  // namespace anvil
