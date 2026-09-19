// The live-stream registry and the per-connection ring.
//
// None of this needs a server or a socket, which is the point of the split: what
// decides how much memory a slow client costs, and what happens when there are
// too many of them, is testable on its own.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <sys/resource.h>
#include <vector>

#include "anvil/core/descriptor_budget.h"
#include "anvil/core/uuid.h"
#include "anvil/notifications/sse.h"

namespace {

using anvil::ErrorCode;
using anvil::Uuid;
namespace n = anvil::notifications;

[[nodiscard]] n::SseEvent event_of(const Uuid& id) {
    n::SseEvent event{};
    event.notification = id;
    event.type = n::SseEventKind::Notification;
    return event;
}

// A hub with an explicit ceiling, so a test asserts the ceiling's BEHAVIOUR
// without reshaping the process's descriptor limit to do it.
[[nodiscard]] n::SseHub hub_with(std::size_t max_streams, std::size_t per_reader = 4) {
    return n::SseHub{n::SseLimits{max_streams, per_reader}};
}

}  // namespace

// --- the ring ----------------------------------------------------------------

TEST(SseStream, AFullRingRefusesRatherThanOverwritingTheOldest) {
    n::SseStream stream{1, anvil::uuid::generate_v7()};

    std::vector<Uuid> pushed;
    for (std::size_t i = 0; i < n::kStreamRingSlots; ++i) {
        const Uuid id = anvil::uuid::generate_v7();
        ASSERT_TRUE(stream.push(event_of(id))) << i;
        pushed.push_back(id);
    }
    // Refused, not absorbed. Overwriting the oldest would keep the connection up
    // while the client silently missed something — a closed connection is a
    // condition the client already handles.
    EXPECT_FALSE(stream.push(event_of(anvil::uuid::generate_v7())));

    std::array<n::SseEvent, n::kMaxDrain> out{};
    ASSERT_EQ(stream.drain(out), n::kStreamRingSlots);
    for (std::size_t i = 0; i < pushed.size(); ++i) {
        EXPECT_EQ(out[i].notification, pushed[i]) << i;
    }
}

TEST(SseStream, ADrainEmptiesTheRingAndItAcceptsAgain) {
    n::SseStream stream{1, anvil::uuid::generate_v7()};
    for (std::size_t i = 0; i < n::kStreamRingSlots; ++i) {
        ASSERT_TRUE(stream.push(event_of(anvil::uuid::generate_v7())));
    }
    EXPECT_EQ(stream.queued(), n::kStreamRingSlots);

    std::array<n::SseEvent, n::kMaxDrain> out{};
    EXPECT_EQ(stream.drain(out), n::kStreamRingSlots);
    EXPECT_EQ(stream.queued(), 0U);
    // A drain that could not empty the ring would leave a connection permanently
    // one burst away from being dropped, which is why kMaxDrain equals the ring.
    EXPECT_TRUE(stream.push(event_of(anvil::uuid::generate_v7())));
}

TEST(SseStream, APartialDrainKeepsTheRestInOrder) {
    n::SseStream stream{1, anvil::uuid::generate_v7()};
    std::vector<Uuid> pushed;
    for (std::size_t i = 0; i < 5; ++i) {
        const Uuid id = anvil::uuid::generate_v7();
        ASSERT_TRUE(stream.push(event_of(id)));
        pushed.push_back(id);
    }

    std::array<n::SseEvent, 2> first{};
    ASSERT_EQ(stream.drain(first), 2U);
    EXPECT_EQ(first[0].notification, pushed[0]);
    EXPECT_EQ(first[1].notification, pushed[1]);

    // The tail is derived from head and count rather than stored, so a partial
    // drain has to leave those two agreeing.
    std::array<n::SseEvent, n::kMaxDrain> rest{};
    ASSERT_EQ(stream.drain(rest), 3U);
    EXPECT_EQ(rest[0].notification, pushed[2]);
    EXPECT_EQ(rest[2].notification, pushed[4]);
}

TEST(SseStream, TheRingWrapsWithoutLosingOrder) {
    n::SseStream stream{1, anvil::uuid::generate_v7()};
    std::array<n::SseEvent, n::kMaxDrain> out{};

    // Three times round, draining in between, so head walks past the end of the
    // array more than once.
    for (int round = 0; round < 3; ++round) {
        std::vector<Uuid> pushed;
        for (std::size_t i = 0; i < n::kStreamRingSlots - 7; ++i) {
            const Uuid id = anvil::uuid::generate_v7();
            ASSERT_TRUE(stream.push(event_of(id)));
            pushed.push_back(id);
        }
        ASSERT_EQ(stream.drain(out), pushed.size());
        for (std::size_t i = 0; i < pushed.size(); ++i) {
            ASSERT_EQ(out[i].notification, pushed[i]) << round << ":" << i;
        }
    }
}

