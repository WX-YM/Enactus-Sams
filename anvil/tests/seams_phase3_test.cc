// The two seams phase 3 adds, exercised from OUTSIDE anvil — which is the only
// place a seam can fail cheaply.
//
// Most of what matters here is a static_assert in the reference application's
// own tables, so most of this suite is a build. What is left is the runtime
// behaviour a table cannot assert about itself: what anvil does with a value the
// table does not declare, which during a rolling deploy is a row or a token
// minted by a newer process.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <iterator>
#include <span>

#include "anvil/audit/action.h"
#include "anvil/db/query_catalogue.h"
#include "anvil/http/rate_limit.h"
#include "anvil/identity/capability_spec.h"
#include "testapp/audit_actions.h"
#include "testapp/capabilities.h"
#include "testapp/indexes.h"
#include "testapp/queries.h"
#include "testapp/rate_limits.h"

namespace {

using anvil::audit::AuditAction;
using anvil::audit::AuditActionSpec;
using anvil::audit::AuditClass;
using anvil::identity::CapabilityScope;
using anvil::identity::CapabilityScopeSpec;

// --- the capability scope seam ----------------------------------------------

TEST(CapabilitySeam, TheApplicationsTableResolvesByValue) {
    const auto* spec = anvil::identity::capability_spec_of(
        testapp::kScopes, CapabilityScope::of(testapp::Scope::MediaUpload));
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->name, "MediaUpload");
    EXPECT_TRUE(spec->single_use);
}

TEST(CapabilitySeam, AnUndeclaredScopeResolvesToNothing) {
    // A token minted by a NEWER process during a rolling deploy. An older
    // instance must be able to say "I do not know this scope" and refuse, rather
    // than invent a meaning for it.
    EXPECT_EQ(anvil::identity::capability_spec_of(testapp::kScopes,
                                                  CapabilityScope::from_stored(9999)),
              nullptr);
    EXPECT_TRUE(anvil::identity::capability_scope_name(testapp::kScopes,
                                                       CapabilityScope::from_stored(9999))
                    .empty());
}

TEST(CapabilitySeam, ZeroIsReservedAndNamesNoScope) {
    // A default-initialised value must never name a real scope, which is why the
    // well-formedness check refuses a non-positive one.
    EXPECT_EQ(anvil::identity::capability_spec_of(testapp::kScopes,
                                                  CapabilityScope::from_stored(0)),
              nullptr);
}

TEST(CapabilitySeam, AMalformedTableIsRejected) {
    // The reference table is asserted well-formed at compile time. What that
    // assertion is actually checking is asserted here, against tables anvil
    // would refuse — because a well_formed() that returns true for everything
    // passes every static_assert in the world.
    static constexpr std::array<CapabilityScopeSpec, 2> kDuplicateValue{{
        {"A", 1, true},
        {"B", 1, true},
    }};
    static constexpr std::array<CapabilityScopeSpec, 2> kDuplicateName{{
        {"A", 1, true},
        {"A", 2, true},
    }};
    static constexpr std::array<CapabilityScopeSpec, 1> kEmptyName{{{"", 1, true}}};
    static constexpr std::array<CapabilityScopeSpec, 1> kZeroValue{{{"A", 0, true}}};

    EXPECT_FALSE(anvil::identity::capability_table_is_well_formed(kDuplicateValue));
    EXPECT_FALSE(anvil::identity::capability_table_is_well_formed(kDuplicateName));
    EXPECT_FALSE(anvil::identity::capability_table_is_well_formed(kEmptyName));
    EXPECT_FALSE(anvil::identity::capability_table_is_well_formed(kZeroValue));
    EXPECT_TRUE(anvil::identity::capability_table_is_well_formed(testapp::kScopes));
}

TEST(CapabilitySeam, AScopeCostsFourBytesAndIsTriviallyCopyable) {
    // It travels through a filter and a decoded row, so its size is a storage
    // decision rather than a style one.
    static_assert(sizeof(CapabilityScope) == 4);
    static_assert(std::is_trivially_copyable_v<CapabilityScope>);
    SUCCEED();
}

// --- the audit action seam --------------------------------------------------

TEST(AuditSeam, TheApplicationsTableResolvesByValueAndByName) {
    const AuditAction denied = AuditAction::of(testapp::Action::AccessDenied);
    EXPECT_EQ(anvil::audit::audit_action_name(testapp::kAuditActions, denied), "AccessDenied");

    const auto parsed =
        anvil::audit::audit_action_from_name(testapp::kAuditActions, "AccessDenied");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, denied);
}

TEST(AuditSeam, AnUnknownNameParsesToNothing) {
    EXPECT_FALSE(
        anvil::audit::audit_action_from_name(testapp::kAuditActions, "NoSuchAction").has_value());
    EXPECT_FALSE(anvil::audit::audit_action_from_name(testapp::kAuditActions, "").has_value());
}

