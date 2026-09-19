#pragma once

// The single owner of mongocxx::instance and mongocxx::pool.
//
// mongocxx::instance is the driver's global initialiser. It must be constructed
// exactly once, before any pool, and must outlive every pool and client.
// Getting that wrong crashes at shutdown, in production, under load — the
// classic static-initialisation-order fiasco (docs/09-mongodb.md §2,
//).
//
// Member DECLARATION ORDER is the mechanism that enforces it: declaration order
// is construction order, and reverse-declaration order is destruction order.
// instance_ before pool_ is the only correct arrangement. -Werror=reorder makes
// a mismatched member-initialiser list a build failure.

#include <cstddef>
#include <string_view>

#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

namespace anvil::db {

class MongoPool final {
public:
    // Must be called once from main(), before any thread pool starts.
    static void init(std::string_view uri, std::size_t max_pool_size);
    [[nodiscard]] static MongoPool& instance();

    // RAII. The entry returns the client to the pool on scope exit.
    // A client is NOT thread-safe: never store the entry beyond the task that
    // acquired it, and never let it cross a thread boundary.
    //
    // Out of line rather than a one-liner here, because it TIMES the wait and
    // records it into anvil_mongo_pool_wait_microseconds. That histogram is the
    // one number that distinguishes "the database is slow" from "there are not
    // enough clients to go round", and the two have opposite fixes. Inlining it
    // would put the analytics headers into every translation unit that opens a
    // connection, for a call that already costs a pool acquisition.
    [[nodiscard]] mongocxx::pool::entry acquire();

    [[nodiscard]] std::size_t max_size() const noexcept { return max_size_; }

    MongoPool(const MongoPool&) = delete;
    MongoPool& operator=(const MongoPool&) = delete;
    MongoPool(MongoPool&&) = delete;
    MongoPool& operator=(MongoPool&&) = delete;

    // Public only so unique_ptr can destroy it; construction stays private.
    ~MongoPool() = default;

private:
    MongoPool(std::string_view uri, std::size_t max_pool_size);

    mongocxx::instance instance_;   // FIRST: constructed first, destroyed last
    mongocxx::pool     pool_;       // SECOND
    std::size_t        max_size_;
};

}  // namespace anvil::db
