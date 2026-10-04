// The chat hub under contention, for TSan (chat/hub.h).
//
// The unit suite covers the policy. These cover the three races the design is
// shaped around: the subscriber thread pushing into a ring its loop is
// draining, a delivery racing the close that removes a socket, and sockets
// opening and closing on several loops at once while the subscription count
// has to come out balanced.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "anvil/chat/frames.h"
#include "anvil/chat/hub.h"

namespace {

namespace chat = anvil::chat;
namespace frames = anvil::chat::frames;
using anvil::Uuid;

[[nodiscard]] Uuid id_of(std::uint32_t tag) {
    Uuid id{};
    id[0] = 0x01;
    id[12] = static_cast<std::uint8_t>(tag >> 24);
    id[13] = static_cast<std::uint8_t>(tag >> 16);
    id[14] = static_cast<std::uint8_t>(tag >> 8);
    id[15] = static_cast<std::uint8_t>(tag);
    return id;
}

[[nodiscard]] std::vector<std::uint8_t> wake(std::int64_t seq) {
    std::vector<std::uint8_t> out(frames::kWakeFixedBytes);
    const std::size_t written = frames::encode(
        frames::Wake{.inline_message = {}, .conversation = id_of(999), .seq = seq}, out);
    out.resize(written);
    return out;
}

[[nodiscard]] std::int64_t seq_of(std::span<const std::uint8_t> frame) {
    const auto decoded = frames::decode_downstream(frame);
    EXPECT_TRUE(decoded);
    return std::get<frames::Wake>(decoded.value()).seq;
}

}  // namespace

TEST(ChatHubConcurrency, ADrainRacingADeliveryLosesNothingAndReordersNothing) {
    constexpr std::int64_t kFrames = 20'000;
    chat::ChatSocket socket{1, id_of(1), id_of(2)};

    std::thread producer{[&socket] {
        std::int64_t sent = 1;
        while (sent <= kFrames) {
            // Retried here only so the count is exact; the hub closes instead.
            if (socket.push(wake(sent))) {
                ++sent;
            } else {
                std::this_thread::yield();
            }
        }
    }};

    std::int64_t expected = 1;
    std::array<std::uint8_t, chat::kSocketRingBytes> out{};
    while (expected <= kFrames) {
        const std::size_t bytes = socket.drain(out);
        std::size_t at = 0;
        while (at < bytes) {
            const std::size_t length = (std::size_t{out[at]} << 8) | out[at + 1];
            at += chat::kSocketRecordHeaderBytes;
            ASSERT_EQ(seq_of(std::span<const std::uint8_t>{out.data() + at, length}), expected);
            ++expected;
            at += length;
        }
    }
    producer.join();
    EXPECT_EQ(socket.queued_bytes(), 0U);
}

TEST(ChatHubConcurrency, DeliveriesRacingClosesNeverTouchAFreedSocket) {
    chat::ChatHub hub{chat::HubLimits{.max_sockets = 4096, .max_per_account = 4}, {}};
    const Uuid user = id_of(7);
    std::atomic<bool> running{true};

    std::thread deliverer{[&] {
        std::int64_t seq = 0;
        while (running.load(std::memory_order_relaxed)) { (void)hub.deliver(user, wake(++seq)); }
    }};
    std::thread churner{[&] {
        for (std::uint32_t i = 0; i < 4000; ++i) {
            auto socket = hub.open(user, id_of(100 + (i % 3)), {});
            if (socket.ok()) { hub.close(socket.value()->id(), chat::CloseReason::Gone); }
        }
        running.store(false, std::memory_order_relaxed);
    }};
    churner.join();
    deliverer.join();
    EXPECT_EQ(hub.open_sockets(), 0U);
    EXPECT_FALSE(hub.has_socket(user));
}

TEST(ChatHubConcurrency, OpensAndClosesOnManyLoopsLeaveTheSubscriptionsBalanced) {
    std::mutex counts_mutex;
    std::map<Uuid, int> net;
    chat::ChatHub hub{
        chat::HubLimits{.max_sockets = 4096, .max_per_account = 8},
        chat::HubSubscriptions{.subscribe =
                                   [&](const Uuid& u) {
                                       const std::lock_guard lock{counts_mutex};
                                       ++net[u];
                                   },
                               .unsubscribe =
                                   [&](const Uuid& u) {
                                       const std::lock_guard lock{counts_mutex};
                                       --net[u];
                                   }}};

    constexpr std::uint32_t kLoops = 4;
    std::vector<std::thread> loops;
    for (std::uint32_t loop = 0; loop < kLoops; ++loop) {
        loops.emplace_back([&hub, loop] {
            for (std::uint32_t i = 0; i < 2000; ++i) {
                // Five users, so loops collide on the same user constantly, and
                // two devices per loop so replacements happen as well.
                const Uuid user = id_of(i % 5);
                const Uuid device = id_of(1000 + (loop * 2) + (i % 2));
                auto socket = hub.open(user, device, {});
                if (socket.ok() && (i % 3) != 0) {
                    hub.close(socket.value()->id(), chat::CloseReason::Gone);
                }
            }
        });
    }
    for (std::thread& loop : loops) { loop.join(); }

    // Whatever is left open, every user with a socket holds exactly one
    // subscription and every user without one holds none. The hub is asked
    // before counts_mutex is taken: the callbacks take it under the registry
    // lock, and the reverse order here is an inversion TSan reports.
    for (std::uint32_t u = 0; u < 5; ++u) {
        const Uuid user = id_of(u);
        const bool open = hub.has_socket(user);
        const std::lock_guard lock{counts_mutex};
        EXPECT_EQ(net[user], open ? 1 : 0);
    }
}
