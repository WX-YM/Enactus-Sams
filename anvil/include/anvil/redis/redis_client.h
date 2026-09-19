#pragma once

// Redis is a cache and a coordination surface, never the system of record.
// Every key must be reconstructible from MongoDB (docs/10-timer-jobs.md §1).
//
// redis-plus-plus owns its own connection pool and is thread-safe, so one
// sw::redis::Redis is shared process-wide. Its calls are blocking, so they run
// on a worker pool and never on a Trantor event-loop thread.

#include <cstddef>
#include <string_view>

#include <sw/redis++/redis++.h>

namespace anvil::redis {

struct RedisConfig final {
    std::string_view url;
    std::size_t      pool_size;
    std::size_t      connect_timeout_ms;
    std::size_t      socket_timeout_ms;
};

class RedisClient final {
public:
    static void init(const RedisConfig& config);
    [[nodiscard]] static sw::redis::Redis& instance();

    // True when Redis answered PING. A false result degrades the service — the
    // section cache falls back to MongoDB, cross-instance invalidation stops —
    // but must never fail a request that does not need Redis.
    [[nodiscard]] static bool healthy() noexcept;
};

}  // namespace anvil::redis
