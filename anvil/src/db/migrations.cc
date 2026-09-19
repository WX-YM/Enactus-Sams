// versioned-write-exempt: the schema-version marker is a HIGH-WATER MARK, not a
// document anybody edits. The write is `$max`, which is monotone — two instances
// applying the same version race harmlessly and an older instance can never walk
// the recorded version backwards — so there is no lost update for a version
// filter to prevent, and a version on the version marker would be a counter
// counting a counter (docs/09-mongodb.md §7).

#include "anvil/db/migrations.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/update.hpp>

#include "anvil/db/collation.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

constexpr std::string_view kVersionDocumentId = "indexes";

[[nodiscard]] bsoncxx::document::value keys_document(const IndexSpec& spec) {
    bsoncxx::builder::basic::document keys;
    for (std::size_t i = 0; i < spec.key_count; ++i) {
        keys.append(kvp(std::string{spec.keys[i].field},
                        bsoncxx::types::b_int32{spec.keys[i].direction}));
    }
    return keys.extract();
}

// `partial` and `collation` are taken by reference and must OUTLIVE the returned
// options.
//
// partial_filter_expression stores a VIEW, not a copy, so passing spec.partial()
// directly leaves the options holding a view into a temporary that died at the
// end of the statement. ASan catches it as a heap-use-after-free inside the
// driver, which is a long way from where the mistake is — so the lifetime is made
// explicit in the signature instead. collation() has exactly the same shape and
// is here for exactly the same reason.
[[nodiscard]] mongocxx::options::index index_options(
    const IndexSpec& spec, const std::optional<bsoncxx::document::value>& partial,
    const std::optional<bsoncxx::document::value>& collation) {
    mongocxx::options::index options{};
    // A stable, explicit name. A generated one changes when a key is reordered,
    // and the index is then created alongside the old one rather than recognised
    // as the same.
    options.name(std::string{spec.name});
    if (spec.unique) { options.unique(true); }
    if (spec.sparse) { options.sparse(true); }
    if (spec.expire_after_seconds >= 0) {
        options.expire_after(std::chrono::seconds{spec.expire_after_seconds});
    }
    if (partial.has_value()) { options.partial_filter_expression(partial->view()); }
    if (collation.has_value()) { options.collation(collation->view()); }
    // Only when the catalogue asks for it. Setting hidden(false) explicitly would
    // put the option into the specification on every index, and a specification
    // that differs from the one a previous version created is an error rather
    // than an update — which is the whole reason hidden is reconciled with
    // collMod below instead of here.
    if (spec.hidden) { options.hidden(true); }
    return options;
}

// Two instances migrating at the same moment is the NORMAL case, not an error
// path: a rolling deploy runs this from every new instance.
[[nodiscard]] bool is_namespace_exists(const mongocxx::operation_exception& error) noexcept {
    return error.code().value() == 48;  // NamespaceExists
}

[[nodiscard]] bool is_index_not_found(const mongocxx::operation_exception& error) noexcept {
    return error.code().value() == 27;  // IndexNotFound
}

// A drop against a collection that does not exist yet, which is EVERY retired
// index on the first migration of a fresh cluster.
//
// Distinct from IndexNotFound (27), and that distinction is why this was
// missing: on a cluster that already has the collection, a retired index the
// deployment never created answers 27 and is tolerated, so the gap is invisible
// everywhere except the one place it matters — the first deploy, where the whole
// migration fails before creating a single index.
[[nodiscard]] bool is_namespace_not_found(const mongocxx::operation_exception& error) noexcept {
    return error.code().value() == 26;  // NamespaceNotFound
}

void create_collection_if_absent(mongocxx::database& database, std::string_view collection) {
    try {
        database.create_collection(std::string{collection});
    } catch (const mongocxx::operation_exception& e) {
        if (!is_namespace_exists(e)) { throw; }
    }
}

// One live index, as much of it as anything here needs to know.
struct LiveIndex final {
    std::string name;
    bool        hidden;
};

