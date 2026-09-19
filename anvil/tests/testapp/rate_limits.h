#pragma once

// The reference application's rate limits (docs/01-seams.md §7).
//
// anvil ships `RateLimitRule`, the sliding window, the shared Redis counter and
// the local fallback bucket. The NUMBERS are here, because a budget is a product
// decision about one deployment's traffic — how many sign-ups an hour is normal
// depends entirely on whether the product gets ten a day or ten thousand, and a
// library that shipped an answer would have decided it for every application at
// once.
//
// --- the two mistakes a first table makes -----------------------------------
//
// PER-IP AND PER-IDENTITY ARE SEPARATE RULES, and both apply. `login` and
// `login-acct` below are not redundant: the identity bucket stops a distributed
// spray against one victim, the IP bucket stops one host spraying across many
// victims, and an identity bucket alone lets an attacker lock somebody out by
// failing their password ten times deliberately.
//
// A BUCKET IS NOT A ROUTE. Two routes that must be indistinguishable from one
// another share a bucket on purpose, so remaining quota says nothing about which
// of them was called — a per-route bucket is an existence oracle that undoes the
// stealth 404 routes.h went to the trouble of declaring.

#include <array>
#include <chrono>

#include "anvil/http/rate_limit.h"

namespace testapp {

namespace h = anvil::http;

// Every rule this application declares, in one table so the conformance check has
// something to check. A rule declared outside it is a rule nothing verifies.
inline constexpr std::array<h::RateLimitRule, 8> kRateLimits{{
    // Sign-in. Two buckets, both applied — see the header comment.
    {"login", std::chrono::seconds{60}, 20},
    {"login-acct", std::chrono::minutes{15}, 10},

    // Sign-up is the expensive one to get wrong in either direction: too loose
    // and it is an account-creation flood, too tight and a shared office NAT
    // locks out a real visitor's first attempt for an hour.
    {"signup", std::chrono::minutes{60}, 5},

    // A refresh is cheap and legitimate clients do it often. The bucket exists to
    // bound a loop, not to ration normal use.
    {"refresh", std::chrono::seconds{60}, 60},

    // An upload costs a disk write, a decode and a full variant set — seconds of
    // cpu_pool per request. This is what stops one authenticated staff account
    // occupying the whole pool, and it is deliberately tighter than the
    // admission check in the controller, which only sheds once the queue is
    // already full.
    {"media", std::chrono::minutes{1}, 20},

    // Address verification. The per-address bucket is the one that matters: it is
    // what stops this application being used to mail somebody else's inbox.
    {"verify", std::chrono::minutes{15}, 10},
    {"verify-addr", std::chrono::minutes{15}, 5},
    {"resend-addr", std::chrono::minutes{60}, 3},
}};

// A malformed table is a build error, not a limiter that silently does not limit.
// Every condition it checks is invisible in a review of the rows above: an empty
// bucket name, a zero window whose key never expires, a zero budget that refuses
// the first request, and two rules sharing a counter so that the tighter of the
// two is spent by the other's traffic.
static_assert(h::rate_limit_table_is_well_formed(kRateLimits),
              "an empty bucket, a zero window, a zero budget, or two rules sharing a bucket");

static_assert(kRateLimits.size() == 8,
              "adding a rule is a deliberate act: the bucket name is part of a Redis key and "
              "is therefore shared with every other process in the deployment");

}  // namespace testapp
