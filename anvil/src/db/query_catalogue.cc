#include "anvil/db/query_catalogue.h"

#include <exception>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/exception.hpp>

#include "anvil/db/codec.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

// The stage names that matter, spelled once.
constexpr std::string_view kCollectionScan = "COLLSCAN";
constexpr std::string_view kIndexScan = "IXSCAN";
constexpr std::string_view kSort = "SORT";

// Walks a winning plan and records what it found.
//
// Recursive because a plan is a TREE: a FETCH sits above an IXSCAN, a SORT above
// a FETCH, and a SHARD_MERGE above several of those. Reading only the root stage
// reports "FETCH" for every indexed query and "SORT" for every sorted one, which
// answers neither question this check asks.
//
// It also descends through `queryPlan`, which is where the slot-based engine
// puts the classic plan it was built from — a server that chose SBE would
// otherwise report a root stage of nothing recognisable and every query would
// look like it passed.
void walk_plan(const bsoncxx::document::view& plan, ExplainResult& out, int depth) {
    // A tree this deep is a plan nobody wrote; the bound exists so a malformed
    // or hostile response cannot recurse without end.
    if (depth > 32) { return; }

    if (auto stage = plan.find("stage"); stage != plan.end()) {
        if (stage->type() == bsoncxx::type::k_string) {
            const bsoncxx::stdx::string_view name = stage->get_string().value;
            const std::string_view view{name.data(), name.size()};
            // The ROOT stage is what the caller sees as "the" stage, so it is
            // recorded only on the way in and never overwritten by a child.
            if (out.stage.empty()) { out.stage.assign(view); }
            if (view == kCollectionScan) { out.scans_collection = true; }
            if (view == kSort) { out.sorts_in_memory = true; }
            if (view == kIndexScan && out.index_name.empty()) {
                if (auto index = plan.find("indexName");
                    index != plan.end() && index->type() == bsoncxx::type::k_string) {
                    const bsoncxx::stdx::string_view value = index->get_string().value;
                    out.index_name.assign(value.data(), value.size());
                }
            }
        }
    }

    for (const bsoncxx::document::element& field : plan) {
        if (field.type() == bsoncxx::type::k_document) {
            walk_plan(field.get_document().value, out, depth + 1);
        } else if (field.type() == bsoncxx::type::k_array) {
            for (const bsoncxx::array::element& entry : field.get_array().value) {
                if (entry.type() == bsoncxx::type::k_document) {
                    walk_plan(entry.get_document().value, out, depth + 1);
                }
            }
        }
    }
}

}  // namespace

Result<ExplainResult> explain_query(mongocxx::client& client, const DatabaseNames& databases,
                                    const QuerySpec& spec) {
    if (spec.filter == nullptr) { return fail(ErrorCode::Internal, "filter"); }

    try {
        const bsoncxx::document::value filter = spec.filter();

        bsoncxx::builder::basic::document find;
        find.append(kvp("find", bsoncxx::types::b_string{codec::key_of(spec.collection)}));
        find.append(kvp("filter", filter.view()));
        bsoncxx::document::value sort = make_document();
        if (spec.sort != nullptr) {
            sort = spec.sort();
            find.append(kvp("sort", sort.view()));
        }

        // queryPlanner verbosity PLANS without executing, so this costs nothing
        // on a populated cluster and still answers on an empty one — which is
        // what lets the check run in CI against a fresh database.
        const bsoncxx::document::value command = make_document(
            kvp("explain", find.view()),
            kvp("verbosity", bsoncxx::types::b_string{codec::key_of("queryPlanner")}));

        mongocxx::database database =
            client[std::string{databases.for_collection(spec.collection)}];
        const bsoncxx::document::value reply = database.run_command(command.view());

        auto planner = reply.view().find("queryPlanner");
        if (planner == reply.view().end() || planner->type() != bsoncxx::type::k_document) {
            return fail(ErrorCode::Internal, "queryPlanner");
        }
        const bsoncxx::document::view plan_root = planner->get_document().value;
        auto winning = plan_root.find("winningPlan");
        if (winning == plan_root.end() || winning->type() != bsoncxx::type::k_document) {
            return fail(ErrorCode::Internal, "winningPlan");
        }

        ExplainResult result{};
        walk_plan(winning->get_document().value, result, 0);
        return result;
    } catch (const mongocxx::exception&) {
        // Translated rather than propagated, for the same reason every other
        // driver call in this tree is: an exception escaping into a pool task
        // calls std::terminate. Nothing of the driver's text is carried.
        return fail(ErrorCode::Internal, "explain");
    } catch (const std::exception&) {
        return fail(ErrorCode::Internal, "explain");
    }
}

std::vector<QueryViolation> check_query_catalogue(mongocxx::client& client,
                                                  const DatabaseNames& databases,
                                                  std::span<const QuerySpec> catalogue) {
    std::vector<QueryViolation> violations;
    for (const QuerySpec& spec : catalogue) {
        const Result<ExplainResult> explained = explain_query(client, databases, spec);
        if (!explained) {
            // A check that could not reach the server has NOT passed. Reported
            // as a violation rather than thrown, so the remaining specs are
            // still checked and the report names every problem at once.
            violations.push_back(QueryViolation{.query = std::string{spec.name},
                                                .collection = std::string{spec.collection},
                                                .reason = "explain failed"});
            continue;
        }
        if (explained.value().scans_collection) {
            violations.push_back(
                QueryViolation{.query = std::string{spec.name},
                               .collection = std::string{spec.collection},
                               .reason = "COLLSCAN — the query has no index to ride"});
        }
        if (explained.value().sorts_in_memory) {
            violations.push_back(
                QueryViolation{.query = std::string{spec.name},
                               .collection = std::string{spec.collection},
                               .reason = "blocking SORT — the index does not provide the "
                                         "order, so the server materialises and sorts"});
        }
    }
    return violations;
}

}  // namespace anvil::db
