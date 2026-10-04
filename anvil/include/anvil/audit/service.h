#pragma once

// Audit writes, and the adapter that lets the access filter produce them without
// knowing what a repository is.
//
// Every write here is FIRE AND FORGET. That is not laziness, it is the only
// correct coupling: a request that already succeeded must not become a 500
// because an audit insert failed, and a request that was already denied cannot
// become more denied. Failures are logged and counted; they never propagate.
//
// It is also always ASYNCHRONOUS. The access filter calls record() from a
// Trantor event-loop thread, immediately after the response has gone out, and
// nothing blocking may run there (CLAUDE.md §4).
//
// --- why it batches ---------------------------------------------------------
//
// One task per row means one pool task and one insert per audited event, and
// audited events include every denial — so a burst against a stealth route
// queues one task per request, the queue fills, and rows are lost. The forensic
// record goes blank for exactly the traffic it exists to describe.
//
// --- why batching alone is not enough ---------------------------------------
//
// Batching removes the amplification and leaves the coupling. A batch sent to
// db_pool — the queue every request already uses — is refused precisely when
// that queue is full, which is precisely the flood the rows describe, and 256
// rows go at once.
//
// The fix is three things in this order — classify, coalesce, isolate — because
// the first two remove most of the pressure the third would have to be sized
// for. The first two live in AuditBuffer, which is where they can be tested. The
// third is here: flushes go to `audit_pool`, which is small, has its own bounded
// queue, and is not spendable by a request path.
//
// --- the storm re-admission opened ------------------------------------------
//
// A refused flush returns its CHANGES to the buffer (anvil/audit/buffer.h), and
// `write_async` posts a flush the moment the buffer crosses `kBatchRows`. Put
// those two together with a pool whose queue stays full and every subsequent row
// drains 256 rows, copies them into a task, is refused, and puts them back:
// roughly 50 KB of memory traffic, three allocations AND one LOG_WARN per row,
// in the state where the process is already behind. The log line alone is the
// shape docs/00-architecture.md §9 forbids by name.
//
// Reaching it takes thousands of distinct CHANGE rows per second against a
// database that has stopped keeping up — traffic folds in the coalescer and
// re-admission does not return it, so it takes changes to hold the buffer above
// the batch size. That is rare and it is not hypothetical, and the cost is paid
// exactly when there is none to spare.
//
// So a refusal ARMS a latch, and while it is armed a flush is skipped for as
// long as `audit_pool` reports itself saturated. The division of labour is the
// one `BoundedThreadPool::saturated()` documents for itself: `try_post` is the
// authority and one refusal is what tells us the pool is full; `saturated()` is
// advisory and is what stops us asking again until the answer can be different.
//
// Paying that one refusal rather than consulting `saturated()` first is also
// what keeps the refusal path reachable by a test at all — and the refusal path
// is where the re-admission lives.
//
// Nothing is lost by waiting: the rows stay in a buffer that is already bounded
// and already sheds oldest-traffic-first, so the loss happens at the bound,
// where the policy is, rather than at every refused flush.
//
// The deferred path REMOVES work rather than adding any. It is one lock of the
// pool's own mutex, which `try_post` was taking anyway as the first thing it
// did — and it skips the drain, the copy, the allocations, the re-admission and
// the line. The nesting is safe in one direction only and it holds: this class
// takes its own mutex and then the pool's, while `post()` takes the pool's,
// releases it inside `try_post`, and only then takes this one.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/audit/action.h"
#include "anvil/audit/buffer.h"
#include "anvil/audit/record.h"
#include "anvil/audit/repository.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::audit {

class AuditService final : public accesscontrol::DenialSink {
public:
    // A flush is one insert_many, so this is the number of rows one pooled task
    // covers. It is the whole reason a burst no longer costs a task per request.
    static constexpr std::size_t kBatchRows = 256;
    // A quiet deployment must not hold the last few rows indefinitely: an audit
    // row that exists only in one process's memory is an audit row a crash
    // erases. It is also the coalescer's window — a fold never outlives it.
    static constexpr std::chrono::milliseconds kFlushInterval{1000};

    // `actions` is the application's table; `denial_action` is the one action in
    // it that anvil's own DenialSink produces. anvil cannot guess which value
    // that is, and a denial written under a value the table does not declare
    // would be classified as a Change and never coalesce — which is the flood
    // case, unshed.
    AuditService(std::string database, std::string_view collection,
                 std::span<const AuditActionSpec> actions, AuditAction denial_action,
                 std::size_t capacity_rows = AuditBuffer::kDefaultCapacityRows);
    ~AuditService();

    // Installs the periodic flush on the event loop. Called from main() as
    // beginning advice, for the same reason a stream registry is: getLoop()
    // before run() has no loop to attach a timer to.
    void start();

    // Flushes what is buffered — INCLUDING the coalescer's open window, which is
    // the newest row and therefore the one a crash would most likely lose —
    // synchronously, and stops accepting new rows. Called during shutdown BEFORE
    // the pools drain, so the last rows are written rather than discarded with
    // the process.
    //
    // It also takes out the timer start() installed, which holds `this`. Call it
    // from the LOOP THREAD, or before the loop runs: Trantor's invalidateTimer
    // is synchronous only there, and queued from anywhere else — which leaves a
    // window in which the loop can fire the timer one more time. Drogon's
    // termination advice already runs on the loop thread, which is where this
    // belongs.
    void stop() noexcept;

