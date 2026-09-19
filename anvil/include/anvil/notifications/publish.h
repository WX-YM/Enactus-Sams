#pragma once

// Publish: the only write path into the notification subsystem.
//
// --- the inbox is the system of record, every transport is best-effort -------
//
// The canonical row is committed FIRST and the transports are enqueued after it.
// The reverse order — mail first, then record — loses the notification entirely
// whenever the process dies in between, and the reader has no way to discover
// that anything was meant to reach them. This way round the worst case is a
// notification that is in the inbox and was never mailed, which a sweeper can
// find and finish.
//
// That is the whole reason `dispatched_at` exists and is written LAST:
//
//     insert/coalesce  ->  fan out inbox rows  ->  enqueue transports  ->  mark
//                     ^                                                    |
//                     |                                                    v
//                     +--- sweep_outbox() re-runs everything after a crash -+
//
// Every step between the insert and the mark is idempotent, which is what makes
// re-running the whole tail safe rather than merely tolerable: the fan-out is
// idempotent through the `{uid, nid}` unique index, the transport enqueue through
// the job queue's idempotency key, and the mark is a `$set` of a constant.
//
// --- the audience is DERIVED, never carried ---------------------------------
//
// A publish does not name its recipients. It cannot: the sweeper that re-runs a
// dispatch after a crash is a different process that has only the stored row, so
// any audience the caller passed in would be gone exactly when it is needed. The
// audience is therefore a function of state that IS stored —
//
//     Scope::Account            the subject, which is the reader's own id
//     Scope::Global, Resource   the owners of the in-app clients subscribed to
//                               (kind, subject)
//
// — which makes the subscription the single source of truth for who receives
// something, and makes a replayed dispatch produce the same audience as the
// original by construction.
//
// --- what publishing refuses ------------------------------------------------
//
// Every check below is a specific failure that would otherwise ship:
//
//   UNKNOWN TOPIC OR TEMPLATE. A code this build does not declare is refused
//   rather than stored, because a stored row outlives the deploy that wrote it
//   and nothing downstream could render it.
//   SCOPE DISAGREEMENT. A Scope::Account topic published with no subject would
//   fan out to nobody; a Scope::Global topic published WITH one would write a
//   row no subscriber's `$or` branch matches. Both look like delivery and are
//   silence.
//   A MISSING PARAMETER. The renderer degrades a missing placeholder to nothing,
//   which is right for a row an older build wrote and wrong for a row being
//   written now — here it is a sentence with a hole in it and it is refused.
//   AN UNUSABLE PARAMETER. Checked at publish as well as at render, so a value
//   that can never be rendered is never stored.
//
// --- threading --------------------------------------------------------------
//
// Every method here BLOCKS on MongoDB and must never be called from a Trantor
// event-loop thread (ENGINEERING_RULES.md §4). db_pool.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/notifications/record.h"
#include "anvil/notifications/repository.h"
#include "anvil/notifications/shedding.h"
#include "anvil/notifications/template_spec.h"
#include "anvil/notifications/topic_spec.h"

