#include "anvil/redis/redis_client.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace anvil::redis {
namespace {

std::unique_ptr<sw::redis::Redis> g_redis;

}  // namespace

void RedisClient::init(const RedisConfig& config) {
    if (g_redis) { throw std::logic_error("RedisClient::init called twice"); }

    // ConnectionOptions has no URI constructor; sw::redis::Uri is the parser.
    // Parsing first and then overriding means a URI may carry host, port, auth
    // and database, while the timeouts and pool size stay under our control —
    // they are operational limits, not connection details.
    const sw::redis::Uri uri{std::string{config.url}};

    sw::redis::ConnectionOptions conn = uri.connection_options();
    conn.connect_timeout = std::chrono::milliseconds{config.connect_timeout_ms};
    conn.socket_timeout = std::chrono::milliseconds{config.socket_timeout_ms};
    conn.keep_alive = true;

    sw::redis::ConnectionPoolOptions pool = uri.connection_pool_options();
    pool.size = config.pool_size;
    // Bounded wait: a blocked worker waiting indefinitely for a connection is
    // how a Redis stall becomes a thread-pool stall.
    pool.wait_timeout = std::chrono::milliseconds{config.socket_timeout_ms};

    g_redis = std::make_unique<sw::redis::Redis>(conn, pool);
}

sw::redis::Redis& RedisClient::instance() {
    if (!g_redis) { throw std::logic_error("RedisClient::init not called"); }
    return *g_redis;
}

bool RedisClient::healthy() noexcept {
    if (!g_redis) { return false; }
    try {
        return g_redis->ping() == "PONG";
    } catch (...) {
        return false;
    }
}

}  // namespace anvil::redis
