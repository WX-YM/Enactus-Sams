#pragma once

// The database probes every *_db_test.cc used to carry its own copy of.
//
// One header, and — because these are `inline` at namespace scope rather than
// static in each translation unit's anonymous namespace — ONE static per test
// BINARY rather than one per file. That distinction is the whole point.
// anvil_db_tests links twelve of these files together, and
// db::MongoPool::init THROWS when it is called a second time. Twelve private
// copies of pool_ready() meant the first translation unit to probe took the
// pool and the other eleven caught "MongoPool::init called twice" and reported
// "no database is reachable" — so 283 of 288 cases skipped, on a replica set as
// much as on a standalone mongod, and the suite was green because it ran
// almost nothing.
//
// The second failure the copies caused: six cases opened
// a transaction without consulting the guard, so on a standalone mongod they
// failed red beside two hundred honest skips, and a suite that is normally red
// is a suite people stop reading.

#include <cstdlib>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "anvil/db/mongo_pool.h"

namespace anvil::testfixture {

// The ceiling has to cover the WIDEST concurrent case in the binary, because
// there is exactly one pool for all of them and a pool narrower than the widest
// case turns a concurrency assertion into a queue — the case then measures the
// pool rather than the thing it was written about, and it still passes.
//
// The widest today is eight simultaneous holders: versioned_concurrency,
// media_concurrency and transaction_retry_db each run that many. The headroom
// above it is deliberate and this is the number to revisit when a wider case is
// added, not after it starts timing out.
inline constexpr std::size_t kTestPoolMax = 40;

[[nodiscard]] inline std::string test_uri() {
    const char* from_env = std::getenv("ANVIL_TEST_MONGODB_URI");
    return (from_env != nullptr) ? std::string{from_env} : std::string{"mongodb://127.0.0.1:27017"};
}

// True when a mongod is reachable. Initialised once per binary on first use.
[[nodiscard]] inline bool pool_ready() {
    static const bool ready = [] {
        try {
            db::MongoPool::init(test_uri(), kTestPoolMax);
            auto client = db::MongoPool::instance().acquire();
            (*client)["admin"].run_command(bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("ping", 1)));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }();
    return ready;
}

// True when that mongod is a replica set and can therefore run a transaction.
//
// A WRITE inside the probe transaction, not merely start/abort: a standalone
// mongod accepts the session and rejects the first operation carrying a
// transaction number, so probing without one reports a capability that is not
// there.
[[nodiscard]] inline bool transactions_available() {
    static const bool available = [] {
        if (!pool_ready()) { return false; }
        try {
            auto client = db::MongoPool::instance().acquire();
            auto session = client->start_session();
            session.start_transaction();
            (*client)["anvil_txn_probe"]["probe"].insert_one(
                session, bsoncxx::builder::basic::make_document(
                             bsoncxx::builder::basic::kvp("probe", 1)));
            session.abort_transaction();
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }();
    return available;
}

}  // namespace anvil::testfixture

// A MACRO, which §7 otherwise forbids, because GTEST_SKIP expands to `return`
// and there is no function that can return on its caller's behalf. staff_db_test
// had this as a fixture member — `require_transactions()` — and it did exactly
// what the shape predicts: the skip returned from the HELPER, the test was
// marked skipped, and then its body ran on to completion anyway. Nine cases were
// affected; four failed and five passed by luck, and both outcomes were reported
// as a skip.
#define ANVIL_REQUIRE_TRANSACTIONS()                                              \
    do {                                                                             \
        if (!::anvil::testfixture::transactions_available()) {                    \
            GTEST_SKIP() << "no replica set at " << ::anvil::testfixture::test_uri() \
                         << " — this case is transactional";                         \
        }                                                                            \
    } while (false)
