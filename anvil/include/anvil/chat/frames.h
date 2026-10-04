#pragma once

// The chat socket's frame grammar (docs/22-chat.md §8).
//
// Every durable write is an HTTP request; the socket carries wakes and typing
// and nothing else (§8.1). Frames are not requests — no rate-limit row, no
// idempotency key, no audit hook sees them — so the grammar is kept to what
// changes nothing: a server says "look", a client says "I am typing".
//
// Binary, big-endian, `u8 version ‖ u8 type ‖ body`, and CANONICAL: one frame
// has exactly one encoding. The decoder refuses anything that would not
// re-encode byte-identically — a trailing byte, a reserved type, a length that
// disagrees with its body. A grammar with two spellings of a frame is a grammar
// in which the server and the client eventually disagree about which one the
// other meant, and the client's decoder (hammer) is written against the bytes
// in tests/chat_frames_test.cc, not against this file.
//
// --- what a refusal means ----------------------------------------------------
//
// A bad UPSTREAM frame CLOSES the socket (`http/upgrade.h`): never an error
// frame, never a 429. Nothing a client legitimately sends can be malformed, so
// a malformed frame is a broken or hostile client, and a client that is closed
// reconnects. The refusal's name is a compile-time constant so the caller can
// count closes by reason without ever logging a byte of the frame.
//
// Decode is noexcept and allocation-free: it refuses on length before it reads
// anything else, and a frame over its direction's bound is refused before the
// version byte is looked at.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/http/upgrade.h"

namespace anvil::chat::frames {

inline constexpr std::uint8_t kFrameVersion = 1;

// A wake carries the message inline when it is at most this many bytes, so a
// busy group's members do not each turn a wake into a database read (§5.4).
// Zero inline bytes means "fetch it".
inline constexpr std::size_t kInlineWakeBytes = 2048;

// The type byte. Downstream (server to client) is 0x01–0x3F and upstream
// (client to server) is 0x40–0x7F, so the direction of an unknown type is
// still known: a server type arriving at the server is a client replaying what
// it was sent, which is a different bug from a type nobody defined.
enum class FrameType : std::uint8_t {
    Wake = 0x01,
    Typing = 0x02,
    Presence = 0x03,
    Receipt = 0x04,
    Membership = 0x05,
    Ping = 0x06,
    Pong = 0x07,
    Sync = 0x08,
    Mutation = 0x09,

