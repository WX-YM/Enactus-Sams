#pragma once

// How many descriptors this process is willing to spend on connections that
// stay open, and who is allowed to spend them.
//
// A connection that is held open holds a file descriptor for its entire life, so
// the real limit on how many of them can exist at once is `RLIMIT_NOFILE` and
// not a number anybody picked. Deriving it means a deployment that raises its
// limit gets the connections, and one that does not gets a clean refusal at the
// door rather than an `accept()` failure that takes down every OTHER kind of
// request at the same time — including the ones a client would use to recover.
//
// --- why this is not in notifications/sse.h, where it was written -----------
//
// It was, and it was correct there while streams were the only thing in this
// library that held a connection open. The moment a second kind exists the
// derivation is no longer a property of either one: they draw on ONE budget, and
// two subsystems each deriving "half of what is left" have spent the same half
// twice. Nothing would fail a build or a test; the process would simply run out
// of descriptors under a load each of them believed it was within.
//
// So the claims live here, beside the budget, where their sum is visible and
// asserted. A new kind of long-lived connection adds its share to this file and
// the assertion below tells it whether the budget has room.

#include <cstddef>

namespace anvil {

// Not all of `RLIMIT_NOFILE`, and the reserve is the reason: the database pool,
// the Redis connections, every open file, every listening socket and every
// ordinary request in flight also need descriptors. A ceiling that consumed the
// whole budget would turn a crowd of clients into an outage for everything else
// — which is exactly the failure the ceiling exists to prevent, relocated.
inline constexpr std::size_t kDescriptorReserve = 512;

// One kind of long-lived connection's claim on what is left after the reserve.
//
// A fraction rather than a count, because the number of descriptors a deployment
// has is a deployment's decision and the SPLIT between kinds is anvil's.
struct DescriptorShare final {
    std::size_t numerator;
    std::size_t denominator;
};

// Server-sent event streams (`notifications/sse.h`).
inline constexpr DescriptorShare kStreamShare{1, 2};

// Upgraded connections (`http/upgrade.h`).
//
// A quarter and not a half. The two are separate registries with separate
// ceilings drawing on one budget, and a deployment that ran both at a half each
// would have promised the whole post-reserve budget to connections and left
// nothing for the requests that open and close them.
inline constexpr DescriptorShare kUpgradeShare{1, 4};

// Whether every claim above fits inside the budget, with the arithmetic done in
// a common denominator so no division truncates the answer into passing.
[[nodiscard]] constexpr bool shares_fit(DescriptorShare first, DescriptorShare second) noexcept {
    if (first.denominator == 0 || second.denominator == 0) { return false; }
    const std::size_t common = first.denominator * second.denominator;
    const std::size_t claimed =
        (first.numerator * second.denominator) + (second.numerator * first.denominator);
    return claimed <= common;
}

static_assert(shares_fit(kStreamShare, kUpgradeShare),
              "the kinds of long-lived connection anvil ships have promised more "
              "descriptors than the process has after the reserve");

// Reads `RLIMIT_NOFILE` and returns what is safe to spend on this share.
//
// Zero when the limit is at or below the reserve, which is a deployment that
// cannot serve long-lived connections at all — and refusing every one of them at
// the door is the correct way to say so, because the alternative is failing
// `accept()` for everything. Zero is also the answer when `getrlimit` fails:
// there is then nothing to derive from, and guessing a ceiling would defeat the
// point of deriving one.
[[nodiscard]] std::size_t descriptor_ceiling(DescriptorShare share) noexcept;

}  // namespace anvil
