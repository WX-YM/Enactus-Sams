#pragma once

// The event sink: the one entry point, and everything that decides whether a
// moment becomes a row.
//
// Four gates, in this order, and the order is the design:
//
//   1. CONSENT, at the door. An event declaring requires_consent is not recorded
//      without it — not recorded and then filtered, not recorded
//      pseudonymously. "Recorded and then excluded from queries" is a policy one
//      forgotten $match away from being no policy at all
//      (docs/17-analytics.md §12).
//   2. A VISITOR. The packed address never reaches a row; what identifies a
//      visitor is the peppered, day-rotating digest in
//      anvil/analytics/sessions.h. A deployment with no pepper installed records
//      nothing rather than recording something reversible.
//   3. SAMPLING, above a high-water mark, and DETERMINISTIC PER SESSION. A
//      session is kept whole or dropped whole. Per-event sampling keeps a random
//      half of every session, and a half-observed funnel is worse than an
//      unobserved one: it reports a drop-off that is an artefact of the sampler,
//      and there is no way to tell it from a real one afterwards
//      (docs/17-analytics.md §13).
//   4. The BUFFER's classify-and-shed policy (anvil/analytics/buffer.h).
//
// Everything after that is batching: one insert_many per flush interval on
// analytics_pool, never one insert per event. Batching is the difference between
// analytics costing a round trip per request and costing a round trip per
// second.
//
// Like AuditService, every write here is FIRE AND FORGET and always
// ASYNCHRONOUS: offer() is called from a Trantor event-loop thread and nothing
// blocking may run there (ENGINEERING_RULES.md §4).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/analytics/buffer.h"
#include "anvil/analytics/event.h"
#include "anvil/analytics/event_spec.h"
#include "anvil/analytics/repository.h"
#include "anvil/analytics/sessions.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/http/client_address.h"

namespace anvil::analytics {

// What a caller hands the sink. Assembled on a request path, so it is trivially
// copyable and carries no allocation.
//
// Ordered largest-alignment-first (ENGINEERING_RULES.md §2.3). Only `code` needs more than
// byte alignment, so it goes first and everything else packs behind it: with the
// address leading instead, `code` lands mid-struct and the padding in front of it
// costs four bytes on every request.
struct Offer final {
    EventCode           code;         //  4
    DimensionValues     dimensions;   //  4
    http::PackedAddress address;      // 16
    std::optional<Uuid> subject;      // 17
    // Whether this visitor has consented. Supplied by the application, because
    // where consent is recorded and what it covers is a product decision anvil
    // cannot infer.
    bool                consented;    //  1
};

static_assert(sizeof(Offer) == 44, "Offer must not grow padding");
static_assert(std::is_trivially_copyable_v<Offer>);

struct IngestConfig final {
    // How long a raw row lives. A TTL index expires it; every read also filters
    // on the field, because the monitor lags by up to a minute.
    std::chrono::seconds retention{std::chrono::hours{24 * 90}};
    // ANALYTICS_SAMPLE_DENOMINATOR. 1 keeps everything, which is the correct
    // default: sampling is a pressure valve, not a policy.
    std::uint32_t        sample_denominator = 1;
    std::size_t          capacity_rows = EventBuffer::kDefaultCapacityRows;
    // Sampling engages only ABOVE this many buffered rows. Below it the sink is
    // keeping up and there is nothing to trade away.
    std::size_t          high_water_rows = EventBuffer::kDefaultCapacityRows / 2;
};

class EventSink final {
public:
    // A flush is one insert_many, so this is the number of rows one pooled task
    // covers.
    static constexpr std::size_t kBatchRows = 512;
    // A quiet deployment must not hold the last few rows indefinitely, and this
    // is also the coalescer's window: a fold never outlives it.
    static constexpr std::chrono::milliseconds kFlushInterval{1000};

    // What offer() did. Returned rather than logged: the caller is on a loop
    // thread, and a line per occurrence is the outage under the load that makes
    // it fire (docs/00-architecture.md §9).
    enum class Outcome : std::uint8_t {
        Recorded,
        // The event declares requires_consent and the offer carried none.
        RefusedConsent,
        // No visitor pepper installed, so no visitor could be derived. The row
        // is refused rather than recorded against an unkeyed digest.
        NoVisitor,
        // Above the high-water mark and this session is not in the kept
        // fraction. Counted, so the true rate is recoverable by multiplication.
        SampledOut,
        // The buffer refused it. Counted against its own class.
        Dropped,
        // stop() has run.
        Stopped,
    };

    // `databases` resolves each collection to the database it was declared in.
    // The events and the sessions may be in a different one from the rollups,
    // and a sink that assumed otherwise would write rows nothing ever reads.
    EventSink(const db::DatabaseNames& databases, const AnalyticsCollections& collections,
              std::span<const EventSpec> events, IngestConfig config);
    ~EventSink();