    ClientTyping = 0x41,
    ClientPong = 0x42,
    ClientPing = 0x43,
};

// --- refusals ------------------------------------------------------------------
//
// Carried in Failure::field. The code is PayloadTooLarge for kFaultSize and
// ValidationFailed for every other.

// Over the direction's bound, before anything was read.
inline constexpr std::string_view kFaultSize = "frame.size";
// Shorter than the header or the type's body, a trailing byte, or a wake whose
// inline length disagrees with the bytes after it.
inline constexpr std::string_view kFaultLength = "frame.length";
inline constexpr std::string_view kFaultVersion = "frame.version";
// No such type in this direction's range.
inline constexpr std::string_view kFaultType = "frame.type";
// A type from the other direction's range.
inline constexpr std::string_view kFaultDirection = "frame.direction";
// An inline message over kInlineWakeBytes.
inline constexpr std::string_view kFaultInline = "frame.inline";
// A nil conversation or user id: every id in a frame names something.
inline constexpr std::string_view kFaultNil = "frame.nil";
// A negative counter or timestamp, an unknown presence state, read > delivered,
// a mutation counter below one.
inline constexpr std::string_view kFaultValue = "frame.value";
// The backstop: what was read does not re-encode to what was sent. Every rule
// above names its own refusal first, so this one firing means a rule is missing.
inline constexpr std::string_view kFaultCanonical = "frame.canonical";

// --- downstream ------------------------------------------------------------------

// A message at `seq` in `conversation`. `inline_message` is the message as the
// HTTP API renders it — opaque bytes here — or empty, meaning "fetch it".
//
// LIFETIME: a decoded wake's `inline_message` is a view INTO THE BUFFER THAT
// WAS DECODED. It is valid exactly as long as that buffer, and a wake that
// outlives it — queued, posted to another thread, kept past the callback that
// delivered the frame — must copy the bytes first (CLAUDE.md §2.2).
struct Wake final {
    std::span<const std::uint8_t> inline_message;  // 16
    Uuid conversation;                             // 16
    std::int64_t seq;                              //  8
};

// Someone other than the reader is typing in `conversation`.
struct Typing final {
    Uuid conversation;
    Uuid user;
};

enum class PresenceState : std::uint8_t { Offline = 0, Online = 1 };

struct Presence final {
    Uuid user;
    // Milliseconds since the Unix epoch; zero when the presence hook withholds
    // it, which is a different answer from "never seen" and is not one this
    // frame can give.
    std::int64_t last_seen_unix_ms;
    PresenceState state;
};

// A member's delivered and read cursors. read <= delivered, because a message
// cannot be read on a device it has not reached.
struct Receipt final {
    Uuid conversation;
    Uuid user;
    std::int64_t delivered_seq;
    std::int64_t read_seq;
};

// The member list changed; a client holding a list at a lower version fetches
// it again. The version and not the change, so the socket never carries who
// joined or left — that is a read the HTTP route authorises.
struct Membership final {
    Uuid conversation;
    std::int64_t membership_version;
};

struct Ping final {};

// The answer to a ClientPing. A browser cannot send a WebSocket control ping,
// so a client that wants to know its socket is alive asks with a frame, and a
// question with no answer would leave it timing out a socket that is fine.
struct Pong final {};

// "You may have missed wakes: catch up from your cursors." Sent when the
// process's subscription to the reader's wake channel is confirmed — the
// first time, and after every reconnect to Redis — because wakes published
// before that moment were never delivered and nothing else says so
// (chat/wakes.h). It carries nothing: a client that syncs on it asks
// `seq > cursor` for each conversation, which is what it does on reconnecting.
struct Sync final {};

// A message in `conversation` the reader may hold was edited, revoked or
// reacted to, and the conversation's mutation counter is now at least
// `mutation` (docs/22 §4.5). The counter and not the message: a client catches
// up with `changed_after=` its own mutation cursor, which is what it does on a
// Sync as well, so a lost frame costs latency and never a change. No seq, so
// the frame says nothing about which message, which the catch-up authorises.
struct Mutation final {
    Uuid conversation;
    std::int64_t mutation;
};

using DownstreamFrame =
    std::variant<Wake, Typing, Presence, Receipt, Membership, Ping, Pong, Sync, Mutation>;

// --- upstream ------------------------------------------------------------------

// The sender is typing in `conversation`. The hub republishes it as a Typing
// wake; whether the sender is a member is the hub's question, not the codec's.
struct ClientTyping final {
    Uuid conversation;
};

struct ClientPong final {};

// The client asking whether the socket is alive, because a browser cannot send
// a WebSocket control ping. How the server answers is the hub's decision; this
// codec only reads and writes the frame.
struct ClientPing final {};

using UpstreamFrame = std::variant<ClientTyping, ClientPong, ClientPing>;

// --- sizes ---------------------------------------------------------------------

inline constexpr std::size_t kHeaderBytes = 2;
inline constexpr std::size_t kWakeFixedBytes = kHeaderBytes + 16 + 8 + 2;
inline constexpr std::size_t kTypingBytes = kHeaderBytes + 16 + 16;
inline constexpr std::size_t kPresenceBytes = kHeaderBytes + 16 + 1 + 8;
inline constexpr std::size_t kReceiptBytes = kHeaderBytes + 16 + 16 + 8 + 8;
inline constexpr std::size_t kMembershipBytes = kHeaderBytes + 16 + 8;
inline constexpr std::size_t kPingBytes = kHeaderBytes;
inline constexpr std::size_t kPongBytes = kHeaderBytes;
inline constexpr std::size_t kSyncBytes = kHeaderBytes;
inline constexpr std::size_t kMutationBytes = kHeaderBytes + 16 + 8;
inline constexpr std::size_t kClientTypingBytes = kHeaderBytes + 16;
inline constexpr std::size_t kClientPongBytes = kHeaderBytes;
inline constexpr std::size_t kClientPingBytes = kHeaderBytes;

// The largest frame each direction can carry: a stack buffer of this size holds
// any frame, and anything longer is refused before it is read.
inline constexpr std::size_t kMaxDownstreamFrameBytes = kWakeFixedBytes + kInlineWakeBytes;
inline constexpr std::size_t kMaxUpstreamFrameBytes = kClientTypingBytes;

static_assert(kMaxDownstreamFrameBytes == 2076);
static_assert(kMaxUpstreamFrameBytes == 18);
static_assert(kTypingBytes <= kMaxDownstreamFrameBytes &&
              kPresenceBytes <= kMaxDownstreamFrameBytes &&
              kReceiptBytes <= kMaxDownstreamFrameBytes &&
              kMembershipBytes <= kMaxDownstreamFrameBytes &&
              kMutationBytes <= kMaxDownstreamFrameBytes);
static_assert(kClientPongBytes <= kMaxUpstreamFrameBytes &&
              kClientPingBytes <= kMaxUpstreamFrameBytes);
// Every frame fits the socket's budget, so the upgrade layer's size close and
// this codec's never disagree about a frame either of them produced.
static_assert(kMaxDownstreamFrameBytes <= http::kMaxFrameBytes);
static_assert(kMaxUpstreamFrameBytes <= http::kMaxFrameBytes);
// The inline length travels as a u16.
static_assert(kInlineWakeBytes <= 0xFFFF);

// A decoded frame is a stack value returned and passed by value, so its size is
// asserted: a field added to one alternative grows every one of them.
static_assert(sizeof(Wake) == 40);
static_assert(sizeof(Receipt) == 48);
static_assert(sizeof(DownstreamFrame) == 56);
static_assert(std::is_trivially_copyable_v<DownstreamFrame>);
static_assert(std::is_trivially_copyable_v<UpstreamFrame>);

// --- the codec -----------------------------------------------------------------

[[nodiscard]] Result<DownstreamFrame> decode_downstream(
    std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] Result<UpstreamFrame> decode_upstream(
    std::span<const std::uint8_t> bytes) noexcept;

// The canonical bytes of one frame, written to the front of `out`. Returns the
// number written, or 0 — never a valid frame length — when `out` is too small
// or the frame breaks a rule the decoder enforces. The second case is a
// programming error on the sending side, and refusing it here means the
// server never emits a frame the client is obliged to close on.
[[nodiscard]] std::size_t encode(const Wake& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Typing& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Presence& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Receipt& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Membership& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Ping& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Pong& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Sync& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const Mutation& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const ClientTyping& frame,
                                 std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const ClientPong& frame, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const ClientPing& frame, std::span<std::uint8_t> out) noexcept;

// Either variant, dispatched. For the hub, which holds a decoded frame and
// writes it on.
[[nodiscard]] std::size_t encode(const DownstreamFrame& frame,
                                 std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::size_t encode(const UpstreamFrame& frame,
                                 std::span<std::uint8_t> out) noexcept;

}  // namespace anvil::chat::frames
