#include "anvil/db/collection_options.h"

#include <cstring>
#include <optional>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/operation_exception.hpp>

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

// The server's own spelling. Sent on create and compared on read, so there is
// one mapping rather than two that can drift.
[[nodiscard]] std::string_view granularity_name(Granularity granularity) noexcept {
    switch (granularity) {
        case Granularity::Seconds: return "seconds";
        case Granularity::Minutes: return "minutes";
        case Granularity::Hours: return "hours";
        case Granularity::None: break;
    }
    return {};
}

[[nodiscard]] std::string_view text_of(const bsoncxx::document::element& field) noexcept {
    if (!field || field.type() != bsoncxx::type::k_string) { return {}; }
    const bsoncxx::stdx::string_view value = field.get_string().value;
    return std::string_view{value.data(), value.size()};
}

[[nodiscard]] bool flag_of(const bsoncxx::document::element& field) noexcept {
    return field && field.type() == bsoncxx::type::k_bool && field.get_bool().value;
}

[[nodiscard]] std::int64_t number_of(const bsoncxx::document::element& field) noexcept {
    if (!field) { return 0; }
    if (field.type() == bsoncxx::type::k_int32) { return field.get_int32().value; }
    if (field.type() == bsoncxx::type::k_int64) { return field.get_int64().value; }
    if (field.type() == bsoncxx::type::k_double) {
        return static_cast<std::int64_t>(field.get_double().value);
    }
    return 0;
}

// The `options` subdocument listCollections reports, or nullopt when the
// collection does not exist.
//
// The whole reply document is returned alongside the view, because a
// document::view borrows: reading the options out of a cursor entry that has
// already advanced is a use-after-free the driver will not warn about.
[[nodiscard]] std::optional<bsoncxx::document::value> live_options(
    mongocxx::database& database, std::string_view collection) {
    auto cursor = database.list_collections(
        make_document(kvp("name", bsoncxx::types::b_string{
                                      bsoncxx::stdx::string_view{collection.data(),
                                                                 collection.size()}})));
    for (const bsoncxx::document::view& entry : cursor) {
        const bsoncxx::document::element options = entry["options"];
        if (options && options.type() == bsoncxx::type::k_document) {
            return bsoncxx::document::value{options.get_document().value};
        }
        // The collection exists and declares no options at all, which is what a
        // collection created implicitly by an insert or a createIndexes looks
        // like. An empty document, not "absent": the difference decides whether
        // this run creates it or compares against it.
        return bsoncxx::document::value{make_document()};
    }
    return std::nullopt;
}

// What createCollection is sent, and — minus the validator — what an existing
// collection is compared against.
[[nodiscard]] bsoncxx::document::value creation_options(const CollectionOptionsSpec& spec,
                                                        bool with_validator) {
    bsoncxx::builder::basic::document options;
    if (spec.clustered_on_id) {
        // The server accepts only this one clustered key, and requires the
        // index to be named and unique. Spelled out rather than defaulted so a
        // reader sees what a clustered collection actually is.
        options.append(kvp("clusteredIndex", [](sub_document sub) {
            sub.append(kvp("key", make_document(kvp("_id", bsoncxx::types::b_int32{1}))));
            sub.append(kvp("unique", bsoncxx::types::b_bool{true}));
            sub.append(kvp("name", std::string{"_id_clustered"}));
        }));
    }
    if (spec.capped_size_bytes > 0) {
        options.append(kvp("capped", bsoncxx::types::b_bool{true}));
        options.append(kvp("size", bsoncxx::types::b_int64{spec.capped_size_bytes}));
        if (spec.capped_max_documents > 0) {
            options.append(kvp("max", bsoncxx::types::b_int64{spec.capped_max_documents}));
        }
    }
    if (spec.granularity != Granularity::None) {
        options.append(kvp("timeseries", [&spec](sub_document sub) {
            sub.append(kvp("timeField", std::string{spec.timeseries_time_field}));
            if (!spec.timeseries_meta_field.empty()) {
                sub.append(kvp("metaField", std::string{spec.timeseries_meta_field}));
            }
            sub.append(kvp("granularity", std::string{granularity_name(spec.granularity)}));
        }));
    }
    if (with_validator && spec.validator != nullptr) {
        const bsoncxx::document::value validator = spec.validator();
        options.append(kvp("validator", validator.view()));
        // `error`, never `warn`. A warn-level validator is a log line per bad
        // write under exactly the load that produces bad writes, and it lets the
        // bad document land anyway (docs/18-data-migrations.md §12).
        options.append(kvp("validationLevel", std::string{"strict"}));
        options.append(kvp("validationAction", std::string{"error"}));
    }
    return options.extract();
}