// listIndexes for one collection. Empty for a collection that does not exist,
// which is the normal state of every collection on a fresh cluster.
[[nodiscard]] std::vector<LiveIndex> live_indexes(mongocxx::database& database,
                                                  std::string_view collection) {
    std::vector<LiveIndex> live;
    try {
        auto cursor = database[std::string{collection}].list_indexes();
        for (const bsoncxx::document::view& index : cursor) {
            const bsoncxx::document::element name = index["name"];
            if (!name || name.type() != bsoncxx::type::k_string) { continue; }
            const bsoncxx::document::element hidden = index["hidden"];
            live.push_back(LiveIndex{
                std::string{name.get_string().value},
                hidden && hidden.type() == bsoncxx::type::k_bool && hidden.get_bool().value});
        }
    } catch (const mongocxx::operation_exception& e) {
        if (!is_namespace_not_found(e)) { throw; }
    }
    return live;
}

// Hiding and un-hiding is a collMod, never a rebuild.
//
// createIndexes refuses a name that exists with a DIFFERENT specification, which
// is exactly the protection docs/09-mongodb.md §7 wants for every other option —
// and exactly the wrong answer for this one, because `hidden` is the option you
// change deliberately to find out whether an index is load-bearing before
// dropping it. So it is reconciled against the live state instead: one collMod
// per index whose visibility does not match the catalogue, and nothing at all
// when it already does.
void reconcile_hidden(mongocxx::database& database, std::string_view collection,
                      std::span<const IndexSpec> catalogue, std::uint8_t which) {
    const std::vector<LiveIndex> live = live_indexes(database, collection);
    for (const IndexSpec& spec : catalogue) {
        if (spec.collection != collection || database_of(spec.collection) != which) { continue; }
        for (const LiveIndex& index : live) {
            if (index.name != spec.name || index.hidden == spec.hidden) { continue; }
            database.run_command(make_document(
                kvp("collMod", std::string{collection}),
                kvp("index", [&spec](sub_document sub) {
                    sub.append(kvp("name", std::string{spec.name}));
                    sub.append(kvp("hidden", bsoncxx::types::b_bool{spec.hidden}));
                })));
        }
    }
}

void drop_if_present(mongocxx::database& database, const RetiredIndex& retired) {
    try {
        database[std::string{retired.collection}].indexes().drop_one(std::string{retired.name});
    } catch (const mongocxx::operation_exception& e) {
        // Already gone is success. Another instance dropped it, this cluster
        // never had it — a deployment created after the index was retired — or
        // the collection itself does not exist yet, which is every retired index
        // on a cluster being migrated for the first time.
        if (!is_index_not_found(e) && !is_namespace_exists(e) && !is_namespace_not_found(e)) {
            throw;
        }
    }
}

[[nodiscard]] std::size_t migrate_database(mongocxx::client& client, std::string_view name,
                                           std::uint8_t which,
                                           std::span<const IndexSpec> catalogue,
                                           std::span<const RetiredIndex> retired,
                                           std::int32_t schema_version) {
    mongocxx::database database = client[std::string{name}];

    // Before anything is created, so a database mid-migration never briefly
    // carries both a superseded constraint and the one replacing it.
    for (const RetiredIndex& entry : retired) {
        if (database_of(entry.collection) != which) { continue; }
        drop_if_present(database, entry);
    }

    std::size_t applied = 0;
    for (const IndexSpec& spec : catalogue) {
        if (database_of(spec.collection) != which) { continue; }
        create_collection_if_absent(database, spec.collection);

        // Both documents are named locals: create_index takes views, and a
        // temporary would be gone before the driver read it.
        const bsoncxx::document::value keys = keys_document(spec);
        const std::optional<bsoncxx::document::value> partial =
            spec.partial == nullptr ? std::nullopt : std::optional{spec.partial()};

        const std::optional<bsoncxx::document::value> collation =
            spec.collation.empty() ? std::nullopt : std::optional{collation_for(spec.collation)};

        database[std::string{spec.collection}].create_index(
            keys.view(), index_options(spec, partial, collation));
        ++applied;
    }

    // A second pass, after every index exists: the visibility of an index that
    // was just created is already right, and the one this is for was created by
    // a previous version with the opposite setting. Once per COLLECTION rather
    // than once per index, because listIndexes answers for the whole collection
    // and collMod takes a collection lock.
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        if (database_of(catalogue[i].collection) != which) { continue; }
        bool already_reconciled = false;
        for (std::size_t j = 0; j < i; ++j) {
            already_reconciled = already_reconciled ||
                                 catalogue[j].collection == catalogue[i].collection;
        }
        if (already_reconciled) { continue; }
        reconcile_hidden(database, catalogue[i].collection, catalogue, which);
    }

    // $max rather than $set: two instances applying the same version race
    // harmlessly, and an OLDER instance can never walk the recorded version
    // backwards. upsert makes the first run create both the row and the
    // collection.
    mongocxx::options::update options{};
    options.upsert(true);
    client[std::string{name}][std::string{kSchemaMetaCollection}].update_one(
        make_document(kvp("_id", std::string{kVersionDocumentId})),
        make_document(kvp("$max",
                          [schema_version](sub_document sub) {
                              sub.append(kvp("version", bsoncxx::types::b_int32{schema_version}));
                          }),
                      kvp("$currentDate",
                          [](sub_document sub) {
                              sub.append(kvp("applied_at", bsoncxx::types::b_bool{true}));
                          })),
        options);

    return applied;
}

}  // namespace

