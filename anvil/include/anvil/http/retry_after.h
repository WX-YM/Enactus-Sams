#pragma once

// `Retry-After`, in the one place it is spelled.
//
// docs/00-architecture.md has described a shed response as `503, Retry-After: 1`
// since phase 0, and for as long as that sentence stood nothing in the library
// emitted the header. The gap is the same class as the validation vocabulary's:
// a contract the documents state, every application re-derives, and two
// applications then answer differently for the same refusal.
//
// --- what the header is worth ----------------------------------------------
//
// A 429 or a 503 without it tells a client to guess. Every generated client
// guesses the same way — immediately, then with a backoff it invented — so a
// limiter that refuses a burst receives that burst again a moment later, and
// the refusal costs the same Redis round trip each time. The header converts a
// retry storm into one scheduled request, which is why it is worth a number
// that is true rather than a constant that is plausible.
//
// --- why the seconds are what they are --------------------------------------
//
// Three rules, each of which is a defect if it goes the other way, and none of
// which is visible at a call site that writes `to_string(ms / 1000)`:
//
//   ROUNDED UP. 1400 ms is 2, never 1. Rounding down sends the client back
//   before the window closes, which answers 429 again — so a header meant to
//   remove one refusal adds one.
//
//   NEVER ZERO. `Retry-After: 0` means retry immediately, which is precisely
//   the stampede the header exists to prevent. A window with 40 ms left is 1.
//
//   CLAMPED TO THE RULE. A key that outlived the rule that made it — a deploy
//   shortening a window while keys are open — must not tell a client to sleep
//   for the old one. The rule's window is the honest ceiling.
//
// --- what it must not become ------------------------------------------------
//
// An oracle. The value is derived from a BUCKET, and a bucket is keyed by
// identity and rule rather than by route (anvil/http/rate_limit.h), so two
// routes that must be indistinguishable produce the same number as well as the
// same status. A per-route limiter would have leaked which of them was called
// through this header even with the bodies byte-identical.

#include <chrono>
#include <cstdint>
#include <string_view>

#include <drogon/HttpResponse.h>

#include "anvil/http/rate_limit.h"

namespace anvil::http {

// The header name, so no call site spells it and no two spell it differently.
inline constexpr std::string_view kRetryAfterHeader = "Retry-After";

// What a queue-full shed answers with (docs/00-architecture.md §3).
//
// A constant rather than a measurement, because a bounded queue has nothing
// honest to measure: its depth is the work in front of the caller, not the time
// that work will take, and a pool that is full now is almost never full a second
// later. One second is short enough that a client's own request deadline still
// covers the retry, and long enough that the retry does not arrive inside the
// same burst that caused the shed.
inline constexpr std::uint32_t kShedRetryAfterSeconds = 1;

// Whole seconds, rounded up, never zero, never past the rule's window.
//
// Takes the rule as well as the verdict because the clamp is against the rule:
// a verdict alone cannot tell a plausible remaining window from one belonging to
// a rule that no longer exists.
[[nodiscard]] constexpr std::uint32_t retry_after_seconds(
    const RateLimitVerdict& verdict, const RateLimitRule& rule) noexcept {
    const std::chrono::milliseconds window = rule.window;
    std::chrono::milliseconds remaining = verdict.remaining;
    if (remaining.count() < 0) { remaining = std::chrono::milliseconds::zero(); }
    if (remaining > window) { remaining = window; }

    // Integer ceiling. The floating-point form is a rounding mode away from
    // being the truncation this exists to avoid.
    const std::int64_t seconds = (remaining.count() + 999) / 1000;
    return seconds <= 0 ? 1U : static_cast<std::uint32_t>(seconds);
}

// Writes the header. Does NOT set the status: a shed and a refusal are different
// statuses and the caller already knows which one it is answering with, so a
// function that set one would be wrong for the other half of its callers.
void apply_retry_after(drogon::HttpResponse& response, std::uint32_t seconds);

}  // namespace anvil::http
