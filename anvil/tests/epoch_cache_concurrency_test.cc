// The epoch cache is read on every authorized request from every event-loop
// thread and written from db_pool. It is the one piece of shared mutable state
// on the hot authorization path, so it carries the "concurrency" label and runs
// under TSan.
//
// The property that matters is not just "no data race". It is that a lookup
// never returns ANOTHER user's epoch: a torn read across a slot being rewritten
// would hand a request the permissions authority of whoever collided with it.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "anvil/auth/epoch_cache.h"
#include "anvil/core/uuid.h"

namespace anvil::auth {
namespace {

TEST(EpochCacheConcurrency, ConcurrentReadersNeverSeeAnotherUsersEpoch) {
    EpochCache cache{std::chrono::seconds{10}};

    // Each user's epoch is its own index, so any value that is not the index is
    // a value that came from somewhere else.
    constexpr std::size_t kUsers = 256;
    std::vector<Uuid> users;
    users.reserve(kUsers);
    for (std::size_t i = 0; i < kUsers; ++i) { users.push_back(uuid::generate_v7()); }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> wrong_values{0};

    std::vector<std::thread> writers;
    writers.reserve(4);
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&cache, &users, &stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (std::size_t i = 0; i < users.size(); ++i) {
                    cache.put(users[i], i, EpochCache::Clock::now());
                }
            }
        });
    }

    std::vector<std::thread> readers;
    readers.reserve(8);
    for (int r = 0; r < 8; ++r) {
        readers.emplace_back([&cache, &users, &stop, &wrong_values] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (std::size_t i = 0; i < users.size(); ++i) {
                    const std::optional<std::uint64_t> cached =
                        cache.get(users[i], EpochCache::Clock::now());
                    // A miss is always legitimate — the slot may hold a
                    // colliding user. A HIT with the wrong value never is.
                    if (cached.has_value() && *cached != i) {
                        wrong_values.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& thread : writers) { thread.join(); }
    for (std::thread& thread : readers) { thread.join(); }

    EXPECT_EQ(wrong_values.load(), 0U);
}

TEST(EpochCacheConcurrency, InvalidateRacesWithReadsWithoutTearing) {
    EpochCache cache{std::chrono::seconds{10}};
    const Uuid user = uuid::generate_v7();

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> wrong_values{0};

    std::thread writer{[&cache, &user, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            cache.put(user, 99, EpochCache::Clock::now());
            cache.invalidate(user);
        }
    }};

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&cache, &user, &stop, &wrong_values] {
            while (!stop.load(std::memory_order_relaxed)) {
                const std::optional<std::uint64_t> cached =
                    cache.get(user, EpochCache::Clock::now());
                if (cached.has_value() && *cached != 99) {
                    wrong_values.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (std::thread& thread : readers) { thread.join(); }

    EXPECT_EQ(wrong_values.load(), 0U);
}

// clear() walks every slot while readers are live. It is called on a
// configuration reload, so it happens in a running process, not at shutdown.
TEST(EpochCacheConcurrency, ClearIsSafeWhileReadersAreLive) {
    EpochCache cache{std::chrono::seconds{10}};
    const Uuid user = uuid::generate_v7();
    cache.put(user, 7, EpochCache::Clock::now());

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&cache, &user, &stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)cache.get(user, EpochCache::Clock::now());
            }
        });
    }

    for (int i = 0; i < 50; ++i) {
        cache.put(user, 7, EpochCache::Clock::now());
        cache.clear();
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& thread : readers) { thread.join(); }
    SUCCEED();
}

}  // namespace
}  // namespace anvil::auth