std::int32_t applied_schema_version(mongocxx::client& client, std::string_view database) {
    const auto document =
        client[std::string{database}][std::string{kSchemaMetaCollection}].find_one(
            make_document(kvp("_id", std::string{kVersionDocumentId})));
    if (!document) { return -1; }

    const bsoncxx::document::element version = document->view()["version"];
    if (!version || version.type() != bsoncxx::type::k_int32) { return -1; }
    return version.get_int32().value;
}

namespace {

// The two names the server owns. `_id_` is on every collection and nobody
// declares it; `_id_clustered` is what a clustered collection's primary key is
// called, and it is created by the collection rather than by the catalogue.
[[nodiscard]] bool is_servers_own_index(std::string_view name) noexcept {
    return name == "_id_" || name == "_id_clustered";
}

}  // namespace

std::vector<IndexFinding> verify_retired(mongocxx::client& client,
                                         const DatabaseNames& databases,
                                         std::span<const RetiredIndex> retired) {
    std::vector<IndexFinding> findings;
    for (const RetiredIndex& entry : retired) {
        const std::string name{databases.for_collection(entry.collection)};
        mongocxx::database database = client[name];
        for (const LiveIndex& live : live_indexes(database, entry.collection)) {
            if (live.name != entry.name) { continue; }
            findings.push_back(IndexFinding{name, std::string{entry.collection},
                                            std::string{entry.name}});
        }
    }
    return findings;
}

std::vector<IndexFinding> report_undeclared(mongocxx::client& client,
                                            const DatabaseNames& databases,
                                            std::span<const IndexSpec> catalogue) {
    std::vector<IndexFinding> findings;
    // Every DECLARED collection, not merely every collection the catalogue
    // indexes: a collection with no declared index and a hand-made one on it is
    // exactly the case worth reporting, and iterating the catalogue would miss
    // it entirely.
    for (const CollectionSpec& spec : config::kCollections) {
        const std::string name{databases.for_collection(spec.name)};
        mongocxx::database database = client[name];
        for (const LiveIndex& live : live_indexes(database, spec.name)) {
            if (is_servers_own_index(live.name)) { continue; }

            bool declared = false;
            for (const IndexSpec& entry : catalogue) {
                declared = declared || (entry.collection == spec.name && entry.name == live.name);
            }
            if (declared) { continue; }
            findings.push_back(IndexFinding{name, std::string{spec.name}, live.name});
        }
    }
    return findings;
}

MigrationReport apply_migrations(mongocxx::client& client, const DatabaseNames& databases,
                                 std::span<const IndexSpec> catalogue,
                                 std::int32_t schema_version,
                                 std::span<const RetiredIndex> retired) {
    // Two declared databases pointing at one physical database gets the
    // single-database layout back with none of the guardrails noticing, so it is
    // refused rather than tolerated. An application's configuration should reject
    // it at boot too; this covers every other caller, tests included.
    for (std::size_t i = 0; i < databases.names.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (databases.names[i] == databases.names[j]) {
                throw std::invalid_argument{
                    "two declared databases name the same physical database"};
            }
        }
    }

    // The FIRST database's marker is the one reported: it is the one a deploy
    // reads to decide whether migrations have run. Each database records its own,
    // so dropping a secondary database entirely leaves no stale marker claiming
    // its indexes exist.
    const std::int32_t previous = applied_schema_version(client, databases.names[0]);

    std::size_t applied = 0;
    for (std::size_t i = 0; i < databases.names.size(); ++i) {
        applied += migrate_database(client, databases.names[i], static_cast<std::uint8_t>(i),
                                    catalogue, retired, schema_version);
    }

    return MigrationReport{applied, previous, schema_version};
}

}  // namespace anvil::db
