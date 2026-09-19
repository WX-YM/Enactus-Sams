#pragma once

// The recurring job that turns raw rows into the only thing anybody queries.
//
// --- why a rollup cannot $inc ----------------------------------------------
//
// Every queue in this system is at-least-once (ENGINEERING_RULES.md §6), so a rollup that
// ADDS to what it finds double-counts the first time a worker is reclaimed after
// a lease expiry — and the resulting number is wrong in a way nothing reports
// and nobody can reconstruct. So a rollup RECOMPUTES its window and $sets the
// result. Running it twice produces one document with identical values, which is
// the only assertion that proves the property (docs/17-analytics.md §14).
//
// That is also why a bucket is computed in ONE run and never in two. A partial
// aggregation written with $set and then resumed would overwrite the first half
// with the second, which is the failure $set exists to avoid, arriving by
// another route. The unit of work is therefore a whole bucket.
//
// --- why windows are closed before they are rolled --------------------------
//
// A bucket is computed only once `now` is past its end plus a grace margin,
// because a row inserted late into an already-rolled bucket is a row the rollup
// will not see. The margin is the sink's flush interval plus the clock skew the
// deployment tolerates, and it is stated rather than guessed.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/analytics/repository.h"
#include "anvil/core/result.h"
#include "anvil/db/codec.h"

namespace anvil::analytics {

// What one bucket cost and produced. A return value rather than an out
// parameter: an out parameter defeats RVO and makes the type non-const
// (ENGINEERING_RULES.md §2.2).
struct BucketReport final {
    std::int64_t rows_read;
    std::int64_t rows_written;
};

struct RollupReport final {
    // The end of the last bucket this run completed. The next run starts here,
    // and it is what a caller persists between runs.
    db::TimeMs   through;
    std::int64_t rows_read;
    std::int64_t buckets_written;
    std::int64_t buckets_rolled;
};

class RollupJob final {
public:
    // Rows read per round trip while walking a bucket. Cursor-paginated by `_id`
    // and never skip(n), which is O(n) server-side on the highest-volume
    // collection in the system.
    static constexpr std::int32_t kPageRows = 1000;

    // At most a day of hourly buckets in one run. A job that fell a month behind
    // would otherwise try to catch up in one invocation and hold the pool for
    // the duration; falling behind should take many runs to recover from, not
    // one long one.
    static constexpr std::int64_t kMaxBucketsPerRun = 24;

    // The memory ceiling on distinct-session counting, per bucket. 200,000
    // visitor ids is 3.2 MB, which is a bound an operator can reason about; past
    // it the job REFUSES the bucket rather than writing a number that is quietly
    // a floor. An application at that scale wants a finer granularity, and a
    // silently wrong count is exactly what §14 is about.
    static constexpr std::size_t kMaxTrackedSessionsPerBucket = 200'000;

    // The sink's flush interval plus the clock skew a deployment tolerates. One
    // second of flush plus four of skew, stated rather than guessed.
    static constexpr std::chrono::seconds kDefaultGrace{5};

    // `databases` resolves each collection to the database it was declared in.
    // The raw events and the rollups are routinely in DIFFERENT databases, and a
    // job that assumed one would read an empty collection and write into a
    // database nothing queries.
    RollupJob(const db::DatabaseNames& databases, const AnalyticsCollections& collections,
              Granularity granularity, std::chrono::seconds grace = kDefaultGrace);

    // Rolls every CLOSED bucket from `from` forward, at most kMaxBucketsPerRun
    // of them. Blocking: it belongs on a pool thread, never on a loop thread.
    [[nodiscard]] Result<RollupReport> run(mongocxx::client& client, db::TimeMs from,
                                           db::TimeMs now) const;

    // One bucket, recomputed from raw rows and written with $set. Exposed so a
    // test can run it twice and assert the documents are identical, which is the
    // whole property.
    [[nodiscard]] Result<BucketReport> roll_bucket(mongocxx::client& client, db::TimeMs bucket,
                                                   db::TimeMs now) const;

    // The first bucket that is NOT yet closed. Everything before it may be
    // rolled; this one and everything after it may not.
    [[nodiscard]] db::TimeMs first_open_bucket(db::TimeMs now) const noexcept;

    [[nodiscard]] Granularity granularity() const noexcept { return granularity_; }

private:
    // Declaration order is construction order.
    EventRepository      events_;
    RollupRepository     rollups_;
    std::chrono::seconds grace_;
    Granularity          granularity_;
};

}  // namespace anvil::analytics
