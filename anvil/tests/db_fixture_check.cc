// The fixture itself, asserted before anything relies on it.
//
// db_fixture.h carries one invariant that is invisible when it breaks: pool_ready
// is `inline` at namespace scope, not `static` in an anonymous namespace, so
// there is ONE probe per test BINARY rather than one per translation unit.
//
// MongoPool::init throws on its second call. With a per-TU copy, the first
// translation unit to probe takes the pool and every other one reports "no
// database reachable" and skips — which is silent, leaves the suite green, and in
// the system anvil came from meant 283 of 288 cases were not running at all.
//
// So this file exists to make the probe's answer visible as a test result rather
// than as an absence of one.

#include <gtest/gtest.h>

#include "app_fixture.h"
#include "db_fixture.h"

namespace {

// Drops this process's two scratch databases when the binary is finished.
//
// `drop_scratch_databases` has existed since the fixture did, and its comment
// said it was "registered by whichever suite owns the process" — but nothing
// registered it, so every run of this binary left two databases behind with
// fourteen collections and their indexes in each. Hundreds of runs later that is
// thousands of collections, and docs/13 §1 is explicit about what that costs: a
// WiredTiger file plus one per index, persistent in-memory metadata per table,
// and a slower `listCollections` every time anything starts. It is also the one
// consumer of this cluster that grows without bound.
//
// Registered HERE, in exactly one translation unit, because a static initialiser
// in the header would register one environment per TU that included it.
//
// It runs after the last test in the process, which means it does not run when
// the binary is killed — so this reduces the leak rather than eliminating it. A
// cleanup that cannot survive SIGKILL is the normal case, and the alternative (a
// sweep of every `anvil_t_*` database at startup) would race a concurrently
// running suite for databases that are not its own.
class ScratchDatabaseCleanup final : public ::testing::Environment {
public:
    void TearDown() override {
        if (!anvil::testfixture::pool_ready()) { return; }
        anvil::testfixture::drop_scratch_databases();
    }
};

const ::testing::Environment* kCleanup =
    ::testing::AddGlobalTestEnvironment(new ScratchDatabaseCleanup{});

TEST(DbFixture, ReportsWhetherADatabaseIsReachable) {
    if (!anvil::testfixture::pool_ready()) {
        GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri()
                     << " — database tests are skipping, which is a RESULT and not a pass";
    }
    SUCCEED() << "MongoDB reachable at " << anvil::testfixture::test_uri();
}

TEST(DbFixture, ReportsWhetherTransactionsAreAvailable) {
    if (!anvil::testfixture::pool_ready()) { GTEST_SKIP() << "no MongoDB"; }
    // Probed with an actual WRITE inside the transaction: a standalone mongod
    // accepts the session and rejects the first operation carrying a transaction
    // number, so probing with a read proves nothing.
    if (!anvil::testfixture::transactions_available()) {
        GTEST_SKIP() << "MongoDB is not a replica set — transaction tests are skipping";
    }
    SUCCEED() << "replica set with transactions";
}

}  // namespace