TEST(SseStream, SequenceNumbersAreMonotonicPerStream) {
    n::SseStream stream{1, anvil::uuid::generate_v7()};
    for (std::size_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(stream.push(event_of(anvil::uuid::generate_v7())));
    }
    std::array<n::SseEvent, n::kMaxDrain> out{};
    ASSERT_EQ(stream.drain(out), 10U);
    // It is the SSE `id:` field, so a reconnecting client sends it back and the
    // application can tell how far behind it had fallen.
    for (std::int64_t i = 0; i < 10; ++i) { EXPECT_EQ(out[static_cast<std::size_t>(i)].sequence, i + 1); }

    // And it does not restart after a drain.
    ASSERT_TRUE(stream.push(event_of(anvil::uuid::generate_v7())));
    ASSERT_EQ(stream.drain(out), 1U);
    EXPECT_EQ(out[0].sequence, 11);
}

TEST(SseStream, AClosedStreamRefusesDeliveryRatherThanResurrecting) {
    n::SseStream stream{1, anvil::uuid::generate_v7()};
    ASSERT_TRUE(stream.push(event_of(anvil::uuid::generate_v7())));
    stream.close();

    EXPECT_TRUE(stream.closed());
    // A delivery racing a disconnect is a no-op. Whatever was queued is in the
    // inbox, which is the system of record.
    EXPECT_FALSE(stream.push(event_of(anvil::uuid::generate_v7())));
    EXPECT_EQ(stream.queued(), 0U);
}

TEST(SseStream, TheWakeCallbackRunsWithoutTheStreamLockHeld) {
    n::SseStream stream{1, anvil::uuid::generate_v7()};
    std::size_t observed = 0;
    // queued() takes the same non-recursive mutex push() holds. If the callback
    // ran inside the critical section this deadlocks rather than fails — which is
    // the assertion: the callback posts a drain onto an event loop, and a loop
    // that happened to be busy must not hold a delivery worker's mutex.
    stream.on_event([&](n::StreamId) { observed = stream.queued(); });

    ASSERT_TRUE(stream.push(event_of(anvil::uuid::generate_v7())));
    EXPECT_EQ(observed, 1U);
}

// --- the ceiling -------------------------------------------------------------

TEST(SseHub, TheProcessCeilingRefusesWithRateLimited) {
    n::SseHub hub = hub_with(2);
    ASSERT_TRUE(hub.open(anvil::uuid::generate_v7()).ok());
    ASSERT_TRUE(hub.open(anvil::uuid::generate_v7()).ok());
    EXPECT_EQ(hub.open_streams(), 2U);

    // A refusal at the door, while every other kind of request still works. The
    // alternative is an accept() failure that takes down the requests the client
    // would use to recover.
    EXPECT_EQ(hub.open(anvil::uuid::generate_v7()).code(), ErrorCode::RateLimited);
}

TEST(SseHub, OneAccountCannotHoldTheWholeProcessCeiling) {
    n::SseHub hub = hub_with(64, 2);
    const Uuid greedy = anvil::uuid::generate_v7();
    ASSERT_TRUE(hub.open(greedy).ok());
    ASSERT_TRUE(hub.open(greedy).ok());
    EXPECT_EQ(hub.open(greedy).code(), ErrorCode::RateLimited);

    // And the process ceiling is nowhere near reached, so somebody else can still
    // connect — which is the entire reason the per-reader limit exists.
    EXPECT_TRUE(hub.open(anvil::uuid::generate_v7()).ok());
}

TEST(SseHub, ARefusedOpenLeavesNoEntryBehind) {
    n::SseHub hub = hub_with(1);
    const Uuid first = anvil::uuid::generate_v7();
    const anvil::Result<n::StreamPtr> held = hub.open(first);
    ASSERT_TRUE(held.ok());

    // A thousand readers refused at the ceiling. Each one that left an empty
    // vector behind would grow the registry by a node that is never removed.
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(hub.open(anvil::uuid::generate_v7()).code(), ErrorCode::RateLimited);
    }
    hub.close(held.value()->id());
    EXPECT_EQ(hub.open_streams(), 0U);
    // The registry is empty again, so the next reader gets the one slot.
    EXPECT_TRUE(hub.open(anvil::uuid::generate_v7()).ok());
}

TEST(SseHub, TheDerivedCeilingLeavesDescriptorsForEverythingElse) {
    rlimit limit{};
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &limit), 0);
    const std::size_t derived = anvil::descriptor_ceiling(anvil::kStreamShare);

    if (limit.rlim_cur <= anvil::kDescriptorReserve) {
        // A deployment that cannot serve streams at all says so with a 429 rather
        // than exhausting descriptors later.
        EXPECT_EQ(derived, 0U);
        return;
    }
    // Never the whole budget: the database pool, the Redis connections, every
    // open file and every ordinary request in flight need descriptors too.
    EXPECT_LT(derived, static_cast<std::size_t>(limit.rlim_cur));
    EXPECT_GT(derived, 0U);
}

