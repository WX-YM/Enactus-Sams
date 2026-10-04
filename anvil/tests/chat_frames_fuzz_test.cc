// Fuzzing the chat socket's frame decoders, and what a frame costs.
//
// chat_frames_test proves each named refusal with one vector apiece. That does
// not prove the refusals are the whole list. The property that does is the
// canonical one: anything either decoder ACCEPTS re-encodes to exactly the
// bytes it was given. Shaped noise and mutations of valid frames are thrown at
// that property here.
//
// It lives in anvil_alloc_tests because the second claim is about allocation.
// Upstream frames arrive at a rate the client chooses, up to the frame budget
// on every open socket, so decoding one — accepted or refused — may not cost a
// heap allocation. Inputs are built in a stack buffer so that the counter,
// which spans the whole loop, measures the decoders and nothing else.
//
// Fixed seeds, as in validation_fuzz_test.cc: a failure is a reproduction, not
// an anecdote. std::mt19937 is banned for anything security-relevant and is
// exactly right for reproducible test input.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <variant>

#include <gtest/gtest.h>

#include "alloc_counter.h"
#include "anvil/chat/frames.h"
#include "anvil/core/types.h"

namespace {

namespace frames = anvil::chat::frames;
using anvil::testing::AllocationCounter;

constexpr std::uint32_t kSeed = 0x46524D53;  // "FRMS"
constexpr int kFuzzIterations = 200000;

// Room past the downstream bound, so oversize inputs are generated too.
constexpr std::size_t kCapacity = frames::kMaxDownstreamFrameBytes + 8;

struct Input final {
    std::array<std::uint8_t, kCapacity> bytes;
    std::size_t size;

    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
        return {bytes.data(), size};
    }
};

constexpr std::array<std::uint8_t, 9> kKnownTypes{
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x41, 0x42, 0x43};

struct Tally final {
    std::size_t accepted_down = 0;
    std::size_t accepted_up = 0;
};

// Both decoders over one input, and the canonical property on whatever either
// accepts. Called inside the allocation counter's span: everything here is on
// the stack.
void decode_and_check(std::span<const std::uint8_t> input, Tally& tally) {
    const auto down = frames::decode_downstream(input);
    if (down) {
        ++tally.accepted_down;
        std::array<std::uint8_t, frames::kMaxDownstreamFrameBytes> again{};
        const std::size_t written = frames::encode(down.value(), again);
        ASSERT_EQ(written, input.size()) << "an accepted frame re-encoded to another length";
        ASSERT_TRUE(std::equal(input.begin(), input.end(), again.begin()))
            << "an accepted frame did not re-encode to its own bytes";
        if (const auto* wake = std::get_if<frames::Wake>(&down.value())) {
            // The inline view lies inside the input, never past it.
            ASSERT_LE(wake->inline_message.size(), frames::kInlineWakeBytes);
            ASSERT_EQ(wake->inline_message.data() + wake->inline_message.size(),
                      input.data() + input.size());
        }
    }
    const auto up = frames::decode_upstream(input);
    if (up) {
        ++tally.accepted_up;
        ASSERT_FALSE(down) << "one frame accepted in both directions";
        std::array<std::uint8_t, frames::kMaxUpstreamFrameBytes> again{};
        const std::size_t written = frames::encode(up.value(), again);
        ASSERT_EQ(written, input.size());
        ASSERT_TRUE(std::equal(input.begin(), input.end(), again.begin()));
    }
}

