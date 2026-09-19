// Phase 9 — the two things the index catalogue could not express.
//
// Both failures are SILENT. A query whose collation does not match its index
// gets a COLLSCAN, which is slowness rather than an error and is therefore found
// under load; a hidden index looks identical to a dropped one from the outside
// until something needs it back. Neither can be asserted by reading the
// catalogue, so both are asserted by asking the SERVER what it chose.

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/index.hpp>

#include "anvil/db/collation.h"
#include "anvil/db/collection_options.h"
#include "anvil/db/migrations.h"
#include "anvil/db/mongo_pool.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/indexes.h"
#include "testapp/migrations.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

// A collection with one declared index of its own, so the two this suite adds
// cannot change what any other suite's planner sees.
constexpr std::string_view kCollection = "drafts";
constexpr std::string_view kCollated = "drafts_title_collated";
constexpr std::string_view kHidden = "drafts_owner_hidden";

// The locale is the application's, taken from its own LocaleSpec table rather
// than written out here — that is the whole point of there being one string.
constexpr std::string_view kCollationLocale = "ar";

[[nodiscard]] IndexSpec collated_index() {
    return IndexSpec{{{{"title", 1}}}, kCollection, kCollated, nullptr, -1, 1,
                     false,            false,       false,     kCollationLocale};
}

[[nodiscard]] IndexSpec owner_index(bool hidden) {
    return IndexSpec{{{{"owner", 1}}}, kCollection, kHidden, nullptr, -1, 1,
                     false,            false,       hidden,  {}};
}

class IndexExpansionDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(MongoPool::instance().acquire());
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{anvil::testfixture::scratch_names().for_collection(kCollection)};
    }

    void apply(std::span<const IndexSpec> catalogue) {
        (void)apply_migrations(db(), anvil::testfixture::scratch_names(), catalogue,
                               testapp::kSchemaVersion);
    }

    void drop(std::string_view name) {
        try {
            db()[database()][std::string{kCollection}].indexes().drop_one(std::string{name});
        } catch (const std::exception&) {
            // Absent is the state this wants; a suite that failed before
            // creating it must not fail again on the way out.
        }
    }

    // The winning plan, as JSON. A test asserts on what the SERVER said it
    // chose, which is the only thing that distinguishes an index scan from a
    // collection scan on a developer's four hundred rows.
    [[nodiscard]] std::string winning_plan(const bsoncxx::document::view& filter,
                                           bool with_collation) {
        bsoncxx::builder::basic::document find;
        find.append(kvp("find", std::string{kCollection}));
        find.append(kvp("filter", filter));
        const bsoncxx::document::value collation = collation_for(kCollationLocale);
        if (with_collation) { find.append(kvp("collation", collation.view())); }

        const bsoncxx::document::value reply = db()[database()].run_command(make_document(
            kvp("explain", find.view()), kvp("verbosity", std::string{"queryPlanner"})));
        return bsoncxx::to_json(reply.view());
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(IndexExpansionDb, ACollatedIndexIsRiddenOnlyByAQueryCarryingTheSameCollation) {
    const std::array<IndexSpec, 1> catalogue{{collated_index()}};
    apply(catalogue);

    const bsoncxx::document::value filter = make_document(kvp("title", std::string{"x"}));

    // The query the index was built for.
    const std::string matched = winning_plan(filter.view(), true);
    EXPECT_NE(matched.find(std::string{kCollated}), std::string::npos) << matched;
    EXPECT_EQ(matched.find("COLLSCAN"), std::string::npos) << matched;

    // The same query without it. This is the failure the field exists to make
    // expressible: MongoDB will not use an index built with a different
    // collation than the query carries, and it does not say so — it scans.
    const std::string unmatched = winning_plan(filter.view(), false);
    EXPECT_EQ(unmatched.find(std::string{kCollated}), std::string::npos) << unmatched;
    EXPECT_NE(unmatched.find("COLLSCAN"), std::string::npos) << unmatched;

    drop(kCollated);
}

TEST_F(IndexExpansionDb, AHiddenIndexIsInvisibleToThePlannerAndStillMaintained) {
    const std::array<IndexSpec, 1> hidden{{owner_index(true)}};
    apply(hidden);

    const bsoncxx::document::value filter = make_document(kvp("owner", std::string{"x"}));
    const std::string while_hidden = winning_plan(filter.view(), false);
    EXPECT_EQ(while_hidden.find(std::string{kHidden}), std::string::npos) << while_hidden;
    EXPECT_NE(while_hidden.find("COLLSCAN"), std::string::npos) << while_hidden;

    // Still MAINTAINED, which is the half that makes hiding worth doing: it is
    // listed, and un-hiding it is one collMod rather than a rebuild under load.
    bool listed = false;
    for (const bsoncxx::document::view& index :
         db()[database()][std::string{kCollection}].list_indexes()) {
        listed = listed || index["name"].get_string().value == std::string{kHidden};
    }
    EXPECT_TRUE(listed);

    const std::array<IndexSpec, 1> visible{{owner_index(false)}};
    apply(visible);

    const std::string while_visible = winning_plan(filter.view(), false);
    EXPECT_NE(while_visible.find(std::string{kHidden}), std::string::npos) << while_visible;

    drop(kHidden);
}

TEST_F(IndexExpansionDb, ReApplyingACatalogueThatDeclaresBothIsANoOp) {
    // createIndexes with an existing name and an identical specification is a
    // no-op, which is what lets N instances converge with no coordination — and
    // both new fields are part of that specification, so a collation or a hidden
    // flag that did not round-trip would surface here as IndexOptionsConflict.
    const std::array<IndexSpec, 2> catalogue{{collated_index(), owner_index(true)}};
    apply(catalogue);
    EXPECT_NO_THROW(apply(catalogue));

    drop(kCollated);
    drop(kHidden);
}

// --- what the cluster carries that the catalogue does not say ---------------

TEST_F(IndexExpansionDb, ARetiredIndexTheClusterStillCarriesIsReported) {
    // Against a cluster deliberately put into that state, because a check that
    // reports nothing passes on any cluster in the world.
    //
    // drop_if_present tolerates IndexNotFound — it has to, since it runs against
    // clusters at different states — so a retired index that was never actually
    // dropped is indistinguishable from one that was. For a UNIQUE index that is
    // a superseded constraint still refusing writes against a rule nobody meant
    // to be in force.
    const std::string users_database{
        anvil::testfixture::scratch_names().for_collection("users")};
    ASSERT_FALSE(testapp::kRetiredIndexes.empty());
    const RetiredIndex& retired = testapp::kRetiredIndexes.front();

    EXPECT_TRUE(verify_retired(db(), anvil::testfixture::scratch_names(),
                               testapp::kRetiredIndexes)
                    .empty());

    mongocxx::options::index recreated{};
    recreated.name(std::string{retired.name});
    db()[users_database][std::string{retired.collection}].create_index(
        make_document(kvp("legacy_probe", bsoncxx::types::b_int32{1})), recreated);

    const std::vector<IndexFinding> findings = verify_retired(
        db(), anvil::testfixture::scratch_names(), testapp::kRetiredIndexes);
    ASSERT_EQ(findings.size(), 1U);
    EXPECT_EQ(findings.front().collection, std::string{retired.collection});
    EXPECT_EQ(findings.front().name, std::string{retired.name});

    // Reported, never dropped: the run has already tried once, and a second
    // attempt in the same process would report the same nothing.
    db()[users_database][std::string{retired.collection}].indexes().drop_one(
        std::string{retired.name});
}

TEST_F(IndexExpansionDb, AnIndexNobodyDeclaredIsReported) {
    const std::string users_database{
        anvil::testfixture::scratch_names().for_collection("users")};

    // The catalogue and the cluster agree before anything is done by hand. That
    // half matters as much as the other: a check that always reports something
    // is a check nobody reads.
    EXPECT_TRUE(report_undeclared(db(), anvil::testfixture::scratch_names(), testapp::kIndexes)
                    .empty());

    mongocxx::options::index by_hand{};
    by_hand.name("users_created_during_an_incident");
    db()[users_database]["users"].create_index(
        make_document(kvp("incident_probe", bsoncxx::types::b_int32{1})), by_hand);

    const std::vector<IndexFinding> findings =
        report_undeclared(db(), anvil::testfixture::scratch_names(), testapp::kIndexes);
    ASSERT_EQ(findings.size(), 1U);
    EXPECT_EQ(findings.front().name, "users_created_during_an_incident");
    // `_id_` is on every collection and nobody declares it, so a check that
    // reported it would report one finding per collection forever.
    EXPECT_NE(findings.front().name, "_id_");

    db()[users_database]["users"].indexes().drop_one("users_created_during_an_incident");
}

// --- collection options -----------------------------------------------------

TEST_F(IndexExpansionDb, ReApplyingTheDeclaredCollectionOptionsIsANoOp) {
    // The fixture has already applied them once for this process, so this is the
    // second application against a cluster that is already converged — which is
    // what every instance of a rolling deploy does.
    const CollectionOptionsReport created = apply_collection_options(
        db(), anvil::testfixture::scratch_names(), testapp::kCollectionOptions,
        OptionsPhase::Create);
    EXPECT_TRUE(created.violations.empty()) << created.violations.front().reason;
    EXPECT_EQ(created.created, 0U);

    const CollectionOptionsReport validated = apply_collection_options(
        db(), anvil::testfixture::scratch_names(), testapp::kCollectionOptions,
        OptionsPhase::Validate);
    EXPECT_TRUE(validated.violations.empty());
    // Nothing to apply: the validator on the cluster is already the declared one,
    // so the comparison is what stops a collMod per collection per deploy.
    EXPECT_EQ(validated.validators_applied, 0U);
}

TEST_F(IndexExpansionDb, DifferentOptionsOnAnExistingCollectionAreAnErrorAndNotADivergence) {
    // `drafts` is declared clustered on `_id`, and the fixture created it that
    // way. Clustered is one of the three one-way doors: changing it is a copy of
    // the whole collection, so the only honest answer to a catalogue that now
    // says otherwise is to refuse and say so.
    const std::array<CollectionOptionsSpec, 1> unclustered{
        {{kCollection, {}, {}, 0, 0, nullptr, false, Granularity::None}}};

    const CollectionOptionsReport report = apply_collection_options(
        db(), anvil::testfixture::scratch_names(), unclustered, OptionsPhase::Create);

    ASSERT_EQ(report.violations.size(), 1U);
    EXPECT_EQ(report.violations.front().collection, std::string{kCollection});
    EXPECT_NE(report.violations.front().reason.find("clustered"), std::string::npos)
        << report.violations.front().reason;
    // Reported, never acted on. A silent divergence is how a production cluster
    // stops matching the catalogue that describes it.
    EXPECT_EQ(report.created, 0U);
}

TEST_F(IndexExpansionDb, ACappedDeclarationAgainstAnUncappedCollectionIsReported) {
    // The other direction of the same rule, because a check that only notices
    // one of them passes on half the clusters in the world.
    const std::array<CollectionOptionsSpec, 1> capped{
        {{"audit_log", {}, {}, 1 << 20, 0, nullptr, false, Granularity::None}}};

    const CollectionOptionsReport report = apply_collection_options(
        db(), anvil::testfixture::scratch_names(), capped, OptionsPhase::Create);

    ASSERT_FALSE(report.violations.empty());
    EXPECT_NE(report.violations.front().reason.find("capped"), std::string::npos)
        << report.violations.front().reason;
}

TEST_F(IndexExpansionDb, TheDeclaredValidatorIsInForceOnTheCluster) {
    // A validator is a NET and not the validation — but a net that was never
    // actually attached catches nothing, and nothing else in the suite would
    // notice. So the assertion is a write the server must refuse.
    const std::string database_name{
        anvil::testfixture::scratch_names().for_collection("audit_log")};

    EXPECT_THROW(
        {
            db()[database_name]["audit_log"].insert_one(
                make_document(kvp("at", std::string{"not a date"})));
        },
        mongocxx::operation_exception);
}

}  // namespace
}  // namespace anvil::db
