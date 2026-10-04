#pragma once

// The chat socket's frame grammar as golden vectors (docs/22-chat.md §8.2):
// every frame's bytes, and every refusal the decoder makes with the name it
// makes it by.
//
// tests/chat_frames_test.cc holds anvil's codec to every entry here, and
// testapp_emit_chat_frames prints them as the fixture hammer's codec is held
// to, so the two codecs are checked against one file rather than two copies
// of one belief. A change here is a change to the wire, made in both codecs at
// once.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/chat/frames.h"
#include "anvil/core/types.h"

namespace testapp::frame_vectors {

namespace frames = anvil::chat::frames;

// One field per line, so a reader checks the layout against the header by eye.
// clang-format off
inline constexpr std::array<std::uint8_t, 30> kWakeInline{{
    0x01, 0x01,                                         // version 1, Wake
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,     // seq 42
    0x00, 0x02,                                         // inline length 2
    0x68, 0x69,                                         // "hi"
}};
inline constexpr std::array<std::uint8_t, 28> kWakeFetch{{
    0x01, 0x01,                                         // version 1, Wake
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01,     // seq 2^40 + 1
    0x00, 0x00,                                         // inline length 0: fetch it
}};
inline constexpr std::array<std::uint8_t, 34> kTyping{{
    0x01, 0x02,                                         // version 1, Typing
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,     // user
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
}};
inline constexpr std::array<std::uint8_t, 27> kPresence{{
    0x01, 0x03,                                         // version 1, Presence
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,     // user
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x01,                                               // online
    0x00, 0x00, 0x01, 0x92, 0x3A, 0xBC, 0xDE, 0xF0,     // last seen, unix ms
}};
inline constexpr std::array<std::uint8_t, 50> kReceipt{{
    0x01, 0x04,                                         // version 1, Receipt
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,     // user
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,     // delivered 9
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,     // read 7
}};
inline constexpr std::array<std::uint8_t, 26> kMembership{{
    0x01, 0x05,                                         // version 1, Membership
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,     // membership version 3
}};
inline constexpr std::array<std::uint8_t, 2> kPing{{0x01, 0x06}};      // version 1, Ping
inline constexpr std::array<std::uint8_t, 2> kPong{{0x01, 0x07}};      // version 1, Pong
inline constexpr std::array<std::uint8_t, 2> kSync{{0x01, 0x08}};      // version 1, Sync
inline constexpr std::array<std::uint8_t, 26> kMutation{{
    0x01, 0x09,                                         // version 1, Mutation
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05,     // mutation 261
}};
inline constexpr std::array<std::uint8_t, 18> kClientTyping{{
    0x01, 0x41,                                         // version 1, ClientTyping
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
}};
inline constexpr std::array<std::uint8_t, 2> kClientPong{{0x01, 0x42}}; // version 1, ClientPong
inline constexpr std::array<std::uint8_t, 2> kClientPing{{0x01, 0x43}}; // version 1, ClientPing
// clang-format on

enum class Direction : std::uint8_t { Down, Up };

[[nodiscard]] constexpr std::string_view direction_name(Direction d) noexcept {
    return d == Direction::Down ? "down" : "up";
}

struct Golden final {
    std::string_view              name;
    std::span<const std::uint8_t> bytes;
    Direction                     direction;
};

inline constexpr std::array<Golden, 13> kGoldens{{
    {"wake_inline", kWakeInline, Direction::Down},
    {"wake_fetch", kWakeFetch, Direction::Down},
    {"typing", kTyping, Direction::Down},
    {"presence", kPresence, Direction::Down},
    {"receipt", kReceipt, Direction::Down},
    {"membership", kMembership, Direction::Down},
    {"ping", kPing, Direction::Down},
    {"pong", kPong, Direction::Down},
    {"sync", kSync, Direction::Down},
    {"mutation", kMutation, Direction::Down},
    {"client_typing", kClientTyping, Direction::Up},
    {"client_pong", kClientPong, Direction::Up},
    {"client_ping", kClientPing, Direction::Up},
}};

// The largest downstream frame: a wake whose inline message is exactly
// kInlineWakeBytes of 'A'. Built rather than spelled out.
[[nodiscard]] inline std::vector<std::uint8_t> largest_wake() {
    std::vector<std::uint8_t> out(kWakeFetch.begin(), kWakeFetch.end());
    out[26] = static_cast<std::uint8_t>(frames::kInlineWakeBytes >> 8U);
    out[27] = static_cast<std::uint8_t>(frames::kInlineWakeBytes & 0xFFU);
    out.resize(frames::kMaxDownstreamFrameBytes, 0x41);
    return out;
}

// One frame a decoder must refuse, and the name and code it refuses it with.
struct Refusal final {
    std::string               name;
    std::vector<std::uint8_t> bytes;
    std::string_view          fault;
    anvil::ErrorCode          code;
    Direction                 direction;
};

[[nodiscard]] inline std::vector<std::uint8_t> copy(std::span<const std::uint8_t> bytes) {
    return {bytes.begin(), bytes.end()};
}

// Every refusal the frame tests name, case by case, then the two families
// that run over every golden frame: each short prefix, and one trailing byte.
[[nodiscard]] inline std::vector<Refusal> refusals() {
    using anvil::ErrorCode;
    std::vector<Refusal> out;
    const auto add = [&](std::string name, std::vector<std::uint8_t> bytes, Direction d,
                         std::string_view fault,
                         ErrorCode code = ErrorCode::ValidationFailed) {
        out.push_back(Refusal{std::move(name), std::move(bytes), fault, code, d});
    };
    const auto hex2 = [](std::uint8_t b) {
        constexpr std::string_view digits = "0123456789abcdef";
        return std::string{digits[b >> 4U], digits[b & 0xFU]};
    };

    for (const std::uint8_t version : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{0xFF}}) {
        std::vector<std::uint8_t> down = copy(kTyping);
        down[0] = version;
        add("version_" + hex2(version) + "_typing", down, Direction::Down, frames::kFaultVersion);
        std::vector<std::uint8_t> up = copy(kClientTyping);
        up[0] = version;
        add("version_" + hex2(version) + "_client_typing", up, Direction::Up,
            frames::kFaultVersion);
    }
    for (const std::uint8_t type : {std::uint8_t{0x00}, std::uint8_t{0x0A}, std::uint8_t{0x3F},
                                    std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
        add("type_" + hex2(type) + "_down", {0x01, type}, Direction::Down, frames::kFaultType);
    }
    for (const std::uint8_t type : {std::uint8_t{0x40}, std::uint8_t{0x44}, std::uint8_t{0x7F},
                                    std::uint8_t{0x80}, std::uint8_t{0x00}}) {
        add("type_" + hex2(type) + "_up", {0x01, type}, Direction::Up, frames::kFaultType);
    }
    add("client_typing_read_down", copy(kClientTyping), Direction::Down, frames::kFaultDirection);
    add("client_pong_read_down", copy(kClientPong), Direction::Down, frames::kFaultDirection);
    add("client_ping_read_down", copy(kClientPing), Direction::Down, frames::kFaultDirection);
    add("ping_read_up", copy(kPing), Direction::Up, frames::kFaultDirection);
    add("pong_read_up", copy(kPong), Direction::Up, frames::kFaultDirection);
    add("sync_read_up", copy(kSync), Direction::Up, frames::kFaultDirection);
    // Longer than the upstream bound, so it is the size that refuses it.
    add("membership_read_up", copy(kMembership), Direction::Up, frames::kFaultSize,
        ErrorCode::PayloadTooLarge);

    {
        std::vector<std::uint8_t> longer = copy(kWakeInline);
        longer[27] = 0x03;  // claims three, carries two
        add("wake_inline_length_claims_more", longer, Direction::Down, frames::kFaultLength);
        std::vector<std::uint8_t> shorter = copy(kWakeInline);
        shorter[27] = 0x01;  // claims one, carries two
        add("wake_inline_length_claims_less", shorter, Direction::Down, frames::kFaultLength);
        std::vector<std::uint8_t> claim = copy(kWakeFetch);
        claim[26] = 0x08;
        claim[27] = 0x01;  // 2049, carrying none
        add("wake_inline_over_cap", claim, Direction::Down, frames::kFaultInline);
        std::vector<std::uint8_t> oversized = claim;
        oversized.resize(frames::kWakeFixedBytes + frames::kInlineWakeBytes + 1, 0x41);
        add("wake_inline_over_bound", oversized, Direction::Down, frames::kFaultSize,
            ErrorCode::PayloadTooLarge);
    }
    for (const std::uint8_t state : {std::uint8_t{0x02}, std::uint8_t{0xFF}}) {
        std::vector<std::uint8_t> bytes = copy(kPresence);
        bytes[18] = state;
        add("presence_state_" + hex2(state), bytes, Direction::Down, frames::kFaultValue);
    }
    {
        std::vector<std::uint8_t> seen = copy(kPresence);
        seen[19] = 0x80;
        add("presence_last_seen_negative", seen, Direction::Down, frames::kFaultValue);
        std::vector<std::uint8_t> read = copy(kReceipt);
        read[49] = 0x0A;  // read 10, delivered 9
        add("receipt_read_past_delivered", read, Direction::Down, frames::kFaultValue);
        std::vector<std::uint8_t> wake = copy(kWakeFetch);
        wake[18] = 0x80;
        add("wake_seq_negative", wake, Direction::Down, frames::kFaultValue);
        std::vector<std::uint8_t> membership = copy(kMembership);
        membership[18] = 0xFF;
        add("membership_version_negative", membership, Direction::Down, frames::kFaultValue);
        std::vector<std::uint8_t> receipt = copy(kReceipt);
        for (std::size_t i = 34; i < receipt.size(); ++i) { receipt[i] = 0xFF; }
        add("receipt_both_negative", receipt, Direction::Down, frames::kFaultValue);
        std::vector<std::uint8_t> zero = copy(kMutation);
        for (std::size_t i = 18; i < zero.size(); ++i) { zero[i] = 0x00; }
        add("mutation_zero", zero, Direction::Down, frames::kFaultValue);
        std::vector<std::uint8_t> negative = copy(kMutation);
        negative[18] = 0x80;
        add("mutation_negative", negative, Direction::Down, frames::kFaultValue);
    }
    for (const Golden& golden :
         {kGoldens[0], kGoldens[2], kGoldens[4], kGoldens[5], kGoldens[9]}) {
        std::vector<std::uint8_t> bytes = copy(golden.bytes);
        for (std::size_t i = 2; i < 18; ++i) { bytes[i] = 0x00; }
        add(std::string{golden.name} + "_nil_conversation", bytes, Direction::Down,
            frames::kFaultNil);
    }
    {
        std::vector<std::uint8_t> up = copy(kClientTyping);
        for (std::size_t i = 2; i < up.size(); ++i) { up[i] = 0x00; }
        add("client_typing_nil_conversation", up, Direction::Up, frames::kFaultNil);
        std::vector<std::uint8_t> typing = copy(kTyping);
        for (std::size_t i = 18; i < typing.size(); ++i) { typing[i] = 0x00; }
        add("typing_nil_user", typing, Direction::Down, frames::kFaultNil);
        std::vector<std::uint8_t> presence = copy(kPresence);
        for (std::size_t i = 2; i < 18; ++i) { presence[i] = 0x00; }
        add("presence_nil_user", presence, Direction::Down, frames::kFaultNil);
    }
    {
        std::vector<std::uint8_t> down(frames::kMaxDownstreamFrameBytes + 1, 0x00);
        add("down_over_bound_garbage", down, Direction::Down, frames::kFaultSize,
            ErrorCode::PayloadTooLarge);
        down[0] = 0x01;
        down[1] = 0x01;
        add("down_over_bound_wake_header", down, Direction::Down, frames::kFaultSize,
            ErrorCode::PayloadTooLarge);
        add("up_over_bound", std::vector<std::uint8_t>(frames::kMaxUpstreamFrameBytes + 1, 0xFF),
            Direction::Up, frames::kFaultSize, ErrorCode::PayloadTooLarge);
    }
    // The families. A trailing byte is a length refusal, except past an
    // upstream frame already at its direction's bound, where it is the size.
    for (const Golden& golden : kGoldens) {
        for (std::size_t n = 0; n < golden.bytes.size(); ++n) {
            add(std::string{golden.name} + "_prefix_" + std::to_string(n),
                std::vector<std::uint8_t>(golden.bytes.begin(),
                                          golden.bytes.begin() + static_cast<std::ptrdiff_t>(n)),
                golden.direction, frames::kFaultLength);
        }
        std::vector<std::uint8_t> trailing = copy(golden.bytes);
        trailing.push_back(0x00);
        const bool at_bound = golden.direction == Direction::Up &&
                              golden.bytes.size() == frames::kMaxUpstreamFrameBytes;
        add(std::string{golden.name} + "_trailing_byte", trailing, golden.direction,
            at_bound ? frames::kFaultSize : frames::kFaultLength,
            at_bound ? ErrorCode::PayloadTooLarge : ErrorCode::ValidationFailed);
    }
    return out;
}

}  // namespace testapp::frame_vectors
