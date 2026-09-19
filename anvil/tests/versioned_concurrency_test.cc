// Two writers, one version, exactly one winner.
//
// This lives in the concurrency suite because it is the test that must also
// pass under TSan: the guarantee comes from the server, but the helper around
// it builds BSON on both threads and must not share any of it.

#include <gtest/gtest.h>

#include "indexes.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/options/index.hpp>

#include "anvil/core/types.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "anvil/db/migrations.h"
#include "anvil/db/mongo_pool.h"

#include "db_fixture.h"
#include "anvil/db/repository.h"
#include "anvil/db/versioned.h"

namespace anvil {
namespace {

using testfixture::test_uri;
using testfixture::pool_ready;

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

constexpr int kWriters = 8;

TEST(VersionedConcurrency, ExactlyOneOfEightConcurrentWritersWins) {
    if (!pool_ready()) { GTEST_SKIP() << "no MongoDB at " << test_uri(); }

    const std::string database = "anvil_test_" + std::to_string(::getpid());
    const Uuid id = uuid::generate_v7();

    {
        auto client = db::MongoPool::instance().acquire();
        mongocxx::collection collection = (*client)[database]["concurrent_probe"];
        bsoncxx::builder::basic::document doc;
        db::codec::append_uuid(doc, "_id", id);
        doc.append(kvp("title", "start"));
        repo::append_initial_version(doc);
        collection.insert_one(doc.view());
    }

    std::atomic<int> winners{0};
    std::atomic<int> mismatches{0};
    std::vector<std::thread> writers;
    writers.reserve(kWriters);

    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&, i] {
            // Each thread takes its own client: a mongocxx::client is not
            // thread-safe, and sharing one is the mistake this suite exists to
            // catch (docs/09-mongodb.md §2).
            auto client = db::MongoPool::instance().acquire();
            mongocxx::collection collection = (*client)[database]["concurrent_probe"];

            const Result<std::int64_t> result = repo::update_versioned(
                collection, make_document(kvp("_id", db::codec::uuid_bin(id))).view(),
                repo::kInitialVersion,
                make_document(kvp("title", "writer-" + std::to_string(i))).view());

            if (result.ok()) {
                ++winners;
            } else if (result.code() == ErrorCode::VersionMismatch) {
                ++mismatches;
            }
        });
    }
    for (std::thread& writer : writers) { writer.join(); }

    EXPECT_EQ(winners.load(), 1) << "a lost update would show as more than one winner";
    EXPECT_EQ(mismatches.load(), kWriters - 1) << "every loser must learn that it lost";

    auto client = db::MongoPool::instance().acquire();
    const auto stored = (*client)[database]["concurrent_probe"].find_one(
        make_document(kvp("_id", db::codec::uuid_bin(id))));
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->view()["v"].get_int64().value, repo::kInitialVersion + 1)
        << "the version advanced exactly once";

    (*client)[database].drop();
}

// The migration is safe to run concurrently from N instances.
// Deploys roll all instances at once; if convergence needed coordination, the
// second instance to boot would fail on an index the first was still building.
TEST(MigrationConcurrency, ConcurrentInstancesConverge) {
    if (!pool_ready()) { GTEST_SKIP() << "no MongoDB at " << test_uri(); }

    const std::string database = "anvil_migrate_" + std::to_string(::getpid());
    const std::string secondary_database = database + "_scratch";
    const db::DatabaseNames databases{{{database, secondary_database}}};
    constexpr int kInstances = 4;

    std::atomic<int> succeeded{0};
    std::vector<std::thread> instances;
    instances.reserve(kInstances);
    for (int i = 0; i < kInstances; ++i) {
        instances.emplace_back([&] {
            try {
                auto client = db::MongoPool::instance().acquire();
                db::apply_migrations(*client, databases, testapp::kIndexes, testapp::kSchemaVersion);
                ++succeeded;
            } catch (const std::exception&) {
                // Counted by omission; the assertion below names the failure.
            }
        });
    }
    for (std::thread& instance : instances) { instance.join(); }

    EXPECT_EQ(succeeded.load(), kInstances) << "a booting instance must never fail on migration";

    auto client = db::MongoPool::instance().acquire();
    EXPECT_EQ(db::applied_schema_version(*client, database), testapp::kSchemaVersion);
    // Both databases converge, not just the one the racing instances happened
    // to reach first.
    EXPECT_EQ(db::applied_schema_version(*client, secondary_database), testapp::kSchemaVersion);

    // Convergence, not accumulation. Four instances raced; the database must end
    // with exactly the catalogued set, not the union of four attempts.
    //
    // The expected count is DERIVED from the catalogue rather than written down,
    // so adding an index does not silently turn this into an assertion about a
    // number somebody forgot to update.
    const auto catalogued = [](std::string_view collection) {
        std::size_t n = 0;
        for (const db::IndexSpec& spec : testapp::kIndexes) {
            if (spec.collection == collection) { ++n; }
        }
        return n;
    };
    const auto present = [&](const std::string& db_name, const std::string& collection) {
        std::size_t n = 0;
        auto cursor = (*client)[db_name][collection].list_indexes();
        for ([[maybe_unused]] const bsoncxx::document::view& index : cursor) { ++n; }
        return n;
    };

    // Plus one for `_id_`, which the server creates and the catalogue never names.
    EXPECT_EQ(present(database, "users"), catalogued("users") + 1)
        << "the catalogued users indexes plus _id_, however many instances ran";
    EXPECT_EQ(present(database, "user_sessions"), catalogued("user_sessions") + 1)
        << "the TTL index plus _id_";

    // The version marker is recorded in EVERY database, so a secondary database
    // dropped whole leaves no stale marker claiming its indexes exist.
    EXPECT_EQ(db::applied_schema_version(*client, database), testapp::kSchemaVersion);
    EXPECT_EQ(db::applied_schema_version(*client, secondary_database), testapp::kSchemaVersion);

    (*client)[database].drop();
}