// Uniform noise almost never passes the version byte and never matches a
// type's exact length. Most inputs here start with version 1 and a known type,
// and a wake's inline length is often made to agree with its body, which is
// what reaches the field checks at all.
void random_frame(std::mt19937& rng, Input& out) {
    std::uniform_int_distribution<int> byte{0, 255};
    const std::uint32_t shape = rng() % 16;
    if (shape == 0) {
        // Around both bounds.
        const std::size_t base = (rng() & 1) != 0 ? frames::kMaxDownstreamFrameBytes
                                                  : frames::kMaxUpstreamFrameBytes;
        out.size = base - 2 + rng() % 5;
    } else {
        out.size = rng() % 64;
    }
    for (std::size_t i = 0; i < out.size; ++i) {
        // Mostly small values and 0xFF: the zero ids, the sign bit and the
        // presence states live there.
        const std::uint32_t pick = rng() % 4;
        out.bytes[i] = pick == 0   ? std::uint8_t{0}
                       : pick == 1 ? static_cast<std::uint8_t>(rng() % 3)
                                   : static_cast<std::uint8_t>(byte(rng));
    }
    if (out.size >= 2 && rng() % 4 != 0) {
        out.bytes[0] = frames::kFrameVersion;
        out.bytes[1] = kKnownTypes[rng() % kKnownTypes.size()];
    }
    if (out.size >= frames::kWakeFixedBytes && out.bytes[1] == 0x01 && (rng() & 1) != 0) {
        const std::size_t inline_len = out.size - frames::kWakeFixedBytes;
        out.bytes[26] = static_cast<std::uint8_t>(inline_len >> 8);
        out.bytes[27] = static_cast<std::uint8_t>(inline_len & 0xFF);
        // Signs off, so the seq check is passed often enough to reach the end.
        out.bytes[18] = static_cast<std::uint8_t>(out.bytes[18] & 0x7F);
    }
}

[[nodiscard]] frames::DownstreamFrame random_valid_down(
    std::mt19937& rng, std::span<const std::uint8_t> inline_pool) {
    anvil::Uuid conversation{};
    anvil::Uuid user{};
    for (std::uint8_t& b : conversation) { b = static_cast<std::uint8_t>(rng()); }
    for (std::uint8_t& b : user) { b = static_cast<std::uint8_t>(rng()); }
    conversation[0] |= 1;  // never nil
    user[0] |= 1;
    const auto counter = [&rng]() {
        return static_cast<std::int64_t>(((std::uint64_t{rng()} << 32) | rng()) >>
                                         (1 + rng() % 63));
    };
    switch (rng() % 9) {
        case 7:
            return frames::Mutation{.conversation = conversation, .mutation = counter() + 1};
        case 0:
            return frames::Wake{.inline_message = inline_pool.first(rng() % 64),
                                .conversation = conversation,
                                .seq = counter()};
        case 1:
            return frames::Typing{.conversation = conversation, .user = user};
        case 2:
            return frames::Presence{.user = user,
                                    .last_seen_unix_ms = counter(),
                                    .state = static_cast<frames::PresenceState>(rng() % 2)};
        case 3: {
            const std::int64_t delivered = counter();
            const std::int64_t read = delivered == 0 ? 0 : delivered - (counter() % delivered);
            return frames::Receipt{.conversation = conversation,
                                   .user = user,
                                   .delivered_seq = delivered,
                                   .read_seq = read};
        }
        case 4:
            return frames::Membership{.conversation = conversation,
                                      .membership_version = counter()};
        case 5:
            return frames::Pong{};
        case 6:
            return frames::Sync{};
        default:
            return frames::Ping{};
    }
}

[[nodiscard]] frames::UpstreamFrame random_valid_up(std::mt19937& rng) {
    anvil::Uuid conversation{};
    for (std::uint8_t& b : conversation) { b = static_cast<std::uint8_t>(rng()); }
    conversation[0] |= 1;
    switch (rng() % 3) {
        case 0:
            return frames::ClientTyping{.conversation = conversation};
        case 1:
            return frames::ClientPong{};
        default:
            return frames::ClientPing{};
    }
}

