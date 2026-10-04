#include "anvil/analytics/query.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace anvil::analytics {
namespace {

// Folds the rollup's per-dimension documents into one series. The rollup writes
// one document per (code, bucket, dimensions), so a query that does not name a
// dimension combination is asking for their sum.
//
// Sessions are SUMMED rather than unioned, and that is a stated approximation:
// the distinct sets behind two dimension combinations are not disjoint, so a
// visitor who viewed a page on web and on ios counts twice. Storing the sets
// themselves to make it exact would put a visitor list in every rollup document,
// which is the personal data §12 keeps out of this collection on purpose.
[[nodiscard]] std::vector<Bucket> fold(std::span<const RollupRow> rows) {
    std::vector<Bucket> out;
    out.reserve(rows.size());
    for (const RollupRow& row : rows) {
        if (!out.empty() && out.back().at == row.bucket) {
            out.back().count += row.count;
            out.back().sessions += row.sessions;
            continue;
        }
        out.push_back(Bucket{row.bucket, row.count, row.sessions});
    }
    return out;
}

// The entity-grouped counterpart of fold(): sums across every bucket and every
// enum dimension combination instead of across dimension combinations within
// one bucket. It is bounded by construction: `rows` is already bounded by the
// caller's `limit`, and every row contributes to at most one entity's total,
// so the map never holds more entries than rows read.
[[nodiscard]] std::vector<EntityCount> fold_by_entity(std::span<const RollupRow> rows) {
    struct UuidHash final {
        [[nodiscard]] std::size_t operator()(const Uuid& id) const noexcept {
            std::size_t hashed = 0;
            for (std::size_t i = 0; i < sizeof(std::size_t) && i < id.size(); ++i) {
                hashed |= static_cast<std::size_t>(id[i]) << (i * 8);
            }
            return hashed;
        }
    };

    std::unordered_map<Uuid, EntityCount, UuidHash> by_entity;
    for (const RollupRow& row : rows) {
        // "No entity" is not an id worth reporting — grouping it in would put
        // an unlabelled bucket in a result whose whole point is a list of ids.
        if (is_nil(row.entity)) { continue; }
        auto [it, inserted] = by_entity.try_emplace(row.entity, EntityCount{row.entity, 0, 0});
        it->second.count += row.count;
        it->second.sessions += row.sessions;
    }

    std::vector<EntityCount> out;
    out.reserve(by_entity.size());
    for (const auto& [id, totals] : by_entity) { out.push_back(totals); }
    // HIGHEST COUNT FIRST — "top entities" is the question this exists to
    // answer — tied entities ordered by id so two runs over an unchanged
    // window return byte-identical output rather than whatever order an
    // unordered_map happened to iterate in.
    std::sort(out.begin(), out.end(), [](const EntityCount& a, const EntityCount& b) {
        if (a.count != b.count) { return a.count > b.count; }
        return a.entity < b.entity;
    });
    return out;
}

}  // namespace

Result<std::vector<Bucket>> AnalyticsQuery::counts_over_time(mongocxx::client& client,
                                                             EventCode code,
                                                             const TimeRange& range,
                                                             Granularity granularity,
                                                             std::int32_t limit) const {
    const RollupQuery query{range.from,   range.until, no_dimensions(),
                            code,         granularity, limit,
                            false};
    Result<std::vector<RollupRow>> rows = rollups_.read(client, query);
    if (!rows) { return rows.error(); }
    // The repository already sorts by bucket, so the fold is a single pass over
    // adjacent equal keys rather than a sort of its own.
    return fold(rows.value());
}

Result<std::vector<Bucket>> AnalyticsQuery::counts_over_time_for(
    mongocxx::client& client, EventCode code, const DimensionValues& dimensions,
    const TimeRange& range, Granularity granularity, std::int32_t limit) const {
    const RollupQuery query{range.from, range.until, dimensions, code, granularity, limit, true};
    Result<std::vector<RollupRow>> rows = rollups_.read(client, query);
    if (!rows) { return rows.error(); }
    return fold(rows.value());
}

Result<std::vector<Bucket>> AnalyticsQuery::counts_over_time_for_entity(
    mongocxx::client& client, EventCode code, const Uuid& entity, const TimeRange& range,
    Granularity granularity, std::int32_t limit) const {
    RollupQuery query{range.from,   range.until, no_dimensions(),
                      code,         granularity, limit,
                      false};
    query.entity = entity;
    query.match_entity = true;
    Result<std::vector<RollupRow>> rows = rollups_.read(client, query);
    if (!rows) { return rows.error(); }
    return fold(rows.value());
}

Result<std::vector<EntityCount>> AnalyticsQuery::counts_by_entity(
    mongocxx::client& client, EventCode code, const TimeRange& range, Granularity granularity,
    std::int32_t limit) const {
    // Unfiltered on both dimensions and entity, exactly like counts_over_time:
    // every combination in the window, so the fold below can group them by
    // entity instead of collapsing them by bucket.
    const RollupQuery query{range.from,   range.until, no_dimensions(),
                            code,         granularity, limit,
                            false};
    Result<std::vector<RollupRow>> rows = rollups_.read(client, query);
    if (!rows) { return rows.error(); }
    return fold_by_entity(rows.value());
}

}  // namespace anvil::analytics
