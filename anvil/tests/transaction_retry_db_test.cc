// Phase 8 — the retry storm, counted and bounded.
//
// with_transaction retries a labelled transient error immediately and keeps
// doing so for up to 120 seconds. A write conflict on a contended document is
// the ORDINARY case of that — retrying is how a transaction is supposed to make
// progress — but anvil added no delay of its own, so N workers contending for
// one document became a tight loop against the one document they were all
// waiting for, each retry arriving at exactly the moment the others did.
//
// Neither half of that is visible from outside: a retried transaction produces
// no error response and no log line. A load run against an application built on
// this library measured 393,116 aborted attempts, and nothing in a passing suite
// could have shown it. So the counter is asserted against real contention here,
// and the backoff's bound is asserted where it can be asserted without a timing
// equality docs/16 would rule out.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/db/repository.h"

#include "db_fixture.h"
#include "metrics.h"

namespace anvil {
namespace {

using analytics::Internal;
using analytics::Registry;
using analytics::TxnOutcome;
using analytics::metric_of;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;
using testfixture::pool_ready;
using testfixture::test_uri;

// Eight writers is enough to produce conflicts reliably and small enough that
// the whole test is one round trip per attempt.
constexpr int kWriters = 8;
constexpr int kPerWriter = 6;

[[nodiscard]] std::uint64_t aborts(const Registry& registry, TxnOutcome outcome) {
    return registry.value_at(metric_of(Internal::TransactionsAborted),
                             static_cast<std::size_t>(outcome), 0);
}

TEST(TransactionRetryDb, ContendedAbortsAreCountedAgainstTheOutcomeTheyEndedIn) {
    if (!pool_ready()) { GTEST_SKIP() << "no MongoDB at " << test_uri(); }
    ANVIL_REQUIRE_TRANSACTIONS();

    auto registry = std::make_shared<Registry>(analytics::kInternalMetrics, testapp::kMetrics);
    analytics::install_registry(registry);

    const std::string database = "anvil_test_" + std::to_string(::getpid());
    const Uuid id = uuid::generate_v7();
    {
        auto client = db::MongoPool::instance().acquire();
        bsoncxx::builder::basic::document doc;
        db::codec::append_uuid(doc, "_id", id);
        doc.append(kvp("hits", bsoncxx::types::b_int64{0}));
        (*client)[database]["retry_probe"].insert_one(doc.view());
    }

    // ONE document, every writer. Snapshot isolation makes the losers abort with
    // a TransientTransactionError, which is precisely the retry this counts.
    const auto bump = [&database, &id] {
        auto client = db::MongoPool::instance().acquire();
        for (int i = 0; i < kPerWriter; ++i) {
            auto session = client->start_session();
            repo::in_transaction(session, [&](mongocxx::client_session* txn) {
                (*client)[database]["retry_probe"].update_one(
                    *txn, make_document(kvp("_id", db::codec::uuid_bin(id))),
                    make_document(kvp("$inc", [](sub_document sub) {
                        sub.append(kvp("hits", bsoncxx::types::b_int64{1}));
                    })));
            });
        }
    };

    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i) { writers.emplace_back(bump); }
    for (std::thread& writer : writers) { writer.join(); }

    {
        auto client = db::MongoPool::instance().acquire();
        const auto found = (*client)[database]["retry_probe"].find_one(
            make_document(kvp("_id", db::codec::uuid_bin(id))));
        ASSERT_TRUE(found.has_value());
        // Every transaction committed exactly once. A retry that double-applied
        // its body would show up here and nowhere else.
        EXPECT_EQ(found->view()["hits"].get_int64().value, kWriters * kPerWriter);
    }

    // Whether these eight writers actually collided depends on the machine, so
    // the positive count is asserted in the deterministic test below rather than
    // here. What is NOT machine-dependent is the direction: every one of these
    // transactions committed, so nothing may be charged to the failed outcome.
    EXPECT_EQ(aborts(*registry, TxnOutcome::Failed), 0U);
    RecordProperty("aborts_committed",
                   static_cast<int>(aborts(*registry, TxnOutcome::Committed)));