namespace anvil::notifications {

// Does this account still hold the topic's required permissions?
//
// anvil cannot answer it. Where permissions live, and whether a "user" here is an
// account or a staff member, is the application's — and the question has to be
// asked at DISPATCH rather than trusted from publish time, because a staff member
// who lost the bit between the two must not receive the row.
//
// BLOCKING: db_pool. Called once per candidate recipient, so an implementation
// that issues a query per call is an N+1 on the fan-out path; the reference
// implementation reads the permission bits it already caches.
//
// Absent means the topic table declares no permission-gated topic. A topic with a
// non-empty `required` and no probe to ask is a configuration mistake and is
// refused rather than delivered — the failure is silence for a topic somebody
// gated deliberately, which is the correct direction.
using RecipientProbe =
    std::function<Result<bool>(mongocxx::client& client, const Uuid& user,
                               const PermSet& required)>;

// Enqueue the outbound transports for a committed notification.
//
// ONE job for the whole notification, never one per subscriber: a broadcast to
// 20 000 endpoints must not become 20 000 queue entries, which is the same write
// storm fan-out-on-read exists to avoid. The sender pages the subscribers itself
// (anvil/notifications/outbound.h).
//
// `channels` is the mask the publish asked for, read back off the row — so a
// replayed dispatch enqueues exactly what the original asked for and never the
// topic's wider default.
//
// BLOCKING: it writes to Redis. Must be idempotent, because the sweeper re-runs
// it: the job queue's idempotency key derived from the notification id is what
// makes a second enqueue a no-op.
using TransportEnqueue = std::function<Status(const Uuid& notification, ChannelMask channels)>;

// Queue pressure as a fraction of the queue's capacity — `queue_depth()` over
// `queue_capacity()` for whichever bounded pool this publish is competing for,
// which for the reference deployment is `db_pool`.
//
// SAMPLED BY THE CALLER, so `shed_verdict` stays pure and has nothing to reach
// for (anvil/notifications/shedding.h). Sampled ONCE per publish, and the one
// reading decides both gates: two samples a few microseconds apart could
// disagree, and a publish that was admitted under one number and deferred under
// another is a decision nobody made.
//
// ABSENT means no shedding at all — no sample, no verdict, no counter — which is
// the configuration every consumer that predates the storm breaker already has.
// A probe that THROWS is treated exactly as a NaN pressure is: proceed. The safe
// answer to a number nobody can read is never to throw a notification away, and
// it is deliberately not logged, because under the storm that makes a probe
// misbehave a log line per publish is itself the outage
// (docs/00-architecture.md §9).
using PressureProbe = std::function<float()>;

struct PublishHooks final {
    RecipientProbe   may_receive;
    TransportEnqueue enqueue_transports;
    PressureProbe    pressure;
};

// What a caller asks to be published.
//
// No recipient list, deliberately — see the header comment. Ordered
// largest-alignment-first so the struct packs (ENGINEERING_RULES.md §2.3).
struct PublishRequest final {
    TopicRef                   topic;
    // Borrowed for the duration of the call and copied into the row's owned
    // StoredParams before anything crosses a pool boundary (ENGINEERING_RULES.md §2.2).
    std::span<const Param>     params;
    // REQUIRED, and it is what makes a retried publish a no-op rather than a
    // second notification. Every queue in this system is at-least-once, so the
    // caller that publishes is itself being retried; an optional idempotency key
    // is one nobody passes until after the duplicate reaches somebody's inbox.
    //
    // For a coalescing topic it is ignored — see `dedupe_key`.
    std::string_view           idempotency_key;
    std::optional<ResourceRef> ref;
    std::optional<Uuid>        actor;
    TemplateId                 tpl;
    // kDefaultChannels means "whatever the topic declares". Narrow it only to
    // take a channel AWAY from one publish; widening past the topic's declaration
    // is not possible, because `should_deliver` intersects with it.
    ChannelMask                channels{kDefaultChannels};
};

struct PublishOutcome final {
    Uuid         id;
    // Inbox rows NEWLY created by this dispatch. Zero for a broadcast topic,
    // which writes none, and zero for a replayed dispatch whose rows all exist.
    std::int64_t delivered;
    // Coalesced repeats, including this one. Always 1 for a topic that does not
    // coalesce.
    std::int32_t count;
    // False when the dedupe key already existed: a retried publish, or a repeat
    // inside a coalescing window.
    bool         created;
};

// How long after a notification is written the sweeper will consider it stranded.
//
// It is not zero, and the reason is a race rather than caution: a publish that has
// committed its row and is three instructions from enqueuing its transports is
// indistinguishable from one whose process died there. Sweeping immediately would
// duplicate the work of every in-flight publish in the deployment — harmless,
// because the tail is idempotent, and a pointless doubling of the fan-out load
// under exactly the traffic that can least afford it.
//
// Measured against the row's UUIDv7 `_id` rather than its `created_at`: the
// leading 48 bits of a v7 id ARE the creation millisecond, so an upper bound on
// the id is an upper bound on age answered by the index the sweep already walks,
// with no second field in the filter.
inline constexpr std::chrono::seconds kOutboxGrace{60};

// Per round trip, not in total. An audience is paged at this width so a topic
// with thousands of subscribers is many bounded reads rather than one unbounded
// one (ENGINEERING_RULES.md §7); the TOTAL is bounded only by the subscription count, which
// is the application's declared decision when it marks a topic FanOut::Write.
inline constexpr std::int32_t kSubscriberPage = 256;

// The ceiling on the outbox-depth gauge, so sampling it costs an index walk of a
// known length rather than one that grows with the backlog — the same shape as
// kUnreadCountLimit, and for the same reason.
//
// A reported value AT the cap means "at least this many", not that number. That
// is enough, because this gauge is an ALARM and not a measurement: a healthy
// deployment reads zero here, and anything that has reached four figures is
// already an incident whose exact size does not change what anybody does next.
inline constexpr std::int64_t kOutboxDepthCap = 10000;

class PublishService final {
public:
    // `templates` and the repository's topic table are views of the application's
    // `constexpr` tables and outlive the service. Held as spans rather than
    // copied: the tables are in `.rodata`, shared across every thread and every
    // request, and costing nothing to consult (ENGINEERING_RULES.md §2.1).
    PublishService(const NotificationRepository& repository,
                   std::span<const TemplateSpec> templates, PublishHooks hooks,
                   ShedPolicy shedding = {}) noexcept
        : repository_{repository},
          templates_{templates},
          hooks_{std::move(hooks)},
          shedding_{shedding} {}