// One structural mutation: the decoders' interesting edges are one byte away
// from a valid frame, where noise never lands.
void mutate(std::mt19937& rng, Input& in) {
    const std::size_t at = in.size == 0 ? 0 : rng() % in.size;
    switch (rng() % 6) {
        case 0:
            if (in.size != 0) {
                in.bytes[at] = static_cast<std::uint8_t>(in.bytes[at] ^ (1U << (rng() % 8)));
            }
            break;
        case 1:
            if (in.size != 0) { in.bytes[at] = static_cast<std::uint8_t>(rng()); }
            break;
        case 2:
            if (in.size < kCapacity) {
                std::copy_backward(in.bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                   in.bytes.begin() + static_cast<std::ptrdiff_t>(in.size),
                                   in.bytes.begin() + static_cast<std::ptrdiff_t>(in.size + 1));
                in.bytes[at] = static_cast<std::uint8_t>(rng());
                ++in.size;
            }
            break;
        case 3:
            if (in.size != 0) {
                std::copy(in.bytes.begin() + static_cast<std::ptrdiff_t>(at + 1),
                          in.bytes.begin() + static_cast<std::ptrdiff_t>(in.size),
                          in.bytes.begin() + static_cast<std::ptrdiff_t>(at));
                --in.size;
            }
            break;
        case 4:
            in.size = at;
            break;
        default:
            // A wake's inline length nudged by one, or any u16 if it is not a
            // wake: the off-by-one of the only length the body carries.
            if (in.size >= 2) {
                const std::size_t hi = in.size >= 28 && in.bytes[1] == 0x01
                                           ? 26
                                           : (at == in.size - 1 ? at - 1 : at);
                auto value = static_cast<std::uint16_t>((in.bytes[hi] << 8) | in.bytes[hi + 1]);
                value = static_cast<std::uint16_t>((rng() & 1) != 0 ? value + 1 : value - 1);
                in.bytes[hi] = static_cast<std::uint8_t>(value >> 8);
                in.bytes[hi + 1] = static_cast<std::uint8_t>(value & 0xFF);
            }
            break;
    }
}

}  // namespace

TEST(ChatFramesFuzz, ShapedNoiseIsCanonicalOrRefusedAndAllocatesNothing) {
    std::mt19937 rng{kSeed};
    Input input{};
    Tally tally{};
    const AllocationCounter counter;
    for (int i = 0; i < kFuzzIterations; ++i) {
        random_frame(rng, input);
        decode_and_check(input.view(), tally);
        if (::testing::Test::HasFatalFailure()) { return; }
    }
    EXPECT_EQ(counter.count(), 0U);
    // Noise that never decodes is testing the version byte.
    EXPECT_GT(tally.accepted_down + tally.accepted_up,
              static_cast<std::size_t>(kFuzzIterations / 100));
}

TEST(ChatFramesFuzz, MutatedFramesAreCanonicalOrRefusedAndAllocateNothing) {
    std::mt19937 rng{kSeed + 1};
    std::array<std::uint8_t, 64> inline_pool{};
    for (std::uint8_t& b : inline_pool) { b = static_cast<std::uint8_t>(rng()); }
    Input input{};
    Tally tally{};
    const AllocationCounter counter;
    for (int i = 0; i < kFuzzIterations; ++i) {
        const std::span<std::uint8_t> buffer{input.bytes};
        input.size = (i & 1) != 0 ? frames::encode(random_valid_down(rng, inline_pool), buffer)
                                  : frames::encode(random_valid_up(rng), buffer);
        ASSERT_GT(input.size, 0U) << "a valid frame did not encode";
        const int rounds = 1 + static_cast<int>(rng() % 3);
        for (int r = 0; r < rounds; ++r) { mutate(rng, input); }
        decode_and_check(input.view(), tally);
        if (::testing::Test::HasFatalFailure()) { return; }
    }
    EXPECT_EQ(counter.count(), 0U);
    EXPECT_GT(tally.accepted_down, static_cast<std::size_t>(kFuzzIterations / 100));
    EXPECT_GT(tally.accepted_up, static_cast<std::size_t>(kFuzzIterations / 100));
}

TEST(ChatFramesAllocations, AnOversizedFrameIsRefusedForNothing) {
    static const std::array<std::uint8_t, anvil::http::kMaxFrameBytes> huge{};
    const AllocationCounter counter;
    EXPECT_FALSE(frames::decode_upstream(huge));
    EXPECT_FALSE(frames::decode_downstream(huge));
    EXPECT_EQ(counter.count(), 0U);
}
