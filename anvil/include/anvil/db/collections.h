#pragma once

// The application's collections, resolved against its declared databases.
//
// anvil never derives a collection or database name from request data, and this
// is the mechanism that makes that structural rather than a rule: every name in
// the system comes from a constexpr table, and a repository is constructed with
// one of them. A name assembled from a request field is an injection primitive no
// amount of validation downstream repairs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <anvil_app_config.h>

#include "anvil/db/collection_spec.h"

namespace anvil::db {

inline constexpr std::size_t kDatabaseCount = config::kDatabases.size();
inline constexpr std::size_t kCollectionCount = config::kCollections.size();

static_assert(kDatabaseCount >= 1, "at least one database must be declared");
static_assert(kDatabaseCount <= 255, "a database is referenced by a one-byte index");
static_assert(kCollectionCount >= 1, "at least one collection must be declared");

// Linear. A collection table is tens of entries of mostly-distinct lengths, so a
// scan over one or two cache lines beats the hash a map would compute — and this
// is constexpr, which a map is not.
[[nodiscard]] constexpr const CollectionSpec* collection_spec(std::string_view name) noexcept {
    for (const CollectionSpec& spec : config::kCollections) {
        if (spec.name == name) { return &spec; }
    }
    return nullptr;
}

// Whether a name is in the table at all, as a BOOLEAN rather than as a pointer
// test against collection_spec().
//
// The two say the same thing, but a pointer derived from a table and compared
// against nullptr is not something every compiler folds in a constant
// expression — and the callers that need this answer are exactly the ones
// asserting their own tables at compile time (anvil/db/query_catalogue.h).
[[nodiscard]] constexpr bool collection_is_declared(std::string_view name) noexcept {
    for (const CollectionSpec& spec : config::kCollections) {
        if (spec.name == name) { return true; }
    }
    return false;
}

// The expiry field of a collection whose TTL index expresses a document LIFETIME,
// or empty for every other collection. See CollectionSpec::expiry_field for why
// the distinction between a lifetime and a retention policy matters.
[[nodiscard]] constexpr std::string_view lifetime_expiry_field(std::string_view name) noexcept {
    const CollectionSpec* spec = collection_spec(name);
    return spec == nullptr ? std::string_view{} : spec->expiry_field;
}

[[nodiscard]] constexpr bool has_lifetime_expiry(std::string_view name) noexcept {
    return !lifetime_expiry_field(name).empty();
}

// Which database a collection lives in — DERIVED from the table rather than
// maintained as a second mapping. A hand-maintained one drifts the day somebody
// adds a collection and updates two of the three places that need to know.
[[nodiscard]] constexpr std::uint8_t database_of(std::string_view name) noexcept {
    const CollectionSpec* spec = collection_spec(name);
    return spec == nullptr ? 0U : spec->database;
}

// The PHYSICAL database names, resolved once at boot from the environment and
// validated distinct there.
//
// Views, not strings: every instance is built from the application's frozen
// configuration, which outlives all of them, or from a test-local string in the
// same scope.
struct DatabaseNames final {
    std::array<std::string_view, kDatabaseCount> names;

    [[nodiscard]] constexpr std::string_view name_of(std::uint8_t database) const noexcept {
        return names[database];
    }

    [[nodiscard]] constexpr std::string_view for_collection(std::string_view collection) const noexcept {
        return name_of(database_of(collection));
    }
};

// A malformed table is a build error, not a runtime surprise. Each condition is a
// mistake that would otherwise ship: a duplicate name makes the second entry
// unreachable, and a database index past the end reads off the array.
[[nodiscard]] constexpr bool collections_are_well_formed() noexcept {
    for (std::size_t i = 0; i < kCollectionCount; ++i) {
        const CollectionSpec& spec = config::kCollections[i];
        if (spec.name.empty()) { return false; }
        if (spec.database >= kDatabaseCount) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (config::kCollections[j].name == spec.name) { return false; }
        }
    }
    for (std::size_t i = 0; i < kDatabaseCount; ++i) {
        if (config::kDatabases[i].key.empty()) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (config::kDatabases[j].key == config::kDatabases[i].key) { return false; }
        }
    }
    return true;
}

static_assert(collections_are_well_formed(),
              "empty or duplicate collection name, duplicate database key, or a "
              "collection naming a database that does not exist");

}  // namespace anvil::db
