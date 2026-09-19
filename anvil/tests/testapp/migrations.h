#pragma once

// The reference application's data migrations.
//
// anvil ships the runner, the cursor, the batching, the ledger and the lock;
// WHAT moves is this table (docs/01-seams.md §13). It is the second half of
// tests/testapp/indexes.h — that one declares the indexes, this one declares the
// documents.
//
// TWO steps on purpose, and the pair is the point: one walks a collection whose
// `_id` is a 16-byte UUIDv7 and one walks `sections`, whose `_id` is a compound
// document. A cursor proved against one `_id` shape is a cursor that breaks on
// the other, and it breaks by walking a prefix and reporting that it finished.

#include <array>
#include <cstdint>

#include <bsoncxx/document/value.hpp>

#include "anvil/db/collection_options.h"
#include "anvil/db/migration_step.h"

namespace testapp {

// `mig`, not `m`: metrics.h already aliases `m` to anvil::analytics inside this
// same namespace, and two aliases for one name in one namespace is a hard error
// the moment an application includes both tables — which is the ordinary case.
namespace mig = anvil::db;

// Declared, not defined — the shape tests/testapp/jobs.cc uses. Taking the
// address of a declared function keeps the table below constexpr and keeps the
// bodies out of every translation unit that reads it; the linker binds them.
// tests/testapp/migrations.cc defines these.
[[nodiscard]] mig::StepOutcome backfill_display_name(
    mig::StepContext& context, std::span<const bsoncxx::document::view> batch) noexcept;

[[nodiscard]] mig::StepOutcome drop_legacy_section_title(
    mig::StepContext& context, std::span<const bsoncxx::document::view> batch) noexcept;

// The name is the ledger's `_id` and a renamed step re-runs from scratch on
// every cluster that already applied it — so the names are date-prefixed and
// then left alone, including when the body is later corrected.
//
// `min_schema_version` is each step's PRECONDITION rather than its position: the
// second one reads through an index this application's catalogue gained at
// version 5, and the runner refuses rather than collection-scanning because a
// deploy landed the steps before the indexes.
inline constexpr std::array<mig::MigrationStep, 2> kSteps{{
    {"2024_06_backfill_display_name", "users", &backfill_display_name, 500, 1,
     mig::Cursor::IdRange},
    {"2024_07_drop_legacy_section_title", "sections", &drop_legacy_section_title, 200, 5,
     mig::Cursor::WholeCollection},
}};

static_assert(mig::step_table_is_well_formed(kSteps),
              "an empty, duplicate or ungrammatical name; a collection that is not declared "
              "in config::kCollections; a zero batch size; a negative schema precondition");

// --- what each collection IS ------------------------------------------------
//
// The index catalogue says how a collection is READ; this says what it is. Three
// of these options are one-way doors — clustered, capped and timeseries are set
// at creation and changing one is a copy of the whole collection — which is why
// they are declared in a table somebody reviews rather than left to whatever the
// first insert happened to create (docs/18-data-migrations.md §11).
//
// Declared, not defined, for the same reason the step bodies are: a BSON
// document is not a constant expression, so the table holds the address of a
// function that builds one.
[[nodiscard]] bsoncxx::document::value draft_shape();
[[nodiscard]] bsoncxx::document::value audit_row_shape();

inline constexpr std::array<mig::CollectionOptionsSpec, 3> kCollectionOptions{{
    // The highest-volume collection in the system, clustered on `_id` — and
    // deliberately NOT capped and NOT a timeseries. It needs both a TTL and a
    // delete_many by subject, and the last two cost one or the other: capped
    // forbids deletes and forbids a TTL index, and a timeseries delete must
    // match on the meta field, which would have to be the erasure key and would
    // therefore be high-cardinality — the one thing the bucketing depends on not
    // being. All three look correct until they are found to be one-way.
    {"analytics_events", {}, {}, 0, 0, nullptr, true, mig::Granularity::None},

    // Clustered on `_id`, which is exactly the shape a draft store wants: the
    // secondary `_id` index disappears, and an `_id`-range scan — which is what
    // every data migration's cursor walks — becomes the collection order.
    //
    // Not capped, and it could not be: `drafts` declares an expiry field in
    // config::kCollections, and a capped collection forbids both deletes and a
    // TTL index. collection_options_are_well_formed refuses that combination, so
    // the contradiction is a build failure rather than a collection that fills
    // up forever.
    {"drafts", {}, {}, 0, 0, &draft_shape, true, mig::Granularity::None},

    // A validator and nothing else. It is a NET, not the validation: request
    // data is validated at the edge in input/ with typed errors and a field name
    // the caller can act on. What this catches is the write nobody expected — a
    // field written by a script, a shell, or a migration written in a hurry —
    // against the one collection in the system whose whole value is that it is
    // an accurate record.
    {"audit_log", {}, {}, 0, 0, &audit_row_shape, false, mig::Granularity::None},
}};

static_assert(mig::collection_options_are_well_formed(kCollectionOptions),
              "a collection that is not declared in config::kCollections, two entries for one "
              "collection, or a combination the server refuses — capped with clustered, "
              "capped with timeseries, a document cap with no size cap, or a capped "
              "collection whose rows have a declared lifetime");

}  // namespace testapp
