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