    // Buffers the row and returns. Never blocks on the database, never throws.
    void record(const accesscontrol::DenialRecord& denial) noexcept override;

    // The general form, for a caller that already holds a client on db_pool.
    [[nodiscard]] Status write(mongocxx::client& client, const AuditEntry& entry) const;

    // The general form, buffered. For a caller on a loop thread.
    void write_async(const AuditEntry& entry) noexcept;

    // The one READ, for a caller that already holds a client on db_pool.
    //
    // It reads only what has been FLUSHED, and that is correct rather than
    // merely tolerable: the buffer holds at most one flush interval of rows, and
    // an investigator looking at the last second of a four-hundred-day log is
    // watching a screen refresh. Draining the buffer to answer a read would put
    // a synchronous flush on a request path to make a page one second fresher.
    [[nodiscard]] Result<AuditPage> read(mongocxx::client& client,
                                         const AuditQuery& query) const;

    // Rows lost since boot, PER CLASS. Zero in every healthy deployment; exposed
    // so a test can assert that and an operator can alert on it.
    //
    // Two counters and not one, because they mean different things. Lost traffic
    // is a load signal — compressible rows the coalescer could not fold fast
    // enough. Lost changes are an OUTAGE: a change is refused only when the
    // buffer holds nothing compressible at all, and each one is the only record
    // that somebody did something. A single number hides the second behind the
    // first.
    [[nodiscard]] std::uint64_t dropped_traffic() const noexcept;
    [[nodiscard]] std::uint64_t dropped_changes() const noexcept;

    // Flushes audit_pool refused, which is NOT a row count: a refused flush
    // returns its changes to the buffer and loses only the compressible half,
    // so the two numbers answer different questions. Zero in a healthy
    // deployment; a load run against a live, busy mongod produced 8,485.
    [[nodiscard]] std::uint64_t refused_flushes() const noexcept;

    // Flushes this sink did not ATTEMPT because the pool was known full. The
    // other half of the number above, and the one that says the storm below is
    // being avoided rather than merely survived.
    //
    // No metric of its own. It is a function of
    // `anvil_pool_queue_depth{pool="audit"}`, which is already sampled, and a
    // second series that only ever moves when the first is at its ceiling is a
    // series nobody can act on separately (docs/17-analytics.md §6). What is
    // exposed here is what a TEST needs to prove the storm is gone.
    [[nodiscard]] std::uint64_t deferred_flushes() const noexcept;

    // Rows currently buffered. For a test that needs to observe the flush, and
    // for the queue-depth metric.
    [[nodiscard]] std::size_t buffered() const noexcept;

    // Drains and writes synchronously on the CALLING thread. Blocking, so it
    // belongs on a pool thread or in a test — never on a loop thread. Exposed so
    // a test can drive the flush without waiting out the interval.
    [[nodiscard]] Status flush_now(mongocxx::client& client);

    AuditService(const AuditService&) = delete;
    AuditService& operator=(const AuditService&) = delete;

private:
    // Moves the buffer out and posts it to audit_pool. Returns the rows it took,
    // so the caller can tell an empty flush from a posted one.
    std::size_t flush_locked(std::vector<AuditRow>& out);
    // Posts one batch, and on a refusal hands it back to the buffer rather than
    // discarding it — the buffer classifies, so what comes back and what is
    // charged as lost is decided in the one place that knows the classes.
    void        post(std::vector<AuditRow> batch) noexcept;

    // Declaration order is construction order: log_ is built from database_, and
    // buffered_ from actions_.
    const std::string                database_;
    std::span<const AuditActionSpec> actions_;
    const AuditAction                denial_action_;
    AuditRepository                  log_;
    mutable std::mutex               mutex_;
    // Guarded by mutex_, including its counters — so "how many were lost" is one
    // reading taken with the buffer that lost them, rather than two that can
    // disagree.
    AuditBuffer                      buffered_;
    // Rows refused after stop(), which is the one loss the buffer cannot count:
    // it never sees them. Rows lost to a refused FLUSH are charged inside the
    // buffer by readmit(), against the same per-class counters a full buffer
    // charges, so a reading is one number rather than two that can disagree.
    std::uint64_t                    refused_traffic_ = 0;
    std::uint64_t                    refused_changes_ = 0;
    // Flushes audit_pool refused. A second stage, not the same number as a row
    // drop: a full buffer is the sink outrunning its pool, a refused flush is
    // the pool outrunning the database, and the two want different responses.
    std::uint64_t                    refused_flushes_ = 0;
    // Flushes skipped because the pool was still full from the last refusal.
    std::uint64_t                    deferred_flushes_ = 0;
    // Reported at most once per flush rather than once per lost row. A log line
    // per occurrence is not a metric: under the load that makes it fire, a
    // per-event line is itself the outage (docs/00-architecture.md §9).
    std::uint64_t                    reported_drops_ = 0;
    std::int64_t                     timer_id_ = 0;
    bool                             stopped_ = false;
    // Set by a refused post, cleared by the first attempt after the pool has
    // room. See "the storm re-admission opened" in the header comment.
    bool                             flush_deferred_ = false;
};

}  // namespace anvil::audit
