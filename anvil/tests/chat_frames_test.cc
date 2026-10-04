// The chat socket's frame grammar (docs/22-chat.md §8).
//
// The golden-bytes tests are the contract: the client's decoder is written
// against these literal arrays, so a change that moves one of them is a change
// to the wire and has to be made in both codecs at once. Every other test here
// is about the server's side of that contract — what it refuses, and by which
// name, since a bad upstream frame closes the socket and the close is counted by
// that name.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "anvil/chat/frames.h"
#include "anvil/core/types.h"
#include "testapp/chat_frame_vectors.h"

namespace {

namespace frames = anvil::chat::frames;
using anvil::ErrorCode;
using anvil::Uuid;
using Bytes = std::vector<std::uint8_t>;

// clang-format off
constexpr Uuid kConversation{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                             0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
constexpr Uuid kUser{0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                     0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
// clang-format on

constexpr std::array<std::uint8_t, 2> kInline{'h', 'i'};

template <typename Frame>
[[nodiscard]] Bytes encoded(const Frame& frame) {
    std::array<std::uint8_t, frames::kMaxDownstreamFrameBytes> buffer{};
    const std::size_t written = frames::encode(frame, buffer);
    return Bytes(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
}

void expect_downstream_refused(const Bytes& bytes,
                               std::string_view fault,
                               ErrorCode code = ErrorCode::ValidationFailed) {
    const auto decoded = frames::decode_downstream(bytes);
    ASSERT_FALSE(decoded) << "accepted a frame that should be " << fault;
    EXPECT_EQ(decoded.error().field, fault);
    EXPECT_EQ(decoded.error().code, code);
}

void expect_upstream_refused(const Bytes& bytes,
                             std::string_view fault,
                             ErrorCode code = ErrorCode::ValidationFailed) {
    const auto decoded = frames::decode_upstream(bytes);
    ASSERT_FALSE(decoded) << "accepted a frame that should be " << fault;
    EXPECT_EQ(decoded.error().field, fault);
    EXPECT_EQ(decoded.error().code, code);
}

// --- golden bytes ------------------------------------------------------------
//
// One field per line, so a reader checks the layout against the header by eye.

// clang-format off
const Bytes kWakeInline{
    0x01, 0x01,                                         // version 1, Wake
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,     // seq 42
    0x00, 0x02,                                         // inline length 2
    0x68, 0x69,                                         // "hi"
};

const Bytes kWakeFetch{
    0x01, 0x01,                                         // version 1, Wake
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01,     // seq 2^40 + 1
    0x00, 0x00,                                         // inline length 0: fetch it
};

const Bytes kTyping{
    0x01, 0x02,                                         // version 1, Typing
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,     // user
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
};

const Bytes kPresence{
    0x01, 0x03,                                         // version 1, Presence
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,     // user
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x01,                                               // online
    0x00, 0x00, 0x01, 0x92, 0x3A, 0xBC, 0xDE, 0xF0,     // last seen, unix ms
};

const Bytes kReceipt{
    0x01, 0x04,                                         // version 1, Receipt
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,     // user
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,     // delivered 9
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,     // read 7
};

const Bytes kMembership{
    0x01, 0x05,                                         // version 1, Membership
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,     // membership version 3
};

const Bytes kPing{0x01, 0x06};                          // version 1, Ping
const Bytes kPong{0x01, 0x07};                          // version 1, Pong
const Bytes kSync{0x01, 0x08};                          // version 1, Sync

const Bytes kMutation{
    0x01, 0x09,                                         // version 1, Mutation
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05,     // mutation 261
};

const Bytes kClientTyping{
    0x01, 0x41,                                         // version 1, ClientTyping
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,     // conversation
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

const Bytes kClientPong{0x01, 0x42};                    // version 1, ClientPong
const Bytes kClientPing{0x01, 0x43};                    // version 1, ClientPing
// clang-format on

TEST(ChatFrames, GoldenWakeWithItsMessageInline) {
    EXPECT_EQ(encoded(frames::Wake{
                  .inline_message = kInline, .conversation = kConversation, .seq = 42}),
              kWakeInline);
    const auto decoded = frames::decode_downstream(kWakeInline);
    ASSERT_TRUE(decoded);
    const auto& wake = std::get<frames::Wake>(decoded.value());
    EXPECT_EQ(wake.conversation, kConversation);
    EXPECT_EQ(wake.seq, 42);
    ASSERT_EQ(wake.inline_message.size(), 2U);
    EXPECT_TRUE(
        std::equal(wake.inline_message.begin(), wake.inline_message.end(), kInline.begin()));
    // A view into the decoded buffer, never a copy of it: that is what makes
    // decode allocation-free, and why the header documents the lifetime.
    EXPECT_EQ(wake.inline_message.data(), kWakeInline.data() + frames::kWakeFixedBytes);
}

TEST(ChatFrames, GoldenWakeThatSaysFetchIt) {
    const std::int64_t seq = (std::int64_t{1} << 40) + 1;
    EXPECT_EQ(
        encoded(frames::Wake{.inline_message = {}, .conversation = kConversation, .seq = seq}),
        kWakeFetch);
    const auto decoded = frames::decode_downstream(kWakeFetch);
    ASSERT_TRUE(decoded);
    const auto& wake = std::get<frames::Wake>(decoded.value());
    EXPECT_EQ(wake.seq, seq);
    EXPECT_TRUE(wake.inline_message.empty());
}

TEST(ChatFrames, GoldenTyping) {
    EXPECT_EQ(encoded(frames::Typing{.conversation = kConversation, .user = kUser}), kTyping);
    const auto decoded = frames::decode_downstream(kTyping);
    ASSERT_TRUE(decoded);
    const auto& typing = std::get<frames::Typing>(decoded.value());
    EXPECT_EQ(typing.conversation, kConversation);
    EXPECT_EQ(typing.user, kUser);
}

TEST(ChatFrames, GoldenPresence) {
    constexpr std::int64_t kSeenMs = 0x0000'0192'3ABC'DEF0;
    EXPECT_EQ(encoded(frames::Presence{.user = kUser,
                                       .last_seen_unix_ms = kSeenMs,
                                       .state = frames::PresenceState::Online}),
              kPresence);
    const auto decoded = frames::decode_downstream(kPresence);
    ASSERT_TRUE(decoded);
    const auto& presence = std::get<frames::Presence>(decoded.value());
    EXPECT_EQ(presence.user, kUser);
    EXPECT_EQ(presence.state, frames::PresenceState::Online);
    EXPECT_EQ(presence.last_seen_unix_ms, kSeenMs);
}

TEST(ChatFrames, GoldenReceipt) {
    EXPECT_EQ(
        encoded(frames::Receipt{
            .conversation = kConversation, .user = kUser, .delivered_seq = 9, .read_seq = 7}),
        kReceipt);
    const auto decoded = frames::decode_downstream(kReceipt);
    ASSERT_TRUE(decoded);
    const auto& receipt = std::get<frames::Receipt>(decoded.value());
    EXPECT_EQ(receipt.conversation, kConversation);
    EXPECT_EQ(receipt.user, kUser);
    EXPECT_EQ(receipt.delivered_seq, 9);
    EXPECT_EQ(receipt.read_seq, 7);
}

TEST(ChatFrames, GoldenMembership) {
    EXPECT_EQ(
        encoded(frames::Membership{.conversation = kConversation, .membership_version = 3}),
        kMembership);
    const auto decoded = frames::decode_downstream(kMembership);
    ASSERT_TRUE(decoded);
    const auto& membership = std::get<frames::Membership>(decoded.value());
    EXPECT_EQ(membership.conversation, kConversation);
    EXPECT_EQ(membership.membership_version, 3);
}

TEST(ChatFrames, GoldenPing) {
    EXPECT_EQ(encoded(frames::Ping{}), kPing);
    const auto decoded = frames::decode_downstream(kPing);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::holds_alternative<frames::Ping>(decoded.value()));
}

TEST(ChatFrames, GoldenPongAndSync) {
    EXPECT_EQ(encoded(frames::Pong{}), kPong);
    EXPECT_EQ(encoded(frames::Sync{}), kSync);
    const auto pong = frames::decode_downstream(kPong);
    ASSERT_TRUE(pong);
    EXPECT_TRUE(std::holds_alternative<frames::Pong>(pong.value()));
    const auto sync = frames::decode_downstream(kSync);
    ASSERT_TRUE(sync);
    EXPECT_TRUE(std::holds_alternative<frames::Sync>(sync.value()));
}

TEST(ChatFrames, GoldenMutation) {
    EXPECT_EQ(encoded(frames::Mutation{.conversation = kConversation, .mutation = 261}),
              kMutation);
    const auto decoded = frames::decode_downstream(kMutation);
    ASSERT_TRUE(decoded);
    const auto& mutation = std::get<frames::Mutation>(decoded.value());
    EXPECT_EQ(mutation.conversation, kConversation);
    EXPECT_EQ(mutation.mutation, 261);
}

TEST(ChatFrames, AMutationCounterBelowOneIsRefused) {
    // Zero announces nothing: the counter's first value is one.
    Bytes zero = kMutation;
    std::fill(zero.begin() + 18, zero.end(), 0x00);
    expect_downstream_refused(zero, frames::kFaultValue);
    Bytes negative = kMutation;
    negative[18] = 0x80;
    expect_downstream_refused(negative, frames::kFaultValue);
    EXPECT_EQ(encoded(frames::Mutation{.conversation = kConversation, .mutation = 0}).size(), 0U);
}

TEST(ChatFrames, GoldenClientTyping) {
    EXPECT_EQ(encoded(frames::ClientTyping{.conversation = kConversation}), kClientTyping);
    const auto decoded = frames::decode_upstream(kClientTyping);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<frames::ClientTyping>(decoded.value()).conversation, kConversation);
}

TEST(ChatFrames, GoldenClientPongAndPing) {
    EXPECT_EQ(encoded(frames::ClientPong{}), kClientPong);
    EXPECT_EQ(encoded(frames::ClientPing{}), kClientPing);
    const auto pong = frames::decode_upstream(kClientPong);
    ASSERT_TRUE(pong);
    EXPECT_TRUE(std::holds_alternative<frames::ClientPong>(pong.value()));
    const auto ping = frames::decode_upstream(kClientPing);
    ASSERT_TRUE(ping);
    EXPECT_TRUE(std::holds_alternative<frames::ClientPing>(ping.value()));
}

// --- round trips ---------------------------------------------------------------

TEST(ChatFrames, EveryDownstreamFrameRoundTripsThroughTheVariant) {
    const std::array<std::uint8_t, frames::kInlineWakeBytes> full{};
    const std::array<frames::DownstreamFrame, 11> cases{{
        frames::Mutation{.conversation = kConversation, .mutation = INT64_MAX},
        frames::Wake{.inline_message = kInline, .conversation = kConversation, .seq = 0},
        frames::Wake{.inline_message = full, .conversation = kConversation, .seq = INT64_MAX},
        frames::Typing{.conversation = kConversation, .user = kUser},
        frames::Presence{
            .user = kUser, .last_seen_unix_ms = 0, .state = frames::PresenceState::Offline},
        frames::Receipt{
            .conversation = kConversation, .user = kUser, .delivered_seq = 5, .read_seq = 5},
        frames::Receipt{
            .conversation = kConversation, .user = kUser, .delivered_seq = 0, .read_seq = 0},
        frames::Membership{.conversation = kConversation, .membership_version = INT64_MAX},
        frames::Ping{},
        frames::Pong{},
        frames::Sync{},
    }};
    for (const frames::DownstreamFrame& frame : cases) {
        SCOPED_TRACE(frame.index());
        std::array<std::uint8_t, frames::kMaxDownstreamFrameBytes> buffer{};
        const std::size_t written = frames::encode(frame, buffer);
        ASSERT_GT(written, 0U);
        const std::span<const std::uint8_t> bytes{buffer.data(), written};
        const auto decoded = frames::decode_downstream(bytes);
        ASSERT_TRUE(decoded) << decoded.error().field;
        ASSERT_EQ(decoded.value().index(), frame.index());
        std::array<std::uint8_t, frames::kMaxDownstreamFrameBytes> again{};
        ASSERT_EQ(frames::encode(decoded.value(), again), written);
        EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), again.begin()));
    }
}