    // Installs the periodic flush on the event loop. Called from main() as
    // beginning advice: getLoop() before run() has no loop to attach a timer to.
    void start();

    // Flushes what is buffered — INCLUDING the coalescer's open window — on the
    // CALLING thread, and stops accepting new rows. Called during shutdown
    // BEFORE the pools drain.
    //
    // It also takes out the timer start() installed, which holds `this`. Call it
    // from the LOOP THREAD, or before the loop runs: Trantor's invalidateTimer
    // is synchronous only there, and queued from anywhere else — which leaves a
    // window in which the loop can fire the timer one more time. Drogon's
    // termination advice already runs on the loop thread, which is where this
    // belongs.
    void stop() noexcept;

    // Buffers the row and returns. Never blocks on the database, never throws.
    [[nodiscard]] Outcome offer(const Offer& offer) noexcept;

    // Drains and writes synchronously on the CALLING thread. Blocking, so it
    // belongs on a pool thread or in a test.
    [[nodiscard]] Status flush_now(mongocxx::client& client);

    [[nodiscard]] std::size_t buffered() const noexcept;
    [[nodiscard]] std::uint64_t dropped_behaviour() const noexcept;
    [[nodiscard]] std::uint64_t dropped_conversions() const noexcept;
    [[nodiscard]] std::uint64_t sampled_out() const noexcept;
    // Batches the pool refused and this sink therefore re-admitted. The number
    // the audit sink's shape loses silently.
    [[nodiscard]] std::uint64_t refused_flushes() const noexcept;

    // Flushes this sink did not ATTEMPT because analytics_pool was known full.
    //
    // A refused flush returns its rows to the buffer and `offer` posts one the
    // moment the buffer crosses kBatchRows, so with a queue that stays full
    // every subsequent event would drain 512 rows, copy them into a task, be
    // refused, and put them back — with a LOG_WARN each time, which is the
    // shape docs/00-architecture.md §9 forbids by name. A refusal therefore arms
    // a latch and the attempt is skipped for as long as the pool reports itself
    // saturated: `try_post` is the authority that tells us, `saturated()` is the
    // advisory that stops us asking again, and the rows wait in a buffer that is
    // already bounded and already sheds by class (anvil/audit/service.h carries
    // the whole account of it).
    //
    // No metric of its own: it is a function of
    // `anvil_pool_queue_depth{pool="analytics"}`, which is already sampled.
    [[nodiscard]] std::uint64_t deferred_flushes() const noexcept;

    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;

private:
    // The visitor digest is uniformly distributed, so its first eight bytes are
    // already a hash. Rehashing them would cost a multiply to gain nothing.
    struct VisitorHash final {
        [[nodiscard]] std::size_t operator()(const VisitorId& id) const noexcept;
    };

    std::size_t flush_locked(std::vector<EventRow>& out,
                             std::vector<VisitorId>& sessions);
    void        post(std::vector<EventRow> batch, std::vector<VisitorId> sessions) noexcept;

    // STATIC, and it takes copies of the two repositories rather than reading
    // them off `this`.
    //
    // The pooled task cannot capture `this`: a task already accepted by
    // analytics_pool runs after post() returns, and there is no ordering rule
    // that keeps a sink alive until then — which the load gate caught as a
    // stack-use-after-return the first time it drove one hard enough to leave a
    // flush in flight. A repository is a string and a view, so copying one into
    // the task is cheaper than the ordering rule would have been to enforce
    // (ENGINEERING_RULES.md §3.3).
    [[nodiscard]] static Status write_batch(mongocxx::client& client,
                                            const EventRepository& events,
                                            const SessionRepository& sessions_repo,
                                            std::chrono::seconds retention,
                                            std::span<const EventRow> batch,
                                            std::span<const VisitorId> sessions);

    // Declaration order is construction order: buffered_ is built from events_
    // and config_.
    const IngestConfig         config_;
    std::span<const EventSpec> events_;
    EventRepository            events_repo_;
    SessionRepository          sessions_repo_;
    mutable std::mutex         mutex_;
    // Guarded by mutex_, including its counters — so "how many were lost" is one
    // reading taken with the buffer that lost them rather than two that can
    // disagree.
    EventBuffer                buffered_;
    // The visitors seen since the last flush, deduplicated. One upsert each per
    // flush rather than one per event: the session row is the same row for every
    // event a visitor sends in a day, and writing it per event would put a round
    // trip back on the path batching exists to take it off.
    std::unordered_set<VisitorId, VisitorHash> pending_sessions_;
    std::uint64_t              sampled_out_ = 0;
    std::uint64_t              refused_flushes_ = 0;
    std::uint64_t              deferred_flushes_ = 0;
    std::int64_t               timer_id_ = 0;
    bool                       stopped_ = false;
    // Set by a refused post, cleared by the first attempt after the pool has
    // room. See deferred_flushes().
    bool                       flush_deferred_ = false;
};

}  // namespace anvil::analytics
