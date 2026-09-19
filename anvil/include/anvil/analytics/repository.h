#pragma once

// The three analytics collections, and the reads each one is allowed to serve.
//
// The RAW EVENT collection has exactly two readers: the rollup job, and the
// erasure path. A dashboard query over raw rows is a scan of the highest-volume
// collection in the system, issued by whoever can open the dashboard — so there
// is no method here that could serve one, which is the same mechanism
// AuditRepository uses to stay append-only (docs/17-analytics.md §15).
//
// Every read against `analytics_events` and `analytics_sessions` also filters on
// the expiry field EXPLICITLY. A TTL index is a garbage collector and not an
// access control: the monitor runs roughly every sixty seconds, so a row past
// its retention window is still readable and would still be rolled up
// (docs/09-mongodb.md §6). tools/check-db-discipline.sh fails the build over a
// query that forgets.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/analytics/event.h"
#include "anvil/analytics/event_spec.h"
#include "anvil/analytics/sessions.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/collections.h"
#include "anvil/db/repository.h"

namespace anvil::analytics {

// The three collection names, which are the APPLICATION's like every other name
// in this library. anvil names none of them.
//
// They are carried together because the services below need two or three of them
// at once, and because TWO OF THE THREE BELONG IN A DIFFERENT DATABASE from the
// third: the raw events and the sessions turn over their whole contents every
// few days, and a collection doing that beside hot application data spends
// WiredTiger cache on rows that are about to expire (docs/17-analytics.md §15).
//
// Which database each one lives in is resolved through db::DatabaseNames rather
// than assumed to be one. Assuming one is not a simplification, it is a bug: a
// rollup job that wrote into the events database would write documents nothing
// ever reads, and every count would silently be zero.
struct AnalyticsCollections final {
    std::string_view events;
    std::string_view sessions;
    std::string_view rollups;
};

// What a rollup counts a window by. Two values and not five: every extra
// granularity multiplies the rollup collection by the number of buckets it adds,
// and an hour and a day answer every question a product dashboard asks. They are
// STORED on the rollup's `_id`.
enum class Granularity : std::int32_t {
    Hour = 0,
    Day = 1,
};

[[nodiscard]] db::TimeMs bucket_start(db::TimeMs at, Granularity granularity) noexcept;
[[nodiscard]] std::chrono::milliseconds bucket_width(Granularity granularity) noexcept;

// One counted bucket. The `_id` it is written under is the identity tuple —
// (code, granularity, bucket, dimensions) — so a re-run is a primary-key upsert
// and the collection carries no secondary index for the write path at all.
struct RollupRow final {
    db::TimeMs      bucket;       //  8
    // Events, summing the coalescer's repeat counts. A folded window standing
    // for four thousand page views contributes four thousand.
    std::int64_t    count;        //  8
    // Distinct sessions in the bucket. Counted from the raw rows at rollup time,
    // because it cannot be derived from `count` afterwards.
    std::int64_t    sessions;     //  8
    DimensionValues dimensions;   //  4
    EventCode       code;         //  4
    Granularity     granularity;  //  4
};

// Where a paginated window walk resumes.
//
// COMPOUND, and (at, _id) rather than `_id` alone, because that is the index the
// walk rides: the filter ranges over `at` and the sort has to come from the same
// index or the server sorts in memory. `_id` is the tiebreaker — a UUIDv7, so it
// is unique and time-ordered — which is what makes the cursor total and the walk
// resumable.
struct EventCursor final {
    db::TimeMs at;
    Uuid       id;
};

struct EventPage final {
    std::vector<EventRow>      rows;
    // Absent when the window is exhausted, which is what lets a rollup
    // interrupted mid-window resume from its boundary and produce the same
    // totals as one that ran straight through.
    std::optional<EventCursor> next_after;
};

class EventRepository final : public repo::RepositoryBase {
public:
    EventRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // One insert_many for a whole batch, UNORDERED so a single rejected row does
    // not discard the rest of the batch behind it. `_id` is a UUIDv7 minted
    // here, so the collection is ordered by time on its primary key and the
    // rollup's cursor needs no secondary index.
    [[nodiscard]] Status append_many(mongocxx::client& client, std::span<const EventRow> rows,
                                     db::TimeMs now,
                                     std::chrono::seconds retention) const;

    // One page of a CLOSED window, in `_id` order, for the rollup job.
    // Cursor-paginated by `_id` and never skip(n), which is O(n) server-side.
    [[nodiscard]] Result<EventPage> window(mongocxx::client& client, db::TimeMs from,
                                           db::TimeMs until,
                                           const std::optional<EventCursor>& after,
                                           std::int32_t limit, db::TimeMs now) const;

    // Erasure: one delete_many on `subject`, riding a PARTIAL index anonymous
    // rows never enter. The index therefore costs nothing for the rows that are
    // the overwhelming majority, and the erasure stays a point query rather than
    // a scan of the largest collection in the system.
    //
    // Deliberately NOT filtered on the expiry field: erasure must reach rows the
    // TTL monitor has not collected yet, which is the one read where "still
    // physically present" is exactly what is being asked about.
    [[nodiscard]] Result<std::int64_t> erase_subject(mongocxx::client& client,
                                                     const Uuid& subject) const;

    [[nodiscard]] Result<std::int64_t> count_in_window(mongocxx::client& client,
                                                       db::TimeMs from, db::TimeMs until,
                                                       db::TimeMs now) const;
};

class SessionRepository final : public repo::RepositoryBase {
public:
    SessionRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // The UPSERT IS the sessionisation. `_id` is the (visitor, day) pair itself,
    // so there is no read-then-write and no secondary index: N instances
    // converge on one row with no coordination at all (ENGINEERING_RULES.md §6).
    //
    // True when this call created the session, which is what a "new visitors
    // today" number is counted from — and it comes from the server's own
    // upserted_id rather than from a read that raced.
    [[nodiscard]] Result<bool> touch(mongocxx::client& client, const VisitorId& visitor,
                                     DayNumber day, db::TimeMs now,
                                     std::chrono::seconds retention) const;

    [[nodiscard]] Result<std::int64_t> count_day(mongocxx::client& client, DayNumber day,
                                                 db::TimeMs now) const;
};

struct RollupQuery final {
    db::TimeMs      from;
    db::TimeMs      until;
    DimensionValues dimensions = no_dimensions();
    EventCode       code = 0;
    Granularity     granularity = Granularity::Day;
    // Bounded like every other read (ENGINEERING_RULES.md §7). A dashboard asking for five
    // years of hourly buckets is asking for 43,800 documents.
    std::int32_t    limit = 512;
    // Whether `dimensions` is a filter at all. A query that does not name them
    // wants every combination, and one that does wants exactly one.
    bool            match_dimensions = false;
};

class RollupRepository final : public repo::RepositoryBase {
public:
    RollupRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // $set, NEVER $inc. Every queue in this system is at-least-once, so a rollup
    // that adds to what it finds double-counts the first time a worker is
    // reclaimed after a lease expiry — and the resulting number is wrong in a
    // way nothing reports and nobody can reconstruct. Recomputing the window and
    // setting the result makes a second run produce an identical document, which
    // is the only assertion that proves the job is re-runnable
    // (docs/17-analytics.md §14).
    [[nodiscard]] Status put(mongocxx::client& client, std::span<const RollupRow> rows) const;

    // Bounded, ordered by bucket, paginated by the indexed bucket key.
    [[nodiscard]] Result<std::vector<RollupRow>> read(mongocxx::client& client,
                                                      const RollupQuery& query) const;
};

}  // namespace anvil::analytics
