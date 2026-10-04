#pragma once

// Versioned index creation, run once at deploy time.
//
// Indexes are NEVER created on a request path: an index build holds its
// collection for the duration and every db_pool thread queues behind it. So this
// is a separate invocation — the application's binary run with a flag — rather
// than a boot step, because a rolling deploy that ran it per instance would stall
// every request the new instance accepted.
//
// Every entry is idempotent: MongoDB treats createIndexes with an existing name
// and an identical specification as a no-op, so N instances converge without
// coordination. A name that exists with a DIFFERENT specification is an error,
// deliberately — silently living with a stale index is how a query starts
// scanning in production.
//
// anvil ships the mechanism. The CATALOGUE is the application's, and it is the
// single source of truth its explain check reads: adding a query without adding
// its index is what anvil/db/query_catalogue.h fails on.
//
// Each entry's database is derived from its collection through database_of,
// never declared here. One mapping means an index and the query that needs it
// cannot end up in different places.

#include <array>
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

// A partial-index filter, as a function that BUILDS one.
//
// The original spelled this as an enum with one enumerator per filter shape, and
// half of them named an application's own fields. A function pointer generalises
// completely: it stays usable in a constexpr table, so the catalogue stays in
// .rodata, and it can express any filter the server accepts rather than the dozen
// somebody happened to think of.
//
// nullptr means a FULL index. That is the common case, and it should read as the
// absence of a filter rather than as a named "None".
using PartialFilterFn = bsoncxx::document::value (*)();

struct IndexKey final {
    std::string_view field;
    std::int32_t     direction;  // 1 ascending, -1 descending
};

// Ordered largest-alignment-first, so the array packs with no interior padding.
//
// Four keys is the cap. A compound index on more than four fields is almost
// always a query that should have been reshaped, and a fixed array keeps the
// whole catalogue in .rodata with no indirection.
struct IndexSpec final {
    std::array<IndexKey, 4> keys;
    std::string_view        collection;
    // Stable and explicit, never generated from the keys. A generated name
    // changes when a key is reordered, and the index is then created again
    // alongside the old one rather than recognised as the same.
    std::string_view        name;
    PartialFilterFn         partial;               // nullptr = a full index
    std::int64_t            expire_after_seconds;  // < 0 when not a TTL index
    std::uint8_t            key_count;
    bool                    unique;
    bool                    sparse;
    // Maintained, but invisible to the planner.
    //
    // It is how you find out whether an index is load-bearing BEFORE dropping
    // one: hide it, watch, and un-hide with a single collMod if something
    // degrades. Recreating a dropped index is a rebuild under load, so the two
    // directions are not symmetric and the cheap one is the one to reach for.
    //
    // Changing this on an existing index is a collMod rather than a rebuild,
    // which is what apply_migrations issues — createIndexes with a different
    // specification is an error by design.
    //
    // A default member initialiser, because the point of appending was that an
    // existing indexes.h still compiles — and without one it compiles while
    // emitting -Wmissing-field-initializers for every entry in every catalogue,
    // which is a warning a reader learns to scroll past.
    bool                    hidden = false;
    // The ICU collation locale this index is built with, or empty for the
    // server's default binary comparison.
    //
    // MongoDB will not use an index built with a different collation than the
    // query carries, and the failure is a COLLSCAN — slowness rather than an
    // error, so it is found under load and by then it is on every query against
    // that collection (docs/09-mongodb.md §3). LocaleSpec::collation is where
    // this string comes from, so the index and the query cannot disagree.
    //
    // On a UNIQUE index it changes what uniqueness MEANS: two values that
    // compare equal under the collation are a duplicate. That is usually the
    // point — one account per address however it is cased — and it is never an
    // accident worth discovering in production.
    //
    // APPENDED LAST, after the three bytes above rather than in
    // largest-alignment-first order (CLAUDE.md §2.3), because every existing
    // indexes.h aggregate-initialises this table positionally and a field
    // inserted in the middle would silently reinterpret every entry. `hidden`
    // fits in the padding the flags already leave, so the table grows by this
    // view alone.
    std::string_view        collation = {};
};

// Stated so a field added without thought is a build failure rather than a
// quiet 16 bytes per entry across the whole catalogue.
static_assert(sizeof(IndexSpec) == 168, "IndexSpec grew: is the new field worth it, and is "
                                        "it appended so existing tables still initialise?");

