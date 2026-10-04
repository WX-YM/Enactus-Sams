// The chat hub's registry and ring, with no socket and no network
// (chat/hub.h). What is asserted is the policy: what a slow client costs, what
// a second socket from one device does, and when a user's wake channel is
// subscribed.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "anvil/chat/frames.h"
#include "anvil/chat/hub.h"

namespace {

namespace chat = anvil::chat;
namespace frames = anvil::chat::frames;
using anvil::ErrorCode;
using anvil::Uuid;

[[nodiscard]] Uuid id_of(std::uint8_t tag) {
    Uuid id{};
    id[0] = tag;
    id[15] = 0x5A;
    return id;
}

const Uuid kAlice = id_of(1);
const Uuid kBob = id_of(2);
const Uuid kPhone = id_of(10);
const Uuid kLaptop = id_of(11);
const Uuid kTablet = id_of(12);
const Uuid kConversation = id_of(20);

[[nodiscard]] std::vector<std::uint8_t> wake(std::int64_t seq, std::size_t inline_bytes = 0) {
    const std::vector<std::uint8_t> message(inline_bytes, 0x7B);
    std::vector<std::uint8_t> out(frames::kMaxDownstreamFrameBytes);
    const std::size_t written = frames::encode(
        frames::Wake{.inline_message = message, .conversation = kConversation, .seq = seq},
        out);
    out.resize(written);
    return out;
}

// The frames a drain handed back, split on their length prefixes.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> drained(chat::ChatSocket& socket) {
    std::array<std::uint8_t, chat::kSocketRingBytes> out{};
    const std::size_t bytes = socket.drain(out);
    std::vector<std::vector<std::uint8_t>> frames_out;
    std::size_t at = 0;
    while (at < bytes) {
        const std::size_t length = (std::size_t{out[at]} << 8) | out[at + 1];
        at += chat::kSocketRecordHeaderBytes;
        frames_out.emplace_back(out.begin() + static_cast<std::ptrdiff_t>(at),
                                out.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    }
    EXPECT_EQ(at, bytes) << "a drain ended inside a record";
    return frames_out;
}

// Counts what the hub asked the subscriber to do.
struct Subscriptions final {
    std::map<Uuid, int> net;
    int calls = 0;

    [[nodiscard]] chat::HubSubscriptions callbacks() {
        return chat::HubSubscriptions{.subscribe = [this](const Uuid& u) { ++net[u]; ++calls; },
                                      .unsubscribe = [this](const Uuid& u) {
                                          --net[u];
                                          ++calls;
                                      }};
    }
};

[[nodiscard]] chat::SocketPtr opened(chat::ChatHub& hub, const Uuid& user, const Uuid& device,
                                     std::function<void(chat::SocketId)> notify = {}) {
    auto socket = hub.open(user, device, std::move(notify));
    EXPECT_TRUE(socket.ok());
    return socket.ok() ? socket.value() : nullptr;
}

// --- the ring ------------------------------------------------------------------------

TEST(ChatHubRing, FramesComeOutWholeAndInOrder) {
    chat::ChatSocket socket{1, kAlice, kPhone};
    const auto first = wake(1);
    const auto second = wake(2, frames::kInlineWakeBytes);
    const auto third = wake(3, 17);
    ASSERT_TRUE(socket.push(first));
    ASSERT_TRUE(socket.push(second));
    ASSERT_TRUE(socket.push(third));
    const auto out = drained(socket);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(out[0], first);
    EXPECT_EQ(out[1], second);
    EXPECT_EQ(out[2], third);
    EXPECT_EQ(socket.queued_bytes(), 0U);
}

TEST(ChatHubRing, ARecordThatWrapsTheRingComesOutIntact) {
    chat::ChatSocket socket{1, kAlice, kPhone};
    const auto big = wake(1, frames::kInlineWakeBytes);
    // Three big frames, drained, then three more: the second batch starts part
    // way round and the third of it wraps the end of the array.
    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(socket.push(big));
        ASSERT_TRUE(socket.push(big));
        ASSERT_TRUE(socket.push(big));
        const auto out = drained(socket);
        ASSERT_EQ(out.size(), 3U);
        for (const auto& frame : out) { EXPECT_EQ(frame, big); }
    }
}