    // Write the row, then run the dispatch tail. A failure in the tail is NOT a
    // failed publish: the row is committed and the sweeper will finish it, so the
    // outcome comes back with `delivered` counting only what this call managed.
    //
    // BOTH STORM-BREAKER GATES ARE HERE, and only here
    // (anvil/notifications/shedding.h). Pressure is sampled once, before
    // anything is written, and the one verdict decides both:
    //
    //   Drop           nothing is written and this returns ServiceUnavailable —
    //                  "shed deliberately, may be retried", which is what that
    //                  code is for. `user_optional` topics only, at any pressure.
    //   DeferDispatch  the row is committed and the TAIL is skipped, leaving
    //                  `dispatched_at` unset. That is byte-for-byte the state a
    //                  process killed between commit and enqueue leaves, and
    //                  sweep_outbox() already finishes exactly it. The outcome
    //                  comes back with `delivered == 0`, which a caller must not
    //                  read as "nobody was reached" — it means "not yet".
    [[nodiscard]] Result<PublishOutcome> publish(mongocxx::client& client,
                                                 const PublishRequest& request,
                                                 db::TimeMs now) const;

    // The transactional overload. It writes the row inside the caller's
    // transaction and DOES NOT dispatch — it cannot, because the row is not
    // visible to another process until the commit, and a transport that fired for
    // a transaction that then aborted is a notification about something that never
    // happened. The caller dispatches after the commit, or the sweeper does.
    //
    // This is the overload a caller uses when the notification must not outlive
    // the row it is about: "your form was submitted" published for a submission
    // that failed to save is worse than no notification at all.
    //
    // NOT SHED, deliberately, and the reason is arithmetic rather than caution:
    // admission control that runs after the caller already holds a transaction
    // and a pooled client has nothing left to save. The pressure has been paid.
    // Refusing here would add a failure mode to a transaction in flight and buy
    // back only the row write it was about to do — while the dispatch gate has
    // nothing to skip, because this overload never dispatches.
    [[nodiscard]] Result<PublishOutcome> publish(mongocxx::client& client,
                                                 mongocxx::client_session& session,
                                                 const PublishRequest& request,
                                                 db::TimeMs now) const;

    // Fan out, enqueue, mark — the idempotent tail, exposed because the sweeper
    // and the post-commit caller both run exactly it. Returns how many inbox rows
    // were NEW, which is zero for a replay and zero for a broadcast topic.
    //
    // A notification that is gone — expired between the read and this call — is
    // not an error: there is nothing to dispatch and nothing to retry.
    [[nodiscard]] Result<std::int64_t> dispatch(mongocxx::client& client, const Uuid& id,
                                                db::TimeMs now) const;