// Every difference the server cannot be asked to fix, named one at a time so an
// operator sees all of them in one pass.
void compare_immutable(const CollectionOptionsSpec& spec, const bsoncxx::document::view& live,
                       std::vector<OptionsViolation>& violations) {
    const auto complain = [&](std::string reason) {
        violations.push_back(
            OptionsViolation{std::string{spec.collection}, std::move(reason)});
    };

    const bool live_clustered = static_cast<bool>(live["clusteredIndex"]);
    if (live_clustered != spec.clustered_on_id) {
        complain(spec.clustered_on_id
                     ? "declared clustered on _id and the collection is not — changing it is a "
                       "copy of the whole collection, so it is a migration and not a flag"
                     : "clustered on _id and the catalogue does not declare it");
    }

    const bool live_capped = flag_of(live["capped"]);
    const bool want_capped = spec.capped_size_bytes > 0;
    if (live_capped != want_capped) {
        complain(want_capped ? "declared capped and the collection is not"
                             : "capped and the catalogue does not declare it — a capped "
                               "collection forbids deletes and forbids a TTL index");
    } else if (want_capped) {
        if (number_of(live["size"]) != spec.capped_size_bytes) {
            complain("capped with a different size than the catalogue declares");
        }
        if (number_of(live["max"]) != spec.capped_max_documents) {
            complain("capped with a different document cap than the catalogue declares");
        }
    }

    const bsoncxx::document::element timeseries = live["timeseries"];
    const bool live_timeseries =
        timeseries && timeseries.type() == bsoncxx::type::k_document;
    const bool want_timeseries = spec.granularity != Granularity::None;
    if (live_timeseries != want_timeseries) {
        complain(want_timeseries ? "declared a timeseries collection and it is not"
                                 : "a timeseries collection and the catalogue does not "
                                   "declare it");
        return;
    }
    if (!want_timeseries) { return; }

    const bsoncxx::document::view live_ts = timeseries.get_document().value;
    if (text_of(live_ts["timeField"]) != spec.timeseries_time_field) {
        complain("a timeseries collection measuring a different time field");
    }
    if (text_of(live_ts["metaField"]) != spec.timeseries_meta_field) {
        complain("a timeseries collection with a different meta field — the meta field is "
                 "what a delete must match on, so it is the erasure path");
    }
    if (text_of(live_ts["granularity"]) != granularity_name(spec.granularity)) {
        complain("a timeseries collection bucketing at a different granularity");
    }
}

// True when the live validator already says what the catalogue says.
//
// A byte comparison of the two BSON documents, which is exact rather than
// approximate: the server stores a validator as it was given, so a document
// built the same way twice compares equal, and anything else is a difference
// worth issuing the collMod for. A false negative costs one no-op command; a
// false positive would leave a stale validator in force.
[[nodiscard]] bool validator_matches(const CollectionOptionsSpec& spec,
                                     const bsoncxx::document::view& live) {
    const bsoncxx::document::element live_validator = live["validator"];
    if (spec.validator == nullptr) {
        return !live_validator || live_validator.type() != bsoncxx::type::k_document;
    }
    if (!live_validator || live_validator.type() != bsoncxx::type::k_document) {
        return false;
    }
    if (text_of(live["validationAction"]) != "error") { return false; }
    if (text_of(live["validationLevel"]) != "strict") { return false; }

    const bsoncxx::document::value declared = spec.validator();
    const bsoncxx::document::view stored = live_validator.get_document().value;
    return declared.view().length() == stored.length() &&
           std::memcmp(declared.view().data(), stored.data(), stored.length()) == 0;
}

}  // namespace

CollectionOptionsReport apply_collection_options(mongocxx::client& client,
                                                 const DatabaseNames& databases,
                                                 std::span<const CollectionOptionsSpec> specs,
                                                 OptionsPhase phase) {
    CollectionOptionsReport report{};

    for (const CollectionOptionsSpec& spec : specs) {
        mongocxx::database database =
            client[std::string{databases.for_collection(spec.collection)}];

        try {
            const std::optional<bsoncxx::document::value> live =
                live_options(database, spec.collection);

            if (!live.has_value()) {
                // A collection this run creates carries its validator
                // immediately: there is nothing in it that could fail one, so
                // the two-phase ordering has nothing to protect.
                if (phase == OptionsPhase::Create) {
                    const bsoncxx::document::value options = creation_options(spec, true);
                    database.create_collection(
                        bsoncxx::stdx::string_view{spec.collection.data(),
                                                   spec.collection.size()},
                        options.view());
                    ++report.created;
                }
                continue;
            }

            if (phase == OptionsPhase::Create) {
                compare_immutable(spec, live->view(), report.violations);
                continue;
            }

            // Phase Validate, after the data steps: this is the only option
            // collMod can change, and the only one whose ordering against the
            // data matters.
            if (validator_matches(spec, live->view())) { continue; }
            if (spec.validator == nullptr) {
                database.run_command(
                    make_document(kvp("collMod", std::string{spec.collection}),
                                  kvp("validator", make_document()),
                                  kvp("validationLevel", std::string{"off"})));
            } else {
                const bsoncxx::document::value validator = spec.validator();
                database.run_command(
                    make_document(kvp("collMod", std::string{spec.collection}),
                                  kvp("validator", validator.view()),
                                  kvp("validationLevel", std::string{"strict"}),
                                  kvp("validationAction", std::string{"error"})));
            }
            ++report.validators_applied;
        } catch (const mongocxx::operation_exception& e) {
            // NamespaceExists (48) is another instance winning the create, which
            // is the normal case on a rolling deploy and not a violation. Every
            // other driver failure is reported rather than thrown: the operator
            // wants the rest of the catalogue checked in the same pass, and no
            // driver text reaches the report (ENGINEERING_RULES.md §5).
            if (e.code().value() != 48) {
                report.violations.push_back(OptionsViolation{
                    std::string{spec.collection},
                    "the server refused the declared options for this collection"});
            }
        }
    }

    return report;
}

}  // namespace anvil::db