TEST(SseHub, AZeroCeilingIsDerivedRatherThanUnlimited) {
    // The production configuration. Whatever it derives to, an explicit zero must
    // not mean "no limit" — that is the one reading that turns the ceiling into
    // decoration.
    n::SseHub hub{n::SseLimits{}};
    EXPECT_EQ(hub.ceiling(), anvil::descriptor_ceiling(anvil::kStreamShare));
}

// --- delivery ----------------------------------------------------------------

TEST(SseHub, DeliveryReachesEveryStreamTheReaderHasOpen) {
    n::SseHub hub = hub_with(64);
    const Uuid reader = anvil::uuid::generate_v7();
    const anvil::Result<n::StreamPtr> phone = hub.open(reader);
    const anvil::Result<n::StreamPtr> laptop = hub.open(reader);
    const anvil::Result<n::StreamPtr> other = hub.open(anvil::uuid::generate_v7());
    ASSERT_TRUE(phone.ok());
    ASSERT_TRUE(laptop.ok());
    ASSERT_TRUE(other.ok());

    const Uuid notification = anvil::uuid::generate_v7();
    EXPECT_EQ(hub.deliver(reader, event_of(notification)), 2U);
    EXPECT_EQ(phone.value()->queued(), 1U);
    EXPECT_EQ(laptop.value()->queued(), 1U);
    // Somebody else's connection is not a place this reader's notification goes.
    EXPECT_EQ(other.value()->queued(), 0U);
}

TEST(SseHub, DeliveringToAFullRingClosesTheConnection) {
    n::SseHub hub = hub_with(64);
    const Uuid reader = anvil::uuid::generate_v7();
    const anvil::Result<n::StreamPtr> stream = hub.open(reader);
    ASSERT_TRUE(stream.ok());

    for (std::size_t i = 0; i < n::kStreamRingSlots; ++i) {
        ASSERT_EQ(hub.deliver(reader, event_of(anvil::uuid::generate_v7())), 1U);
    }
    // The client stopped reading. There is no safe amount of memory to spend
    // waiting for it to start again.
    EXPECT_EQ(hub.deliver(reader, event_of(anvil::uuid::generate_v7())), 0U);
    EXPECT_TRUE(stream.value()->closed());
    EXPECT_EQ(hub.open_streams(), 0U);
}

TEST(SseHub, ClosingTwiceIsNotAnError) {
    n::SseHub hub = hub_with(64);
    const anvil::Result<n::StreamPtr> stream = hub.open(anvil::uuid::generate_v7());
    ASSERT_TRUE(stream.ok());

    // The client disconnects and the writer notices. Both call close, and that is
    // the normal case rather than a fault.
    hub.close(stream.value()->id());
    hub.close(stream.value()->id());
    EXPECT_EQ(hub.open_streams(), 0U);
}

TEST(SseHub, DeliveryToAReaderWithNoStreamsIsANoOp) {
    n::SseHub hub = hub_with(64);
    EXPECT_EQ(hub.deliver(anvil::uuid::generate_v7(), event_of(anvil::uuid::generate_v7())), 0U);
}

TEST(SseHub, APingReachesEveryOpenStreamWhoeverOwnsIt) {
    n::SseHub hub = hub_with(64);
    const anvil::Result<n::StreamPtr> one = hub.open(anvil::uuid::generate_v7());
    const anvil::Result<n::StreamPtr> two = hub.open(anvil::uuid::generate_v7());
    ASSERT_TRUE(one.ok());
    ASSERT_TRUE(two.ok());

    EXPECT_EQ(hub.broadcast_ping(3), 2U);
    std::array<n::SseEvent, n::kMaxDrain> out{};
    ASSERT_EQ(one.value()->drain(out), 1U);
    EXPECT_EQ(out[0].type, n::SseEventKind::Ping);
    // The badge travels with the keepalive, so an idle tab whose count changed on
    // another device corrects itself without a request.
    EXPECT_EQ(out[0].unread, 3);
}

TEST(SseHub, APingSweepReapsAConnectionTooFullToTakeOne) {
    n::SseHub hub = hub_with(64);
    const Uuid reader = anvil::uuid::generate_v7();
    const anvil::Result<n::StreamPtr> stream = hub.open(reader);
    ASSERT_TRUE(stream.ok());
    for (std::size_t i = 0; i < n::kStreamRingSlots; ++i) {
        ASSERT_TRUE(stream.value()->push(event_of(anvil::uuid::generate_v7())));
    }

    // A ping is the cheapest event there is, so a ring too full to take one is a
    // connection nobody has drained in a very long time.
    EXPECT_EQ(hub.broadcast_ping(0), 0U);
    EXPECT_EQ(hub.open_streams(), 0U);
}
