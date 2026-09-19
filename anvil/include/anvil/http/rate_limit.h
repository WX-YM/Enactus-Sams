#pragma once

// Rate limiting, shared across every instance (docs/00-architecture.md §4
// stage 3, docs/10-timer-jobs.md §1).
//
// Three properties, each of which a naive implementation gets wrong:
//
//   SHARED, not process-local. With N instances behind a load balancer, a
//   per-process counter multiplies every limit by N and the attacker picks
//   which process to talk to. The counter lives in Redis.
//
//   ONE round trip. INCR, the expiry and the read of what is left of the window
//   as separate calls is three round trips AND a race: a process that dies
//   between the first two leaves a key with no TTL, which is a permanent
//   lockout. They go in one script, which is also what makes the remaining
//   window an honest number rather than one this process computed from a clock
//   it does not share with Redis.
//
//   NOT KEYED BY ROUTE. A per-route bucket is an existence oracle: a denied
//   admin route consumes budget that a nonexistent one does not, so an attacker
//   distinguishes them by watching their own remaining quota — the exact side
//   channel the stealth 404 exists to close. Buckets are keyed by
//   identity and by RULE, never by route: routes that must be indistinguishable
//   share a rule and therefore share one counter. Keying by identity ALONE was
//   the defect that shipped — see RateLimitRule::bucket, which carries the whole
//   account of it — and it did not buy this property either, because an attacker
//   watching one shared counter learns just as much as one watching the right
//   one of several.
//
// When Redis is unreachable the limiter degrades to a per-process token bucket
// rather than failing open. That is weaker — N instances allow up to N times the
// configured rate — but it is bounded, and it is COUNTED:
// `anvil_rate_limit_decisions{source="local"}` is what makes it visible.
//
// It was not, for a phase. `RateLimitVerdict::degraded` said in its own comment
// that the degradation was surfaced so it would appear in metrics, no metric
// consumed it, and what a deployment got instead was one LOG_WARN per request
// for as long as Redis was unreachable — the shape docs/00-architecture.md §9
// forbids by name. The line survives as ONE line per transition into the
// degraded state and one on the way out, because the reason for an outage is
// something a counter cannot carry and a rate is something a log line must not.
//
// BLOCKING. Every call here talks to Redis and must run on a worker pool, never
// on a Trantor event-loop thread.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

#include "anvil/core/types.h"

namespace anvil::http {

// Ordered largest-alignment-first (ENGINEERING_RULES.md §2.3).
struct RateLimitRule final {
    // The BUCKET this rule counts into, and the whole of.
    //
    // Every rule used to share one counter per identity, because the key named
    // the identity and nothing else. Eighteen rules with eighteen different
    // budgets then incremented one number, each comparing it against its own
    // ceiling — so the TIGHTEST budget in the system was spent by traffic that
    // had nothing to do with it. Signup is five an hour; opening the site and
    // letting the notification bell poll drained it, and a real visitor's first
    // signup answered 429 for an hour. The window was not even the right one:
    // the TTL was set by whichever rule reached the key first, so a login put a
    // sixty-second expiry on a bucket signup believed was hourly.
    //
    // Naming the bucket rather than the ROUTE is what preserves the property the
    // original design was reaching for: routes that must be
    // indistinguishable from one another share a rule, and therefore share a
    // counter, so remaining quota still says nothing about which of them was
    // called. What it no longer says is which rule was called by SOMEBODY ELSE'S
    // request an hour ago.
    //
    // A literal with static storage duration, never a computed string: it is
    // part of a Redis key and it must be the same bytes in every process.
    std::string_view          bucket;
    std::chrono::milliseconds window;
    std::uint32_t             max_events;
};

// --- table conformance -------------------------------------------------------
//
// The rules are the application's (docs/01-seams.md §7), and every other seam
// anvil ships fails at COMPILE time rather than at runtime. This is the check
// that makes that true here too: `static_assert` it over your own table.
//
// Each condition is a mistake that would otherwise ship as a limit that does not
// limit, and none of them is visible in a code review of the table itself.
[[nodiscard]] constexpr bool rate_limit_table_is_well_formed(
    std::span<const RateLimitRule> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        const RateLimitRule& rule = table[i];
        // The bucket is part of a Redis key. An empty one makes every rule that
        // has it share one counter, which is the exact failure the bucket field
        // exists to have fixed.
        if (rule.bucket.empty()) { return false; }
        // A zero window is a key with no expiry: the first burst fills the bucket
        // and it never drains, so the rule becomes a permanent refusal.
        if (rule.window.count() <= 0) { return false; }
        // Zero events refuses everything, including the first request. A rule
        // meant to be disabled is deleted, not set to zero.
        if (rule.max_events == 0) { return false; }
        // Two rules sharing a bucket share a COUNTER, so the tighter budget is
        // spent by traffic belonging to the other one. That is the defect the
        // comment above describes, reintroduced by a copy-paste — which is how it
        // arrives in practice.
        for (std::size_t j = 0; j < i; ++j) {
            if (table[j].bucket == rule.bucket) { return false; }
        }
    }
    return true;
}

