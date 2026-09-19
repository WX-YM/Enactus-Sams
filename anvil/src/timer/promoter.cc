#include "anvil/timer/queue.h"
#include "anvil/timer/registry.h"

#include <utility>

#include <trantor/utils/Logger.h>

#include "anvil/core/thread_pools.h"
#include "anvil/db/mongo_pool.h"

namespace anvil::timer {
namespace {

// One tick promotes at most this many entries, in batches. 10 000 jobs due in
// the same second therefore arrive over a few ticks rather than materialising in
// the stream at once, which is what keeps Redis memory and the pending list
// bounded (docs/10-timer-jobs.md §6).
constexpr std::size_t kMaxPromotedPerTick = 1024;

// How often the reclaim pass runs. Independent of the idle threshold: the pass is
// cheap (one XAUTOCLAIM that usually returns nothing), and running it often means
// a crashed worker's jobs restart as soon as their lease has actually lapsed.
constexpr std::chrono::seconds kReclaimInterval{30};

}  // namespace

void JobQueue::start() {
    if (running_.exchange(true)) { return; }

    // The group must exist before any claimer reads. A failure here is not fatal:
    // the claimers retry, and a Redis that is down at boot must not stop the
    // service from answering requests that do not need it.
    if (const Status ready = ensure_group(); !ready) {
        LOG_ERROR << "job queue starting without a consumer group; claimers will retry";
    }

    promoter_ = std::thread{[this]() { promoter_loop(); }};

    claimers_.reserve(config_.claimers);
    for (std::size_t i = 0; i < config_.claimers; ++i) {
        claimers_.emplace_back([this, i]() { claimer_loop(i); });
    }

    LOG_INFO << "job queue up: prefix=" << config_.prefix << " consumer=" << config_.consumer
             << " claimers=" << config_.claimers;
}

void JobQueue::stop() noexcept {
    if (!running_.exchange(false)) { return; }

    // Wake the promoter out of its interval wait immediately. The claimers are
    // parked in XREADGROUP BLOCK and observe the flag within one block period,
    // which is why that period is seconds rather than minutes.
    {
        const std::lock_guard<std::mutex> lock{wake_mutex_};
        wake_.notify_all();
    }

    for (std::thread& claimer : claimers_) {
        if (claimer.joinable()) { claimer.join(); }
    }
    claimers_.clear();
    if (promoter_.joinable()) { promoter_.join(); }

    // After the threads, so nothing can renew it back. The lease is released
    // rather than left to expire so a successor takes over in milliseconds
    // instead of waiting out the TTL on a clean shutdown.
    release_lease();
}

// --- the promoter -----------------------------------------------------------

void JobQueue::promoter_loop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        // A lease we do not hold means another instance is promoting. Skipping is
        // the whole point: correctness comes from the promote script being
        // atomic, and the lease only stops N instances doing the same work
        // (docs/10-timer-jobs.md §5).
        if (hold_lease()) {
            const db::TimeMs now = db::now_ms();

            // Recurrences first: a due recurrence that has not been scheduled yet
            // would otherwise wait a whole extra tick.
            if (const Result<std::size_t> ensured = ensure_recurring(now); !ensured) {
                LOG_WARN << "recurring schedules not reconciled this tick";
            }

            std::size_t promoted = 0;
            while (promoted < kMaxPromotedPerTick && running_.load(std::memory_order_acquire)) {
                const Result<std::size_t> moved = promote_due(now, config_.batch);
                if (!moved || moved.value() == 0) { break; }
                promoted += moved.value();
                // A short batch means the ZSET is drained to `now`; anything else
                // is a full batch and there may be more.
                if (moved.value() < config_.batch) { break; }
            }
            if (promoted != 0) { LOG_DEBUG << "promoted " << promoted << " due jobs"; }
        }

        std::unique_lock<std::mutex> lock{wake_mutex_};
        wake_.wait_for(lock, config_.promoter_interval,
                       [this]() { return !running_.load(std::memory_order_acquire); });
    }
}

// --- the claimers -----------------------------------------------------------

void JobQueue::claimer_loop(std::size_t index) noexcept {
    // steady_clock, not system_clock: this is a duration between two local
    // events, and a system clock that steps backwards would stall the reclaim
    // pass until it caught up.
    auto next_reclaim = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_reclaim) {
            next_reclaim = now + kReclaimInterval;
            // Entries whose claimer never acknowledged them: a killed process, or
            // one whose pool post was shed. Without this a job dies with the
            // worker that claimed it (c).
            Result<std::vector<ClaimedJob>> stalled =
                reclaim_stalled(std::chrono::seconds{kMaxLeaseSeconds}, config_.batch);
            if (stalled) {
                std::vector<ClaimedJob> jobs = std::move(stalled).value();
                if (!jobs.empty()) {
                    LOG_WARN << "reclaimed " << jobs.size()
                             << " stalled job(s) from a worker that never acknowledged them";
                }
                for (ClaimedJob& job : jobs) {
                    if (!post_to_pool(std::move(job))) { break; }
                }
            }
        }

        // Zero CPU while parked here — this is what makes an idle worker free
        // and what removes poll-interval latency entirely (docs/10-timer-jobs.md §4, §9).
        Result<std::vector<ClaimedJob>> claimed = claim(config_.block, config_.batch);
        if (!claimed) {
            // Redis unreachable. Back off rather than spinning on a refused
            // connection, and observe the stop flag while doing it.
            std::unique_lock<std::mutex> lock{wake_mutex_};
            wake_.wait_for(lock, std::chrono::seconds{1},
                           [this]() { return !running_.load(std::memory_order_acquire); });
            continue;
        }

        std::vector<ClaimedJob> jobs = std::move(claimed).value();
        for (ClaimedJob& job : jobs) {
            const JobSpec* spec = spec_of(job.header.kind);
            if (!post_to_pool(std::move(job))) {
                // Deliberately NOT acknowledged: the entry stays pending and is
                // reclaimed once its lease lapses. Acknowledging work that was
                // never done would lose it, and blocking here would park the
                // claimer behind a saturated pool.
                LOG_WARN << "job shed: "
                         << std::string{spec == nullptr ? std::string_view{"unknown"}
                                                        : spec->key}
                         << " pool saturated; entry left pending for reclaim";
                break;
            }
        }
    }

    LOG_INFO << "job claimer " << index << " stopped";
}

bool JobQueue::post_to_pool(ClaimedJob job) {
    const JobSpec* spec = spec_of(job.header.kind);
    if (spec == nullptr) {
        // Nothing to run. Dead-letter it here rather than on a pool thread: the
        // decision needs no client and no work.
        (void)complete(job, JobOutcome::Failed, db::now_ms());
        return true;
    }

    BoundedThreadPool& pool = spec->pool == JobPool::Cpu ? Pools::cpu() : Pools::db();

    // Everything the task needs is captured BY VALUE or moved. Capturing the
    // claimed job by reference would be a use-after-free the moment this function
    // returns (ENGINEERING_RULES.md §3.3).
    //
    // `this` is captured raw, which is safe only because the queue outlives every
    // pool: main() stops the queue, then shuts the pools down, then destroys the
    // queue. Reversing those two would be a use-after-free in a drained task.
    return pool.try_post(guarded(pool.name(), [this, job = std::move(job)]() {
        // The client is acquired INSIDE the task, so a job waiting for a pool
        // slot is not also holding a database connection.
        auto client = db::MongoPool::instance().acquire();
        const JobOutcome outcome = dispatch(job, *client);
        (void)complete(job, outcome, db::now_ms());
    }));
}

}  // namespace anvil::timer
