// The live-stream registry under contention.
//
// These run under TSan. What they are looking for is not a wrong number — the
// unit suite covers the arithmetic — but the two races the design is shaped
// around: a delivery thread and a writer thread touching one ring at the same
// time, and a delivery racing the disconnect that removes the stream out from
// under it.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/notifications/sse.h"

namespace {

using anvil::Uuid;
namespace n = anvil::notifications;

[[nodiscard]] n::SseEvent event_of(const Uuid& id) {
    n::SseEvent event{};
    event.notification = id;
    event.type = n::SseEventKind::Notification;
    return event;
}

// Every id these tests need, minted BEFORE any thread starts.
//
// Not a convenience. `uuid::generate_v7()` calls RAND_bytes, and vcpkg builds
// OpenSSL without TSan instrumentation — so its lock-free algorithm cache reports
// as a race that is not one, in a library none of this is testing. Suppressing
// that would hide real races in the same stacks; taking the call out of the
// threaded section removes the question instead.
[[nodiscard]] std::vector<Uuid> minted(std::size_t count) {
    std::vector<Uuid> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) { ids.push_back(anvil::uuid::generate_v7()); }
    return ids;
}

}  // namespace

TEST(SseConcurrency, ADrainRacingADeliveryLosesNothingAndDuplicatesNothing) {
    constexpr std::int64_t kEvents = 4000;
    const std::vector<Uuid> ids = minted(static_cast<std::size_t>(kEvents) + 1);
    n::SseStream stream{1, ids.back()};

    std::atomic<bool> producing{true};
    std::thread producer{[&] {
        std::int64_t sent = 0;
        while (sent < kEvents) {
            // A refusal is a full ring, not a failure: back off and try again,
            // which is what a real delivery worker does NOT do — it closes the
            // connection. Here the retry is what keeps the count exact so the
            // ring's own arithmetic is what is under test.
            if (stream.push(event_of(ids[static_cast<std::size_t>(sent)]))) {
                ++sent;
            } else {
                std::this_thread::yield();
            }
        }
        producing.store(false, std::memory_order_release);
    }};

    std::vector<std::int64_t> sequences;
    sequences.reserve(static_cast<std::size_t>(kEvents));
    std::array<n::SseEvent, n::kMaxDrain> out{};
    while (producing.load(std::memory_order_acquire) ||
           static_cast<std::int64_t>(sequences.size()) < kEvents) {
        const std::size_t taken = stream.drain(out);
        for (std::size_t i = 0; i < taken; ++i) { sequences.push_back(out[i].sequence); }
        if (taken == 0) { std::this_thread::yield(); }
    }
    producer.join();

    ASSERT_EQ(static_cast<std::int64_t>(sequences.size()), kEvents);
    // Strictly increasing across every drain: nothing was dropped, nothing was
    // handed out twice, and no drain saw a slot mid-write.
    for (std::int64_t i = 0; i < kEvents; ++i) {
        ASSERT_EQ(sequences[static_cast<std::size_t>(i)], i + 1) << i;
    }
}

TEST(SseConcurrency, ManyDeliverersAndOneWriterAgreeOnTheCount) {
    n::SseHub hub{n::SseLimits{256, 8}};
    constexpr int kThreads = 8;
    constexpr int kPerThread = 400;
    const std::vector<Uuid> ids = minted(static_cast<std::size_t>(kThreads * kPerThread) + 1);

    const anvil::Result<n::StreamPtr> stream = hub.open(ids.back());
    ASSERT_TRUE(stream.ok());
    std::atomic<int> accepted{0};
    std::atomic<int> finished{0};

    std::vector<std::thread> deliverers;
    deliverers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        deliverers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                if (stream.value()->push(
                        event_of(ids[static_cast<std::size_t>((t * kPerThread) + i)]))) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    int drained = 0;
    std::array<n::SseEvent, n::kMaxDrain> out{};
    while (finished.load(std::memory_order_acquire) < kThreads) {
        drained += static_cast<int>(stream.value()->drain(out));
    }
    for (std::thread& worker : deliverers) { worker.join(); }
    drained += static_cast<int>(stream.value()->drain(out));

    // Some pushes are refused — that is the ring doing its job under a burst
    // eight threads wide. What must hold is that every push reported as accepted
    // came back out exactly once.
    EXPECT_EQ(drained, accepted.load(std::memory_order_relaxed));
}

TEST(SseConcurrency, DeliveryRacingACloseIsANoOpRatherThanAUseAfterFree) {
    n::SseHub hub{n::SseLimits{4096, 64}};
    constexpr int kReaders = 32;
    constexpr int kRounds = 60;

    const std::vector<Uuid> readers = minted(kReaders);
    const std::vector<Uuid> payloads = minted(kReaders);

    std::atomic<bool> stop{false};
    // Opens and closes, continuously, while deliveries are in flight against the
    // same readers. The stream is held by shared_ptr precisely so a delivery that
    // won the registry lookup and then lost the close still has something valid
    // to push into.
    std::thread churn{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            for (const Uuid& reader : readers) {
                const anvil::Result<n::StreamPtr> opened = hub.open(reader);
                if (opened.ok()) { hub.close(opened.value()->id()); }
            }
        }
    }};

    std::vector<std::thread> senders;
    senders.reserve(4);
    for (int t = 0; t < 4; ++t) {
        senders.emplace_back([&] {
            for (int round = 0; round < kRounds; ++round) {
                for (std::size_t i = 0; i < readers.size(); ++i) {
                    static_cast<void>(hub.deliver(readers[i], event_of(payloads[i])));
                }
            }
        });
    }
    for (std::thread& sender : senders) { sender.join(); }
    stop.store(true, std::memory_order_release);
    churn.join();

    // Whatever the churn left open is consistent between the two indexes: the
    // count comes from one of them, and closing drains both.
    for (const Uuid& reader : readers) {
        while (true) {
            const anvil::Result<n::StreamPtr> opened = hub.open(reader);
            if (!opened.ok()) { break; }
            hub.close(opened.value()->id());
            break;
        }
    }
    EXPECT_LE(hub.open_streams(), static_cast<std::size_t>(kReaders));
}

TEST(SseConcurrency, ConcurrentOpensRespectTheCeilingExactly) {
    constexpr std::size_t kCeiling = 50;
    n::SseHub hub{n::SseLimits{kCeiling, kCeiling}};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 40;
    const std::vector<Uuid> readers = minted(kThreads * kPerThread);

    std::atomic<int> granted{0};
    std::vector<std::thread> racers;
    racers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        racers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                if (hub.open(readers[static_cast<std::size_t>((t * kPerThread) + i)]).ok()) {
                    granted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& racer : racers) { racer.join(); }

    // Exactly the ceiling, not "about" the ceiling. A check-then-act would let
    // several threads past it at once, and the ceiling exists precisely because
    // the thing beyond it is descriptor exhaustion.
    EXPECT_EQ(granted.load(std::memory_order_relaxed), static_cast<int>(kCeiling));
    EXPECT_EQ(hub.open_streams(), kCeiling);
}