TEST(ChatHubRing, AFullRingRefusesRatherThanOverwriting) {
    chat::ChatSocket socket{1, kAlice, kPhone};
    const auto big = wake(1, frames::kInlineWakeBytes);
    std::size_t taken = 0;
    while (socket.push(big)) { ++taken; }
    EXPECT_EQ(taken, chat::kSocketRingBytes / (big.size() + chat::kSocketRecordHeaderBytes));
    // A small frame that still fits is taken: the bound is bytes, not frames.
    const auto small = wake(2);
    const std::size_t left = chat::kSocketRingBytes - socket.queued_bytes();
    EXPECT_EQ(socket.push(small), left >= small.size() + chat::kSocketRecordHeaderBytes);
    // Nothing that was queued was lost to make room.
    const auto out = drained(socket);
    EXPECT_GE(out.size(), taken);
    EXPECT_EQ(out.front(), big);
}

TEST(ChatHubRing, NoFrameAnyClientWouldCloseOnIsQueued) {
    chat::ChatSocket socket{1, kAlice, kPhone};
    EXPECT_FALSE(socket.push({}));
    const std::vector<std::uint8_t> oversized(frames::kMaxDownstreamFrameBytes + 1, 0x01);
    EXPECT_FALSE(socket.push(oversized));
}

TEST(ChatHubRing, TheWriterIsWokenOncePerBurstAndOnClose) {
    int woken = 0;
    chat::ChatSocket socket{7, kAlice, kPhone};
    socket.on_ready([&woken](chat::SocketId id) {
        EXPECT_EQ(id, 7U);
        ++woken;
    });
    ASSERT_TRUE(socket.push(wake(1)));
    ASSERT_TRUE(socket.push(wake(2)));
    EXPECT_EQ(woken, 1) << "the drain the first push asked for takes the second too";
    (void)drained(socket);
    ASSERT_TRUE(socket.push(wake(3)));
    EXPECT_EQ(woken, 2);
    EXPECT_TRUE(socket.close(chat::CloseReason::Overflow));
    EXPECT_EQ(woken, 3) << "a socket the hub closed is shut by its own writer";
}

TEST(ChatHubRing, ACloseKeepsTheFirstReasonAndRefusesEveryPushAfter) {
    chat::ChatSocket socket{1, kAlice, kPhone};
    ASSERT_TRUE(socket.push(wake(1)));
    EXPECT_TRUE(socket.close(chat::CloseReason::Replaced));
    EXPECT_FALSE(socket.close(chat::CloseReason::Gone));
    EXPECT_EQ(socket.close_reason(), chat::CloseReason::Replaced);
    EXPECT_EQ(socket.queued_bytes(), 0U) << "a closed socket holds no ring";
    EXPECT_FALSE(socket.push(wake(2)));
}

// --- the registry ------------------------------------------------------------------

TEST(ChatHub, ASecondSocketFromOneDeviceClosesTheFirst) {
    Subscriptions subs;
    chat::ChatHub hub{chat::HubLimits{.max_sockets = 8, .max_per_account = 2},
                      subs.callbacks()};
    const auto first = opened(hub, kAlice, kPhone);
    const auto laptop = opened(hub, kAlice, kLaptop);
    // At the account's limit, and the phone can still replace its own socket.
    const auto second = opened(hub, kAlice, kPhone);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->close_reason(), chat::CloseReason::Replaced);
    EXPECT_FALSE(second->closed());
    EXPECT_FALSE(laptop->closed());
    EXPECT_EQ(hub.open_sockets(), 2U);

    // The replaced socket's own close, arriving later, touches nothing.
    hub.close(first->id(), chat::CloseReason::Gone);
    EXPECT_EQ(hub.open_sockets(), 2U);
    EXPECT_EQ(subs.net[kAlice], 1) << "one subscription however many sockets came and went";
}