// An index a previous version created and this one no longer wants.
//
// Dropped BEFORE anything is created, so a database mid-migration never briefly
// carries both a superseded constraint and the one replacing it — which for a
// unique index means writes failing against a rule nobody meant to still be in
// force.
//
// A retired index is named here rather than deleted from the catalogue, because
// deleting it from the catalogue leaves it in place on every cluster that already
// has it. Removing the entry stops creating it; naming it here removes it.
struct RetiredIndex final {
    std::string_view collection;
    std::string_view name;
};

struct MigrationReport final {
    std::size_t  indexes_applied;   // across every database
    std::int32_t previous_version;  // -1 when never migrated
    std::int32_t current_version;
};

// Creates every absent index in `catalogue` and records `schema_version` in each
// database's schema_meta collection.
//
// `schema_version` is the APPLICATION's: it counts that application's migrations,
// and anvil has no opinion about how many there have been. Recording it is what
// lets a deploy tell whether migrations have run against a given cluster.
//
// Safe to run concurrently from N instances. A duplicate-key race on the version
// marker is tolerated rather than retried, because both writers are writing the
// same value.
MigrationReport apply_migrations(mongocxx::client& client, const DatabaseNames& databases,
                                 std::span<const IndexSpec> catalogue,
                                 std::int32_t schema_version,
                                 std::span<const RetiredIndex> retired = {});

// Where the version marker is recorded. anvil's own bookkeeping, not an
// application collection: it is the mechanism's storage, so an application does
// not declare it and cannot accidentally collide with it.
inline constexpr std::string_view kSchemaMetaCollection = "anvil_schema_meta";

// The version recorded in `database`, or -1 when migrations have never run there.
[[nodiscard]] std::int32_t applied_schema_version(mongocxx::client& client,
                                                  std::string_view database);

// --- two checks that read the cluster and REPORT ----------------------------
//
// drop_if_present tolerates IndexNotFound, and it has to: it runs against
// clusters at different states, and a retired index a given deployment never
// created answers exactly that. The consequence is that a retired index which
// was never actually dropped anywhere is indistinguishable from one that was —
// and for a UNIQUE index that means a superseded constraint still refusing
// writes against a rule nobody meant to be in force, with a rejection that looks
// exactly like a legitimate conflict.
//
// Neither of these fails a run. They are the --status output, because an
// operator deciding what to do about a hand-made index needs to be reading it at
// a moment of their choosing.

// One index the cluster and the catalogue disagree about.
struct IndexFinding final {
    std::string database;
    std::string collection;
    std::string name;
};

// Retired indexes the cluster still carries.
//
// Reported and not dropped: the run has already tried once, and a second attempt
// in the same process would report the same nothing.
[[nodiscard]] std::vector<IndexFinding> verify_retired(mongocxx::client& client,
                                                       const DatabaseNames& databases,
                                                       std::span<const RetiredIndex> retired);

// Indexes on a declared collection that the catalogue does not name.
//
// Somebody created it by hand during an incident, and it is either load-bearing
// and undocumented or dead weight being maintained on every write. Both are
// worth knowing. `_id_` and the clustered `_id` index are the server's own and
// are never reported.
[[nodiscard]] std::vector<IndexFinding> report_undeclared(mongocxx::client& client,
                                                          const DatabaseNames& databases,
                                                          std::span<const IndexSpec> catalogue);

// A malformed catalogue is a build error when an application static_asserts it.
// Each condition is a mistake that would otherwise produce an index that is not
// the one anybody meant: a zero key count indexes nothing, a duplicate name
// collides with a different specification, and a key count past the array reads
// off the end.
[[nodiscard]] constexpr bool catalogue_is_well_formed(
    std::span<const IndexSpec> catalogue) noexcept {
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        const IndexSpec& spec = catalogue[i];
        if (spec.collection.empty() || spec.name.empty()) { return false; }
        if (spec.key_count == 0 || spec.key_count > spec.keys.size()) { return false; }
        for (std::size_t k = 0; k < spec.key_count; ++k) {
            if (spec.keys[k].field.empty()) { return false; }
            if (spec.keys[k].direction != 1 && spec.keys[k].direction != -1) { return false; }
        }
        // Names are unique per COLLECTION, which is the scope the server enforces.
        for (std::size_t j = 0; j < i; ++j) {
            if (catalogue[j].collection == spec.collection && catalogue[j].name == spec.name) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace anvil::db
