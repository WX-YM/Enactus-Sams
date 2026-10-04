#pragma once

// The chat socket (docs/22-chat.md §8.2): the WebSocket a client holds open to
// be told, rather than to ask.
//
// Registered with register_websocket_route, so the handshake inherits
// everything a handshake has: the upgrade gate, the Origin check, the access
// filter, and the stealth 404 for a refusal (docs/04 §8.2–8.3). After the
// handshake it inherits the frame budget and the re-check (http/upgrade.h).
//
// --- it changes nothing ------------------------------------------------------
//
// The socket carries wakes down and typing up, and NO DURABLE WRITE in either
// direction. Every write is an HTTP route, because the rate limiter,
// idempotency, the stealth filter and the audit hooks see requests and never
// frames (docs/04 §8.4). The upstream grammar is ClientTyping, ClientPing and
// ClientPong, and the first of those changes nothing either.
//
// --- what a client does with it --------------------------------------------------
//
//   * On open, and on every Sync frame, catch up from its cursors: `seq > cursor`
//     per conversation over HTTP. A wake is a hint; the log is the record.
//   * On a Wake, take the inline message if there is one, else fetch `seq`.
//   * Answer every Ping with a ClientPong. A socket that stays silent for
//     kSilenceLimit is closed: Drogon exposes no write-buffer level, so a client
//     that is not reading is found by not answering (chat/hub.h).
//   * On close, reconnect with backoff — EXCEPT on kCloseReplaced, which means
//     another socket from this device took over and reconnecting would take it
//     back, and so on for ever.
//
// Every bad thing a client can do closes the socket and is answered with
// nothing: a text frame, a malformed or oversized frame, a frame past the
// budget. Nothing a correct client sends can be any of those.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/accesscontrol/route_registry.h"
#include "anvil/chat/live.h"
#include "anvil/chat/service.h"
#include "anvil/descriptor/route_description.h"
#include "anvil/http/upgrade.h"

namespace anvil::chat {

// Close codes in the 4000–4999 range RFC 6455 leaves to applications, so a
// client can tell each one from a network fault.
//
// Another socket from this device took over. Do NOT reconnect automatically.
inline constexpr std::uint16_t kCloseReplaced = 4001;
// The socket fell behind its ring (chat/hub.h). Reconnect and sync.
inline constexpr std::uint16_t kCloseOverflow = 4002;
// No frame from the client for kSilenceLimit. Reconnect and sync.
inline constexpr std::uint16_t kCloseSilent = 4003;
// The process or the account has as many sockets as it allows. Back off; the
// HTTP routes still work, and so does polling.
inline constexpr std::uint16_t kCloseFull = 4004;
// The authority the handshake proved has expired or been revoked. Reconnect,
// which re-authenticates — with a refreshed token, or not at all.
inline constexpr std::uint16_t kCloseReauthenticate = 4005;
// A frame no correct client sends: text, malformed, oversized, or past the
// budget. Reconnecting will not help a broken client; it is closed anyway.
inline constexpr std::uint16_t kCloseBadFrame = 4006;

// How often the server pings, and how long a socket may say nothing at all
// before it is closed. Twenty seconds is inside every proxy idle timeout this
// library has met; forty-five is two missed pings and a slow answer.
inline constexpr std::chrono::seconds kPingInterval{20};
inline constexpr std::chrono::seconds kSilenceLimit{45};

// Typing is relayed at most once per this long per socket per conversation
// (docs/22 §8.3); a frame inside it is dropped, not refused. The socket keeps
// the last kTypingSlots conversations it relayed for, oldest replaced first: a
// person types in one conversation at a time, and a client cycling through more
// than eight is still inside the frame budget.
inline constexpr std::chrono::seconds kTypingInterval{3};
inline constexpr std::size_t kTypingSlots = 8;

// One timer per socket ticks at the re-check period, which is the shortest of
// the three schedules it serves: the re-check (http/upgrade.h), the ping and
// the silence deadline.
inline constexpr std::chrono::seconds kSocketTick = http::kRecheckPeriod;
static_assert(kSocketTick <= kPingInterval && kPingInterval < kSilenceLimit);

// Registers the socket at the pattern the application declared for
// `route_id`, which must be a GET with no placeholders. `live` and `service`
// must outlive serving: the socket's handlers reach them through process-wide
// pointers, because Drogon builds a WebSocket controller itself and cannot be
// handed one. Throws std::invalid_argument at boot when the id is not
// described or has placeholders, and std::logic_error when the table has no
// policy for it (register_websocket_route). Call it ONCE per process.
void install_chat_socket(ChatLive& live, const ChatService& service,
                         std::span<const accesscontrol::RoutePolicy> routes,
                         std::span<const descriptor::RouteDescription> descriptions,
                         std::string_view route_id);

}  // namespace anvil::chat