TEST(AuditSeam, AnActionThisBuildDoesNotDeclareHasNoName) {
    // Rows outlive the code that wrote them by far longer than any deploy cycle,
    // so a reader has to be able to say "I do not know this action" rather than
    // render a plausible name for it.
    EXPECT_TRUE(anvil::audit::audit_action_name(testapp::kAuditActions,
                                                AuditAction::from_stored(9999))
                    .empty());
}

TEST(AuditSeam, AnUndeclaredActionClassifiesAsAChange) {
    // The SAFE direction. Treating an unknown row as compressible traffic would
    // let an older instance shed a newer one's rows during exactly the flood
    // they were written to describe.
    EXPECT_EQ(anvil::audit::audit_class_of(testapp::kAuditActions,
                                           AuditAction::from_stored(9999)),
              AuditClass::Change);
}

TEST(AuditSeam, AMalformedTableIsRejected) {
    static constexpr std::array<AuditActionSpec, 2> kDuplicateValue{{
        {"A", 1, AuditClass::Change},
        {"B", 1, AuditClass::Change},
    }};
    static constexpr std::array<AuditActionSpec, 1> kEmptyName{{{"", 1, AuditClass::Change}}};
    static constexpr std::array<AuditActionSpec, 1> kZeroValue{{{"A", 0, AuditClass::Change}}};

    EXPECT_FALSE(anvil::audit::audit_table_is_well_formed(kDuplicateValue));
    EXPECT_FALSE(anvil::audit::audit_table_is_well_formed(kEmptyName));
    EXPECT_FALSE(anvil::audit::audit_table_is_well_formed(kZeroValue));
    EXPECT_TRUE(anvil::audit::audit_table_is_well_formed(testapp::kAuditActions));
}

TEST(AuditSeam, ExactlyOneActionIsCompressible) {
    // Not a rule anvil enforces — an application may class more than one row as
    // traffic — but a property of THIS table, asserted so that flipping a
    // mutation to Traffic is a test failure rather than a shedding policy that
    // quietly starts discarding the only copy of what somebody did.
    std::size_t traffic = 0;
    for (const AuditActionSpec& spec : testapp::kAuditActions) {
        if (spec.cls == AuditClass::Traffic) { ++traffic; }
    }
    EXPECT_EQ(traffic, 1U);
}

// --- the index and query catalogues -----------------------------------------

TEST(QueryCatalogue, EveryQueryNamesACollectionTheApplicationDeclares) {
    // A collection outside the table would be explained against database index
    // 0 — the first one declared — which is a check that passes by looking at
    // the wrong collection entirely.
    for (const anvil::db::QuerySpec& spec : testapp::queries::kQueries) {
        EXPECT_TRUE(anvil::db::collection_is_declared(spec.collection))
            << spec.name << " names " << spec.collection;
    }
}

TEST(QueryCatalogue, EveryQueryCanRideAnIndex) {
    // The cheap half of the explain contract, and the half that needs no server:
    // a query with no index available to it cannot possibly ride one. The
    // expensive half — WHICH index, and whether the sort comes off the walk — is
    // query_catalogue_db_test.cc.
    //
    // "Available" is not the same as "declared". Every collection has an
    // automatic `_id_` index, so a query whose filter is an `_id` equality and
    // nothing else is covered without the catalogue naming anything — which is
    // the whole reason a collection under a compound primary key can carry no
    // secondary index at all (anvil/sections/repository.h).
    //
    // The filter is BUILT rather than assumed, because the distinction is a
    // property of the query's shape: a read filtering on a SUB-KEY of the
    // primary key, or on `_id` alongside something else, does not get the free
    // ride, and the symptom would be a COLLSCAN rather than an error.
    for (const anvil::db::QuerySpec& query : testapp::queries::kQueries) {
        bool declared = false;
        for (const anvil::db::IndexSpec& index : testapp::kIndexes) {
            declared = declared || index.collection == query.collection;
        }
        if (declared) { continue; }

        ASSERT_NE(query.filter, nullptr) << query.name;
        const bsoncxx::document::value filter = query.filter();
        const bsoncxx::document::view view = filter.view();
        const bool primary_key_only =
            std::distance(view.begin(), view.end()) == 1 && view.begin()->key() == "_id";
        EXPECT_TRUE(primary_key_only)
            << query.name << " queries " << query.collection
            << ", which declares no index, and does not filter on the primary key alone";
    }
}