TEST(ChatFrames, EveryUpstreamFrameRoundTripsThroughTheVariant) {
    const std::array<frames::UpstreamFrame, 3> cases{{
        frames::ClientTyping{.conversation = kConversation},
        frames::ClientPong{},
        frames::ClientPing{},
    }};
    for (const frames::UpstreamFrame& frame : cases) {
        std::array<std::uint8_t, frames::kMaxUpstreamFrameBytes> buffer{};
        const std::size_t written = frames::encode(frame, buffer);
        ASSERT_GT(written, 0U);
        const std::span<const std::uint8_t> bytes{buffer.data(), written};
        const auto decoded = frames::decode_upstream(bytes);
        ASSERT_TRUE(decoded) << decoded.error().field;
        EXPECT_EQ(decoded.value().index(), frame.index());
    }
}

TEST(ChatFrames, TheLargestFrameOfEachDirectionIsItsBound) {
    const std::array<std::uint8_t, frames::kInlineWakeBytes> full{};
    EXPECT_EQ(
        encoded(frames::Wake{.inline_message = full, .conversation = kConversation, .seq = 1})
            .size(),
        frames::kMaxDownstreamFrameBytes);
    EXPECT_EQ(kClientTyping.size(), frames::kMaxUpstreamFrameBytes);
}

