#pragma once

// The vocabulary an application needs to declare its collections, with no
// dependency on the application's own configuration header.
//
// Same shape and same reason as anvil/core/locale_spec.h and
// anvil/fs/namespace_spec.h: <anvil_app_config.h> includes THIS to spell its
// tables, and anvil/db/collections.h includes the application's header to read
// them.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace anvil::db {

// One logical database.
//
// A deployment may own more than one on a cluster, and there is a real reason to:
// WiredTiger's cache is shared, so a high-churn collection kept beside the hot
// application data spends pages on rows that are about to expire. Separate
// databases make that churn visible per-namespace in db.stats() and let the churn
// be moved to different storage later without a schema change.
//
// Separate DATABASES, never separate clusters. A replica-set transaction spans
// databases; it cannot span deployments, and a multi-document invariant that
// crosses the boundary needs one.
struct DatabaseSpec final {
    // A stable key, not the physical name. The physical name is a deployment
    // decision read from the environment at boot; this is what the code refers to.
    std::string_view key;
};

static_assert(sizeof(DatabaseSpec) == sizeof(std::string_view));

// One collection: its name, which database it lives in, and whether its rows have
// a LIFETIME.
struct CollectionSpec final {
    std::string_view name;          // 16

    // The field a TTL index expires on, when that index expresses a document
    // LIFETIME. Empty for every other collection.
    //
    // The distinction is not cosmetic. A TTL index is a garbage collector, not an
    // access control: the monitor runs roughly every 60 seconds, so an expired
    // session or capability token stays READABLE and would still authenticate.
    // Every query against a collection that names a field here must carry an
    // explicit expiry predicate as well, which is what append_not_expired is for,
    // and what tools/check-db-discipline.sh fails the build over.
    //
    // A RETENTION policy is the opposite case and must leave this empty. An audit
    // log with a 400-day TTL is keeping history, not expressing a lifetime;
    // filtering its reads on `at > now` would return nothing at all.
    std::string_view expiry_field;  // 16

    // Index into the application's database table.
    std::uint8_t     database;      //  1
};

static_assert(sizeof(CollectionSpec) == 2 * sizeof(std::string_view) + sizeof(std::size_t),
              "CollectionSpec must not grow padding");

}  // namespace anvil::db