    // The reconciliation pass. Redis is not the system of record (docs/10 §"Redis
    // is not the system of record"): anything that must happen records its intent
    // in MongoDB, and this is the pass that finds intents with no completion.
    //
    // Returns how many rows it dispatched. A row whose dispatch fails is left
    // undispatched deliberately, so the next sweep sees it again — the alternative
    // is marking work that was never done.
    [[nodiscard]] Result<std::size_t> sweep_outbox(mongocxx::client& client, db::TimeMs now,
                                                   std::int32_t limit,
                                                   std::chrono::seconds grace = kOutboxGrace) const;

    // Samples anvil_notifications_outbox_rows: how much work the sweeper has
    // waiting, capped at kOutboxDepthCap.
    //
    // It is the gauge the dispatch gate makes necessary. That gate converts
    // queue pressure into outbox backlog — the correct trade, because a backlog
    // is durable and bounded by disk rather than by RSS — but a backlog growing
    // faster than the sweeper drains it is the failure mode it introduces, and
    // it is invisible without this.
    //
    // BLOCKING, and one count per call. Call it from the same recurring job that
    // runs the sweep, on db_pool, NEVER from a scrape: a sampler that queries the
    // database turns every scrape into a load test that fires every fifteen
    // seconds (anvil/analytics/gauges.h).
    [[nodiscard]] Status sample_outbox_depth(mongocxx::client& client, db::TimeMs now,
                                             std::chrono::seconds grace = kOutboxGrace) const;

    // The dedupe key, exposed so a test and an operator can compute the same 16
    // bytes the service will rather than reconstructing the derivation.
    //
    // TWO DOMAINS, separated by a prefix so a coalescing key can never collide
    // with an idempotency key:
    //
    //   coalescing      H("c" ‖ kind ‖ subject ‖ tpl ‖ window bucket)
    //   otherwise       H("i" ‖ kind ‖ subject ‖ idempotency key)
    //
    // The coalescing key deliberately identifies the EVENT CLASS and not the event
    // instance — that is what makes three form submissions inside the window
    // become one row saying three. The cost is stated rather than hidden: a
    // retried publish inside a coalescing window increments the count a second
    // time, because a key that distinguished the retry would also distinguish the
    // two submissions and nothing would ever coalesce. A caller that needs an
    // exact count needs a topic that does not coalesce.
    //
    // The window is a FIXED bucket, not a sliding one: two events a second apart
    // that straddle a boundary do not coalesce. A sliding window would need a read
    // before the write, which is the check-then-act the atomic upsert exists to
    // avoid (ENGINEERING_RULES.md §6).
    //
    // Truncated to 128 bits of SHA-256. A collision suppresses one notification,
    // and at 2^-64 birthday bound over any realistic publish volume it is not the
    // thing that will go wrong.
    [[nodiscard]] static std::array<std::uint8_t, 16> dedupe_key(
        const TopicSpec& spec, const TopicRef& topic, TemplateId tpl,
        std::string_view idempotency_key, db::TimeMs now) noexcept;

private:
    // Samples the pressure probe once, if there is one, and answers what to do.
    // Proceed whenever there is no probe, the probe throws, or the number it
    // gives back is not one a comparison can read.
    [[nodiscard]] ShedVerdict shed_decision(const TopicSpec& spec) const noexcept;

    // Everything the dispatch tail needs that is derivable from the stored row, so
    // the sweeper and the publish path share one implementation rather than two
    // that drift.
    [[nodiscard]] Result<std::int64_t> fan_out_targeted(mongocxx::client& client,
                                                        const TopicSpec& spec,
                                                        const NotificationRow& row) const;

    // Validation and row construction, shared by both publish overloads.
    [[nodiscard]] Result<NotificationRow> build_row(const PublishRequest& request,
                                                    const TopicSpec& spec, db::TimeMs now) const;

    // Declaration order is initialisation order (ENGINEERING_RULES.md §3.2). The reference
    // and both spans outlive the service; the hooks are owned.
    const NotificationRepository& repository_;
    std::span<const TemplateSpec> templates_;
    PublishHooks                  hooks_;
    const ShedPolicy              shedding_;
};

}  // namespace anvil::notifications