// --- refusals --------------------------------------------------------------------

TEST(ChatFrames, VersionsOtherThanOneAreRefused) {
    for (const std::uint8_t version : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{0xFF}}) {
        Bytes down = kTyping;
        down[0] = version;
        expect_downstream_refused(down, frames::kFaultVersion);
        Bytes up = kClientTyping;
        up[0] = version;
        expect_upstream_refused(up, frames::kFaultVersion);
    }
}

TEST(ChatFrames, UnknownTypesAreRefused) {
    for (const std::uint8_t type : {std::uint8_t{0x00},
                                    std::uint8_t{0x0A},
                                    std::uint8_t{0x3F},
                                    std::uint8_t{0x80},
                                    std::uint8_t{0xFF}}) {
        expect_downstream_refused(Bytes{0x01, type}, frames::kFaultType);
    }
    for (const std::uint8_t type : {std::uint8_t{0x40},
                                    std::uint8_t{0x44},
                                    std::uint8_t{0x7F},
                                    std::uint8_t{0x80},
                                    std::uint8_t{0x00}}) {
        expect_upstream_refused(Bytes{0x01, type}, frames::kFaultType);
    }
}

TEST(ChatFrames, ATypeFromTheOtherDirectionIsRefusedByName) {
    expect_downstream_refused(kClientTyping, frames::kFaultDirection);
    expect_downstream_refused(kClientPong, frames::kFaultDirection);
    expect_downstream_refused(kClientPing, frames::kFaultDirection);
    // Short enough to pass the upstream bound, so the direction is what is
    // reached; the longer server frames are refused on size first.
    expect_upstream_refused(kPing, frames::kFaultDirection);
    expect_upstream_refused(kPong, frames::kFaultDirection);
    expect_upstream_refused(kSync, frames::kFaultDirection);
    expect_upstream_refused(kMembership, frames::kFaultSize, ErrorCode::PayloadTooLarge);
}