TEST(QueryCatalogue, AMalformedCatalogueIsRejected) {
    static constexpr std::array<anvil::db::QuerySpec, 1> kEmptyName{
        {{"", "users", &testapp::queries::login_by_email, nullptr}}};
    static constexpr std::array<anvil::db::QuerySpec, 1> kUnknownCollection{
        {{"a", "no_such_collection", &testapp::queries::login_by_email, nullptr}}};

    EXPECT_FALSE(anvil::db::query_catalogue_is_well_formed(kEmptyName));
    EXPECT_FALSE(anvil::db::query_catalogue_is_well_formed(kUnknownCollection));
    EXPECT_TRUE(anvil::db::query_catalogue_is_well_formed(testapp::queries::kQueries));
}

TEST(QueryCatalogue, EveryQueryCarriesAFilter) {
    // Checked here rather than in query_catalogue_is_well_formed, because
    // comparing a function pointer against nullptr is not foldable in a constant
    // expression on every compiler. A null filter still cannot reach a server:
    // explain_query refuses one by name.
    for (const anvil::db::QuerySpec& spec : testapp::queries::kQueries) {
        EXPECT_NE(spec.filter, nullptr) << spec.name << " declares no filter";
    }
}

TEST(IndexCatalogue, EveryLifetimeBoundedCollectionHasATtlIndexOnItsExpiryField) {
    // The two halves of a lifetime have to agree: kCollections names the field
    // every query must filter on, and the catalogue has to actually expire that
    // field. Naming one without the other gives either rows that never expire or
    // an expiry filter over a column nothing sets.
    for (const anvil::db::CollectionSpec& collection : anvil::config::kCollections) {
        if (collection.expiry_field.empty()) { continue; }
        bool has_ttl = false;
        for (const anvil::db::IndexSpec& index : testapp::kIndexes) {
            if (index.collection != collection.name) { continue; }
            if (index.expire_after_seconds < 0) { continue; }
            has_ttl = has_ttl || index.keys[0].field == collection.expiry_field;
        }
        EXPECT_TRUE(has_ttl) << collection.name << " names an expiry field with no TTL index";
    }
}

}  // namespace

// --- the rate-limit seam ----------------------------------------------------
//
// The application's table is static_asserted in testapp/rate_limits.h, so the
// build already proved it well-formed. What is left is the part a table cannot
// assert about itself: that the check actually rejects each of the four mistakes,
// none of which is visible in a review of the rows.

TEST(RateLimitSeam, TheApplicationsTableIsWellFormed) {
    EXPECT_TRUE(anvil::http::rate_limit_table_is_well_formed(testapp::kRateLimits));
    // An empty table is well-formed: a deployment with no limits declared has
    // made a choice, and refusing to build would be anvil having an opinion about
    // it. What is not permitted is a MALFORMED rule.
    EXPECT_TRUE(anvil::http::rate_limit_table_is_well_formed({}));
}

TEST(RateLimitSeam, AMalformedTableIsRejected) {
    using anvil::http::RateLimitRule;
    using anvil::http::rate_limit_table_is_well_formed;

    // An empty bucket name puts every rule that has one into the same Redis key.
    const std::array<RateLimitRule, 1> nameless{{{"", std::chrono::seconds{60}, 10}}};
    EXPECT_FALSE(rate_limit_table_is_well_formed(nameless));

    // A zero window is a key with no expiry: the first burst fills the bucket and
    // it never drains, so the rule becomes a permanent refusal.
    const std::array<RateLimitRule, 1> endless{{{"login", std::chrono::seconds{0}, 10}}};
    EXPECT_FALSE(rate_limit_table_is_well_formed(endless));

    // Zero events refuses the first request. A rule meant to be off is deleted.
    const std::array<RateLimitRule, 1> closed{{{"login", std::chrono::seconds{60}, 0}}};
    EXPECT_FALSE(rate_limit_table_is_well_formed(closed));

    // The one that arrives by copy-paste: two rules sharing a bucket share a
    // COUNTER, so the tighter budget is spent by the other rule's traffic. This
    // is the defect RateLimitRule::bucket exists to have fixed, reintroduced.
    const std::array<RateLimitRule, 2> shared{{
        {"login", std::chrono::seconds{60}, 20},
        {"login", std::chrono::minutes{15}, 10},
    }};
    EXPECT_FALSE(rate_limit_table_is_well_formed(shared));
}

TEST(RateLimitSeam, EveryRuleHasItsOwnBucket) {
    // Restated over the REAL table rather than only over a constructed one: the
    // static_assert proves it at the declaration, and this proves the property
    // somebody would actually go looking for after a 429 nobody can explain.
    for (std::size_t i = 0; i < testapp::kRateLimits.size(); ++i) {
        for (std::size_t j = i + 1; j < testapp::kRateLimits.size(); ++j) {
            EXPECT_NE(testapp::kRateLimits[i].bucket, testapp::kRateLimits[j].bucket)
                << i << " and " << j;
        }
    }
}