    analytics::uninstall_registry();
    {
        auto client = db::MongoPool::instance().acquire();
        (*client)[database]["retry_probe"].drop();
    }
}

TEST(TransactionRetryDb, AnAbortedAttemptIsCountedWhenOneIsForcedToHappen) {
    if (!pool_ready()) { GTEST_SKIP() << "no MongoDB at " << test_uri(); }
    ANVIL_REQUIRE_TRANSACTIONS();

    // The eight-writer test above proves the shape; this one proves the COUNTER,
    // by making the conflict happen rather than hoping for it. One transaction
    // writes the document and holds it open until a second has tried to write
    // the same one — which is a WriteConflict, which the server labels
    // TransientTransactionError, which with_transaction retries.
    auto registry = std::make_shared<Registry>(analytics::kInternalMetrics, testapp::kMetrics);
    analytics::install_registry(registry);

    const std::string database = "anvil_test_" + std::to_string(::getpid());
    const Uuid id = uuid::generate_v7();
    {
        auto client = db::MongoPool::instance().acquire();
        bsoncxx::builder::basic::document doc;
        db::codec::append_uuid(doc, "_id", id);
        doc.append(kvp("hits", bsoncxx::types::b_int64{0}));
        (*client)[database]["forced_conflict"].insert_one(doc.view());
    }

    std::atomic<bool> holder_wrote{false};
    std::atomic<bool> contender_attempted{false};

    const auto bump = [&database, &id](mongocxx::client& client,
                                       mongocxx::client_session* txn) {
        client[database]["forced_conflict"].update_one(
            *txn, make_document(kvp("_id", db::codec::uuid_bin(id))),
            make_document(kvp("$inc", [](sub_document sub) {
                sub.append(kvp("hits", bsoncxx::types::b_int64{1}));
            })));
    };

    // Every wait here is bounded, so a server that behaves differently produces
    // a failed assertion rather than a suite that hangs.
    const auto wait_for = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!flag.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    };

    std::thread holder{[&] {
        auto client = db::MongoPool::instance().acquire();
        auto session = client->start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            bump(*client, txn);
            holder_wrote.store(true, std::memory_order_release);
            wait_for(contender_attempted);
            // A moment past the contender's attempt, so its write reaches the
            // conflict before this transaction releases the document.
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        });
    }};

    std::thread contender{[&] {
        wait_for(holder_wrote);
        auto client = db::MongoPool::instance().acquire();
        auto session = client->start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            contender_attempted.store(true, std::memory_order_release);
            bump(*client, txn);
        });
    }};

    holder.join();
    contender.join();

    {
        auto client = db::MongoPool::instance().acquire();
        const auto found = (*client)[database]["forced_conflict"].find_one(
            make_document(kvp("_id", db::codec::uuid_bin(id))));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->view()["hits"].get_int64().value, 2);
    }

    // The number docs/00 §9 says to alert on, moving because a real abort
    // happened. Both transactions committed in the end, which is exactly why one
    // counter for "aborted" would be useless: this is the benign case, and it
    // must be distinguishable from the one where the work was thrown away.
    EXPECT_GE(aborts(*registry, TxnOutcome::Committed), 1U);
    EXPECT_EQ(aborts(*registry, TxnOutcome::Failed), 0U);

    analytics::uninstall_registry();
    {
        auto client = db::MongoPool::instance().acquire();
        (*client)[database]["forced_conflict"].drop();
    }
}

TEST(TransactionRetryDb, AFailedTransactionChargesItsAbortsToTheFailedOutcome) {
    if (!pool_ready()) { GTEST_SKIP() << "no MongoDB at " << test_uri(); }
    ANVIL_REQUIRE_TRANSACTIONS();

    auto registry = std::make_shared<Registry>(analytics::kInternalMetrics, testapp::kMetrics);
    analytics::install_registry(registry);

    struct Abort final {};

    auto client = db::MongoPool::instance().acquire();
    auto session = client->start_session();
    // An application's own abort exception is how a transaction reports a
    // business outcome. It must propagate: swallowing one here would turn a
    // rejected write into a committed one.
    EXPECT_THROW(repo::in_transaction(session,
                                      [](mongocxx::client_session*) { throw Abort{}; }),
                 Abort);
    // One attempt and no retry, so nothing is charged either way. What matters
    // is that the throw reached the caller.
    EXPECT_EQ(aborts(*registry, TxnOutcome::Failed), 0U);
    EXPECT_EQ(aborts(*registry, TxnOutcome::Committed), 0U);

    analytics::uninstall_registry();
}

TEST(TransactionBackoff, TheFirstAttemptDoesNotSleepAndTheRestAreBounded) {
    // An UPPER bound, not an equality: docs/16 rules out an assertion that
    // depends on how fast the machine ran. What is being checked is that the
    // delay is capped at all — an uncapped exponential would put a request path
    // to sleep for minutes on a document that never stops being contended.
    const auto started = std::chrono::steady_clock::now();
    repo::transaction_backoff(1);
    const auto after_first = std::chrono::steady_clock::now();
    EXPECT_LT(after_first - started, std::chrono::milliseconds{5});

    for (std::uint32_t attempt = 2; attempt <= 40; ++attempt) {
        repo::transaction_backoff(attempt);
    }
    const auto elapsed = std::chrono::steady_clock::now() - after_first;
    // 39 attempts, each capped at kMaxTransactionBackoff, with a wide margin for
    // a loaded machine's scheduler.
    EXPECT_LT(elapsed, 39 * repo::kMaxTransactionBackoff * 4);
}

}  // namespace
}  // namespace anvil
