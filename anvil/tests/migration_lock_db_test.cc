// Phase 9 — the suite that matters, because everything the lock design rests on
// is a property under concurrency.
//
// The lock is NOT what makes a step correct; idempotence is. What the lock buys
// is that the wasted work and the write conflicts do not happen, and every
// claim here is one find_one_and_update against an expiring lease — never
// check-then-act (CLAUDE.md §6).
//
// The crash is simulated by stopping where the crash would stop, rather than by
// back-dating a ledger row: a back-dated row tests the reclaim path against a
// state no crash produces.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "anvil/core/uuid.h"
#include "anvil/db/migration_ledger.h"
#include "anvil/db/mongo_pool.h"

#include "app_fixture.h"
#include "db_fixture.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

constexpr std::chrono::seconds kLease{30};

class MigrationLockDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        client_ = std::make_unique<mongocxx::pool::entry>(MongoPool::instance().acquire());
        (**client_)[database()][std::string{kMigrationLedgerCollection}].delete_many(
            make_document());
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return anvil::testfixture::scratch_database();
    }

    [[nodiscard]] static MigrationLedger ledger(std::string_view label) {
        return MigrationLedger{database(), uuid::generate_v4(), std::string{label}};
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(MigrationLockDb, EightRunnersClaimOneStepAndExactlyOneWins) {
    constexpr std::size_t kRunners = 8;
    constexpr std::string_view kStep = "contended_step";

    std::atomic<int> won{0};
    std::atomic<int> held{0};
    std::vector<std::thread> runners;
    runners.reserve(kRunners);

    for (std::size_t i = 0; i < kRunners; ++i) {
        runners.emplace_back([&, i]() {
            auto entry = MongoPool::instance().acquire();
            MigrationLedger mine{database(), uuid::generate_v4(), "runner-" + std::to_string(i)};
            const Result<LedgerEntry> claimed = mine.claim(*entry, kStep, kLease);
            if (claimed) {
                won.fetch_add(1, std::memory_order_relaxed);
            } else if (claimed.code() == ErrorCode::Conflict) {
                held.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& runner : runners) { runner.join(); }

    // Exactly one, and the other seven learn that it is HELD rather than
    // waiting. A second operator watching a migration hang is how two of them
    // end up force-killing the one that was working.
    EXPECT_EQ(won.load(), 1);
    EXPECT_EQ(held.load(), static_cast<int>(kRunners) - 1);
}

TEST_F(MigrationLockDb, ARunnerWhoseLeaseHasLapsedIsReclaimedByTheNext) {
    constexpr std::string_view kStep = "lapsing_step";

    // A lease of zero seconds is a lease that has already expired by the time
    // the next claim reads it — which is what a stalled process looks like from
    // the outside, and the only part of that state a second runner can observe.
    MigrationLedger first = ledger("first");
    ASSERT_TRUE(first.claim(db(), kStep, std::chrono::seconds{0}));

    MigrationLedger second = ledger("second");
    const Result<LedgerEntry> reclaimed = second.claim(db(), kStep, kLease);
    ASSERT_TRUE(reclaimed);
    EXPECT_EQ(reclaimed.value().owner, second.runner());
    // The attempt count is what tells an operator this step has been picked up
    // twice, which is the only trace a lapsed lease leaves.
    EXPECT_EQ(reclaimed.value().attempts, 2);
}

TEST_F(MigrationLockDb, ARunnerWhoseOwnerHasChangedWritesNothingFurther) {
    constexpr std::string_view kStep = "fenced_step";

    MigrationLedger first = ledger("first");
    const Result<LedgerEntry> claimed = first.claim(db(), kStep, std::chrono::seconds{0});
    ASSERT_TRUE(claimed);

    LedgerProgress progress{};
    progress.documents = 10;
    progress.batches = 1;
    ASSERT_TRUE(first.record(db(), kStep, progress, std::chrono::seconds{0}));

    // A second runner takes the lapsed lease. The first is now fenced and does
    // not know it.
    MigrationLedger second = ledger("second");
    ASSERT_TRUE(second.claim(db(), kStep, kLease));

    // Every write the first runner makes from here carries its own id in the
    // FILTER, so it matches nothing and the runner learns it from the same
    // operation that would have renewed the lease. There is no separate check to
    // race with.
    progress.documents = 20;
    progress.batches = 2;
    const Status renewed = first.record(db(), kStep, progress, kLease);
    EXPECT_FALSE(renewed);
    EXPECT_EQ(renewed.error().code, ErrorCode::Conflict);

    EXPECT_FALSE(first.finish(db(), kStep, progress));
    EXPECT_FALSE(first.record_failure(db(), kStep, progress, "should not land"));

    // Asserted by looking at what it WROTE, not by trusting the branch that was
    // supposed to return.
    const Result<std::optional<LedgerEntry>> row = second.read(db(), kStep);
    ASSERT_TRUE(row);
    ASSERT_TRUE(row.value().has_value());
    EXPECT_EQ(row.value()->documents, 10);
    EXPECT_EQ(row.value()->batches, 1);
    EXPECT_FALSE(row.value()->done);
    EXPECT_TRUE(row.value()->last_error.empty());
}

TEST_F(MigrationLockDb, FinishingReleasesTheLeaseRatherThanLettingItLapse) {
    constexpr std::string_view kStep = "finishing_step";

    MigrationLedger first = ledger("first");
    ASSERT_TRUE(first.claim(db(), kStep, kLease));

    LedgerProgress progress{};
    progress.documents = 3;
    progress.batches = 1;
    ASSERT_TRUE(first.finish(db(), kStep, progress));

    const Result<std::optional<LedgerEntry>> row = first.read(db(), kStep);
    ASSERT_TRUE(row);
    ASSERT_TRUE(row.value().has_value());
    EXPECT_TRUE(row.value()->done);
    EXPECT_FALSE(row.value()->owner.has_value());
    // A successor takes over in milliseconds rather than waiting out the lease,
    // which on a clean finish is the difference between a deploy that continues
    // and one that stalls for a minute.
    EXPECT_FALSE(row.value()->lease_expires_at.has_value());
}

TEST_F(MigrationLockDb, UnlockOverridesAHeldLeaseWhoeverHoldsIt) {
    constexpr std::string_view kStep = "stuck_step";

    MigrationLedger first = ledger("first");
    ASSERT_TRUE(first.claim(db(), kStep, std::chrono::hours{24}));

    MigrationLedger second = ledger("second");
    EXPECT_FALSE(second.claim(db(), kStep, kLease));

    // The one operation whose whole purpose is to override the safety net, which
    // is why it is deliberately NOT filtered on the owner: an override that only
    // works when you are already the holder overrides nothing.
    ASSERT_TRUE(second.release(db(), kStep));
    EXPECT_TRUE(second.claim(db(), kStep, kLease));
}

TEST_F(MigrationLockDb, UnlockingAStepNobodyHasRunIsNotFound) {
    // --unlock prints the owner and expiry before it acts, so it has to be able
    // to say "there is no such row" rather than creating one.
    MigrationLedger only = ledger("only");
    const Status released = only.release(db(), "never_claimed_step");
    EXPECT_FALSE(released);
    EXPECT_EQ(released.error().code, ErrorCode::NotFound);
}

}  // namespace
}  // namespace anvil::db
