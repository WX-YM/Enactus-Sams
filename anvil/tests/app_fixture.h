#pragma once

// What the phase-3 database suites share: a database nobody else is using, the
// indexes their queries need, and the Redis probe the authz path depends on.
//
// It sits beside tests/db_fixture.h rather than inside it because the two answer
// different questions. db_fixture.h answers "is there a server", once per
// BINARY, and that singleton-per-binary property is load-bearing — see its own
// comment. This answers "give me somewhere to write", which every fixture needs
// separately.
//
// --- why a per-run database rather than a per-run collection prefix ---------
//
// Collection names come from the application's constexpr table, so a test cannot
// vary them — that is the whole point of the seam. What a test CAN vary is the
// physical database name, which is a deployment decision resolved at boot
// through DatabaseNames. So each run gets its own database and drops it
// afterwards, and two runs on one cluster cannot see each other's rows.

#include <atomic>
#include <cstdlib>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/client.hpp>

#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/db/collection_options.h"
#include "anvil/db/collections.h"
#include "anvil/db/migrations.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/redis/redis_client.h"
#include "db_fixture.h"
#include "testapp/indexes.h"
#include "testapp/migrations.h"

namespace anvil::testfixture {

[[nodiscard]] inline std::string redis_url() {
    const char* from_env = std::getenv("ANVIL_TEST_REDIS_URL");
    return (from_env != nullptr) ? std::string{from_env} : std::string{"tcp://127.0.0.1:6379"};
}

// True when a Redis is reachable. Initialised once per binary, for the same
// reason pool_ready() is: RedisClient::init is a process-wide singleton, and a
// per-translation-unit copy would leave every file after the first believing
// there is no Redis.
[[nodiscard]] inline bool redis_ready() {
    static const bool ready = [] {
        try {
            const std::string url = redis_url();
            redis::RedisClient::init(
                redis::RedisConfig{.url = url, .pool_size = 4, .connect_timeout_ms = 500,
                                   .socket_timeout_ms = 500});
            return redis::RedisClient::healthy();
        } catch (const std::exception&) {
            return false;
        }
    }();
    return ready;
}

// The process-wide thread pools, initialised once per BINARY.
//
// Pools::init throws on its second call and shutdown() does not undo it, so a
// per-test or per-translation-unit initialiser would leave every user after the
// first either throwing or holding a pool that has already drained. Same hazard
// and same shape as pool_ready() — see tests/db_fixture.h for what that failure
// looked like when it was not centralised.
//
// The guard's DESTRUCTOR drains them, at process exit rather than between tests.
// Leaving worker threads running past main() is not merely untidy: each holds
// thread-local OpenSSL state that LeakSanitizer then reports as leaked, and a
// suite that is normally red is a suite people stop reading.
//
// Deliberately tiny: a queue of one is what makes shedding observable, which is
// the property most callers of this are testing.
inline bool pools_ready() {
    struct PoolGuard final {
        bool ready;

        PoolGuard() : ready{false} {
            try {
                Pools::init(PoolSizes{.db_threads = 2,
                                      .db_queue = 4,
                                      .cpu_threads = 1,
                                      .cpu_queue = 1,
                                      .hash_threads = 1,
                                      .hash_queue = 1,
                                      .audit_threads = 1,
                                      .audit_queue = 4,
                                      .analytics_threads = 1,
                                      .analytics_queue = 4});
                ready = true;
            } catch (const std::exception&) {
                ready = false;
            }
        }

        ~PoolGuard() {
            if (ready) { Pools::shutdown(); }
        }

        PoolGuard(const PoolGuard&) = delete;
        PoolGuard& operator=(const PoolGuard&) = delete;
    };

    static const PoolGuard guard{};
    return guard.ready;
}

// A database name no other run is using. One per PROCESS, not one per test:
// apply_migrations builds every index in the catalogue and that is not something
// to pay for per case.
//
// Unless ANVIL_TEST_SCRATCH_DB names one: a process a case spawns as its peer
// (chat_peers_listener_test.cc) is the same deployment as its parent, and two
// processes are one deployment only if they share a database.
//
// Whether THIS process minted its scratch database's name, and so owns it. A
// name inherited through ANVIL_TEST_SCRATCH_DB belongs to the process that set
// it, and is that process's to drop.
inline std::atomic<bool> g_scratch_database_owned{false};

[[nodiscard]] inline const std::string& scratch_database() {
    static const std::string name = [] {
        const char* shared = std::getenv("ANVIL_TEST_SCRATCH_DB");
        if (shared != nullptr && *shared != '\0') { return std::string{shared}; }
        g_scratch_database_owned.store(true);
        return "anvil_t_" + uuid::to_string(uuid::generate_v4()).substr(0, 8);
    }();
    return name;
}

// The second database the reference application declares. Distinct from the
// first, because a test that let them collapse into one would pass while the
// multi-database path was broken.
[[nodiscard]] inline const std::string& scratch_database_secondary() {
    static const std::string name = scratch_database() + "_2";
    return name;
}

[[nodiscard]] inline db::DatabaseNames scratch_names() {
    return db::DatabaseNames{{std::string_view{scratch_database()},
                              std::string_view{scratch_database_secondary()}}};
}

// Applies the reference application's schema, once per process: the collection
// options first, then the index catalogue, then the validators.
//
// Without the indexes every query in these suites is a COLLSCAN that still
// returns the right answer, so the suite would be green while proving nothing
// about the shapes it is actually asserting — which is exactly what the explain
// check exists to catch.
//
// The ORDER is the deployment order and it is not incidental. createIndexes
// creates a missing collection implicitly, and a collection created that way is
// not clustered, not capped and not a timeseries — so options have to land
// first or the one-way doors are decided by whichever call arrived earliest.
// Validators land last, after the data steps a real run makes here, because
// adding one to a collection that already holds documents rejects the writes
// that would have made them conform (docs/18-data-migrations.md §12).
inline void ensure_indexes() {
    static const bool applied = [] {
        auto client = db::MongoPool::instance().acquire();
        (void)db::apply_collection_options(*client, scratch_names(),
                                           testapp::kCollectionOptions,
                                           db::OptionsPhase::Create);
        (void)db::apply_migrations(*client, scratch_names(), testapp::kIndexes,
                                   testapp::kSchemaVersion, testapp::kRetiredIndexes);
        (void)db::apply_collection_options(*client, scratch_names(),
                                           testapp::kCollectionOptions,
                                           db::OptionsPhase::Validate);
        return true;
    }();
    (void)applied;
}

// Empties one collection between cases. Cheaper and far less surprising than
// dropping and rebuilding it: a drop takes the indexes with it, and a case that
// ran after one would silently be measuring an unindexed collection.
inline void clear_collection(mongocxx::client& client, std::string_view collection) {
    client[std::string{scratch_names().for_collection(collection)}][std::string{collection}]
        .delete_many(
        bsoncxx::builder::basic::make_document());
}

// Removes both scratch databases. Registered by whichever suite owns the
// process, so a run leaves a cluster as it found it.
inline void drop_scratch_databases() noexcept {
    try {
        auto client = db::MongoPool::instance().acquire();
        (*client)[scratch_database()].drop();
        (*client)[scratch_database_secondary()].drop();
    } catch (const std::exception&) {
        // A leftover scratch database is untidy, never incorrect: the next run
        // picks a fresh name. Failing a suite over cleanup would turn a green
        // run into a red one for no reason anybody can act on.
    }
}

}  // namespace anvil::testfixture

// GTEST_SKIP expands to `return`, so this has to be a macro for the same reason
// ANVIL_REQUIRE_TRANSACTIONS does: a helper function's `return` skips the helper
// and lets the test body run on regardless.
#define ANVIL_REQUIRE_REDIS()                                                      \
    do {                                                                           \
        if (!::anvil::testfixture::redis_ready()) {                                \
            GTEST_SKIP() << "no Redis at " << ::anvil::testfixture::redis_url()    \
                         << " — this case needs the revocation channel";           \
        }                                                                          \
    } while (false)