TEST(ChatFrames, ATrailingByteIsRefused) {
    for (const Bytes& golden :
         {kWakeInline, kWakeFetch, kTyping, kPresence, kReceipt, kMembership, kPing, kPong,
          kSync, kMutation}) {
        Bytes bytes = golden;
        bytes.push_back(0x00);
        expect_downstream_refused(bytes, frames::kFaultLength);
    }
    for (const Bytes& golden : {kClientPong, kClientPing}) {
        Bytes bytes = golden;
        bytes.push_back(0x00);
        expect_upstream_refused(bytes, frames::kFaultLength);
    }
    // ClientTyping is the upstream bound, so a byte past it is a size refusal.
    Bytes typing = kClientTyping;
    typing.push_back(0x00);
    expect_upstream_refused(typing, frames::kFaultSize, ErrorCode::PayloadTooLarge);
}

TEST(ChatFrames, EveryShortPrefixIsRefusedWithoutReadingPastIt) {
    for (const Bytes& golden :
         {kWakeInline, kWakeFetch, kTyping, kPresence, kReceipt, kMembership, kPing, kPong,
          kSync, kMutation}) {
        for (std::size_t n = 0; n < golden.size(); ++n) {
            SCOPED_TRACE(n);
            // A copy of exactly n bytes, so ASan sees a read past the prefix.
            const Bytes prefix(golden.begin(), golden.begin() + static_cast<std::ptrdiff_t>(n));
            expect_downstream_refused(prefix, frames::kFaultLength);
        }
    }
    for (std::size_t n = 0; n < kClientTyping.size(); ++n) {
        const Bytes prefix(kClientTyping.begin(),
                           kClientTyping.begin() + static_cast<std::ptrdiff_t>(n));
        expect_upstream_refused(prefix, frames::kFaultLength);
    }
}

TEST(ChatFrames, AWakeWhoseInlineLengthDisagreesWithItsBodyIsRefused) {
    Bytes longer = kWakeInline;
    longer[27] = 0x03;  // claims three, carries two
    expect_downstream_refused(longer, frames::kFaultLength);
    Bytes shorter = kWakeInline;
    shorter[27] = 0x01;  // claims one, carries two
    expect_downstream_refused(shorter, frames::kFaultLength);
}