// --- what anvil does NOT ship -----------------------------------------------
//
// The rules themselves. There is no `kLoginPerIp` here, and there was: thirteen
// constants lived in this header, five of them budgets for a subsystem of one
// application — a subsystem anvil has never contained. A table anvil populates is a bug (ENGINEERING_RULES.md §1), and a table
// of rate limits is the clearest case of it — a budget is a product decision
// about one deployment's traffic, and a library that ships one has decided it for
// every application at once.
//
// Declare them beside your routes and `static_assert` the table:
//
//     inline constexpr std::array<anvil::http::RateLimitRule, 3> kRateLimits{{
//         {"login",  std::chrono::seconds{60}, 20},
//         {"signup", std::chrono::minutes{60},  5},
//         {"media",  std::chrono::minutes{1},  20},
//     }};
//     static_assert(anvil::http::rate_limit_table_is_well_formed(kRateLimits));
//
// `tests/testapp/rate_limits.h` is the compiled worked example.
//
// Two properties of a rule are worth stating where the rules are not, because
// they are the ones a first table gets wrong:
//
//   PER-IP AND PER-IDENTITY ARE TWO RULES, and both apply. The identity bucket
//   stops a distributed password spray against one victim; the IP bucket stops
//   one host spraying across many identities. Neither alone is sufficient, and
//   the identity bucket alone lets one attacker lock a victim out cheaply.
//
//   A BUCKET IS NOT A ROUTE. Routes that must be indistinguishable from one
//   another share a rule and therefore share a counter, so remaining quota still
//   says nothing about which of them was called — which is what keeps a limiter
//   from undoing the stealth 404 a route table declared.

struct RateLimitVerdict final {
    std::uint64_t count;

    // What is left of the window this hit counted into, which is what a refusal
    // has to tell the client: `Retry-After` is the difference between a client
    // that waits and a client that retries into the same refusal immediately.
    // It is read from Redis inside the same script as the INCR rather than
    // derived from a local clock, because the window belongs to whichever
    // process opened it and the others do not know when that was.
    //
    // Present on an ALLOWED verdict too. The same number is what a client is
    // told when it asks how much budget it has left, and a field that is only
    // meaningful on one branch is a field somebody reads on the other.
    std::chrono::milliseconds remaining;

    bool          allowed;
    // True when Redis could not be reached and the local bucket answered. It
    // never changes the response the client sees; what it changes is which
    // series of anvil_rate_limit_decisions the hit is counted into, which is the
    // only place a deployment learns that its limits are now per process.
    bool          degraded;
};

// What one hit against a bucket observed. The same two facts the verdict
// carries, before the rule's ceiling has been applied to them.
struct BucketHit final {
    std::uint64_t             count;
    std::chrono::milliseconds remaining;
};

// A fixed-capacity fallback. Direct-mapped over a compile-time slot array for
// the same reason EpochCache is: the key is attacker-chosen, so growth is a
// memory-exhaustion vector and eviction is the correct failure.
class LocalBuckets final {
public:
    static constexpr std::size_t kSlots = 4096;
    static constexpr std::size_t kShards = 64;
    static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");
    static_assert(kSlots % kShards == 0);

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] BucketHit hit(std::string_view key, const RateLimitRule& rule,
                                Clock::time_point now) noexcept;
    void clear() noexcept;

private:
    struct Slot final {
        std::uint64_t     fingerprint;   // 0 means empty
        Clock::time_point window_ends;
        std::uint32_t     count;
    };

    mutable std::array<std::mutex, kShards> shards_;
    std::array<Slot, kSlots>                slots_{};
};

class RateLimiter final {
public:
    RateLimiter() = default;

    // `identity` is an IP or a user id, never a route. Returns the count within
    // the current window; `allowed` is false once it exceeds the rule.
    [[nodiscard]] RateLimitVerdict check_ip(const std::array<std::uint8_t, 16>& ip,
                                            const RateLimitRule& rule);
    // Keyed by the NORMALISED IDENTIFIER, not by a user id. Keying on the id
    // would mean a login attempt against a nonexistent account performs one
    // fewer Redis operation than one against a real account, which is an
    // account-enumeration oracle measurable in the response time.
    // Hashing it also keeps the email out of the Redis keyspace.
    [[nodiscard]] RateLimitVerdict check_account(std::string_view normalised_identifier,
                                                 const RateLimitRule& rule);

    // Exposed for the fallback's tests, and so a boot check can prove the local
    // bucket works before Redis is ever needed.
    [[nodiscard]] LocalBuckets& local() noexcept { return local_; }

    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

private:
    [[nodiscard]] RateLimitVerdict check(std::string_view key, const RateLimitRule& rule);

    LocalBuckets local_;
    // Whether the LAST answer came from the local bucket, so the log line is
    // edge-triggered rather than per request. Relaxed throughout: nothing is
    // ordered against it and a lost race costs one duplicated or one missing log
    // line, never a wrong decision — the decision is the verdict, and the
    // verdict is counted.
    //
    // Read with a plain load before any exchange on the healthy path. An
    // unconditional read-modify-write there would be a contended write to this
    // line on every rate-limited request, which is the cost ENGINEERING_RULES.md §3.1 names
    // for an atomic increment, paid to maintain a flag that almost never moves.
    std::atomic<bool> degraded_{false};
};

}  // namespace anvil::http
