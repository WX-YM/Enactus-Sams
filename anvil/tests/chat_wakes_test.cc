// The cross-process wake channel against a live Redis (docs/22-chat.md §5.4).
//
// Every wait here is a bounded poll against a deadline, never a fixed sleep:
// pub/sub delivery time is the scheduler's business, and a case that sleeps
// "long enough" is a case that fails on a loaded CI machine.
//
// Absence is never asserted by waiting. A wake that must NOT arrive is checked
// after a sentinel wake published LATER on the same path has arrived: one
// subscriber connection reads its pushes in order, so by then the unwanted one
// would already be here.
//
// Every case mints its own users, so cases running in parallel processes
// against the one Redis cannot hear each other.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sw/redis++/redis++.h>

#include "anvil/chat/frames.h"
#include "anvil/chat/wakes.h"
#include "anvil/core/uuid.h"
#include "anvil/redis/redis_client.h"
#include "app_fixture.h"

namespace {

using anvil::Uuid;
namespace chat = anvil::chat;
namespace frames = anvil::chat::frames;
namespace testfixture = anvil::testfixture;

using Bytes = std::vector<std::uint8_t>;

constexpr std::chrono::seconds kDeadline{5};

[[nodiscard]] bool eventually(const std::function<bool()>& condition) {
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < until) {
        if (condition()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return condition();
}

// A wake carrying `inline_bytes` of message, so byte-identity is checked over a
// payload long enough to notice truncation or re-encoding.
[[nodiscard]] Bytes wake_frame(std::int64_t seq, std::size_t inline_bytes) {
    Bytes message(inline_bytes);
    for (std::size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<std::uint8_t>((i * 31U) + static_cast<std::size_t>(seq));
    }
    const Uuid conversation = anvil::uuid::generate_v4();
    std::array<std::uint8_t, frames::kMaxDownstreamFrameBytes> buffer{};
    const std::size_t written = frames::encode(
        frames::Wake{.inline_message = message, .conversation = conversation, .seq = seq},
        buffer);
    EXPECT_GT(written, 0U);
    return Bytes(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
}

// What the subscriber thread hands out, read by the test thread.
class Recorder final {
public:
    [[nodiscard]] chat::WakeDelivery delivery() {
        return [this](const Uuid& user, std::span<const std::uint8_t> frame) {
            const std::lock_guard lock{mutex_};
            // Copied: the span dies with the call.
            received_.emplace_back(user, Bytes(frame.begin(), frame.end()));
        };
    }
    [[nodiscard]] chat::WakeSubscribed subscribed() {
        return [this](const Uuid& user) {
            const std::lock_guard lock{mutex_};
            confirmed_.insert(user);
        };
    }

    [[nodiscard]] bool confirmed(const Uuid& user) {
        const std::lock_guard lock{mutex_};
        return confirmed_.contains(user);
    }
    [[nodiscard]] std::size_t confirmations(const Uuid& user) {
        const std::lock_guard lock{mutex_};
        return confirmed_.count(user);
    }
    [[nodiscard]] std::vector<std::pair<Uuid, Bytes>> received() {
        const std::lock_guard lock{mutex_};
        return received_;
    }
    [[nodiscard]] bool has(const Uuid& user, const Bytes& frame) {
        const std::lock_guard lock{mutex_};
        return std::ranges::any_of(received_, [&](const auto& entry) {
            return entry.first == user && entry.second == frame;
        });
    }
    [[nodiscard]] std::size_t count_for(const Uuid& user) {
        const std::lock_guard lock{mutex_};
        return static_cast<std::size_t>(std::ranges::count_if(
            received_, [&](const auto& entry) { return entry.first == user; }));
    }

private:
    std::mutex mutex_;
    std::vector<std::pair<Uuid, Bytes>> received_;
    std::multiset<Uuid> confirmed_;
};

[[nodiscard]] chat::WakeSubscriberConfig live_config() {
    chat::WakeSubscriberConfig config{};
    config.url = testfixture::redis_url();
    config.poll_interval = std::chrono::milliseconds{50};
    return config;
}

// A Redis client that cannot connect: port 1 on loopback is closed on any
// machine that runs this suite.
[[nodiscard]] sw::redis::ConnectionOptions unreachable() {
    sw::redis::ConnectionOptions options{};
    options.host = "127.0.0.1";
    options.port = 1;
    options.connect_timeout = std::chrono::milliseconds{200};
    options.socket_timeout = std::chrono::milliseconds{200};
    return options;
}

// --- the channel name ------------------------------------------------------------

TEST(ChatWakes, ChannelNameRoundTripsAndRefusesWhatItDidNotWrite) {
    const Uuid user = anvil::uuid::generate_v4();
    const chat::WakeChannel channel{user};
    EXPECT_TRUE(channel.view().starts_with("anvil:chat:wake:"));
    EXPECT_EQ(channel.view().size(), chat::kWakeChannelPrefix.size() + 32);
    EXPECT_EQ(chat::parse_wake_channel(channel.view()), user);

    std::string upper{channel.view()};
    std::ranges::transform(upper, upper.begin(), [](char c) {
        return (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
    });
    if (upper != channel.view()) { EXPECT_FALSE(chat::parse_wake_channel(upper)); }
    EXPECT_FALSE(chat::parse_wake_channel(channel.view().substr(1)));
    EXPECT_FALSE(chat::parse_wake_channel(std::string{channel.view()} + "0"));
    EXPECT_FALSE(chat::parse_wake_channel("anvil:chat:wakx:00000000000000000000000000000000"));
    EXPECT_FALSE(chat::parse_wake_channel("anvil:chat:wake:0000000000000000000000000000000g"));
}

// --- the cases that need no Redis ----------------------------------------------------

TEST(ChatWakes, PublishWithRedisUnreachableReturnsWithoutThrowing) {
    sw::redis::Redis dead{unreachable()};
    chat::WakePublisher publisher{dead};
    const std::array<Uuid, 3> recipients{anvil::uuid::generate_v4(), anvil::uuid::generate_v4(),
                                         anvil::uuid::generate_v4()};
    const Bytes frame = wake_frame(1, 16);

    EXPECT_EQ(publisher.publish(recipients, frame), 0U);
    EXPECT_EQ(publisher.published(), 0U);
    EXPECT_EQ(publisher.publish_failed(), 3U);
}

TEST(ChatWakes, PublishRefusesAMalformedFrameAndAnOversizedFanOut) {
    sw::redis::Redis dead{unreachable()};
    chat::WakePublisher publisher{dead};
    const std::array<Uuid, 1> one{anvil::uuid::generate_v4()};
    const Bytes garbage{0x01, 0xFF, 0x00};
    EXPECT_EQ(publisher.publish(one, garbage), 0U);
    EXPECT_EQ(publisher.publish_failed(), 1U);

    const std::vector<Uuid> too_many(chat::kMaxWakeRecipients + 1, anvil::uuid::generate_v4());
    EXPECT_EQ(publisher.publish(too_many, wake_frame(1, 0)), 0U);
    EXPECT_EQ(publisher.publish_failed(), 2U + chat::kMaxWakeRecipients);
}

TEST(ChatWakes, SubscriberAgainstAnUnreachableRedisStopsCleanly) {
    chat::WakeSubscriberConfig config{};
    config.url = "tcp://127.0.0.1:1";
    config.connect_timeout = std::chrono::milliseconds{100};
    config.poll_interval = std::chrono::milliseconds{50};
    Recorder recorder;
    chat::WakeSubscriber subscriber{config, recorder.delivery(), recorder.subscribed()};
    subscriber.subscribe(anvil::uuid::generate_v4());
    std::this_thread::sleep_for(std::chrono::milliseconds{150});  // let it fail at least once
    subscriber.stop();
    EXPECT_EQ(subscriber.delivered(), 0U);
}

TEST(ChatWakes, AnUnparseableUrlIsAConfigureTimeError) {
    chat::WakeSubscriberConfig config{};
    config.url = "not a url";
    EXPECT_THROW((chat::WakeSubscriber{config, {}, {}}), std::invalid_argument);
}

// --- live --------------------------------------------------------------------------

TEST(ChatWakes, OnlyTheSubscribedUserReceivesAndTheBytesAreIdentical) {
    ANVIL_REQUIRE_REDIS();
    const Uuid a = anvil::uuid::generate_v4();
    const Uuid b = anvil::uuid::generate_v4();
    Recorder recorder;
    chat::WakeSubscriber subscriber{live_config(), recorder.delivery(), recorder.subscribed()};
    subscriber.subscribe(a);
    ASSERT_TRUE(eventually([&] { return recorder.confirmed(a); }));

    chat::WakePublisher publisher{anvil::redis::RedisClient::instance()};
    const Bytes frame = wake_frame(7, frames::kInlineWakeBytes);
    const std::array<Uuid, 2> both{a, b};
    EXPECT_EQ(publisher.publish(both, frame), 2U);
    EXPECT_EQ(publisher.published(), 2U);

    ASSERT_TRUE(eventually([&] { return recorder.has(a, frame); }));
    const std::array<Uuid, 1> only_a{a};
    const Bytes sentinel = wake_frame(8, 0);
    ASSERT_EQ(publisher.publish(only_a, sentinel), 1U);
    ASSERT_TRUE(eventually([&] { return recorder.has(a, sentinel); }));

    const auto received = recorder.received();
    ASSERT_EQ(received.size(), 2U);
    EXPECT_EQ(received[0].first, a);
    EXPECT_EQ(received[0].second, frame);
    EXPECT_EQ(recorder.count_for(b), 0U);
    EXPECT_EQ(subscriber.delivered(), 2U);
    EXPECT_EQ(subscriber.dropped_malformed(), 0U);
}

TEST(ChatWakes, SubscriptionsAreReferenceCountedPerUser) {
    ANVIL_REQUIRE_REDIS();
    const Uuid a = anvil::uuid::generate_v4();
    const Uuid sentinel_user = anvil::uuid::generate_v4();
    Recorder recorder;
    chat::WakeSubscriber subscriber{live_config(), recorder.delivery(), recorder.subscribed()};
    chat::WakePublisher publisher{anvil::redis::RedisClient::instance()};

    subscriber.subscribe(a);
    subscriber.subscribe(a);
    ASSERT_TRUE(eventually([&] { return recorder.confirmed(a); }));

    // Two sockets, one closes: still subscribed.
    subscriber.unsubscribe(a);
    const Bytes first = wake_frame(1, 32);
    const std::array<Uuid, 1> only_a{a};
    ASSERT_EQ(publisher.publish(only_a, first), 1U);
    ASSERT_TRUE(eventually([&] { return recorder.has(a, first); }));

    // The last one closes. The sentinel user's subscribe is queued AFTER the
    // unsubscribe, so its confirmation proves the unsubscribe was applied.
    subscriber.unsubscribe(a);
    subscriber.subscribe(sentinel_user);
    ASSERT_TRUE(eventually([&] { return recorder.confirmed(sentinel_user); }));

    const Bytes second = wake_frame(2, 32);
    const Bytes sentinel = wake_frame(3, 0);
    const std::array<Uuid, 1> only_sentinel{sentinel_user};
    (void)publisher.publish(only_a, second);
    ASSERT_EQ(publisher.publish(only_sentinel, sentinel), 1U);
    ASSERT_TRUE(eventually([&] { return recorder.has(sentinel_user, sentinel); }));

    EXPECT_FALSE(recorder.has(a, second));
    EXPECT_EQ(recorder.count_for(a), 1U);
}

TEST(ChatWakes, AMalformedPayloadOnTheChannelIsDroppedAndCounted) {
    ANVIL_REQUIRE_REDIS();
    const Uuid a = anvil::uuid::generate_v4();
    Recorder recorder;
    chat::WakeSubscriber subscriber{live_config(), recorder.delivery(), recorder.subscribed()};
    subscriber.subscribe(a);
    ASSERT_TRUE(eventually([&] { return recorder.confirmed(a); }));

    // Something else with access to this Redis, publishing around the
    // publisher's own check.
    const chat::WakeChannel channel{a};
    sw::redis::Redis& redis = anvil::redis::RedisClient::instance();
    const std::string_view name = channel.view();
    (void)redis.spublish(sw::redis::StringView{name.data(), name.size()},
                         sw::redis::StringView{"\x01\x01not a frame", 13});
    // A well-formed frame with a byte too many: refused on canonical length.
    Bytes trailing = wake_frame(4, 8);
    trailing.push_back(0);
    (void)redis.spublish(
        sw::redis::StringView{name.data(), name.size()},
        sw::redis::StringView{reinterpret_cast<const char*>(trailing.data()), trailing.size()});

    chat::WakePublisher publisher{redis};
    const Bytes sentinel = wake_frame(5, 0);
    const std::array<Uuid, 1> only_a{a};
    ASSERT_EQ(publisher.publish(only_a, sentinel), 1U);
    ASSERT_TRUE(eventually([&] { return recorder.has(a, sentinel); }));

    EXPECT_EQ(recorder.received().size(), 1U);
    EXPECT_EQ(subscriber.dropped_malformed(), 2U);
    EXPECT_EQ(subscriber.delivered(), 1U);
}

// Two subscriber instances are two processes as far as Redis can tell: two
// connections, each subscribed to its own users' channels.
TEST(ChatWakes, TwoSubscribersEachReceiveOnlyTheirOwnUsers) {
    ANVIL_REQUIRE_REDIS();
    const Uuid a = anvil::uuid::generate_v4();
    const Uuid b = anvil::uuid::generate_v4();
    Recorder on_first;
    Recorder on_second;
    chat::WakeSubscriber first{live_config(), on_first.delivery(), on_first.subscribed()};
    chat::WakeSubscriber second{live_config(), on_second.delivery(), on_second.subscribed()};
    first.subscribe(a);
    second.subscribe(b);
    ASSERT_TRUE(eventually([&] { return on_first.confirmed(a) && on_second.confirmed(b); }));

    chat::WakePublisher publisher{anvil::redis::RedisClient::instance()};
    const Bytes frame = wake_frame(9, 100);
    const std::array<Uuid, 2> both{a, b};
    ASSERT_EQ(publisher.publish(both, frame), 2U);
    ASSERT_TRUE(eventually([&] { return on_first.has(a, frame) && on_second.has(b, frame); }));

    // Each received exactly one, and a sentinel on each path shows nothing else
    // was on its way.
    const Bytes sentinel = wake_frame(10, 0);
    ASSERT_EQ(publisher.publish(both, sentinel), 2U);
    ASSERT_TRUE(eventually(
        [&] { return on_first.has(a, sentinel) && on_second.has(b, sentinel); }));
    EXPECT_EQ(on_first.count_for(b), 0U);
    EXPECT_EQ(on_second.count_for(a), 0U);
    EXPECT_EQ(on_first.received().size(), 2U);
    EXPECT_EQ(on_second.received().size(), 2U);
}

// The subscriber's connection is killed server-side, which is what a Redis
// restart or a network partition looks like from this end. It must come back on
// its own and re-subscribe every referenced user — and say so through
// on_subscribed, which is the hub's cue to make those sockets sync, because
// wakes published while it was gone are lost.
TEST(ChatWakes, AKilledConnectionReconnectsAndResubscribesEveryone) {
    ANVIL_REQUIRE_REDIS();
    const Uuid a = anvil::uuid::generate_v4();
    const Uuid b = anvil::uuid::generate_v4();
    chat::WakeSubscriberConfig config = live_config();
    config.client_name = "anvil-wake-test-" + anvil::uuid::to_string(a);
    config.reconnect_initial = std::chrono::milliseconds{20};
    Recorder recorder;
    chat::WakeSubscriber subscriber{config, recorder.delivery(), recorder.subscribed()};
    subscriber.subscribe(a);
    subscriber.subscribe(b);
    ASSERT_TRUE(eventually([&] { return recorder.confirmed(a) && recorder.confirmed(b); }));

    // Find this subscriber's connection by its name and kill exactly it; a
    // CLIENT KILL by type would take down every other case's subscriber too.
    sw::redis::Redis& redis = anvil::redis::RedisClient::instance();
    const auto list = redis.command<std::string>("CLIENT", "LIST");
    const std::string needle = " name=" + config.client_name + " ";
    const std::size_t at = list.find(needle);
    ASSERT_NE(at, std::string::npos) << "the subscriber's connection is not named";
    const std::size_t line_start = list.rfind('\n', at);
    const std::size_t id_at = list.find("id=", line_start == std::string::npos ? 0 : line_start);
    const std::size_t id_end = list.find(' ', id_at);
    const std::string id = list.substr(id_at + 3, id_end - id_at - 3);
    (void)redis.command<long long>("CLIENT", "KILL", "ID", id);

    ASSERT_TRUE(eventually([&] {
        return recorder.confirmations(a) >= 2 && recorder.confirmations(b) >= 2;
    }));
    chat::WakePublisher publisher{redis};
    const Bytes frame = wake_frame(11, 64);
    const std::array<Uuid, 2> both{a, b};
    ASSERT_EQ(publisher.publish(both, frame), 2U);
    EXPECT_TRUE(eventually([&] { return recorder.has(a, frame) && recorder.has(b, frame); }));
}

// What ASan is looking at: the thread joined while (un)subscribes are still
// queued and callbacks capturing `this` are installed on a live connection.
TEST(ChatWakes, ShutdownWithPendingSubscribesIsClean) {
    ANVIL_REQUIRE_REDIS();
    for (int round = 0; round < 5; ++round) {
        Recorder recorder;
        chat::WakeSubscriber subscriber{live_config(), recorder.delivery(),
                                        recorder.subscribed()};
        // Half the rounds stop on a LIVE connection with its callbacks
        // installed; the other half may stop while it is still connecting.
        if (round % 2 == 1) {
            const Uuid first = anvil::uuid::generate_v4();
            subscriber.subscribe(first);
            ASSERT_TRUE(eventually([&] { return recorder.confirmed(first); }));
        }
        for (int i = 0; i < 64; ++i) {
            const Uuid user = anvil::uuid::generate_v4();
            subscriber.subscribe(user);
            if (i % 3 == 0) { subscriber.unsubscribe(user); }
        }
        if (round % 3 == 0) { subscriber.stop(); }
        // The other rounds leave it to the destructor.
    }
    SUCCEED();
}

}  // namespace
