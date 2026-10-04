#pragma once

// Reads hit ROLLUPS, never raw events.
//
// A dashboard query over raw rows is a scan of the highest-volume collection in
// the system, issued by whoever can open the dashboard. The raw collection has
// exactly two readers — the rollup job and the erasure path — and neither of
// them is here (docs/17-analytics.md §15).
//
// Everything is bounded by a limit like every other read (CLAUDE.md §7) and
// paginated by the indexed bucket key, never skip(n).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/analytics/event.h"
#include "anvil/analytics/repository.h"
#include "anvil/core/result.h"
#include "anvil/db/codec.h"

namespace anvil::analytics {

// Half-open: [from, until). Two adjacent ranges then neither double-count a
// bucket on the boundary nor drop one.
struct TimeRange final {
    db::TimeMs from;
    db::TimeMs until;
};

struct Bucket final {
    db::TimeMs   at;
    std::int64_t count;
    std::int64_t sessions;
};

// One entity's total over a window, folded across every bucket and every enum
// dimension combination the same way `Bucket` folds across combinations within
// one bucket. `entity` is the raw 16 bytes, as everywhere else in anvil — a
// caller building JSON converts it with anvil::uuid::to_string, the same
// boundary uuid.h itself draws.
struct EntityCount final {
    Uuid         entity;
    std::int64_t count;
    std::int64_t sessions;
};

class AnalyticsQuery final {
public:
    AnalyticsQuery(const db::DatabaseNames& databases,
                   const AnalyticsCollections& collections)
        : rollups_{std::string{databases.for_collection(collections.rollups)},
                   collections.rollups} {}

    // One series, oldest bucket first. Buckets with no events are ABSENT rather
    // than zero: the rollup writes only what it counted, and a caller drawing a
    // chart fills the gaps with the granularity it asked for. Materialising
    // empty buckets here would mean writing a document per bucket per dimension
    // combination for every window nothing happened in.
    [[nodiscard]] Result<std::vector<Bucket>> counts_over_time(
        mongocxx::client& client, EventCode code, const TimeRange& range,
        Granularity granularity, std::int32_t limit = 512) const;

    // The same series narrowed to ONE dimension combination.
    [[nodiscard]] Result<std::vector<Bucket>> counts_over_time_for(
        mongocxx::client& client, EventCode code, const DimensionValues& dimensions,
        const TimeRange& range, Granularity granularity, std::int32_t limit = 512) const;

    // The same series narrowed to ONE entity id, across every bucket and every
    // enum dimension combination it appears under — the entity-dimension
    // counterpart of counts_over_time_for.
    [[nodiscard]] Result<std::vector<Bucket>> counts_over_time_for_entity(
        mongocxx::client& client, EventCode code, const Uuid& entity, const TimeRange& range,
        Granularity granularity, std::int32_t limit = 512) const;

    // GROUPED by entity instead of narrowed to one: every id that appeared in
    // the window, each with its own total. This is the "top projects" query —
    // the rollup already grouped by entity when it wrote one document per
    // (code, bucket, dimensions, entity); this folds those documents by entity
    // instead of by bucket, discarding the ones that carry no entity at all,
    // because "no entity" is not an id worth reporting.
    //
    // Returned HIGHEST COUNT FIRST, ties broken by the id's byte order, so two
    // reads of an unchanged window return byte-identical output rather than
    // whatever order the fold's hash map happened to iterate in.
    //
    // `limit` bounds the ROLLUP DOCUMENTS READ, like every other read here, not
    // the number of distinct entities returned — the same relationship
    // counts_over_time has to its own limit, and for the same reason: the
    // entity count can never exceed it, because each read row contributes to
    // at most one entity's total.
    [[nodiscard]] Result<std::vector<EntityCount>> counts_by_entity(
        mongocxx::client& client, EventCode code, const TimeRange& range,
        Granularity granularity, std::int32_t limit = 512) const;

    [[nodiscard]] const RollupRepository& rollups() const noexcept { return rollups_; }

private:
    RollupRepository rollups_;
};

}  // namespace anvil::analytics
