#include "anvil/chat/frames.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::chat::frames {
namespace {

constexpr std::uint8_t kDownstreamFirst = 0x01;
constexpr std::uint8_t kDownstreamLast = 0x3F;
constexpr std::uint8_t kUpstreamFirst = 0x40;
constexpr std::uint8_t kUpstreamLast = 0x7F;

[[nodiscard]] Failure refuse(std::string_view fault) noexcept {
    return Failure{ErrorCode::ValidationFailed, fault};
}

// Reads are unchecked: every caller has compared the frame's whole length
// against its type's exact size before the first field is read, so a bounds
// check per field would be the same comparison made again.
[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* at) noexcept {
    return static_cast<std::uint16_t>((at[0] << 8) | at[1]);
}

[[nodiscard]] std::int64_t read_i64(const std::uint8_t* at) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) { value = (value << 8) | at[i]; }
    // Two's complement by definition since C++20, so the conversion is exact.
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] Uuid read_uuid(const std::uint8_t* at) noexcept {
    Uuid id{};
    std::copy_n(at, id.size(), id.begin());
    return id;
}

// A bump writer over a buffer the caller has already checked is large enough.
class Writer final {
public:
    explicit Writer(std::uint8_t* at) noexcept : at_{at}, written_{0} {}

    void u8(std::uint8_t value) noexcept { at_[written_++] = value; }

    void u16(std::uint16_t value) noexcept {
        u8(static_cast<std::uint8_t>(value >> 8));
        u8(static_cast<std::uint8_t>(value & 0xFF));
    }

    void i64(std::int64_t value) noexcept {
        const auto bits = static_cast<std::uint64_t>(value);
        for (int shift = 56; shift >= 0; shift -= 8) {
            u8(static_cast<std::uint8_t>((bits >> shift) & 0xFF));
        }
    }

    void bytes(std::span<const std::uint8_t> value) noexcept {
        std::copy(value.begin(), value.end(), at_ + written_);
        written_ += value.size();
    }

    void header(FrameType type) noexcept {
        u8(kFrameVersion);
        u8(static_cast<std::uint8_t>(type));
    }

    [[nodiscard]] std::size_t written() const noexcept { return written_; }

private:
    std::uint8_t* at_;
    std::size_t written_;
};

// The shared opening of both decoders, in the order the header promises: the
// bound before anything, the header's length before the header, the version
// before the type.
[[nodiscard]] Status check_header(std::span<const std::uint8_t> bytes,
                                  std::size_t bound) noexcept {
    if (bytes.size() > bound) {
        return Failure{ErrorCode::PayloadTooLarge, kFaultSize};
    }
    if (bytes.size() < kHeaderBytes) { return refuse(kFaultLength); }
    if (bytes[0] != kFrameVersion) { return refuse(kFaultVersion); }
    return ok();
}

// Whatever was accepted must be exactly what it encodes to. The rules give a
// refusal its name; this comparison is what makes the list of rules complete,
// and it costs a stack buffer and a memcmp of at most two kilobytes.
template <typename Frame, std::size_t N>
[[nodiscard]] bool reencodes(const Frame& frame, std::span<const std::uint8_t> bytes) noexcept {
    std::array<std::uint8_t, N> again{};
    const std::size_t written = encode(frame, again);
    return written == bytes.size() && std::equal(bytes.begin(), bytes.end(), again.begin());
}

