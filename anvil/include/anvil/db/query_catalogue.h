#pragma once

// The explain contract: every query an application issues is covered by an index
// it declares, and a query that is not fails the build rather than production.
//
// Adding a query without adding its index is not allowed (ENGINEERING_RULES.md §7), and
// that rule is unenforceable by inspection — a COLLSCAN on a collection with
// four hundred rows in a developer's database is indistinguishable from an index
// scan, and stays that way until the collection has four hundred thousand.
// Asking the SERVER is the only check that means anything, so this runs the
// planner against a live cluster and reads back what it chose.
//
// anvil ships the mechanism. The CATALOGUE is the application's: which queries
// exist is a fact about its code, and anvil has none of its own to describe.
// It is the counterpart of the index catalogue in anvil/db/migrations.h, and the
// two are checked against each other — a query with no index fails here, and an
// index no query needs is visible as one nothing cites.
//
// --- why a builder function rather than a document --------------------------
//
// A filter has to be built at runtime (a BSON document is not a constant
// expression) but the TABLE has to be constexpr, so the table holds function
// pointers. The same shape, and the same reason, as PartialFilterFn.
//
// The filter's VALUES do not matter — the planner picks a plan from the query's
// shape, not from what it matches — so a spec builds a filter with placeholder
// ids and still proves exactly what it needs to.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <bsoncxx/document/value.hpp>
#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/db/collections.h"

namespace anvil::db {

using FilterFn = bsoncxx::document::value (*)();

// One query shape to explain. Ordered largest-alignment-first.
struct QuerySpec final {
    // What this query is, for the failure message. A planner failure names a
    // collection and a stage; without this, finding which of the eleven queries
    // over that collection regressed is a search.
    std::string_view name;
    std::string_view collection;
    FilterFn         filter;
    // nullptr when the query is unsorted. A sort is part of the shape: a filter
    // that rides an index perfectly can still force a blocking in-memory sort,
    // and that is a separate failure with a separate fix.
    FilterFn         sort;
};

// What the planner chose. `stage` is the server's own word for it, carried
// verbatim so a failure message says what the server said rather than anvil's
// interpretation of it.
struct ExplainResult final {
    std::string stage;
    // Empty when the winning plan is not an index scan.
    std::string index_name;
    // A COLLSCAN anywhere in the winning plan. The whole point of the check.
    bool        scans_collection;
    // A blocking SORT stage: the server materialised the result set and sorted
    // it in memory. It is bounded by a memory budget and fails outright past it,
    // so a query that passes today on a small collection is one that stops
    // working at a size nobody chose.
    bool        sorts_in_memory;
};

// One failing query, as a line somebody can act on.
struct QueryViolation final {
    std::string query;
    std::string collection;
    std::string reason;
};

// Runs the planner for ONE spec. `queryPlanner` verbosity only: it plans without
// executing, so this costs nothing on a populated cluster and returns something
// meaningful on an empty one.
[[nodiscard]] Result<ExplainResult> explain_query(mongocxx::client& client,
                                                  const DatabaseNames& databases,
                                                  const QuerySpec& spec);

// Every spec in the catalogue. Returns the ones that FAILED, so an empty vector
// is the passing result — which makes the assertion in a test read as
// `EXPECT_TRUE(violations.empty())` and the failure print the list.
//
// A driver error is itself a violation rather than an exception: a check that
// cannot reach the server has not passed, and reporting that as a crash loses
// the other twenty queries it had not reached yet.
[[nodiscard]] std::vector<QueryViolation> check_query_catalogue(
    mongocxx::client& client, const DatabaseNames& databases,
    std::span<const QuerySpec> catalogue);

// A malformed catalogue is a build error when an application static_asserts it.
[[nodiscard]] constexpr bool query_catalogue_is_well_formed(
    std::span<const QuerySpec> catalogue) noexcept {
    // A null `filter` is deliberately NOT checked here. Comparing a function
    // pointer against nullptr is not foldable in a constant expression on every
    // compiler, and a well_formed() that does not compile is worse than one that
    // checks less — the same reason catalogue_is_well_formed leaves
    // PartialFilterFn alone. explain_query refuses a null filter at runtime with
    // the field named, and check_query_catalogue reports it as a violation
    // rather than skipping the spec.
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        if (catalogue[i].name.empty()) { return false; }
        // A collection that is not in the application's table would be explained
        // against a database index of 0 — the first one declared — which is a
        // check that passes by looking at the wrong collection.
        if (!collection_is_declared(catalogue[i].collection)) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (catalogue[j].name == catalogue[i].name) { return false; }
        }
    }
    return true;
}

}  // namespace anvil::db
