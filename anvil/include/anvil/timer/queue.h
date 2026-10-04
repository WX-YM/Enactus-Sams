#pragma once

// The durable job queue (docs/10-timer-jobs.md).
//
// --- What belongs here, and what emphatically does not --------
//
// A scheduled state change that is a pure function of the clock is NOT a job.
// Publishing a note at 18:00, a shared draft link expiring, an event becoming
// live, a session expiring — all of those are computed at read time from a
// stored
// instant, and making any of them a job buys nothing while adding lost jobs on
// restart, duplicate fires, drift, cancellation bookkeeping and a recovery path
// after downtime. What is left is side effects that are idempotent and for which
// being a minute late is harmless: transport delivery, sweeps, purges, digests.
//
// --- Why Streams and not a ZSET --------------------------------
//
//   ZRANGEBYSCORE + ZREM is a race: two workers read the same due entry and both
//   execute before either removes it. A Lua claim fixes the race but not the
//   second problem — it removes the entry BEFORE the work runs, so a worker that
//   dies mid-execution takes the job with it, and for a nightly purge that is a
//   silent data-integrity gap.
//
// Redis Streams with a consumer group solve both. Delivery is at-least-once, an
// unacknowledged entry is reclaimable after a visibility timeout, XREADGROUP with
// BLOCK removes polling entirely (an idle worker burns zero CPU), and the pending
// entries list is real observability.
//
// Streams have no per-entry scheduling, so future-dated jobs live in a ZSET and a
// leased promoter moves due entries into the stream:
//
//     ZSET <p>:sched  --[promoter, every few seconds]-->  XADD <p>
//
// --- Redis is not the system of record (d) ----------------------
//
// Run with `appendonly yes, appendfsync everysec` or scheduled jobs vanish on a
// crash. Even then: anything that MUST happen records its intent in MongoDB and
// the Redis entry is only the delivery mechanism. That is what the notification
// outbox sweeper is — a reconciliation pass that re-enqueues intents with no
// completion record.
//
// --- Layout of a stored job -------------------------------------------------
//
// One binary field on the stream entry and one value in the payload hash, so
// there is no per-field key overhead and no parsing beyond a bounds check:
//
//     [ 0..15]  job id            16 B, UUIDv4 (a JobId is a handle, not a clock)
//     [16..23]  not_before_ms     int64 LE
//     [24..25]  kind             uint16 LE
//     [26..27]  args_len         uint16 LE
//     [28]      attempt          uint8, 1 on first delivery
//     [29]      version          uint8
//     [30..31]  reserved         zero
//     [32..47]  linked_trace     16 B, VERSION 2 ONLY — the trace that enqueued
//     [32.. ] or [48.. ]  args   <= 4096 B, never a pointer, never address-space
//                                dependent (docs/10-timer-jobs.md §2)
//
// --- two versions, and why the old one is still written ---------------------
//
// Version 2 exists to carry the link and nothing else, so it is written ONLY
// when there is a link to carry. A deployment that does not trace produces the
// same 32-byte envelope it always did, byte for byte.
//
// That is not a size optimisation, it is the rollout. An older worker rejects an
// unknown version, and a rejected envelope is ACKed and dropped — a LOST JOB, not
// a retried one. So a mixed fleet must not see a version half of it cannot read,
// and the only thing that produces one is a traced enqueue. Trace ingest is off
// by default and turning it on needs a parameter that only a build with this
// decoder has (`http/request_scope.h`), so the ordering enforces itself for a
// single-binary deployment: upgrade, then enable. A fleet running two builds at
// once must finish the upgrade BEFORE enabling ingest.
//
// --- Threading --------------------------------------------------------------
//
// Every method here BLOCKS on Redis and must never be called from a Trantor
// event-loop thread. The claimer threads are dedicated and separate from the
// pools: a thread parked in XREADGROUP BLOCK for five seconds would occupy a
// bounded pool slot for five seconds while doing nothing. Handlers run on
// db_pool or cpu_pool by workload class, never on the thread that claimed them
// (docs/10-timer-jobs.md §7).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/timer/job_spec.h"
#include "anvil/db/codec.h"
#include "anvil/timer/registry.h"

