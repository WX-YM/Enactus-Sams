#include "anvil/http/rate_limit.h"

#include <atomic>
#include <chrono>
#include <span>
#include <string>
#include <tuple>

#include <trantor/utils/Logger.h>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/digest.h"
#include "anvil/redis/redis_client.h"

namespace anvil::http {
namespace {

// INCR, the expiry and what is left of the window, in one round trip. Separate
// calls would be three round trips and a race: a client that dies between the
// first two leaves a key with no expiry, which is a permanent lockout for
// whoever owns that identity.
//
// The expiry is driven by the PTTL rather than by `hits == 1`, which is a wider
// condition for the same cost. A fresh key answers -1 and gets its window; so
// does a key that has somehow LOST its expiry, which is the permanent-lockout
// state above. Under the `hits == 1` form that key stays lost forever, because
// it can never be at one again.
//
// Both values come back because a refusal has to tell the client when to return,
// and the window belongs to whichever process opened it: no other process can
// compute what is left of it from a clock of its own.
constexpr std::string_view kWindowScript = R"lua(
local hits = redis.call('INCR', KEYS[1])
local ttl = redis.call('PTTL', KEYS[1])
if ttl < 0 then
  redis.call('PEXPIRE', KEYS[1], ARGV[1])
  ttl = tonumber(ARGV[1])
end
return {hits, ttl}
)lua";

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] std::uint64_t fingerprint_of(std::string_view key) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const char c : key) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= kFnvPrime;
    }
    // Zero marks an empty slot, so it must not be a reachable fingerprint.
    return hash == 0 ? 1 : hash;
}

// A window that has already closed is zero, never negative: the caller turns
// this into whole seconds, and a negative duration rounds to a value a client
// reads as "retry immediately" — the stampede Retry-After exists to prevent.
// A value PAST the rule's own window can only be a key that outlived its rule,
// which is what a deploy shortening a window does to every key already open, and
// the rule is the honest bound in that case too.
[[nodiscard]] std::chrono::milliseconds clamp_remaining(std::chrono::milliseconds remaining,
                                                        const RateLimitRule& rule) noexcept {
    if (remaining.count() < 0) { return std::chrono::milliseconds::zero(); }
    return remaining > rule.window ? rule.window : remaining;
}

// One increment per decision, on both paths, because the interesting number is
// a RATIO: how much of what this deployment allowed or refused was decided by a
// bucket only one process can see. Two counters would be two things that can
// disagree about the denominator.
void count_decision(bool degraded, bool allowed) noexcept {
    analytics::count(
        analytics::Internal::RateLimitDecisions,
        analytics::LabelIndex{static_cast<std::uint16_t>(
            degraded ? analytics::RateLimitSource::Local : analytics::RateLimitSource::Shared)},
        analytics::LabelIndex{static_cast<std::uint16_t>(
            allowed ? analytics::RateLimitDecision::Allowed
                    : analytics::RateLimitDecision::Refused)});
}

}  // namespace

BucketHit LocalBuckets::hit(std::string_view key, const RateLimitRule& rule,
                            Clock::time_point now) noexcept {
    const std::uint64_t fingerprint = fingerprint_of(key);
    const std::size_t index = static_cast<std::size_t>(fingerprint) & (kSlots - 1);
    const std::lock_guard<std::mutex> lock{shards_[index % kShards]};

    Slot& slot = slots_[index];
    // A different identity in this slot, or a window that has closed, restarts
    // the count. Eviction on collision is deliberate: two identities sharing a
    // slot each get a full budget rather than one silently consuming the
    // other's, and the fallback is already the weaker of the two paths.
    if (slot.fingerprint != fingerprint || slot.window_ends <= now) {
        slot.fingerprint = fingerprint;
        slot.window_ends = now + rule.window;
        slot.count = 1;
        return BucketHit{.count = 1, .remaining = rule.window};
    }
    // Saturating rather than wrapping: an unsigned wrap here would hand an
    // attacker a fresh budget at 2^32 requests (ENGINEERING_RULES.md §5).
    if (slot.count != UINT32_MAX) { ++slot.count; }
    // The slot carries the window's END rather than what is left of it, so the
    // subtraction happens here and against the same steady_clock the slot was
    // stamped from. A wall clock would make the answer follow an NTP step.
    return BucketHit{
        .count = slot.count,
        .remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(slot.window_ends - now),
    };
}