// A superseded index must be DROPPED, not merely stopped from being recreated.
//
// Deleting an entry from the catalogue stops creating that index and leaves it in
// place on every cluster that already has one. For a unique index that is the
// dangerous case: the superseded constraint keeps rejecting writes against a rule
// nobody meant to still be in force, and the rejection is indistinguishable from
// a legitimate conflict.
//
// The drop also has to happen BEFORE anything is created, so a database
// mid-migration never briefly carries both the old constraint and the one
// replacing it.
TEST(MigrationRetirement, ASupersededIndexIsDroppedRatherThanCollidingOnItsName) {
    if (!pool_ready()) { GTEST_SKIP() << "no MongoDB at " << test_uri(); }

    const std::string database = "anvil_retire_" + std::to_string(::getpid());
    const std::string secondary_database = database + "_scratch";
    const db::DatabaseNames databases{{{database, secondary_database}}};

    auto client = db::MongoPool::instance().acquire();
    (*client)[database].drop();

    // Stand up the superseded shapes by hand, under the names a previous version
    // used. One unique, because that is the case with teeth.
    mongocxx::collection users = (*client)[database]["users"];
    {
        mongocxx::options::index unique{};
        unique.unique(true);
        unique.name("users_email_legacy");
        users.create_index(bsoncxx::builder::basic::make_document(
                               bsoncxx::builder::basic::kvp("email_legacy", 1)),
                           unique);

        mongocxx::options::index plain{};
        plain.name("users_status_legacy");
        users.create_index(bsoncxx::builder::basic::make_document(
                               bsoncxx::builder::basic::kvp("status", 1)),
                           plain);
    }

    ASSERT_NO_THROW(db::apply_migrations(*client, databases, testapp::kIndexes,
                                         testapp::kSchemaVersion, testapp::kRetiredIndexes))
        << "a superseded index name must not stop a migrating instance";

    std::vector<std::string> names;
    auto cursor = users.list_indexes();
    for (const bsoncxx::document::view& index : cursor) {
        names.emplace_back(index["name"].get_string().value);
    }
    const auto has = [&names](std::string_view wanted) {
        return std::find(names.begin(), names.end(), wanted) != names.end();
    };

    EXPECT_FALSE(has("users_email_legacy")) << "the superseded unique index must be gone";
    EXPECT_FALSE(has("users_status_legacy"));

    // And everything the catalogue does declare is present.
    for (const db::IndexSpec& spec : testapp::kIndexes) {
        if (spec.collection != "users") { continue; }
        EXPECT_TRUE(has(spec.name)) << spec.name;
    }

    // Idempotent: the second run has nothing to drop and must not object.
    EXPECT_NO_THROW(db::apply_migrations(*client, databases, testapp::kIndexes,
                                         testapp::kSchemaVersion, testapp::kRetiredIndexes));

    (*client)[database].drop();
    (*client)[secondary_database].drop();
}

}  // namespace
}  // namespace anvil
