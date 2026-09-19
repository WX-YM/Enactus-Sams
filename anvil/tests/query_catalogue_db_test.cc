// The explain contract, run against a live cluster.
//
// This is the only check in the suite that can tell an index scan from a
// collection scan, and it is the reason the rule in ENGINEERING_RULES.md §7 — add a query
// and you add its index in the same commit — is enforceable at all. On a
// developer's database a COLLSCAN over four hundred rows is indistinguishable
// from an index scan, and stays that way until the collection has four hundred
// thousand.
//
// It runs against the reference application's own catalogues, which makes it a
// test of the MECHANISM and a test of the example simultaneously: a query anvil
// issues that testapp forgot to declare is not caught here, but a query testapp
// declares with no index to ride is.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "anvil/db/query_catalogue.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/queries.h"

namespace {

using anvil::db::QueryViolation;
using anvil::db::check_query_catalogue;
using anvil::db::explain_query;
using anvil::testfixture::scratch_names;

class QueryCatalogueDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(QueryCatalogueDb, NoDeclaredQueryScansACollection) {
    const std::vector<QueryViolation> violations =
        check_query_catalogue(db(), scratch_names(), testapp::queries::kQueries);

    std::string report;
    for (const QueryViolation& violation : violations) {
        report += "\n  " + violation.query + " on " + violation.collection + ": " +
                  violation.reason;
    }
    EXPECT_TRUE(violations.empty()) << "queries with no index to ride:" << report;
}

TEST_F(QueryCatalogueDb, TheLoginPathRidesItsUniqueIndex) {
    // Named individually as well as covered by the sweep above, because this is
    // the one path an attacker can drive hardest: a scan here is the whole
    // database walked per credential guess.
    const auto explained =
        explain_query(db(), scratch_names(), testapp::queries::kQueries[0]);
    ASSERT_TRUE(explained.ok());
    EXPECT_FALSE(explained.value().scans_collection);
    EXPECT_EQ(explained.value().index_name, "users_email_unique");
}

TEST_F(QueryCatalogueDb, ASortedQueryTakesItsOrderFromTheIndexWalk) {
    // A filter that rides an index perfectly can still force a blocking in-memory
    // sort, which is bounded by a server-side memory budget and fails outright
    // past it — so a query that passes today on a small collection stops working
    // at a size nobody chose.
    for (const anvil::db::QuerySpec& spec : testapp::queries::kQueries) {
        if (spec.sort == nullptr) { continue; }
        const auto explained = explain_query(db(), scratch_names(), spec);
        ASSERT_TRUE(explained.ok()) << spec.name;
        EXPECT_FALSE(explained.value().sorts_in_memory)
            << spec.name << " materialises and sorts rather than walking an index";
    }
}

TEST_F(QueryCatalogueDb, TheCheckActuallyDetectsAScan) {
    // A check that returns "no violations" for everything passes on any codebase
    // in the world. This drives it against a query deliberately written to have
    // no index — over a field nothing indexes — and asserts it is reported.
    static constexpr std::array<anvil::db::QuerySpec, 1> kUnindexed{
        {{"deliberately_unindexed", "users",
          +[] {
              return bsoncxx::builder::basic::make_document(
                  bsoncxx::builder::basic::kvp("no_such_field", bsoncxx::types::b_int32{1}));
          },
          nullptr}}};

    const std::vector<QueryViolation> violations =
        check_query_catalogue(db(), scratch_names(), kUnindexed);
    ASSERT_EQ(violations.size(), 1U);
    EXPECT_EQ(violations[0].query, "deliberately_unindexed");
    EXPECT_NE(violations[0].reason.find("COLLSCAN"), std::string::npos);
}

TEST_F(QueryCatalogueDb, ANullFilterIsReportedRatherThanSkipped) {
    static constexpr std::array<anvil::db::QuerySpec, 1> kBroken{
        {{"no_filter", "users", nullptr, nullptr}}};

    const std::vector<QueryViolation> violations =
        check_query_catalogue(db(), scratch_names(), kBroken);
    ASSERT_EQ(violations.size(), 1U);
    EXPECT_EQ(violations[0].query, "no_filter");
}

TEST_F(QueryCatalogueDb, EveryDeclaredIndexIsCitedByAtLeastOneQuery) {
    // The other direction of the same contract. An index nothing needs is paid
    // for on every write to its collection, and the only way to notice is to
    // ask.
    //
    // Two kinds of index do a job that no read can cite, and both are exempt:
    //
    //   TTL       it exists to EXPIRE rows rather than to serve a query.
    //   UNIQUE    it exists to REFUSE A WRITE. The one-submission-per-person and
    //             one-object-per-submission rules have no read at all — that is
    //             the whole point of making them the server's job, because a
    //             count-then-insert loses the race to a double-click. Citing them
    //             would mean declaring a query nothing issues, which is worse
    //             than the gap it closes.
    //
    // A unique index that is ALSO read from is still checked, because it is
    // checked by the query that reads it: `users_email_unique` is cited by
    // `login_by_email` either way.
    std::vector<std::string> uncited;
    for (const anvil::db::IndexSpec& index : testapp::kIndexes) {
        if (index.expire_after_seconds >= 0) { continue; }
        if (index.unique) { continue; }

        bool cited = false;
        for (const anvil::db::QuerySpec& query : testapp::queries::kQueries) {
            if (query.collection != index.collection) { continue; }
            const auto explained = explain_query(db(), scratch_names(), query);
            if (!explained) { continue; }
            cited = cited || explained.value().index_name == index.name;
        }
        if (!cited) { uncited.emplace_back(index.name); }
    }

    std::string report;
    for (const std::string& name : uncited) { report += "\n  " + name; }
    EXPECT_TRUE(uncited.empty()) << "indexes no declared query rides:" << report;
}

}  // namespace
