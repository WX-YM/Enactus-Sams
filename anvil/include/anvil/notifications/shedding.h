#pragma once

// The storm breaker: which publishes a saturated deployment gives up on, and
// which it only delays.
//
// --- what was actually missing ----------------------------------------------
//
// Shedding was never absent. `BoundedThreadPool::try_post` refuses a full queue
// and the caller sheds (anvil/core/thread_pools.h), so a publish storm cannot
// grow the heap. What was missing is PRIORITY: the queue refuses whatever
// arrives while it is full, in arrival order, so a password reset published
// during a like-storm was refused exactly as readily as a like. The pool
// protected the process and lost the one message that mattered.
//
// --- two gates, because a publish is two things -----------------------------
//
// The existing write path already separates them (anvil/notifications/publish.h):
//
//     insert/coalesce  ->  fan out inbox rows  ->  enqueue transports  ->  mark
//                     ^                                                    |
//                     |                                                    v
//                     +--- sweep_outbox() re-runs everything after a crash -+
//
// The ROW is the system of record, and shedding it DROPS the notification. The
// dispatch tail is idempotent and already has a recovery path: shedding it
// leaves `dispatched_at` unset, which is byte-for-byte the state a crashed
// process leaves, and `sweep_outbox()` finishes exactly that. Shedding it
// DELAYS.
//
//     Gate            Where                       Effect   Legal for
//     admission       before the row is written   drop     user_optional only
//     dispatch        after the row is committed  delay    every topic
//
// Conflating them is how a storm breaker turns into data loss, so the property
// this header exists to hold is:
//
//     A TOPIC THE READER IS NOT ALLOWED TO SILENCE IS A TOPIC THE STORM BREAKER
//     IS NOT ALLOWED TO DROP.
//
// Under any pressure whatsoever such a topic is only ever delayed, and the delay
// is bounded by the sweep interval plus kOutboxGrace.
//
// --- the discriminator is a flag that already exists ------------------------
//
// `TopicSpec::user_optional`, and no new field. topic_spec.h already argues that
// the two must be the same flag: a security topic the reader cannot disable must
// also be one the storm breaker will not shed. A second `sheddable` bit would
// let them drift, and the first time they drifted it would be a security topic
// somebody marked droppable to make a graph look better. It would also grow
// TopicSpec past its asserted 48 bytes.
//
// --- and coalescing does more than this does, and does it first -------------
//
// `TopicSpec::coalesce_window_s` collapses repeats of one event class into one
// row with a count — "3 new form submissions", not three rows. Against the storm
// that actually happens, which is thousands of instances of ONE event class,
// coalescing removes the load and loses nothing at all. Shedding is for a storm
// of DISTINCT events, which is rarer.
//
// So the operational advice belongs here rather than in a runbook nobody reads:
// A TOPIC THAT CAN STORM GETS A NON-ZERO `coalesce_window_s` FIRST. The storm
// breaker is the second line and it is the lossy one.

#include <cstdint>

#include "anvil/notifications/topic_spec.h"

namespace anvil::notifications {

// Where the watermarks sit, as a FRACTION of the queue's capacity.
//
// A fraction and not an absolute rate, which is why this ships with a default
// where an absolute rate was refused for years as unguessable. The absolute
// number depends on a traffic shape anvil cannot know; the fraction is read
// against a queue the operator already sized, so it scales with their tuning
// instead of overriding it.
struct ShedPolicy final {
    // Above this, `user_optional` topics are refused admission. Below it,
    // NOTHING is ever dropped.
    float drop_optional_above{0.75F};
    // Above this, the dispatch tail is skipped and left to the sweeper.
    //
    // Lower than the drop watermark on purpose, and `shed_policy_is_well_formed`
    // refuses a policy where it is not: deferring is cheap and loses nothing, so
    // it is the response that has to engage FIRST. A policy that dropped before
    // it deferred would throw a notification away while the outbox path it could
    // have used was still idle.
    float defer_dispatch_above{0.50F};
};

enum class ShedVerdict : std::uint8_t {
    Proceed = 0,
    // Write the row, skip the tail. sweep_outbox() will finish it.
    DeferDispatch = 1,
    // Refuse the publish outright. `user_optional` topics only.
    Drop = 2,
};

// Both watermarks inside [0, 1], and the cheap response before the lossy one.
//
// Written as negated comparisons so a NaN watermark — a policy computed from a
// division somebody did not check — is MALFORMED rather than accepted: every
// comparison against NaN is false, so each `!(…)` is true and this returns
// false at the first one.
[[nodiscard]] constexpr bool shed_policy_is_well_formed(const ShedPolicy& policy) noexcept {
    if (!(policy.defer_dispatch_above >= 0.0F) || !(policy.defer_dispatch_above <= 1.0F)) {
        return false;
    }
    if (!(policy.drop_optional_above >= 0.0F) || !(policy.drop_optional_above <= 1.0F)) {
        return false;
    }
    return policy.defer_dispatch_above <= policy.drop_optional_above;
}

static_assert(shed_policy_is_well_formed(ShedPolicy{}),
              "the shipped default must satisfy the check it ships beside");

// PURE, and deliberately so. It samples nothing and it can acquire nothing
// later, because it has nothing to call — the same reason accesscontrol's
// decision is a pure function, and the same failure it avoids: the authorisation
// path acquired a Redis GET because the function it lived in already had a
// reason to reach for a singleton (docs/04 §3). The CALLER samples `pressure`,
// as `queue_depth() / queue_capacity()`.
//
// A NaN `pressure` — which is what `0 / 0` gives a caller whose pool has no
// capacity — answers Proceed, by the same negated comparison. That is the
// correct direction to fail: a number nobody can read is not grounds for
// throwing a notification away.
[[nodiscard]] constexpr ShedVerdict shed_verdict(const TopicSpec& spec,
                                                 const ShedPolicy& policy,
                                                 float pressure) noexcept {
    // Below the lower watermark nothing is shed, whatever the topic. Stated
    // first because it is the steady state and it is the branch that has to be
    // free.
    if (!(pressure > policy.defer_dispatch_above)) { return ShedVerdict::Proceed; }
    // The whole security property, in one conjunction: a topic the reader may
    // not silence never reaches Drop, at any pressure, ever.
    if (spec.user_optional && pressure > policy.drop_optional_above) {
        return ShedVerdict::Drop;
    }
    return ShedVerdict::DeferDispatch;
}

}  // namespace anvil::notifications