namespace anvil::timer {

// A JobId is a handle used to cancel, and it appears in log lines. UUIDv4 rather
// than v7: it carries no meaning, and a time-ordered id here would leak nothing
// useful while inviting a reader to treat it as an ordering.
using JobId = Uuid;

inline constexpr std::size_t kMaxJobArgsBytes = 4096;

// Two header sizes, one per version, and no constant meaning "the header size".
// A name that used to mean one thing and now means one of two is the trap
// `core/user_context.h` records under a different name — code written against it
// keeps compiling and reads the wrong offset.
inline constexpr std::size_t kJobHeaderBytesUnlinked = 32;
inline constexpr std::size_t kJobHeaderBytesLinked = 48;

inline constexpr std::uint8_t kJobEnvelopeVersionUnlinked = 1;
inline constexpr std::uint8_t kJobEnvelopeVersionLinked = 2;

// How many header bytes precede the args in an envelope of this version. Zero
// for a version this build does not know, which every caller treats as a refusal.
[[nodiscard]] constexpr std::size_t job_header_bytes(std::uint8_t version) noexcept {
    if (version == kJobEnvelopeVersionUnlinked) { return kJobHeaderBytesUnlinked; }
    if (version == kJobEnvelopeVersionLinked) { return kJobHeaderBytesLinked; }
    return 0;
}

// The stream is allowed to grow while consumers fall behind — dropping a job to
// bound memory would be strictly worse than the backpressure. Instead the
// promoter stops promoting above this depth, leaving due entries in the ZSET,
// which is where they are durable anyway. 10 000 jobs due in one second
// therefore drain in batches rather than materialising at once (docs/10-timer-jobs.md §6).
inline constexpr std::uint64_t kStreamHighWater = 20000;

// Ordered largest-alignment-first, no interior padding (CLAUDE.md §2.3).
struct JobHeader final {
    Uuid          id;           // 16
    db::TimeMs    not_before;   //  8
    std::uint16_t kind;         //  2  index into the application job table
    std::uint16_t args_len;     //  2
    std::uint8_t  attempt;      //  1
    // An OUTPUT of decode_header, not an input to encode_envelope. The writer
    // derives the version from whether there is a link, because the two must
    // never disagree: a header claiming version 1 with a link in it would be
    // written as 32 bytes and read back with the link gone.
    std::uint8_t  version;      //  1
    std::array<std::uint8_t, 2> reserved;   // keeps sizeof stable across additions
    // The trace that enqueued this job, all-zero when there was none.
    //
    // A LINK, and never a parent. A job is at-least-once, may be reclaimed after
    // a lease expiry, and may run days after it was enqueued; a span covering the
    // publish and Thursday's retry is not a trace of anything. So each execution
    // mints a new ROOT and names this one beside it. Getting that backwards is
    // the single most common mistake in traced job systems, which is why it is
    // written here rather than left to the next reader of `dispatch`.
    std::array<std::uint8_t, 16> linked_trace;  // 16
};
static_assert(sizeof(JobHeader) == 48);
static_assert(std::is_trivially_copyable_v<JobHeader>);

// Byte-exact and explicit, never a memcpy of the struct: a struct copy would
// commit this wire format to the compiler's padding and to the host's byte order,
// and an entry written by one build would decode differently in another.
void encode_envelope(const JobHeader& header, std::span<const std::uint8_t> args,
                     std::string& out);

// nullopt on a truncated envelope, an unknown version, or an args_len that
// disagrees with the actual size. A job blob is data from another process — it
// gets the same treatment a request body would.
[[nodiscard]] std::optional<JobHeader> decode_header(std::string_view envelope) noexcept;

// The args region of a validated envelope. Borrows; valid while `envelope` is.
[[nodiscard]] std::span<const std::uint8_t> envelope_args(std::string_view envelope,
                                                          const JobHeader& header) noexcept;

// One claimed entry. `envelope` is owned because redis-plus-plus materialises
// reply values as std::string and moving one is free; holding a view into the
// driver's reply would dangle the moment the reply is released.
struct ClaimedJob final {
    std::string stream_id;   // the Redis entry id — the XACK/XDEL key
    std::string envelope;
    JobHeader   header;
};

struct QueueStats final {
    std::uint64_t stream_length;
    std::uint64_t pending;         // claimed but not acknowledged
    std::uint64_t scheduled;       // future-dated, in the ZSET
    std::uint64_t dead_lettered;
};

// Recurrences are DECLARED, not scheduled by a timer.
//
// An in-process timer does not survive SIGTERM and fires N times with N
// instances. A declaration fires once per UTC bucket across the whole deployment,
// with jitter, and a crashed worker's claim is reclaimed rather than lost.
//
// EVERYTHING HERE IS UTC, and the bucket arithmetic below has no timezone in it
// at all — which is the only way to be sure. A recurrence expressed in a local
// zone runs twice, or not at all, on a DST transition day.
//
// A cron expression was the obvious spelling and is not used: it is a parser, the
// only thing it would ever parse is a compile-time constant, and a parser on a
// scheduling path is a liability for no benefit. Period plus offset is the same
// declaration with no parser, and it is arithmetic the compiler can check.
//
// RecurringSpec is in anvil/timer/job_spec.h; the TABLE is the application's and
// lives beside its job table (docs/01-seams.md §7).

// Which period slot `now` falls in. Pure integer arithmetic on the Unix epoch:
// no timezone, no calendar, no DST.
[[nodiscard]] constexpr std::int64_t recurrence_bucket(std::int64_t now_ms,
                                                       const RecurringSpec& spec) noexcept {
    const std::int64_t period_ms = spec.period.count() * 1000;
    // Floor division, so instants before the epoch (a badly set clock) still map
    // monotonically instead of folding two buckets into one.
    const std::int64_t quotient = now_ms / period_ms;
    return (now_ms % period_ms < 0) ? quotient - 1 : quotient;
}

[[nodiscard]] constexpr std::int64_t recurrence_due_ms(std::int64_t bucket,
                                                       const RecurringSpec& spec) noexcept {
    return (bucket * spec.period.count() * 1000) + (spec.offset_in_period.count() * 1000);
}

// The well-formedness of the application's recurrence table — that every offset
// and jitter fits inside its period, and that every declaration names a job kind
// that exists — is asserted in anvil/timer/registry.h, over the whole table at
// once. The original spelled one assert per index, which is a line somebody has
// to remember to add.

struct JobQueueConfig final {
    // Every key this queue owns starts with it. Production uses "jobs"; a test
    // uses a unique prefix so concurrent suites cannot see each other's entries.
    std::string prefix;
    // Identifies this process in the consumer group, so the pending list
    // attributes a stalled entry to a host rather than to "somebody".
    std::string consumer;
    // How long a claimer parks in XREADGROUP. Longer means fewer round trips and
    // a slower response to stop(); it does not affect latency, because the read
    // returns the instant an entry arrives.
    std::chrono::milliseconds block{std::chrono::seconds{5}};
    std::chrono::milliseconds promoter_interval{std::chrono::seconds{5}};
    // The promoter's lease. Renewed by a heartbeat rather than set to the
    // expected duration up front (docs/10-timer-jobs.md §5).
    std::chrono::seconds      lease_ttl{30};
    std::uint32_t             batch{16};
    std::size_t               claimers{1};
};

class JobQueue final {
public:
    explicit JobQueue(JobQueueConfig config);
    ~JobQueue();

