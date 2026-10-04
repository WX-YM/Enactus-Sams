#pragma once

// Rate-limit budgets (anvil docs/01-seams.md §7). Counted in Redis, so every
// instance of the backend shares them; the bucket name is part of the Redis key.

#include <array>
#include <chrono>
#include <string_view>

#include "anvil/http/rate_limit.h"

namespace enactus {

namespace h = anvil::http;

inline constexpr std::array<h::RateLimitRule, 9> kRateLimits{{
    // Account flows. `login-acct` is per identity, so an attacker spreading a
    // guess list over many addresses still meets one ceiling per account.
    {"login",      std::chrono::seconds{60}, 20},
    {"login-acct", std::chrono::minutes{15}, 10},
    {"refresh",    std::chrono::seconds{60}, 60},
    // Unused by any route this application declares (there is no public
    // signup, verification or reset), but the account service takes a budget
    // for every role, and an unspendable one is the honest value.
    {"closed",     std::chrono::minutes{60}, 1},
    // Public writes, per client address.
    {"apply",      std::chrono::minutes{10}, 5},
    {"submit",     std::chrono::minutes{10}, 10},
    {"visit",      std::chrono::minutes{1},  30},
    // Staff writes, per account.
    {"staff-write", std::chrono::minutes{1}, 120},
    {"media",       std::chrono::minutes{1}, 30},
}};

static_assert(h::rate_limit_table_is_well_formed(kRateLimits),
              "an empty bucket, a zero window, a zero budget, or two rules sharing a bucket");

[[nodiscard]] constexpr const h::RateLimitRule& rate_rule(std::string_view bucket) {
    for (const h::RateLimitRule& rule : kRateLimits) {
        if (rule.bucket == bucket) { return rule; }
    }
    // Unreachable for any literal in this codebase: a constexpr call site with an
    // unknown bucket fails to compile on the throw.
    throw "no rate-limit rule with that bucket";
}

}  // namespace enactus
