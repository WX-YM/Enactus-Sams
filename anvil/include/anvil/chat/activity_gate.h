#pragma once

// The coalescing gate for the chat-list activity bump (docs/22-chat.md §5.3).
//
// Every send has to bump `act` on every member's row for the chat list to
// reorder, and for a group of 1 024 that is 1 024 index updates per message —
// the whole cost of the send path for a busy group. The gate lets one send per
// window per conversation do it: a Redis `SET NX` with an expiry decides, so the
// chat list can lag a busy group by at most the window, which is invisible on a
// screen that sorts by the minute.
//
// It fails OPEN: when Redis cannot be asked, every send bumps. A chat list that
// stops reordering is a visible fault; a write amplification for the length of
// an outage is not.

#include <chrono>
#include <functional>

#include "anvil/core/types.h"

namespace anvil::chat {

inline constexpr std::chrono::milliseconds kDefaultActivityWindow{2000};

// For ChatHooks::claim_activity_bump. Blocking (one Redis round trip), and
// therefore called only where the send already is: on db_pool.
[[nodiscard]] std::function<bool(const Uuid& conversation)> redis_activity_gate(
    std::chrono::milliseconds window = kDefaultActivityWindow);

}  // namespace anvil::chat