    // --- the interface a domain API sees (docs/10-timer-jobs.md §7) ---------------------

    // A past `when` runs immediately; it is NOT an error. Clock skew across
    // instances makes rejection a source of spurious failures (docs/10-timer-jobs.md §7).
    //
    // `idempotency_key` makes double-scheduling safe: two callers passing the
    // same key inside the dedupe window get the same JobId back and exactly one
    // entry exists. It is required, not optional — every queue in this system is
    // at-least-once, so the caller that schedules is itself retryable.
    [[nodiscard]] Result<JobId> schedule_at(std::uint16_t kind, std::span<const std::uint8_t> args,
                                           db::TimeMs when, std::string_view idempotency_key);
    [[nodiscard]] Result<JobId> schedule_in(std::uint16_t kind, std::span<const std::uint8_t> args,
                                           std::chrono::seconds delay,
                                           std::string_view idempotency_key);

    // Best-effort BY CONSTRUCTION: an entry already promoted into the stream, or
    // already executing, cannot be recalled. Handlers must therefore re-check
    // their preconditions at execution time. Returns true only
    // when a scheduled entry was actually removed.
    [[nodiscard]] bool cancel(const JobId& id) noexcept;

    // --- the mechanism -----------------------------------------------------
    //
    // Exposed rather than buried in the worker loop so that the properties that
    // matter can be tested directly: that two workers claiming one due job get
    // it once between them, that an unacknowledged entry is reclaimable, that a
    // failing job backs off and then dead-letters. A test that had to race a
    // live worker to observe any of that would be a flaky test.

    // XGROUP CREATE with MKSTREAM. Idempotent, and safe from N instances at
    // once — BUSYGROUP is the normal case, not an error.
    [[nodiscard]] Status ensure_group();

    // Moves due ZSET entries into the stream, atomically per entry. Returns how
    // many moved; zero when nothing is due or when the stream is above its high
    // water mark.
    [[nodiscard]] Result<std::size_t> promote_due(db::TimeMs now, std::uint32_t limit);

    // Ensures each declared recurrence has an entry for its current bucket.
    // Idempotent through the recurrence's own idempotency key, so N instances
    // converge on one entry per bucket without coordination.
    [[nodiscard]] Result<std::size_t> ensure_recurring(db::TimeMs now);

