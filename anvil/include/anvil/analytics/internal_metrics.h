#pragma once

// The one metric table anvil POPULATES rather than ships.
//
// ENGINEERING_RULES.md §1 draws the line: a constexpr table anvil ships is machinery; a
// constexpr table anvil populates is a bug. This is the exception, and the test
// it passes is whether a name here would have to change if the application
// changed. None of them would — every one names a mechanism that lives in this
// repository. `checkout_completed_total` would, and that is the line
// (docs/17-analytics.md §3).
//
// Two of the eight exist because their absence once hid a real failure, and
// docs/00-architecture.md §9 says why: anvil_audit_rows_dropped_total and
// anvil_transactions_aborted_total are both zero in a healthy deployment and
// neither is visible from outside. A dropped audit row produces no error
// response, and a correctly retried transaction produces none either.
//
// A build that excludes an optional module still declares its series and reports
// zero, which is the right answer: a metric that disappears from a scrape is
// indistinguishable from a collector that broke.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/analytics/metric_spec.h"
#include "anvil/db/collections.h"

namespace anvil::analytics {

// --- label value spaces -----------------------------------------------------
//
// Every one is closed and constexpr, which is the rule the application's table
// lives under and therefore the rule anvil's own has to live under first
// (docs/17-analytics.md §6).

// Which tier answered an authorisation. The hit RATE is local / the sum over
// this label, so the denominator is inside the same family rather than in a
// second metric that can disagree with it.
inline constexpr std::array<std::string_view, 3> kAuthzTierValues{"local", "mirror",
                                                                 "authority"};
enum class AuthzTier : std::uint8_t { Local = 0, Mirror = 1, Authority = 2 };

// The five pools of docs/00-architecture.md §3. Declared here in full even in a
// build that never constructs one of them, for the reason above.
inline constexpr std::array<std::string_view, 5> kPoolValues{"db", "cpu", "hash", "audit",
                                                             "analytics"};
enum class PoolLabel : std::uint8_t { Db = 0, Cpu = 1, Hash = 2, Audit = 3, Analytics = 4 };

// Per class, because the two mean different things and a single counter hides
// the second behind the first: lost traffic is a load signal, and a lost change
// is the only record that somebody did something (anvil/audit/buffer.h).
inline constexpr std::array<std::string_view, 2> kAuditClassValues{"traffic", "change"};
enum class AuditDropClass : std::uint8_t { Traffic = 0, Change = 1 };

// WHERE the row was lost. A full buffer is the sink outrunning its pool; a
// refused flush is the pool outrunning the database, and the second is the one
// whose fix is a re-admission rather than a bigger bound.
inline constexpr std::array<std::string_view, 2> kAuditStageValues{"buffer", "flush"};
enum class AuditDropStage : std::uint8_t { Buffer = 0, Flush = 1 };

// 393,116 aborts that all eventually committed and 393,116 that did not are
// different incidents, and a counter without this label cannot tell them apart.
inline constexpr std::array<std::string_view, 2> kTxnOutcomeValues{"committed", "failed"};
enum class TxnOutcome : std::uint8_t { Committed = 0, Failed = 1 };

inline constexpr std::array<std::string_view, 2> kSweepOutcomeValues{"deleted", "failed"};
enum class SweepOutcome : std::uint8_t { Deleted = 0, Failed = 1 };

// The true reason, even where the client saw a byte-identical 404. Coarsened to
// three values rather than carrying ErrorCode, because ErrorCode is append-only
// and a label space that grows with it is a label space nobody reviews.
inline constexpr std::array<std::string_view, 3> kDenialReasonValues{"unauthenticated",
                                                                     "forbidden", "other"};
enum class DenialReason : std::uint8_t { Unauthenticated = 0, Forbidden = 1, Other = 2 };

// Whether the client was answered with the stealth 404 or with a real status.
inline constexpr std::array<std::string_view, 2> kStealthedValues{"no", "yes"};
enum class Stealthed : std::uint8_t { No = 0, Yes = 1 };

// Which of the five answers a claim against the idempotency store got.
//
// Every one of them is invisible from outside. A replay is a 200 with a body,
// indistinguishable to an operator from the request that produced it, so without
// this counter there is no way to learn whether clients are retrying at all —
// which is the only reason the store exists. `mismatch` is the one to alert on:
// it is a client reusing one key for two different requests, which is a bug in
// that client rather than a property of the traffic.
//
// The values are in IdempotencyState's order and anvil/http/idempotency.cc
// static_asserts that they still are, so the label index is a cast rather than a
// second table to keep in step.
inline constexpr std::array<std::string_view, 6> kIdempotencyOutcomeValues{
    "fresh", "in_flight", "replay", "completed", "mismatch", "unavailable"};
enum class IdempotencyOutcome : std::uint8_t {
    Fresh = 0,
    InFlight = 1,
    Replay = 2,
    Completed = 3,
    Mismatch = 4,
    // Not a state a claim can return — the store refuses the request when Redis
    // cannot answer it. It is counted anyway, and it is the reason this metric
    // could not have been five values: a claim happens once per protected
    // request, so during an outage the alternative is a log line per request,
    // and docs/00-architecture.md §9 says plainly that under the load which
    // makes such a line fire, the line is itself the outage.
    Unavailable = 5,
};

// Which side answered a rate-limit decision, and what it decided.
//
// `shared` is the Redis counter every instance counts into. `local` is the
// per-process token bucket the limiter falls back to when Redis cannot be
// reached — weaker by a factor of N instances, bounded, and until this metric
// existed, invisible: RateLimitVerdict::degraded said in its own comment that
// the degradation was "surfaced so it appears in metrics" and no metric
// consumed it. What happened instead was a LOG_WARN per request for as long as
// the outage lasted, which is the shape docs/00-architecture.md §9 forbids by
// name — and unlike the idempotency store, nothing here refuses the request
// first, so every request during an outage reached that line.
//
// The DECISION is the second label rather than a second metric, because the
// question an operator has is "how much of what we refused was refused by a
// bucket that only this process can see", and that is one ratio inside one
// family rather than two counters that can disagree.
//
// The BUCKET is deliberately not a label. It is the application's vocabulary,
// and — unlike the TTL collection label below — the rules arrive as a
// std::span handed to a service rather than from a config header anvil can
// read at compile time, so its value space is not constexpr and could not be
// closed even if the name were anvil's to use.
inline constexpr std::array<std::string_view, 2> kRateLimitSourceValues{"shared", "local"};
enum class RateLimitSource : std::uint8_t { Shared = 0, Local = 1 };

inline constexpr std::array<std::string_view, 2> kRateLimitDecisionValues{"allowed",
                                                                          "refused"};
enum class RateLimitDecision : std::uint8_t { Allowed = 0, Refused = 1 };

// What the storm breaker did to a publish. The two are one family and not two
// metrics because they are one decision with two outcomes, and the question an
// operator has — "how much of what we shed was actually LOST" — is a ratio
// inside one family rather than two counters that can disagree.
//
// `dropped` is the one to alert on and it means a reader did not get something.
// `deferred` is expected to be non-zero under load and is not an alert on its
// own: the row is committed and `sweep_outbox()` finishes it.
//
// The TOPIC is deliberately not a label, for the reason the rate-limit bucket
// above is not one: a topic table arrives as a std::span handed to a service
// rather than from the config header anvil reads at compile time, so its value
// space is not constexpr and cannot be closed. Per-topic attribution lives on
// the notification ROW, which is bounded by a retention window rather than by
// resident memory (docs/17-analytics.md §6).
inline constexpr std::array<std::string_view, 2> kShedOutcomeValues{"dropped", "deferred"};
enum class ShedOutcome : std::uint8_t { Dropped = 0, Deferred = 1 };

// --- the TTL collection label ----------------------------------------------
//
// The VALUES come from the application's own collection table, filtered to the
// collections whose rows have a lifetime. That keeps the rule intact — the value
// space is still constexpr and the series count is still a compile-time
// number — while letting anvil count rows in collections it does not name.
//
// This is the one label here whose width an application chooses, so it is also
// the one that can push the table past kMaxCellsPerMetric. It does so as a build
// error with the ceiling named in it, which is the whole point of §6.

namespace detail {

[[nodiscard]] constexpr std::size_t count_ttl_collections() noexcept {
    std::size_t count = 0;
    for (const db::CollectionSpec& spec : config::kCollections) {
        if (!spec.expiry_field.empty()) { ++count; }
    }
    return count;
}

}  // namespace detail

inline constexpr std::size_t kTtlCollectionCount = detail::count_ttl_collections();

namespace detail {

[[nodiscard]] constexpr std::array<std::string_view, kTtlCollectionCount>
ttl_collection_names() noexcept {
    std::array<std::string_view, kTtlCollectionCount> names{};
    std::size_t next = 0;
    for (const db::CollectionSpec& spec : config::kCollections) {
        if (!spec.expiry_field.empty()) { names[next++] = spec.name; }
    }
    return names;
}

}  // namespace detail

inline constexpr std::array<std::string_view, kTtlCollectionCount> kTtlCollectionValues =
    detail::ttl_collection_names();

inline constexpr std::array<LabelSpec, 1> kTtlCollectionLabelStorage{
    {{"collection", kTtlCollectionValues}}};

// An application with no lifetime-bounded collection at all gets an UNLABELLED
// gauge rather than a label with an empty value space, which would multiply the
// series count to zero and make every sample silently do nothing.
inline constexpr std::span<const LabelSpec> kTtlCollectionLabels =
    kTtlCollectionCount == 0 ? std::span<const LabelSpec>{}
                             : std::span<const LabelSpec>{kTtlCollectionLabelStorage};

// --- bucket boundaries ------------------------------------------------------

// Microseconds, because a pool wait expressed in whole seconds has one useful
// boundary and a floating-point boundary is refused by §5 of docs/17: two
// processes would disagree about which bucket a value landed in, and a count
// differing by one between instances is indistinguishable from a real signal.
//
// The low end is dense because the healthy case is a wait of zero: the signal
// this metric carries is the shape of the tail against a flat head, and
// boundaries starting at a millisecond would report every healthy deployment as
// one bucket.
inline constexpr std::array<std::int64_t, 11> kMongoWaitBucketsUs{
    50, 100, 250, 500, 1000, 2500, 5000, 10000, 50000, 250000, 1000000};

// --- labels -----------------------------------------------------------------

inline constexpr std::array<LabelSpec, 1> kAuthzLabels{{{"tier", kAuthzTierValues}}};
inline constexpr std::array<LabelSpec, 1> kPoolLabels{{{"pool", kPoolValues}}};
inline constexpr std::array<LabelSpec, 2> kAuditDropLabels{
    {{"class", kAuditClassValues}, {"stage", kAuditStageValues}}};
inline constexpr std::array<LabelSpec, 1> kTxnLabels{{{"outcome", kTxnOutcomeValues}}};
inline constexpr std::array<LabelSpec, 1> kSweepLabels{{{"outcome", kSweepOutcomeValues}}};
inline constexpr std::array<LabelSpec, 2> kDenialLabels{
    {{"reason", kDenialReasonValues}, {"stealthed", kStealthedValues}}};
inline constexpr std::array<LabelSpec, 1> kIdempotencyLabels{
    {{"outcome", kIdempotencyOutcomeValues}}};
inline constexpr std::array<LabelSpec, 2> kRateLimitLabels{
    {{"source", kRateLimitSourceValues}, {"decision", kRateLimitDecisionValues}}};
inline constexpr std::array<LabelSpec, 1> kShedLabels{{{"outcome", kShedOutcomeValues}}};

// --- the table --------------------------------------------------------------
//
// ORDER IS THE SCRAPE ORDER. Two scrapes of an unchanged registry are
// byte-identical including series order, which is what lets an operator diff
// them, so reordering this table changes an output somebody diffs.

inline constexpr std::array<MetricSpec, 12> kInternalMetrics{{
    {"anvil_authz_cache_hits", "Authorisation decisions, by the tier that answered",
     kAuthzLabels, {}, MetricKind::Counter, MetricUnit::None},

    {"anvil_pool_queue_depth", "Tasks queued on each bounded thread pool, sampled at scrape",
     kPoolLabels, {}, MetricKind::Gauge, MetricUnit::None},

    {"anvil_mongo_pool_wait_microseconds", "Time a caller waited to acquire a mongocxx client",
     {}, kMongoWaitBucketsUs, MetricKind::Histogram, MetricUnit::Microseconds},

    {"anvil_ttl_collection_rows", "Rows resident in each lifetime-bounded collection",
     kTtlCollectionLabels, {}, MetricKind::Gauge, MetricUnit::Rows},

    {"anvil_orphan_files_swept", "Stored objects the orphan sweep acted on", kSweepLabels, {},
     MetricKind::Counter, MetricUnit::None},

    // The coarsened source network is deliberately NOT a label here, although
    // docs/00-architecture.md §9 once described one: the value space of a label
    // is the internet, which is the cardinality explosion §6 exists to refuse.
    // The network is carried on the audit ROW instead, where it is bounded by a
    // retention window rather than by resident memory, and the row is where an
    // intrusion view groups by it anyway.
    {"anvil_stealth_denials", "Denied requests, by true reason and whether they were stealthed",
     kDenialLabels, {}, MetricKind::Counter, MetricUnit::None},

    {"anvil_audit_rows_dropped", "Audit rows lost, by class and by where they were lost",
     kAuditDropLabels, {}, MetricKind::Counter, MetricUnit::None},

    {"anvil_transactions_aborted",
     "Transaction attempts the server aborted, by the outcome of the transaction that "
     "contained them",
     kTxnLabels, {}, MetricKind::Counter, MetricUnit::None},

    // APPENDED, and it has to be: this table's order is the scrape order, and
    // two scrapes of an unchanged registry are byte-identical so that an
    // operator can diff them. Inserting a row in the middle rewrites an output
    // somebody compares.
    {"anvil_idempotency_claims", "Claims against the idempotency store, by what they found",
     kIdempotencyLabels, {}, MetricKind::Counter, MetricUnit::None},

    // APPENDED for the same reason the row above was, and the rule is worth
    // restating where it is easiest to break: this table's order IS the scrape
    // order, and two scrapes of an unchanged registry are byte-identical so an
    // operator can diff them. A row inserted in the middle rewrites a file
    // somebody compares.
    {"anvil_rate_limit_decisions",
     "Rate-limit decisions, by the counter that answered and by what it decided",
     kRateLimitLabels, {}, MetricKind::Counter, MetricUnit::None},

    // APPENDED, and the rule is the same one the two rows above restate: this
    // table's order IS the scrape order.
    //
    // A shed that is not counted is silent data loss, which is why the storm
    // breaker ships with this row rather than acquiring it later
    // (anvil/notifications/shedding.h).
    {"anvil_notifications_shed",
     "Publishes the storm breaker acted on, by whether the notification was lost or only "
     "delayed",
     kShedLabels, {}, MetricKind::Counter, MetricUnit::None},

    // The honest cost of the design above, and the reason it is a row here
    // rather than a footnote. The dispatch gate converts queue pressure into
    // outbox backlog — the correct trade, because a backlog is durable and
    // bounded by disk rather than by RSS — but a backlog growing faster than the
    // sweeper drains it is the failure mode that gate introduces, and it is
    // invisible without this.
    //
    // Sampled from a recurring job on db_pool, never at scrape: reading it is a
    // round trip, and a sampler that queries the database turns every scrape
    // into a load test that fires every fifteen seconds (anvil/analytics/gauges.h).
    {"anvil_notifications_outbox_rows",
     "Committed notifications past the sweeper's grace with their dispatch unfinished, capped",
     {}, {}, MetricKind::Gauge, MetricUnit::Rows},
}};

static_assert(internal_metric_table_is_well_formed(kInternalMetrics),
              "an empty, duplicate or ungrammatical name; an empty help; a counter named "
              "_total; a name that disagrees with its unit; non-increasing buckets; a "
              "duplicate label; a MISSING anvil_ prefix; or a cell count past the ceiling — "
              "which for anvil_ttl_collection_rows means the application declares more "
              "lifetime-bounded collections than kMaxCellsPerMetric allows");

// Index into kInternalMetrics. These are anvil's own increment sites, so the
// names are the mechanisms rather than the series: a call site says which thing
// happened, and the table says what it is called in a scrape.
enum class Internal : std::uint16_t {
    AuthzCacheHits = 0,
    PoolQueueDepth = 1,
    MongoPoolWaitMicroseconds = 2,
    TtlCollectionRows = 3,
    OrphanFilesSwept = 4,
    StealthDenials = 5,
    AuditRowsDropped = 6,
    TransactionsAborted = 7,
    IdempotencyClaims = 8,
    RateLimitDecisions = 9,
    NotificationsShed = 10,
    NotificationsOutboxRows = 11,
};

static_assert(static_cast<std::size_t>(Internal::NotificationsOutboxRows) + 1 ==
                  kInternalMetrics.size(),
              "every internal metric needs an enumerator, and in table order: the enumerator "
              "IS the index");

}  // namespace anvil::analytics