TEST(ChatFrames, AnInlineMessageOverTheCapIsRefused) {
    // Exactly as long as it claims, so only the cap refuses it.
    Bytes bytes = kWakeFetch;
    bytes[26] = 0x08;
    bytes[27] = 0x01;  // 2049
    bytes.resize(frames::kWakeFixedBytes + frames::kInlineWakeBytes + 1, 0x41);
    // Over the downstream bound too, which is refused first.
    expect_downstream_refused(bytes, frames::kFaultSize, ErrorCode::PayloadTooLarge);

    // Within the bound, a claim over the cap is its own refusal.
    Bytes claim = kWakeFetch;
    claim[26] = 0x08;
    claim[27] = 0x01;
    expect_downstream_refused(claim, frames::kFaultInline);

    const std::array<std::uint8_t, frames::kInlineWakeBytes + 1> over{};
    EXPECT_EQ(
        encoded(frames::Wake{.inline_message = over, .conversation = kConversation, .seq = 1})
            .size(),
        0U);
}

TEST(ChatFrames, APresenceStateOtherThanOfflineOrOnlineIsRefused) {
    Bytes bytes = kPresence;
    bytes[18] = 0x02;
    expect_downstream_refused(bytes, frames::kFaultValue);
    bytes[18] = 0xFF;
    expect_downstream_refused(bytes, frames::kFaultValue);
}

TEST(ChatFrames, ANegativeLastSeenIsRefused) {
    Bytes bytes = kPresence;
    bytes[19] = 0x80;
    expect_downstream_refused(bytes, frames::kFaultValue);
}

TEST(ChatFrames, ReadPastDeliveredIsRefused) {
    Bytes bytes = kReceipt;
    bytes[49] = 0x0A;  // read 10, delivered 9
    expect_downstream_refused(bytes, frames::kFaultValue);
    EXPECT_EQ(encoded(frames::Receipt{.conversation = kConversation,
                                      .user = kUser,
                                      .delivered_seq = 9,
                                      .read_seq = 10})
                  .size(),
              0U);
}

TEST(ChatFrames, NegativeCountersAreRefused) {
    Bytes wake = kWakeFetch;
    wake[18] = 0x80;
    expect_downstream_refused(wake, frames::kFaultValue);

    Bytes membership = kMembership;
    membership[18] = 0xFF;
    expect_downstream_refused(membership, frames::kFaultValue);

    // Both negative and read <= delivered: still refused, because a cursor
    // below zero names no message.
    Bytes receipt = kReceipt;
    std::fill(receipt.begin() + 34, receipt.end(), 0xFF);
    expect_downstream_refused(receipt, frames::kFaultValue);

    EXPECT_EQ(
        encoded(frames::Wake{.inline_message = {}, .conversation = kConversation, .seq = -1})
            .size(),
        0U);
    EXPECT_EQ(
        encoded(frames::Membership{.conversation = kConversation, .membership_version = -1})
            .size(),
        0U);
}

TEST(ChatFrames, ANilConversationIsRefused) {
    for (const Bytes& golden : {kWakeInline, kTyping, kReceipt, kMembership, kMutation}) {
        Bytes bytes = golden;
        std::fill(bytes.begin() + 2, bytes.begin() + 18, 0x00);
        expect_downstream_refused(bytes, frames::kFaultNil);
    }
    Bytes up = kClientTyping;
    std::fill(up.begin() + 2, up.end(), 0x00);
    expect_upstream_refused(up, frames::kFaultNil);
    EXPECT_EQ(encoded(frames::ClientTyping{.conversation = anvil::kNilUuid}).size(), 0U);
}

TEST(ChatFrames, ANilUserIsRefused) {
    Bytes typing = kTyping;
    std::fill(typing.begin() + 18, typing.end(), 0x00);
    expect_downstream_refused(typing, frames::kFaultNil);
    Bytes presence = kPresence;
    std::fill(presence.begin() + 2, presence.begin() + 18, 0x00);
    expect_downstream_refused(presence, frames::kFaultNil);
}