[[nodiscard]] Result<DownstreamFrame> decode_downstream_body(
    std::uint8_t type, std::span<const std::uint8_t> bytes) noexcept {
    const std::uint8_t* body = bytes.data() + kHeaderBytes;
    switch (static_cast<FrameType>(type)) {
        case FrameType::Wake: {
            if (bytes.size() < kWakeFixedBytes) { return refuse(kFaultLength); }
            const std::size_t inline_len = read_u16(body + 24);
            // Over the cap is its own name even when the body happens to carry
            // that many bytes, because the sender broke a different rule.
            if (inline_len > kInlineWakeBytes) { return refuse(kFaultInline); }
            if (bytes.size() != kWakeFixedBytes + inline_len) { return refuse(kFaultLength); }
            const Wake wake{.inline_message = bytes.subspan(kWakeFixedBytes, inline_len),
                            .conversation = read_uuid(body),
                            .seq = read_i64(body + 16)};
            if (is_nil(wake.conversation)) { return refuse(kFaultNil); }
            if (wake.seq < 0) { return refuse(kFaultValue); }
            return DownstreamFrame{wake};
        }
        case FrameType::Typing: {
            if (bytes.size() != kTypingBytes) { return refuse(kFaultLength); }
            const Typing typing{.conversation = read_uuid(body), .user = read_uuid(body + 16)};
            if (is_nil(typing.conversation) || is_nil(typing.user)) {
                return refuse(kFaultNil);
            }
            return DownstreamFrame{typing};
        }
        case FrameType::Presence: {
            if (bytes.size() != kPresenceBytes) { return refuse(kFaultLength); }
            const std::uint8_t state = body[16];
            // A state this build does not know is a state the next build means
            // something by, and reading it as either of these would be a guess.
            if (state > static_cast<std::uint8_t>(PresenceState::Online)) {
                return refuse(kFaultValue);
            }
            const Presence presence{.user = read_uuid(body),
                                    .last_seen_unix_ms = read_i64(body + 17),
                                    .state = static_cast<PresenceState>(state)};
            if (is_nil(presence.user)) { return refuse(kFaultNil); }
            if (presence.last_seen_unix_ms < 0) { return refuse(kFaultValue); }
            return DownstreamFrame{presence};
        }
        case FrameType::Receipt: {
            if (bytes.size() != kReceiptBytes) { return refuse(kFaultLength); }
            const Receipt receipt{.conversation = read_uuid(body),
                                  .user = read_uuid(body + 16),
                                  .delivered_seq = read_i64(body + 32),
                                  .read_seq = read_i64(body + 40)};
            if (is_nil(receipt.conversation) || is_nil(receipt.user)) {
                return refuse(kFaultNil);
            }
            if (receipt.read_seq < 0 || receipt.read_seq > receipt.delivered_seq) {
                return refuse(kFaultValue);
            }
            return DownstreamFrame{receipt};
        }
        case FrameType::Membership: {
            if (bytes.size() != kMembershipBytes) { return refuse(kFaultLength); }
            const Membership membership{.conversation = read_uuid(body),
                                        .membership_version = read_i64(body + 16)};
            if (is_nil(membership.conversation)) { return refuse(kFaultNil); }
            if (membership.membership_version < 0) { return refuse(kFaultValue); }
            return DownstreamFrame{membership};
        }
        case FrameType::Mutation: {
            if (bytes.size() != kMutationBytes) { return refuse(kFaultLength); }
            const Mutation mutation{.conversation = read_uuid(body),
                                    .mutation = read_i64(body + 16)};
            if (is_nil(mutation.conversation)) { return refuse(kFaultNil); }
            // The counter starts at zero and a mutation moves it first, so a
            // frame announcing zero announces nothing.
            if (mutation.mutation < 1) { return refuse(kFaultValue); }
            return DownstreamFrame{mutation};
        }
        case FrameType::Ping: {
            if (bytes.size() != kPingBytes) { return refuse(kFaultLength); }
            return DownstreamFrame{Ping{}};
        }
        case FrameType::Pong: {
            if (bytes.size() != kPongBytes) { return refuse(kFaultLength); }
            return DownstreamFrame{Pong{}};
        }
        case FrameType::Sync: {
            if (bytes.size() != kSyncBytes) { return refuse(kFaultLength); }
            return DownstreamFrame{Sync{}};
        }
        default:
            break;
    }
    return refuse(kFaultType);
}

[[nodiscard]] Result<UpstreamFrame> decode_upstream_body(
    std::uint8_t type, std::span<const std::uint8_t> bytes) noexcept {
    const std::uint8_t* body = bytes.data() + kHeaderBytes;
    switch (static_cast<FrameType>(type)) {
        case FrameType::ClientTyping: {
            if (bytes.size() != kClientTypingBytes) { return refuse(kFaultLength); }
            const ClientTyping typing{.conversation = read_uuid(body)};
            if (is_nil(typing.conversation)) { return refuse(kFaultNil); }
            return UpstreamFrame{typing};
        }
        case FrameType::ClientPong: {
            if (bytes.size() != kClientPongBytes) { return refuse(kFaultLength); }
            return UpstreamFrame{ClientPong{}};
        }
        case FrameType::ClientPing: {
            if (bytes.size() != kClientPingBytes) { return refuse(kFaultLength); }
            return UpstreamFrame{ClientPing{}};
        }
        default:
            break;
    }
    return refuse(kFaultType);
}

}  // namespace

Result<DownstreamFrame> decode_downstream(std::span<const std::uint8_t> bytes) noexcept {
    if (const Status header = check_header(bytes, kMaxDownstreamFrameBytes); !header) {
        return header.error();
    }
    const std::uint8_t type = bytes[1];
    if (type >= kUpstreamFirst && type <= kUpstreamLast) { return refuse(kFaultDirection); }
    if (type < kDownstreamFirst || type > kDownstreamLast) { return refuse(kFaultType); }

    Result<DownstreamFrame> frame = decode_downstream_body(type, bytes);
    if (!frame) { return frame; }
    const bool same = std::visit(
        [bytes](const auto& f) noexcept {
            return reencodes<std::decay_t<decltype(f)>, kMaxDownstreamFrameBytes>(f, bytes);
        },
        frame.value());
    if (!same) { return refuse(kFaultCanonical); }
    return frame;
}

