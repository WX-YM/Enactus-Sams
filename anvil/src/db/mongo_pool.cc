#include "anvil/db/mongo_pool.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "anvil/analytics/counters.h"

namespace anvil::db {
namespace {

std::unique_ptr<MongoPool>& storage() {
    static std::unique_ptr<MongoPool> pool;
    return pool;
}

}  // namespace

namespace {

// mongocxx takes the pool ceiling from the connection string, so a maxPoolSize
// passed only as a C++ argument would be silently ignored. Boot asserts
// max_pool_size >= db_pool thread count; this makes the URI agree.
std::string with_pool_size(std::string_view uri, std::size_t max_pool_size) {
    std::string out{uri};
    if (out.find("maxPoolSize=") != std::string::npos) { return out; }
    out += (out.find('?') == std::string::npos) ? '?' : '&';
    out += "maxPoolSize=" + std::to_string(max_pool_size);
    return out;
}

}  // namespace

MongoPool::MongoPool(std::string_view uri, std::size_t max_pool_size)
    // Member-initialiser list order must match declaration order (ENGINEERING_RULES.md §3.2).
    : instance_{},
      pool_{mongocxx::uri{with_pool_size(uri, max_pool_size)}},
      max_size_{max_pool_size} {}

mongocxx::pool::entry MongoPool::acquire() {
    // steady_clock, not system_clock: this measures an INTERVAL, and a system
    // clock that steps backwards over an NTP correction would produce a
    // negative duration charged to the first bucket.
    const auto started = std::chrono::steady_clock::now();
    mongocxx::pool::entry client = pool_.acquire();
    const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    analytics::observe(analytics::Internal::MongoPoolWaitMicroseconds, waited.count());
    return client;
}

void MongoPool::init(std::string_view uri, std::size_t max_pool_size) {
    if (storage()) { throw std::logic_error("MongoPool::init called twice"); }
    // make_unique cannot reach the private constructor.
    storage().reset(new MongoPool{uri, max_pool_size});
}

MongoPool& MongoPool::instance() {
    if (!storage()) { throw std::logic_error("MongoPool::init not called"); }
    return *storage();
}

}  // namespace anvil::db