TEST(ChatFrames, AFrameOverTheBoundIsRefusedBeforeItsHeaderIsRead) {
    // A valid header on an oversized body, and a garbage header: both are size
    // refusals, because the bound is the first thing compared.
    Bytes down(frames::kMaxDownstreamFrameBytes + 1, 0x00);
    expect_downstream_refused(down, frames::kFaultSize, ErrorCode::PayloadTooLarge);
    down[0] = 0x01;
    down[1] = 0x01;
    expect_downstream_refused(down, frames::kFaultSize, ErrorCode::PayloadTooLarge);

    Bytes up(frames::kMaxUpstreamFrameBytes + 1, 0xFF);
    expect_upstream_refused(up, frames::kFaultSize, ErrorCode::PayloadTooLarge);
    const Bytes huge(anvil::http::kMaxFrameBytes, 0x01);
    expect_upstream_refused(huge, frames::kFaultSize, ErrorCode::PayloadTooLarge);
}

TEST(ChatFrames, EncodeRefusesABufferTooSmall) {
    std::array<std::uint8_t, frames::kTypingBytes - 1> small{};
    EXPECT_EQ(
        frames::encode(frames::Typing{.conversation = kConversation, .user = kUser}, small),
        0U);
    std::array<std::uint8_t, 1> tiny{};
    EXPECT_EQ(frames::encode(frames::Ping{}, tiny), 0U);
    EXPECT_EQ(frames::encode(frames::Pong{}, tiny), 0U);
    EXPECT_EQ(frames::encode(frames::Sync{}, tiny), 0U);
    EXPECT_EQ(frames::encode(frames::ClientPong{}, tiny), 0U);
    std::array<std::uint8_t, frames::kWakeFixedBytes + 1> wake{};
    EXPECT_EQ(
        frames::encode(
            frames::Wake{.inline_message = kInline, .conversation = kConversation, .seq = 1},
            wake),
        0U);
}

TEST(ChatFrames, PresenceEncodeRefusesAStateTheDecoderWouldRefuse) {
    EXPECT_EQ(encoded(frames::Presence{.user = kUser,
                                       .last_seen_unix_ms = 1,
                                       .state = static_cast<frames::PresenceState>(2)})
                  .size(),
              0U);
}

// --- the printed fixture -----------------------------------------------------------
//
// tests/testapp/chat_frame_vectors.h is what testapp_emit_chat_frames prints for
// hammer. Every golden there is the frame this file spells, and every refusal
// there is refused with the name and code it gives.

TEST(ChatFrameVectors, EveryPrintedGoldenDecodesAndReencodesToItsBytes) {
    namespace fv = testapp::frame_vectors;
    std::vector<Bytes> all;
    for (const fv::Golden& golden : fv::kGoldens) { all.emplace_back(golden.bytes.begin(), golden.bytes.end()); }
    EXPECT_EQ(all[0], kWakeInline);
    EXPECT_EQ(all[9], kMutation);
    EXPECT_EQ(all[10], kClientTyping);
    for (const fv::Golden& golden : fv::kGoldens) {
        SCOPED_TRACE(golden.name);
        std::array<std::uint8_t, frames::kMaxDownstreamFrameBytes> again{};
        if (golden.direction == fv::Direction::Down) {
            const auto decoded = frames::decode_downstream(golden.bytes);
            ASSERT_TRUE(decoded);
            const std::size_t n = frames::encode(decoded.value(), again);
            EXPECT_TRUE(n == golden.bytes.size() &&
                        std::equal(golden.bytes.begin(), golden.bytes.end(), again.begin()));
        } else {
            const auto decoded = frames::decode_upstream(golden.bytes);
            ASSERT_TRUE(decoded);
            const std::size_t n = frames::encode(decoded.value(), again);
            EXPECT_TRUE(n == golden.bytes.size() &&
                        std::equal(golden.bytes.begin(), golden.bytes.end(), again.begin()));
        }
    }
    const std::vector<std::uint8_t> largest = fv::largest_wake();
    ASSERT_EQ(largest.size(), frames::kMaxDownstreamFrameBytes);
    EXPECT_TRUE(frames::decode_downstream(largest));
}

TEST(ChatFrameVectors, EveryPrintedRefusalIsRefusedByItsName) {
    for (const auto& refusal : testapp::frame_vectors::refusals()) {
        SCOPED_TRACE(refusal.name);
        if (refusal.direction == testapp::frame_vectors::Direction::Down) {
            expect_downstream_refused(refusal.bytes, refusal.fault, refusal.code);
        } else {
            expect_upstream_refused(refusal.bytes, refusal.fault, refusal.code);
        }
    }
}

}  // namespace