    // Blocks up to `block` waiting for new entries. Zero CPU while parked.
    [[nodiscard]] Result<std::vector<ClaimedJob>> claim(std::chrono::milliseconds block,
                                                        std::uint32_t limit);

    // Entries claimed by a worker that never acknowledged them — a killed
    // process, or one whose pool task was lost. This is what stops a job dying
    // with the worker that claimed it (c).
    [[nodiscard]] Result<std::vector<ClaimedJob>> reclaim_stalled(
        std::chrono::milliseconds min_idle, std::uint32_t limit);

    // Runs the handler on the CALLING thread, so the caller decides which pool
    // pays for it. Never throws: a handler that manages to is reported as a
    // retryable failure rather than taking the process down.
    [[nodiscard]] JobOutcome dispatch(const ClaimedJob& job, mongocxx::client& client) const noexcept;

    // Acknowledge, retry with backoff, or dead-letter — whichever the outcome and
    // the attempt budget imply. One Lua script per branch, so a crash cannot
    // leave a job both acknowledged and unscheduled.
    [[nodiscard]] Status complete(const ClaimedJob& job, JobOutcome outcome, db::TimeMs now);

    // --- lifecycle ---------------------------------------------------------
    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] Result<QueueStats> stats() const;

    // Deletes every key this queue owns. An operator tool for a poisoned queue,
    // and what a test calls to leave Redis as it found it. Never called from a
    // request path.
    [[nodiscard]] Status purge() noexcept;

    // Key names, exposed so an operator runbook and a test can name the same
    // strings the code does rather than reconstructing them.
    [[nodiscard]] const std::string& stream_key() const noexcept { return stream_key_; }
    [[nodiscard]] const std::string& schedule_key() const noexcept { return schedule_key_; }
    [[nodiscard]] const std::string& payload_key() const noexcept { return payload_key_; }
    [[nodiscard]] const std::string& dead_key() const noexcept { return dead_key_; }
    [[nodiscard]] const std::string& lease_key() const noexcept { return lease_key_; }

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

private:
    // Acknowledge and delete an entry without any retry bookkeeping. For entries
    // that cannot be dispatched at all — a missing payload field, an envelope
    // that fails to decode — which have nothing to retry INTO and would
    // otherwise sit in the pending list being reclaimed forever.
    [[nodiscard]] Status ack_only(std::string_view stream_id) noexcept;

    // schedule_at with the dedupe window under the caller's control. Recurrences
    // need a window that outlives their own period: a daily job whose dedupe key
    // expires six hours in would be scheduled again inside the same day and would
    // run twice. Private because the window is mechanism, not something a domain
    // API should have to reason about.
    [[nodiscard]] Result<JobId> schedule_with_dedupe_ttl(std::uint16_t kind,
                                                         std::span<const std::uint8_t> args,
                                                         db::TimeMs when,
                                                         std::string_view dedupe_key,
                                                         std::chrono::seconds dedupe_ttl);

    void claimer_loop(std::size_t index) noexcept;
    void promoter_loop() noexcept;
    // Posts one claimed job to its kind's pool. Returns false when the pool is
    // saturated, in which case the entry is deliberately left unacknowledged: it
    // stays pending and is reclaimed later, which is the correct backpressure —
    // acknowledging work that was never done would lose it.
    [[nodiscard]] bool post_to_pool(ClaimedJob job);

    // The promoter's lease. Acquire is SET NX PX; renew is a Lua
    // compare-and-extend and release a Lua compare-and-delete, because a plain
    // DEL can release a lock that already expired and was re-acquired by someone
    // else (docs/10-timer-jobs.md §5). Never relied on for correctness — the promote script is
    // atomic on its own, so two promoters produce one stream entry per job
    // anyway. The lease only stops the duplicated work.
    [[nodiscard]] bool hold_lease() noexcept;
    void release_lease() noexcept;

    // Declaration order is construction order: every key is built from config_.
    const JobQueueConfig config_;
    const std::string    stream_key_;
    const std::string    schedule_key_;
    const std::string    payload_key_;
    const std::string    dead_key_;
    const std::string    lease_key_;
    const std::string    idem_prefix_;
    const std::string    group_;
    const std::string    lease_token_;
    std::atomic<bool>    running_;
    std::atomic<bool>    leased_;
    // The promoter ticks on an interval, so it waits on a condition rather than
    // sleeping: stop() then returns in microseconds instead of holding a
    // SIGTERM drain open for a whole promoter interval.
    mutable std::mutex      wake_mutex_;
    std::condition_variable wake_;
    std::thread          promoter_;
    std::vector<std::thread> claimers_;   // LAST: spawned after everything above
};

}  // namespace anvil::timer
