#include "anvil/analytics/query.h"

#include <algorithm>
#include <utility>

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

}  // namespace anvil::analytics