TEST(ChatHub, TheAccountAndProcessCeilingsRefuseAtTheDoor) {
    chat::ChatHub hub{chat::HubLimits{.max_sockets = 3, .max_per_account = 2}, {}};
    (void)opened(hub, kAlice, kPhone);
    (void)opened(hub, kAlice, kLaptop);
    const auto third = hub.open(kAlice, kTablet, {});
    ASSERT_FALSE(third.ok());
    EXPECT_EQ(third.code(), ErrorCode::RateLimited);

    (void)opened(hub, kBob, kPhone);
    const auto over = hub.open(kBob, kLaptop, {});
    ASSERT_FALSE(over.ok());
    EXPECT_EQ(over.code(), ErrorCode::RateLimited);
    EXPECT_EQ(hub.open_sockets(), 3U);
    EXPECT_GT(chat::ChatHub(chat::HubLimits{}, {}).ceiling(), 0U)
        << "a derived ceiling of zero refuses every socket on a machine with descriptors";
}

TEST(ChatHub, AUsersChannelIsSubscribedByTheFirstSocketAndReleasedByTheLast) {
    Subscriptions subs;
    chat::ChatHub hub{chat::HubLimits{.max_sockets = 8, .max_per_account = 4},
                      subs.callbacks()};
    const auto phone = opened(hub, kAlice, kPhone);
    EXPECT_EQ(subs.net[kAlice], 1);
    const auto laptop = opened(hub, kAlice, kLaptop);
    EXPECT_EQ(subs.calls, 1);
    hub.close(phone->id(), chat::CloseReason::Gone);
    EXPECT_EQ(subs.net[kAlice], 1);
    EXPECT_TRUE(hub.has_socket(kAlice));
    hub.close(laptop->id(), chat::CloseReason::Gone);
    EXPECT_EQ(subs.net[kAlice], 0);
    EXPECT_FALSE(hub.has_socket(kAlice));
    hub.close(laptop->id(), chat::CloseReason::Gone);
    EXPECT_EQ(subs.calls, 2) << "closing twice unsubscribed twice";
}

TEST(ChatHub, AFrameReachesEverySocketOfItsUserAndNoOtherUsers) {
    chat::ChatHub hub{chat::HubLimits{.max_sockets = 8, .max_per_account = 4}, {}};
    const auto phone = opened(hub, kAlice, kPhone);
    const auto laptop = opened(hub, kAlice, kLaptop);
    const auto bob = opened(hub, kBob, kPhone);
    const auto frame = wake(9);
    EXPECT_EQ(hub.deliver(kAlice, frame), 2U);
    EXPECT_EQ(drained(*phone), std::vector<std::vector<std::uint8_t>>{frame});
    EXPECT_EQ(drained(*laptop), std::vector<std::vector<std::uint8_t>>{frame});
    EXPECT_TRUE(drained(*bob).empty());
}

TEST(ChatHub, AClientThatStopsReadingIsDroppedNotBuffered) {
    chat::ChatHub hub{chat::HubLimits{.max_sockets = 8, .max_per_account = 4}, {}};
    const auto slow = opened(hub, kAlice, kPhone);
    const auto fast = opened(hub, kAlice, kLaptop);
    const auto big = wake(1, frames::kInlineWakeBytes);
    std::size_t rounds = 0;
    while (!slow->closed() && rounds < 100) {
        (void)hub.deliver(kAlice, big);
        (void)drained(*fast);
        ++rounds;
    }
    EXPECT_EQ(slow->close_reason(), chat::CloseReason::Overflow);
    EXPECT_LT(rounds, 5U);
    EXPECT_FALSE(fast->closed()) << "one slow device must not cost the account its others";
    EXPECT_EQ(hub.open_sockets(), 1U);
    EXPECT_EQ(hub.overflowed(), 1U);
}

TEST(ChatHub, AResyncSendsEverySocketOfTheUserOneSyncFrame) {
    chat::ChatHub hub{chat::HubLimits{.max_sockets = 8, .max_per_account = 4}, {}};
    const auto phone = opened(hub, kAlice, kPhone);
    const auto laptop = opened(hub, kAlice, kLaptop);
    EXPECT_EQ(hub.resync(kAlice), 2U);
    for (const auto& socket : {phone, laptop}) {
        const auto out = drained(*socket);
        ASSERT_EQ(out.size(), 1U);
        const auto decoded = frames::decode_downstream(out[0]);
        ASSERT_TRUE(decoded);
        EXPECT_TRUE(std::holds_alternative<frames::Sync>(decoded.value()));
    }
    EXPECT_EQ(hub.resync(kBob), 0U);
}

}  // namespace
