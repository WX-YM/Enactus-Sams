#pragma once

// What an upgraded connection costs while it is open, and when it has to be
// asked again whether it may still be.
//
// The handshake is one request and every rule in this library already covers it:
// the origin check (`accesscontrol/upgrade_filter.h`), the access filter, the
// rate-limit table. Everything AFTER the handshake is invisible to all three. A
// client that connects once and then sends ten thousand frames a second has
// consumed exactly one rate-limit event, and the authority its handshake proved
// is never asked about again.
//
// --- what anvil ships here, and what it does not ----------------------------
//
// It does not ship a connection registry or an outbound ring, and the design
// that produced this file said it would. That was right for `notifications/sse.h`
// and is wrong here, for one reason: SSE's ring exists because
// `notifications::publish()` is a producer INSIDE this library that has to
// deliver into it. A WebSocket carries the application's own messages and anvil
// has no producer, so a ring here would be a container with no writer in this
// library — something an application can write in twenty lines, permanently part
// of the ABI, and one more thing to keep correct under concurrency.
//
// What is genuinely anvil's is the DESCRIPTOR BUDGET, because the budget is one
// process-wide number that two subsystems would otherwise each spend in full.
// `kUpgradeShare` in `core/descriptor_budget.h` is the share upgrades may take
// and `descriptor_ceiling` turns it into a count; an application holds its
// connections in whatever it holds them in and refuses past that number.
//
// The drop-the-connection POLICY is reused from SSE, and it is the part worth
// reusing: a connection that has broken its budget is closed, not answered. A
// rate-limit response on a socket the client is still flooding is one more thing
// to write, and the client already implements reconnecting — it does it after a
// deploy, a network blip and a laptop lid.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "anvil/auth/epoch_cache.h"
#include "anvil/core/user_context.h"

namespace anvil::http {

// Frames per window, per connection. Exceeding it CLOSES the socket.
//
// Twelve a second sustained, which is far above anything an interface generates
// and far below what a loop can. The budget is per connection and not per
// holder, because the cost it bounds — parsing and dispatching a frame — is paid
// per connection.
inline constexpr std::size_t kMaxFramesPerWindow = 120;
inline constexpr std::chrono::seconds kFrameWindow{10};

// A frame larger than this is a close, not a truncation.
//
// ENGINEERING_RULES.md §2.4: any payload that can exceed 256 KB is streamed or rejected,
// and a WebSocket frame is neither streamable nor worth 256 KB of inbound buffer
// per connection. Truncating instead would hand a handler a message that is not
// the message that was sent, which is worse than not handling it.
inline constexpr std::size_t kMaxFrameBytes = 64U * 1024U;

// How often an open connection re-runs the filter's own check.
//
// The epoch cache's TTL, and not a number of its own. That TTL IS the revocation
// latency (docs/04-access-control.md §5), so re-checking faster cannot produce a
// different answer — it is work whose result is already determined. Re-checking
// slower would make a connection a way to outlive a revocation that every
// request-path caller already honours.
inline constexpr std::chrono::seconds kRecheckPeriod{
    std::chrono::duration_cast<std::chrono::seconds>(auth::kDefaultEpochTtl)};

static_assert(kRecheckPeriod.count() > 0,
              "a re-check period that rounds to zero would re-check on every frame");

enum class FrameVerdict : std::uint8_t {
    Accept,
    // Close the connection. Never an error frame and never a 429: there is
    // nobody to answer, and a client that has broken its budget is a client that
    // reconnects.
    Close,
};

// One upgraded connection's authorization and budget, with no socket in it.
//
// The application owns the socket and calls this; every property below is
// therefore testable without a network, which is the same line `sse.h` draws.
//
// Not thread-safe, and deliberately so: a connection is served by one event loop
// and a mutex per connection would be a lock taken on every frame to protect
// state only one thread reaches. An application that hands one of these between
// threads has to say so itself.
class UpgradedConnection final {
public:
    // `expires_at_unix` is the `expires_at` of the token the handshake verified.
    // The connection keeps it because `UserContext` deliberately does not — that
    // field would push the context to 72 bytes and across a second cache line,
    // and no request-path reader wants it.
    UpgradedConnection(const UserContext& ctx, std::uint32_t expires_at_unix,
                       std::int64_t opened_at_unix) noexcept
        : ctx_{ctx},
          window_started_unix_{opened_at_unix},
          next_recheck_unix_{opened_at_unix + kRecheckPeriod.count()},
          expires_at_unix_{expires_at_unix},
          frames_in_window_{0} {}

    // Whether this frame may be handled. Counts it when it may.
    //
    // The window is a fixed one that restarts rather than a sliding one: a
    // sliding window needs the timestamp of every frame in it, which is the
    // per-connection memory this budget exists to bound. The cost is that a
    // client can send two windows' worth across a boundary, which is twice the
    // budget over twice the period and is not the flood the rule is about.
    [[nodiscard]] FrameVerdict admit_frame(std::size_t bytes, std::int64_t now_unix) noexcept {
        // Size first, and it does not consume budget. A frame over the cap is
        // refused on a comparison against a length the framework has already
        // told us, before anything reads the payload.
        if (bytes > kMaxFrameBytes) { return FrameVerdict::Close; }

        if (now_unix - window_started_unix_ >= kFrameWindow.count()) {
            window_started_unix_ = now_unix;
            frames_in_window_ = 0;
        }
        // A clock that went backwards restarts the window rather than granting
        // an unbounded one. `now_unix` is the local clock, so this is a step
        // from ntp rather than anything a client can cause — but a budget that
        // depends on the clock being monotonic is a budget with a hole in it.
        if (now_unix < window_started_unix_) {
            window_started_unix_ = now_unix;
            frames_in_window_ = 0;
        }

        if (frames_in_window_ >= kMaxFramesPerWindow) { return FrameVerdict::Close; }
        ++frames_in_window_;
        return FrameVerdict::Accept;
    }

    // Whether the authorization is due to be checked again. The caller then runs
    // `accesscontrol::still_authorized` with this context and expiry, and closes
    // the connection on anything but `Keep`.
    [[nodiscard]] bool due_for_recheck(std::int64_t now_unix) const noexcept {
        return now_unix >= next_recheck_unix_;
    }

    // Records that a re-check happened. Scheduled from NOW rather than by adding
    // a period to the last due time, so a sweep that fell behind does not then
    // run a burst of catch-up checks it can learn nothing from.
    void note_recheck(std::int64_t now_unix) noexcept {
        next_recheck_unix_ = now_unix + kRecheckPeriod.count();
    }

    [[nodiscard]] const UserContext& context() const noexcept { return ctx_; }
    [[nodiscard]] std::uint32_t expires_at_unix() const noexcept { return expires_at_unix_; }
    [[nodiscard]] std::size_t frames_in_window() const noexcept { return frames_in_window_; }

private:
    // Largest alignment first (ENGINEERING_RULES.md §2.3). UserContext is 64 bytes and
    // alignment 8, and the four members after it pack into the next sixteen with
    // no interior padding.
    UserContext   ctx_;                   // 64
    std::int64_t  window_started_unix_;   //  8
    std::int64_t  next_recheck_unix_;     //  8
    std::uint32_t expires_at_unix_;       //  4
    std::uint32_t frames_in_window_;      //  4
};

// 88, and asserted because one of these exists per open connection: ten thousand
// connections is 880 KB of state and a member added carelessly is another 80 KB.
static_assert(sizeof(UpgradedConnection) == 88);
static_assert(alignof(UpgradedConnection) == 8);
static_assert(std::is_trivially_copyable_v<UpgradedConnection>);

}  // namespace anvil::http
