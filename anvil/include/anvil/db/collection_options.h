#pragma once

// What createCollection needs and what collMod may change.
//
// An index catalogue describes how a collection is READ. This describes what the
// collection IS: clustered or not, capped or not, a timeseries or not, and what
// the server will refuse to store in it. Three of those are decided once, at
// creation, and cannot be changed afterwards without copying the whole
// collection — so they are declared in a table that is reviewed rather than
// discovered from whatever the first `insert` happened to create
// (docs/18-data-migrations.md §11).
//
// An existing collection whose options DIFFER is an error, not a silent
// divergence. That is the stance createIndexes already takes on a differing
// specification, for the same reason: living with a stale one is how a
// production cluster stops matching the catalogue that describes it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <bsoncxx/document/value.hpp>
#include <mongocxx/client.hpp>

#include "anvil/db/collections.h"

namespace anvil::db {

// A $jsonSchema document, as a function that BUILDS one — the same shape and the
// same reason as PartialFilterFn and FilterFn: a BSON document is not a constant
// expression, but the TABLE has to be.
//
// nullptr means no validator, which is the common case and should read as the
// absence of one rather than as a named "None".
using ValidatorFn = bsoncxx::document::value (*)();

// The bucketing interval a timeseries collection groups measurements into.
// None is "not a timeseries collection".
enum class Granularity : std::uint8_t { None = 0, Seconds = 1, Minutes = 2, Hours = 3 };

// Which pass of the migration a call is making. The split exists for one
// sentence in docs/18-data-migrations.md §12: adding a validator to a collection
// that already holds documents is a migration in its own right, and in that
// order — the data step that makes every existing document conform, then the
// validator. The reverse rejects the writes that would have fixed it.
//
// So Create runs before the data steps and Validate runs after them. A
// collection this run CREATES carries its validator immediately, because there
// is nothing in it that could fail one.
enum class OptionsPhase : std::uint8_t { Create = 0, Validate = 1 };

// One collection's shape. Ordered largest-alignment-first, so the table packs
// with no interior padding and stays in .rodata.
struct CollectionOptionsSpec final {
    std::string_view collection;             // 16
    // The measurement's time field, and the field a delete must match on. Both
    // empty for every collection that is not a timeseries.
    //
    // Making the erasure key the meta field would work and would also make it
    // high-cardinality, which is the one thing the bucketing depends on not
    // being — so a collection that needs erasure by subject is a collection that
    // is not a timeseries (docs/18-data-migrations.md §11).
    std::string_view timeseries_time_field;  // 16
    std::string_view timeseries_meta_field;  // 16
    // <= 0 when the collection is not capped. A capped collection forbids
    // deletes AND forbids a TTL index, so an erasure path is impossible — which
    // is why well_formed() refuses one on a collection whose rows have a
    // declared lifetime.
    std::int64_t     capped_size_bytes;      //  8
    std::int64_t     capped_max_documents;   //  8  <= 0 = no document cap
    ValidatorFn      validator;              //  8  nullptr = no validator
    // The secondary `_id` index disappears and an `_id`-range scan becomes the
    // collection order — which is exactly what a data migration's cursor walks.
    //
    // Set at CREATION only. Changing it is a copy of the whole collection, so it
    // is one of the three one-way doors this table exists to make deliberate.
    bool             clustered_on_id;        //  1
    Granularity      granularity;            //  1
};

static_assert(sizeof(CollectionOptionsSpec) == 80,
              "CollectionOptionsSpec grew: is the new field worth it on every entry?");

// One collection whose live options are not the declared ones.
struct OptionsViolation final {
    std::string collection;
    std::string reason;
};

struct CollectionOptionsReport final {
    // Empty is the passing result, so an assertion reads as
    // `EXPECT_TRUE(report.violations.empty())` and the failure prints the list.
    //
    // Collected rather than thrown at the first one: an operator reconciling a
    // cluster against a catalogue wants every difference in one pass, not the
    // alphabetically first.
    std::vector<OptionsViolation> violations;
    std::size_t                   created;
    std::size_t                   validators_applied;
};

// Creates absent collections with their declared options, and reports every
// existing collection whose options differ.
//
// Runs BEFORE the index catalogue: createIndexes creates a missing collection
// implicitly, and one created that way carries none of this.
[[nodiscard]] CollectionOptionsReport apply_collection_options(
    mongocxx::client& client, const DatabaseNames& databases,
    std::span<const CollectionOptionsSpec> specs, OptionsPhase phase);

// A malformed table is a build error when an application static_asserts it.
//
// Every condition here is a combination the server would either reject or —
// worse — accept while quietly making something else impossible.
[[nodiscard]] constexpr bool collection_options_are_well_formed(
    std::span<const CollectionOptionsSpec> specs) noexcept {
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const CollectionOptionsSpec& spec = specs[i];
        if (!collection_is_declared(spec.collection)) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            // Two entries for one collection means one of them is the live
            // declaration and nobody can tell which.
            if (specs[j].collection == spec.collection) { return false; }
        }

        const bool capped = spec.capped_size_bytes > 0;
        const bool timeseries = spec.granularity != Granularity::None;

        // A document cap with no size cap is not a capped collection: the server
        // requires the size, and `max` alone is silently ignored.
        if (!capped && spec.capped_max_documents > 0) { return false; }

        // The three shapes are mutually exclusive, and each pair is a
        // combination the server refuses.
        if (capped && timeseries) { return false; }
        if (capped && spec.clustered_on_id) { return false; }
        if (timeseries && spec.clustered_on_id) { return false; }

        // A timeseries collection is defined by its time field; the meta field
        // is optional. A time field without a granularity is a table saying
        // "timeseries" in one column and "no" in another.
        if (timeseries == spec.timeseries_time_field.empty()) { return false; }
        if (!timeseries && !spec.timeseries_meta_field.empty()) { return false; }

        // A capped collection forbids deletes and forbids a TTL index, so a
        // collection whose rows have a declared LIFETIME can never be capped —
        // the rows would be readable forever and no erasure path could remove
        // them. The application's own table is what says which those are, so
        // this contradiction is a build failure rather than a discovery.
        if (capped && has_lifetime_expiry(spec.collection)) { return false; }
    }
    return true;
}

}  // namespace anvil::db