Result<UpstreamFrame> decode_upstream(std::span<const std::uint8_t> bytes) noexcept {
    if (const Status header = check_header(bytes, kMaxUpstreamFrameBytes); !header) {
        return header.error();
    }
    const std::uint8_t type = bytes[1];
    if (type >= kDownstreamFirst && type <= kDownstreamLast) { return refuse(kFaultDirection); }
    if (type < kUpstreamFirst || type > kUpstreamLast) { return refuse(kFaultType); }

    Result<UpstreamFrame> frame = decode_upstream_body(type, bytes);
    if (!frame) { return frame; }
    const bool same = std::visit(
        [bytes](const auto& f) noexcept {
            return reencodes<std::decay_t<decltype(f)>, kMaxUpstreamFrameBytes>(f, bytes);
        },
        frame.value());
    if (!same) { return refuse(kFaultCanonical); }
    return frame;
}

// --- encode ----------------------------------------------------------------------

std::size_t encode(const Wake& frame, std::span<std::uint8_t> out) noexcept {
    if (frame.inline_message.size() > kInlineWakeBytes) { return 0; }
    if (is_nil(frame.conversation) || frame.seq < 0) { return 0; }
    if (out.size() < kWakeFixedBytes + frame.inline_message.size()) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Wake);
    w.bytes(frame.conversation);
    w.i64(frame.seq);
    w.u16(static_cast<std::uint16_t>(frame.inline_message.size()));
    w.bytes(frame.inline_message);
    return w.written();
}

std::size_t encode(const Typing& frame, std::span<std::uint8_t> out) noexcept {
    if (is_nil(frame.conversation) || is_nil(frame.user)) { return 0; }
    if (out.size() < kTypingBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Typing);
    w.bytes(frame.conversation);
    w.bytes(frame.user);
    return w.written();
}

std::size_t encode(const Presence& frame, std::span<std::uint8_t> out) noexcept {
    const auto state = static_cast<std::uint8_t>(frame.state);
    if (is_nil(frame.user) || frame.last_seen_unix_ms < 0 ||
        state > static_cast<std::uint8_t>(PresenceState::Online)) {
        return 0;
    }
    if (out.size() < kPresenceBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Presence);
    w.bytes(frame.user);
    w.u8(state);
    w.i64(frame.last_seen_unix_ms);
    return w.written();
}

std::size_t encode(const Receipt& frame, std::span<std::uint8_t> out) noexcept {
    if (is_nil(frame.conversation) || is_nil(frame.user)) { return 0; }
    if (frame.read_seq < 0 || frame.read_seq > frame.delivered_seq) { return 0; }
    if (out.size() < kReceiptBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Receipt);
    w.bytes(frame.conversation);
    w.bytes(frame.user);
    w.i64(frame.delivered_seq);
    w.i64(frame.read_seq);
    return w.written();
}

std::size_t encode(const Membership& frame, std::span<std::uint8_t> out) noexcept {
    if (is_nil(frame.conversation) || frame.membership_version < 0) { return 0; }
    if (out.size() < kMembershipBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Membership);
    w.bytes(frame.conversation);
    w.i64(frame.membership_version);
    return w.written();
}

std::size_t encode(const Mutation& frame, std::span<std::uint8_t> out) noexcept {
    if (is_nil(frame.conversation) || frame.mutation < 1) { return 0; }
    if (out.size() < kMutationBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Mutation);
    w.bytes(frame.conversation);
    w.i64(frame.mutation);
    return w.written();
}

std::size_t encode(const Ping& /*frame*/, std::span<std::uint8_t> out) noexcept {
    if (out.size() < kPingBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Ping);
    return w.written();
}

std::size_t encode(const Pong& /*frame*/, std::span<std::uint8_t> out) noexcept {
    if (out.size() < kPongBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Pong);
    return w.written();
}

std::size_t encode(const Sync& /*frame*/, std::span<std::uint8_t> out) noexcept {
    if (out.size() < kSyncBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::Sync);
    return w.written();
}

std::size_t encode(const ClientTyping& frame, std::span<std::uint8_t> out) noexcept {
    if (is_nil(frame.conversation)) { return 0; }
    if (out.size() < kClientTypingBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::ClientTyping);
    w.bytes(frame.conversation);
    return w.written();
}

std::size_t encode(const ClientPong& /*frame*/, std::span<std::uint8_t> out) noexcept {
    if (out.size() < kClientPongBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::ClientPong);
    return w.written();
}

std::size_t encode(const ClientPing& /*frame*/, std::span<std::uint8_t> out) noexcept {
    if (out.size() < kClientPingBytes) { return 0; }
    Writer w{out.data()};
    w.header(FrameType::ClientPing);
    return w.written();
}

std::size_t encode(const DownstreamFrame& frame, std::span<std::uint8_t> out) noexcept {
    return std::visit([out](const auto& f) noexcept { return encode(f, out); }, frame);
}

std::size_t encode(const UpstreamFrame& frame, std::span<std::uint8_t> out) noexcept {
    return std::visit([out](const auto& f) noexcept { return encode(f, out); }, frame);
}

}  // namespace anvil::chat::frames