void LocalBuckets::clear() noexcept {
    for (std::size_t shard = 0; shard < kShards; ++shard) {
        const std::lock_guard<std::mutex> lock{shards_[shard]};
        for (std::size_t index = shard; index < kSlots; index += kShards) {
            slots_[index].fingerprint = 0;
        }
    }
}

RateLimitVerdict RateLimiter::check(std::string_view key, const RateLimitRule& rule) {
    try {
        sw::redis::Redis& redis = redis::RedisClient::instance();
        // A tuple rather than a container filled through an output iterator: the
        // reply is exactly two integers, and a heap-allocated sequence to hold
        // them would be an allocation on a path whose whole design is that it
        // does none (ENGINEERING_RULES.md §2.1).
        const auto [hits, ttl_ms] = redis.eval<std::tuple<long long, long long>>(
            sw::redis::StringView{kWindowScript.data(), kWindowScript.size()},
            {sw::redis::StringView{key.data(), key.size()}},
            {std::to_string(rule.window.count())});

        const auto count = static_cast<std::uint64_t>(hits);
        // Announced once, on the way back, and only after a load says there is
        // something to announce. An unconditional exchange here would be a
        // contended write on every rate-limited request to maintain a flag that
        // moves twice an outage.
        if (degraded_.load(std::memory_order_relaxed) &&
            degraded_.exchange(false, std::memory_order_relaxed)) {
            LOG_WARN << "rate limiter recovered: the shared counter is answering again, so "
                        "limits are no longer per process";
        }
        const bool allowed = count <= rule.max_events;
        count_decision(false, allowed);
        return RateLimitVerdict{
            .count = count,
            // The script already guarantees a non-negative TTL, so the clamp is
            // not what makes this correct; it is what keeps a Redis answering
            // something else from becoming a Retry-After no client will honour.
            .remaining = clamp_remaining(std::chrono::milliseconds{ttl_ms}, rule),
            .allowed = allowed,
            .degraded = false,
        };
    } catch (const std::exception& e) {
        // Degrade, never fail open. The local bucket is per process, so N
        // instances allow up to N times the configured rate — weaker, bounded,
        // and COUNTED.
        //
        // One line per TRANSITION into this state, not one per request. Every
        // request reaches this path for as long as Redis is unreachable —
        // nothing refuses them earlier, which is what distinguishes this from
        // the idempotency store — so a line here is a line at request rate, and
        // under the load that makes it fire the line is itself the outage
        // (docs/00-architecture.md §9). The rate is the counter's job; the
        // reason is this line's, and the reason does not change per request.
        if (!degraded_.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR << "rate limiter degraded to local buckets, so limits are now per "
                         "process: "
                      << e.what();
        }
        const BucketHit local = local_.hit(key, rule, LocalBuckets::Clock::now());
        const bool allowed = local.count <= rule.max_events;
        count_decision(true, allowed);
        return RateLimitVerdict{
            .count = local.count,
            .remaining = clamp_remaining(local.remaining, rule),
            .allowed = allowed,
            .degraded = true,
        };
    }
}

RateLimitVerdict RateLimiter::check_ip(const std::array<std::uint8_t, 16>& ip,
                                       const RateLimitRule& rule) {
    // The RULE and the identity, never the route. Without the rule every budget
    // in the system counted into one number and the tightest one lost — see
    // RateLimitRule::bucket. Routes that must stay indistinguishable share a
    // rule and still share a counter.
    std::string key = "rl:ip:";
    key += rule.bucket;
    key.push_back(':');
    key += crypto::base64url_encode(ip);
    return check(key, rule);
}

RateLimitVerdict RateLimiter::check_account(std::string_view normalised_identifier,
                                            const RateLimitRule& rule) {
    // Same split, and it matters as much here: verifying an address five times
    // used to spend the RESEND budget for that address, because both counted
    // into one key.
    //
    // Truncated to 16 bytes: this is a bucket key, not a credential, and 128
    // bits is far past any collision that matters over a 15-minute window.
    const crypto::Digest256 digest = crypto::sha256(normalised_identifier);
    std::string key = "rl:acct:";
    key += rule.bucket;
    key.push_back(':');
    key += crypto::base64url_encode(std::span<const std::uint8_t>{digest.data(), 16});
    return check(key, rule);
}

}  // namespace anvil::http
