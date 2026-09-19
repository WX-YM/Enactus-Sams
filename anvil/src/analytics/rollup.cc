#include "anvil/analytics/rollup.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <trantor/utils/Logger.h>

#include "anvil/analytics/counters.h"

namespace anvil::analytics {
namespace {

// The identity tuple a rollup document is written under, minus the bucket and
// the granularity — both of which are fixed for one call to roll_bucket.
struct AggregateKey final {
    DimensionValues dimensions;
    EventCode       code;

    [[nodiscard]] bool operator==(const AggregateKey& other) const noexcept = default;
};

struct AggregateKeyHash final {
    [[nodiscard]] std::size_t operator()(const AggregateKey& key) const noexcept {
        // The dimension slots are one byte each and the code is small, so a
        // shift-and-mix over five bytes is the whole of what is needed. There is
        // no adversary here: the key space is closed by the event table.
        std::size_t hashed = static_cast<std::size_t>(key.code);
        for (const std::uint8_t value : key.dimensions) {
            hashed = (hashed * 131U) + value;
        }
        return hashed;
    }
};

struct VisitorHash final {
    [[nodiscard]] std::size_t operator()(const VisitorId& id) const noexcept {
        // The visitor digest is uniformly distributed, so its first bytes are
        // already a hash.
        std::size_t hashed = 0;
        for (std::size_t i = 0; i < sizeof(std::size_t) && i < id.size(); ++i) {
            hashed |= static_cast<std::size_t>(id[i]) << (i * 8);
        }
        return hashed;
    }
};

struct Aggregate final {
    std::unordered_set<VisitorId, VisitorHash> sessions;
    std::int64_t                               count = 0;
};

}  // namespace

RollupJob::RollupJob(const db::DatabaseNames& databases,
                     const AnalyticsCollections& collections, Granularity granularity,
                     std::chrono::seconds grace)
    : events_{std::string{databases.for_collection(collections.events)}, collections.events},
      rollups_{std::string{databases.for_collection(collections.rollups)},
               collections.rollups},
      grace_{grace},
      granularity_{granularity} {}

db::TimeMs RollupJob::first_open_bucket(db::TimeMs now) const noexcept {
    // A row inserted late into an already-rolled bucket is a row the rollup will
    // not see, so the margin has to cover the sink's flush interval and whatever
    // clock skew the deployment tolerates.
    return bucket_start(now - std::chrono::duration_cast<std::chrono::milliseconds>(grace_),
                        granularity_);
}

Result<BucketReport> RollupJob::roll_bucket(mongocxx::client& client, db::TimeMs bucket,
                                            db::TimeMs now) const {
    const db::TimeMs start = bucket_start(bucket, granularity_);
    const db::TimeMs end = start + bucket_width(granularity_);

    std::unordered_map<AggregateKey, Aggregate, AggregateKeyHash> aggregated;
    std::size_t tracked_sessions = 0;
    std::int64_t rows_read = 0;

    std::optional<EventCursor> after;
    for (;;) {
        const Result<EventPage> page =
            events_.window(client, start, end, after, kPageRows, now);
        if (!page) { return page.error(); }

        for (const EventRow& row : page.value().rows) {
            ++rows_read;
            Aggregate& into = aggregated[AggregateKey{row.event.dimensions, row.event.code}];
            // The coalescer's REPEAT COUNT, not one per row. A folded window
            // standing for four thousand page views is four thousand page views;
            // counting it as one would make every rate in the dashboard wrong by
            // exactly the ratio the coalescer achieved.
            into.count += static_cast<std::int64_t>(row.repeats);

            if (tracked_sessions >= kMaxTrackedSessionsPerBucket) {
                // REFUSED rather than truncated. A distinct-session count that
                // silently became a floor is the shape of wrongness §14 is
                // about: nothing reports it and nobody can reconstruct it.
                LOG_ERROR << "analytics: rollup refused a bucket holding more than "
                          << kMaxTrackedSessionsPerBucket
                          << " distinct sessions — use a finer granularity";
                return fail(ErrorCode::Internal, "rollup.sessions");
            }
            if (into.sessions.insert(row.event.session).second) { ++tracked_sessions; }
        }

        if (!page.value().next_after.has_value()) { break; }
        after = page.value().next_after;
    }

    if (aggregated.empty()) { return BucketReport{rows_read, 0}; }

    std::vector<RollupRow> rows;
    rows.reserve(aggregated.size());
    for (const auto& [key, aggregate] : aggregated) {
        rows.push_back(RollupRow{start, aggregate.count,
                                 static_cast<std::int64_t>(aggregate.sessions.size()),
                                 key.dimensions, key.code, granularity_});
    }

    // $set, so a second run over the same window produces one document with
    // identical values. The whole bucket is recomputed here rather than resumed,
    // because a partial aggregation written with $set and then continued would
    // overwrite the first half with the second.
    if (const Status written = rollups_.put(client, rows); !written) { return written.error(); }
    return BucketReport{rows_read, static_cast<std::int64_t>(rows.size())};
}

Result<RollupReport> RollupJob::run(mongocxx::client& client, db::TimeMs from,
                                    db::TimeMs now) const {
    RollupReport report{bucket_start(from, granularity_), 0, 0, 0};
    const db::TimeMs open = first_open_bucket(now);
    const auto width = bucket_width(granularity_);

    db::TimeMs bucket = bucket_start(from, granularity_);
    for (std::int64_t rolled = 0; rolled < kMaxBucketsPerRun && bucket < open; ++rolled) {
        const Result<BucketReport> bucket_report = roll_bucket(client, bucket, now);
        if (!bucket_report) { return bucket_report.error(); }
        report.rows_read += bucket_report.value().rows_read;
        report.buckets_written += bucket_report.value().rows_written;
        ++report.buckets_rolled;
        bucket = bucket + width;
        // Advanced only after the bucket COMPLETED. A report that moved the
        // marker past a bucket whose write failed would skip it forever, and a
        // forward-only rollup has no way back.
        report.through = bucket;
    }
    return report;
}

}  // namespace anvil::analytics
