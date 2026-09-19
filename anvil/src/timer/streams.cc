#include "anvil/timer/queue.h"
#include "anvil/timer/registry.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <sw/redis++/redis++.h>
#include <trantor/utils/Logger.h>

#include "anvil/crypto/base64url.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/core/uuid.h"
#include "anvil/http/trace_context.h"
#include "anvil/redis/redis_client.h"

namespace anvil::timer {
namespace {

// --- Lua ------------------------------------------------------------------
//
// Every multi-step Redis mutation in this file is one script. Not for speed —
// though one round trip instead of four is that too — but because a crash
// between two of these calls would leave the queue in a state that has no
// recovery path: a job acknowledged but not rescheduled is a lost job, and a job
// rescheduled but not acknowledged is a job that runs twice per attempt forever.
// Redis executes a script atomically, so neither state is reachable.

// Schedule, deduplicated. KEYS: sched zset, payload hash, idem key, idemof hash.
// ARGV: job id, score (ms), envelope, idem ttl (ms), idem key name.
//
// The GET-then-SET is safe without NX precisely because the script is atomic.
// Returning the EXISTING id rather than an error is what makes a retried
// scheduler harmless: the caller gets the id of the job that is already queued.
constexpr std::string_view kScheduleScript = R"lua(
local existing = redis.call('GET', KEYS[3])
if existing then return existing end
redis.call('SET', KEYS[3], ARGV[1], 'PX', ARGV[4])
redis.call('HSET', KEYS[2], ARGV[1], ARGV[3])
redis.call('HSET', KEYS[4], ARGV[1], ARGV[5])
redis.call('ZADD', KEYS[1], ARGV[2], ARGV[1])
return ARGV[1]
)lua";

// Cancel. KEYS: sched zset, payload hash, idemof hash.
//
// The idempotency key is released along with the entry. Without that, cancelling
// a job would permanently poison its natural identity: unpublishing a note and
// republishing it would find the key still present and silently schedule
// nothing (docs/10-timer-jobs.md §7).
constexpr std::string_view kCancelScript = R"lua(
local removed = redis.call('ZREM', KEYS[1], ARGV[1])
local idem = redis.call('HGET', KEYS[3], ARGV[1])
if idem then redis.call('DEL', idem) end
redis.call('HDEL', KEYS[2], ARGV[1])
redis.call('HDEL', KEYS[3], ARGV[1])
return removed
)lua";

// Promote due entries into the stream. KEYS: sched, payload, stream, idemof.
// ARGV: now (ms), limit.
//
// The whole point of the design: ZRANGEBYSCORE and the XADD that consumes it are
// in one atomic unit, so N promoters produce exactly one stream entry per job
// (b) — and the entry lands in the stream BEFORE it leaves the ZSET,
// so nothing can be lost between the two (c).
//
// A due id whose payload is gone is dropped rather than promoted: that is a
// cancelled job whose ZREM lost a race, and resurrecting it would defeat the
// cancel.
constexpr std::string_view kPromoteScript = R"lua(
local due = redis.call('ZRANGEBYSCORE', KEYS[1], '-inf', ARGV[1], 'LIMIT', 0, ARGV[2])
local moved = 0
for i = 1, #due do
  local id = due[i]
  local envelope = redis.call('HGET', KEYS[2], id)
  if envelope then
    redis.call('XADD', KEYS[3], '*', 'e', envelope)
    moved = moved + 1
  end
  redis.call('HDEL', KEYS[2], id)
  redis.call('HDEL', KEYS[4], id)
  redis.call('ZREM', KEYS[1], id)
end
return moved
)lua";

// Acknowledge. KEYS: stream. ARGV: group, entry id.
//
// XDEL as well as XACK: an acknowledged entry stays in the stream forever
// otherwise, and a queue that has processed ten million jobs would hold ten
// million dead entries in Redis memory.
constexpr std::string_view kAckScript = R"lua(
redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[1], ARGV[2])
return 1
)lua";

// Retry with backoff. KEYS: stream, sched, payload, idemof.
// ARGV: group, entry id, job id, next score (ms), envelope with attempt+1,
//       idem key name.
//
// The attempt count travels with the entry, never in worker memory (docs/10-timer-jobs.md §8):
// a worker that dies between attempts must not reset the budget, and a different
// worker must see the same count.
constexpr std::string_view kRetryScript = R"lua(
redis.call('HSET', KEYS[3], ARGV[3], ARGV[5])
redis.call('HSET', KEYS[4], ARGV[3], ARGV[6])
redis.call('ZADD', KEYS[2], ARGV[4], ARGV[3])
redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[1], ARGV[2])
return 1
)lua";

// Dead-letter. KEYS: stream, dead stream. ARGV: group, entry id, envelope,
// reason, attempts.
//
// The record carries the envelope and the reason CODE, never the arguments as
// text: args may contain identifiers, and a dead-letter stream is read by
// operators and shipped to log aggregation (docs/10-timer-jobs.md §8).
constexpr std::string_view kDeadLetterScript = R"lua(
redis.call('XADD', KEYS[2], '*', 'e', ARGV[3], 'why', ARGV[4], 'n', ARGV[5])
redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[1], ARGV[2])
return 1
)lua";

// Reclaim entries whose claimer never acknowledged them. KEYS: stream.
// ARGV: group, consumer, min idle (ms), count.
//
// XAUTOCLAIM needs Redis >= 6.2. Returned as a flat [entry id, envelope, ...]
// array rather than the native nested reply so the shape this code parses is one
// this file defines, not one that changed between Redis 6.2 and 7.0 (7.0 added a
// third top-level element).
constexpr std::string_view kReclaimScript = R"lua(
local claimed = redis.call('XAUTOCLAIM', KEYS[1], ARGV[1], ARGV[2], ARGV[3], '0',
                           'COUNT', ARGV[4])
local out = {}
local entries = claimed[2]
for i = 1, #entries do
  local entry = entries[i]
  if entry and entry[2] and entry[2][2] then
    out[#out + 1] = entry[1]
    out[#out + 1] = entry[2][2]
  end
end
return out
)lua";

// Renew the promoter lease only if we still hold it. KEYS: lease. ARGV: token,
// ttl (ms).
constexpr std::string_view kLeaseRenewScript = R"lua(
if redis.call('GET', KEYS[1]) == ARGV[1] then
  return redis.call('PEXPIRE', KEYS[1], ARGV[2])
end
return 0
)lua";

// Compare-and-delete. A plain DEL can release a lease that already expired and
// was re-acquired by someone else (docs/10-timer-jobs.md §5).
constexpr std::string_view kLeaseReleaseScript = R"lua(
if redis.call('GET', KEYS[1]) == ARGV[1] then
  return redis.call('DEL', KEYS[1])
end
return 0
)lua";

// Delete every structure this queue owns, plus the idempotency keys the idemof
// hash still names. KEYS: sched, payload, stream, dead, lease, idemof.
constexpr std::string_view kPurgeScript = R"lua(
local keys = redis.call('HVALS', KEYS[6])
for i = 1, #keys do redis.call('DEL', keys[i]) end
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3], KEYS[4], KEYS[5], KEYS[6])
return 1
)lua";

// The pending-entries depth, and nothing else. XPENDING's summary reply nests a
// per-consumer list whose counts are bulk strings under RESP2; pulling the one integer
// out in Lua means this code never depends on how a driver chooses to type that.
constexpr std::string_view kPendingDepthScript = R"lua(
local summary = redis.call('XPENDING', KEYS[1], ARGV[1])
return summary[1]
)lua";

constexpr std::string_view kFieldName = "e";

// The dedupe window a schedule_at gets when the caller has no opinion. Long
// enough to cover a retried scheduler and a redeploy, short enough that a
// genuinely recurring event is not suppressed for a day.
constexpr std::chrono::seconds kDefaultDedupeTtl{6 * 3600};

[[nodiscard]] sw::redis::StringView view_of(std::string_view text) noexcept {
    return sw::redis::StringView{text.data(), text.size()};
}

// trantor's LogStream has no std::string_view overload, and a JobSpec key is a
// string_view over a literal rather than a guaranteed-terminated buffer. One
// conversion at the log site is cheaper than a read past the end, and every use
// of it is on an error path.
[[nodiscard]] std::string text_of(std::string_view value) { return std::string{value}; }

[[nodiscard]] std::string binary_of(const Uuid& id) {
    return std::string{reinterpret_cast<const char*>(id.data()), id.size()};
}

[[nodiscard]] bool read_uuid(std::string_view bytes, Uuid& out) noexcept {
    if (bytes.size() != out.size()) { return false; }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    return true;
}

void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void put_i64(std::string& out, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((bits >> (i * 8U)) & 0xFFU));
    }
}

[[nodiscard]] std::uint16_t get_u16(std::string_view bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + 1]) << 8U);
}

[[nodiscard]] std::int64_t get_i64(std::string_view bytes, std::size_t offset) noexcept {
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        bits |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[offset + i]))
                << (i * 8U);
    }
    return static_cast<std::int64_t>(bits);
}

// Not a security boundary — this only has to make two different logical events
// land on two different keys. SHA-256 truncated to 128 bits rather than a fast
// hash because a collision here is a job that silently never runs, and 64 bits
// is not enough margin for something that fails invisibly.
[[nodiscard]] std::string idem_key_of(std::string_view prefix, std::string_view key) {
    const crypto::Digest256 digest = crypto::sha256(key);
    std::string out{prefix};
    out += crypto::base64url_encode(std::span<const std::uint8_t>{digest.data(), 16});
    return out;
}

// Deterministic per bucket and identical on every instance, so N instances
// racing to create the same recurrence collide on one key rather than producing
// N entries (docs/10-timer-jobs.md §7).
[[nodiscard]] std::string recurrence_key(const RecurringSpec& spec, std::int64_t bucket) {
    const JobSpec* declared = spec_of(spec.kind);
    std::string key{"recur:"};
    key.append(declared == nullptr ? std::string_view{"unknown"} : declared->key);
    key.push_back(':');
    key.append(std::to_string(bucket));
    return key;
}


// The policy lives in http/trace_context.h, not here: whether deferred work
// carries a link and what it runs under is a statement about traces, and a queue
// that decided it privately would be a second answer nothing else can see.
using http::has_trace_link;

}  // namespace

// --- envelope codec ---------------------------------------------------------

void encode_envelope(const JobHeader& header, std::span<const std::uint8_t> args,
                     std::string& out) {
    // Derived, never taken from the header. The version and the presence of a
    // link are one fact, and a writer that could be told them separately is a
    // writer that can be told two different answers.
    const bool linked = has_trace_link(header.linked_trace);
    const std::uint8_t version =
        linked ? kJobEnvelopeVersionLinked : kJobEnvelopeVersionUnlinked;

    out.clear();
    out.reserve(job_header_bytes(version) + args.size());
    out.append(reinterpret_cast<const char*>(header.id.data()), header.id.size());
    put_i64(out, header.not_before.time_since_epoch().count());
    put_u16(out, static_cast<std::uint16_t>(header.kind));
    put_u16(out, static_cast<std::uint16_t>(args.size()));
    out.push_back(static_cast<char>(header.attempt));
    out.push_back(static_cast<char>(version));
    out.push_back('\0');
    out.push_back('\0');
    if (linked) {
        out.append(reinterpret_cast<const char*>(header.linked_trace.data()),
                   header.linked_trace.size());
    }
    out.append(reinterpret_cast<const char*>(args.data()), args.size());
}

std::optional<JobHeader> decode_header(std::string_view envelope) noexcept {
    // The shorter of the two, because the version that decides which one applies
    // is inside it. Reading the version out of bytes that may not be there is
    // the bug this ordering exists to make impossible.
    if (envelope.size() < kJobHeaderBytesUnlinked) { return std::nullopt; }
    JobHeader header{};
    if (!read_uuid(envelope.substr(0, header.id.size()), header.id)) { return std::nullopt; }
    header.not_before = db::TimeMs{std::chrono::milliseconds{get_i64(envelope, 16)}};
    header.kind = static_cast<std::uint16_t>(get_u16(envelope, 24));
    header.args_len = get_u16(envelope, 26);
    header.attempt = static_cast<std::uint8_t>(envelope[28]);
    header.version = static_cast<std::uint8_t>(envelope[29]);
    header.reserved = {};
    header.linked_trace = {};

    const std::size_t header_bytes = job_header_bytes(header.version);
    if (header_bytes == 0) { return std::nullopt; }
    if (header.args_len > kMaxJobArgsBytes) { return std::nullopt; }
    // A declared length that disagrees with the bytes actually present is a
    // truncated or forged entry. Coercing it — trusting whichever is smaller —
    // would let a handler read a partial argument blob as a complete one.
    if (envelope.size() != header_bytes + header.args_len) { return std::nullopt; }

    if (header.version == kJobEnvelopeVersionLinked) {
        std::memcpy(header.linked_trace.data(),
                    envelope.data() + kJobHeaderBytesUnlinked,
                    header.linked_trace.size());
        // A version 2 envelope whose link is zero is a writer that disagrees
        // with itself: nothing here writes one, so reading one back means the
        // bytes were not produced by this codec.
        if (!has_trace_link(header.linked_trace)) { return std::nullopt; }
    }
    return header;
}

std::span<const std::uint8_t> envelope_args(std::string_view envelope,
                                            const JobHeader& header) noexcept {
    const std::size_t header_bytes = job_header_bytes(header.version);
    if (header_bytes == 0) { return {}; }
    if (envelope.size() < header_bytes + header.args_len) { return {}; }
    return std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(envelope.data()) + header_bytes,
        header.args_len};
}

// --- construction -----------------------------------------------------------

JobQueue::JobQueue(JobQueueConfig config)
    : config_{std::move(config)},
      stream_key_{config_.prefix},
      schedule_key_{config_.prefix + ":sched"},
      payload_key_{config_.prefix + ":payload"},
      dead_key_{config_.prefix + ":dead"},
      lease_key_{config_.prefix + ":lease:promoter"},
      idem_prefix_{config_.prefix + ":idem:"},
      group_{"workers"},
      // A fresh CSPRNG token per process. The lease is released only when the
      // stored value still matches this, so a process cannot release the lease a
      // successor took over after its own expired (docs/10-timer-jobs.md §5).
      lease_token_{uuid::to_string(uuid::generate_v4())},
      running_{false},
      leased_{false},
      promoter_{},
      claimers_{} {}

JobQueue::~JobQueue() { stop(); }

// --- scheduling -------------------------------------------------------------

Result<JobId> JobQueue::schedule_at(std::uint16_t kind, std::span<const std::uint8_t> args,
                                    db::TimeMs when, std::string_view idempotency_key) {
    if (spec_of(kind) == nullptr) { return fail(ErrorCode::ValidationFailed, "kind"); }
    if (args.size() > kMaxJobArgsBytes) { return fail(ErrorCode::PayloadTooLarge, "args"); }
    if (idempotency_key.empty()) {
        return fail(ErrorCode::ValidationFailed, "idempotency_key");
    }

    const db::TimeMs now = db::now_ms();
    // A past instant is due immediately, not rejected. Clock skew across
    // instances makes rejection a source of spurious failures (docs/10-timer-jobs.md §7).
    const db::TimeMs due = std::max(when, now);

    // The dedupe window has to outlive the delay, or a job scheduled for tomorrow
    // could be scheduled a second time this afternoon.
    const auto delay =
        std::chrono::duration_cast<std::chrono::seconds>(due.time_since_epoch() -
                                                         now.time_since_epoch());
    const std::chrono::seconds ttl = delay + kDefaultDedupeTtl;

    const JobId id = uuid::generate_v4();
    const JobHeader header{
        .id = id,
        .not_before = due,
        .kind = kind,
        .args_len = static_cast<std::uint16_t>(args.size()),
        .attempt = 1,
        // Derived by the encoder from the link; see JobHeader::version.
        .version = kJobEnvelopeVersionUnlinked,
        .reserved = {},
        .linked_trace = http::trace_link_of(http::current_trace()),
    };

    std::string envelope;
    encode_envelope(header, args, envelope);

    const std::string idem = idem_key_of(idem_prefix_, idempotency_key);
    const std::string id_bytes = binary_of(id);
    const std::string score = std::to_string(due.time_since_epoch().count());
    const std::string ttl_ms = std::to_string(ttl.count() * 1000);

    try {
        const std::string stored = redis::RedisClient::instance().eval<std::string>(
            view_of(kScheduleScript),
            {view_of(schedule_key_), view_of(payload_key_), view_of(idem),
             view_of(config_.prefix + ":idemof")},
            {view_of(id_bytes), view_of(score), view_of(envelope), view_of(ttl_ms),
             view_of(idem)});
        JobId existing{};
        if (!read_uuid(stored, existing)) { return fail(ErrorCode::Internal, "job"); }
        return existing;
    } catch (const std::exception& e) {
        // Redis is the delivery mechanism, not the system of record. A caller
        // whose work MUST happen records its intent in MongoDB and lets the
        // reconciliation sweep re-enqueue it (docs/10-timer-jobs.md §6).
        LOG_ERROR << "job schedule failed for " << text_of(spec_of(kind)->key) << ": " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

Result<JobId> JobQueue::schedule_in(std::uint16_t kind, std::span<const std::uint8_t> args,
                                    std::chrono::seconds delay,
                                    std::string_view idempotency_key) {
    return schedule_at(kind, args, db::now_ms() + delay, idempotency_key);
}

bool JobQueue::cancel(const JobId& id) noexcept {
    const std::string id_bytes = binary_of(id);
    try {
        const long long removed = redis::RedisClient::instance().eval<long long>(
            view_of(kCancelScript),
            {view_of(schedule_key_), view_of(payload_key_),
             view_of(config_.prefix + ":idemof")},
            {view_of(id_bytes)});
        return removed > 0;
    } catch (const std::exception& e) {
        // Cancellation is best-effort by construction, so a failure here is
        // reported as "not cancelled" and the handler's precondition check is
        // what actually makes the job a no-op.
        LOG_WARN << "job cancel failed: " << e.what();
        return false;
    }
}

// --- consumer group ---------------------------------------------------------

Status JobQueue::ensure_group() {
    try {
        // MKSTREAM so the group can be created before any job exists. "$" would
        // skip everything already in the stream; "0" means a group created after
        // a restart still sees entries that were promoted before it.
        redis::RedisClient::instance().xgroup_create(view_of(stream_key_), view_of(group_), "0",
                                                     true);
        return ok();
    } catch (const sw::redis::Error& e) {
        // BUSYGROUP is the normal case: N instances booting at once, or this
        // instance restarting.
        if (std::string_view{e.what()}.find("BUSYGROUP") != std::string_view::npos) {
            return ok();
        }
        LOG_ERROR << "job consumer group unavailable: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    } catch (const std::exception& e) {
        LOG_ERROR << "job consumer group unavailable: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

// --- promotion --------------------------------------------------------------

Result<std::size_t> JobQueue::promote_due(db::TimeMs now, std::uint32_t limit) {
    try {
        sw::redis::Redis& redis = redis::RedisClient::instance();
        // Backpressure rather than a MAXLEN trim: trimming the stream to bound
        // memory would silently DROP jobs, while leaving them in the ZSET keeps
        // them durable and simply delays them (docs/10-timer-jobs.md §6).
        if (static_cast<std::uint64_t>(redis.xlen(view_of(stream_key_))) >= kStreamHighWater) {
            return std::size_t{0};
        }
        const std::string now_ms = std::to_string(now.time_since_epoch().count());
        const std::string count = std::to_string(limit);
        const long long moved = redis.eval<long long>(
            view_of(kPromoteScript),
            {view_of(schedule_key_), view_of(payload_key_), view_of(stream_key_),
             view_of(config_.prefix + ":idemof")},
            {view_of(now_ms), view_of(count)});
        return static_cast<std::size_t>(moved < 0 ? 0 : moved);
    } catch (const std::exception& e) {
        LOG_ERROR << "job promoter tick failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

Result<std::size_t> JobQueue::ensure_recurring(db::TimeMs now) {
    const std::int64_t now_ms = now.time_since_epoch().count();
    std::size_t created = 0;

    for (const RecurringSpec& spec : config::kRecurringJobs) {
        if (spec_of(spec.kind) == nullptr) { continue; }

        const std::int64_t bucket = recurrence_bucket(now_ms, spec);
        const std::int64_t due_ms = recurrence_due_ms(bucket, spec);

        // Jitter is drawn per instance, and that is fine: whichever instance wins
        // the idempotency key decides the instant, and the others no-op. The
        // effect is one run per bucket at an unpredictable offset, which is
        // exactly what avoids the herd (docs/10-timer-jobs.md §7).
        const std::uint32_t jitter =
            spec.max_jitter.count() == 0
                ? 0U
                : crypto::random_below(static_cast<std::uint32_t>(spec.max_jitter.count()));
        const db::TimeMs due{std::chrono::milliseconds{due_ms + (jitter * 1000)}};

        const std::string key = recurrence_key(spec, bucket);
        // 2x the period: the key must outlive its own bucket, or a second
        // instance re-schedules the same bucket after the key expires and a
        // nightly job runs twice.
        const std::chrono::seconds ttl = spec.period * 2;

        const Result<JobId> scheduled = schedule_with_dedupe_ttl(spec.kind, {}, due, key, ttl);
        if (!scheduled) { return scheduled.error(); }
        ++created;
    }
    return created;
}

// --- claiming ---------------------------------------------------------------

Result<std::vector<ClaimedJob>> JobQueue::claim(std::chrono::milliseconds block,
                                                std::uint32_t limit) {
    // The driver materialises reply values as std::string and builds one map per
    // entry. That allocation is the driver's parse, not ours, and it is bounded
    // by `limit` — sixteen small maps per round trip, not one per queued job.
    using Attrs = std::unordered_map<std::string, std::string>;
    using Item = std::pair<std::string, Attrs>;
    using ItemStream = std::vector<Item>;

    std::vector<ClaimedJob> claimed;
    try {
        std::unordered_map<std::string, ItemStream> reply;
        redis::RedisClient::instance().xreadgroup(
            view_of(group_), view_of(config_.consumer), view_of(stream_key_), ">", block,
            static_cast<long long>(limit), false, std::inserter(reply, reply.end()));

        for (auto& [stream, items] : reply) {
            claimed.reserve(claimed.size() + items.size());
            for (auto& [entry_id, attrs] : items) {
                const auto field = attrs.find(std::string{kFieldName});
                if (field == attrs.end()) {
                    // An entry with no payload cannot be dispatched and cannot be
                    // retried into something valid. Acknowledge it so it stops
                    // occupying the pending list.
                    (void)ack_only(entry_id);
                    continue;
                }
                const std::optional<JobHeader> header = decode_header(field->second);
                if (!header) {
                    LOG_ERROR << "job envelope rejected: malformed or truncated";
                    (void)ack_only(entry_id);
                    continue;
                }
                claimed.push_back(ClaimedJob{entry_id, std::move(field->second), *header});
            }
        }
        return claimed;
    } catch (const std::exception& e) {
        LOG_WARN << "job claim failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

Result<std::vector<ClaimedJob>> JobQueue::reclaim_stalled(std::chrono::milliseconds min_idle,
                                                          std::uint32_t limit) {
    std::vector<ClaimedJob> claimed;
    try {
        std::vector<std::string> flat;
        const std::string idle = std::to_string(min_idle.count());
        const std::string count = std::to_string(limit);
        redis::RedisClient::instance().eval(
            view_of(kReclaimScript), {view_of(stream_key_)},
            {view_of(group_), view_of(config_.consumer), view_of(idle), view_of(count)},
            std::back_inserter(flat));

        claimed.reserve(flat.size() / 2);
        for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
            const std::optional<JobHeader> header = decode_header(flat[i + 1]);
            if (!header) {
                LOG_ERROR << "reclaimed job envelope rejected: malformed or truncated";
                (void)ack_only(flat[i]);
                continue;
            }
            claimed.push_back(ClaimedJob{std::move(flat[i]), std::move(flat[i + 1]), *header});
        }
        return claimed;
    } catch (const std::exception& e) {
        LOG_WARN << "job reclaim failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

// --- execution --------------------------------------------------------------

JobOutcome JobQueue::dispatch(const ClaimedJob& job, mongocxx::client& client) const noexcept {
    const JobSpec* spec = spec_of(job.header.kind);
    if (spec == nullptr) {
        // An unknown or retired kind. Permanent, not retryable: four more
        // attempts will not teach this build a handler it does not have.
        LOG_ERROR << "job kind " << static_cast<unsigned>(job.header.kind)
                  << " has no handler in this build";
        return JobOutcome::Failed;
    }

    const JobRunContext context{
        .args = envelope_args(job.envelope, job.header),
        .id = job.header.id,
        .linked_trace = job.header.linked_trace,
        .client = &client,
        .attempt = job.header.attempt,
    };

    // The handler is noexcept, so this catch is belt and braces for a handler
    // that manages to throw through a C library. It must not propagate: an
    // exception escaping a pool task calls std::terminate.
    //
    // `root_linked_to` mints inside the try because it draws from the CSPRNG,
    // which throws rather than degrading to a weaker source. A job that cannot
    // be given a trace id is a job that retries, not a process that dies.
    try {
        const http::TraceScope scope{http::root_linked_to(job.header.linked_trace)};
        return spec->handler(context);
    } catch (...) {
        LOG_ERROR << "job " << text_of(spec->key) << " threw; treating as retryable";
        return JobOutcome::Retry;
    }
}

Status JobQueue::complete(const ClaimedJob& job, JobOutcome outcome, db::TimeMs now) {
    const JobSpec* spec = spec_of(job.header.kind);
    const std::uint8_t max_attempts = spec == nullptr ? 1 : spec->max_attempts;
    const std::string_view kind_key = spec == nullptr ? std::string_view{"unknown"} : spec->key;

    try {
        sw::redis::Redis& redis = redis::RedisClient::instance();

        if (outcome == JobOutcome::Done) {
            (void)redis.eval<long long>(view_of(kAckScript), {view_of(stream_key_)},
                                        {view_of(group_), view_of(job.stream_id)});
            return ok();
        }

        const bool exhausted =
            outcome == JobOutcome::Failed || job.header.attempt >= max_attempts;
        if (exhausted) {
            const std::string_view reason =
                outcome == JobOutcome::Failed ? "permanent" : "attempts_exhausted";
            const std::string attempts = std::to_string(job.header.attempt);
            (void)redis.eval<long long>(
                view_of(kDeadLetterScript), {view_of(stream_key_), view_of(dead_key_)},
                {view_of(group_), view_of(job.stream_id), view_of(job.envelope),
                 view_of(reason), view_of(attempts)});

            // The kind and the attempt count, never the arguments: they may
            // contain identifiers (docs/10-timer-jobs.md §8).
            if (spec != nullptr && spec->alert_on_dead_letter) {
                LOG_ERROR << "job dead-lettered: kind=" << text_of(kind_key)
                          << " attempts=" << static_cast<unsigned>(job.header.attempt)
                          << " reason=" << text_of(reason) << " — this work did not happen";
            } else {
                LOG_WARN << "job dead-lettered: kind=" << text_of(kind_key)
                         << " attempts=" << static_cast<unsigned>(job.header.attempt)
                         << " reason=" << text_of(reason);
            }
            return ok();
        }

        const std::uint32_t base = retry_delay_seconds(job.header.attempt);
        // Jitter up to a quarter of the delay, so N workers failing on the same
        // downstream outage do not synchronise into a second storm.
        const std::uint32_t jitter = base < 4 ? 0 : crypto::random_below(base / 4);
        const db::TimeMs due = now + std::chrono::seconds{base + jitter};

        JobHeader next = job.header;
        next.attempt = static_cast<std::uint8_t>(job.header.attempt + 1);
        std::string envelope;
        encode_envelope(next, envelope_args(job.envelope, job.header), envelope);

        const std::string id_bytes = binary_of(job.header.id);
        const std::string score = std::to_string(due.time_since_epoch().count());
        // A retry keeps a dedupe key so that a second scheduler for the same
        // logical event during the backoff window still collapses onto it.
        const std::string idem =
            idem_key_of(idem_prefix_, std::string{"retry:"} + uuid::to_string(job.header.id));

        (void)redis.eval<long long>(
            view_of(kRetryScript),
            {view_of(stream_key_), view_of(schedule_key_), view_of(payload_key_),
             view_of(config_.prefix + ":idemof")},
            {view_of(group_), view_of(job.stream_id), view_of(id_bytes), view_of(score),
             view_of(envelope), view_of(idem)});

        LOG_WARN << "job retry: kind=" << text_of(kind_key) << " attempt="
                 << static_cast<unsigned>(job.header.attempt) << " in " << (base + jitter)
                 << "s";
        return ok();
    } catch (const std::exception& e) {
        // The entry stays pending, so the reclaim path picks it up. Nothing is
        // lost; the job may simply run again, which every handler tolerates.
        LOG_ERROR << "job completion failed for " << text_of(kind_key) << ": " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

// --- observability ----------------------------------------------------------

Result<QueueStats> JobQueue::stats() const {
    try {
        sw::redis::Redis& redis = redis::RedisClient::instance();
        QueueStats out{};
        out.stream_length = static_cast<std::uint64_t>(redis.xlen(view_of(stream_key_)));
        out.scheduled = static_cast<std::uint64_t>(redis.zcard(view_of(schedule_key_)));
        out.dead_lettered = static_cast<std::uint64_t>(redis.xlen(view_of(dead_key_)));
        try {
            // XPENDING's summary reply is [count, min-id, max-id, [[consumer, count], ...]],
            // and the per-consumer counts come back as bulk strings under RESP2. Asking
            // the driver to parse that nested shape into a typed container is one
            // container choice away from throwing, and the catch below would then report a
            // depth of ZERO for a queue that has entries stuck — which is precisely the
            // observability failure this field exists to surface. So the script returns
            // the one number wanted, and there is no nested reply to mis-type.
            out.pending = static_cast<std::uint64_t>(std::max<long long>(
                0, redis.eval<long long>(view_of(kPendingDepthScript),
                                         {view_of(stream_key_)}, {view_of(group_)})));
        } catch (const std::exception&) {
            // No consumer group yet, which is not an error before start().
            out.pending = 0;
        }
        return out;
    } catch (const std::exception& e) {
        LOG_WARN << "job stats unavailable: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

Status JobQueue::purge() noexcept {
    try {
        (void)redis::RedisClient::instance().eval<long long>(
            view_of(kPurgeScript),
            {view_of(schedule_key_), view_of(payload_key_), view_of(stream_key_),
             view_of(dead_key_), view_of(lease_key_), view_of(config_.prefix + ":idemof")},
            {});
        return ok();
    } catch (const std::exception& e) {
        LOG_WARN << "job purge failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

// --- internals shared with promoter.cc --------------------------------------

Status JobQueue::ack_only(std::string_view stream_id) noexcept {
    try {
        (void)redis::RedisClient::instance().eval<long long>(
            view_of(kAckScript), {view_of(stream_key_)},
            {view_of(group_), view_of(stream_id)});
        return ok();
    } catch (const std::exception& e) {
        LOG_WARN << "job ack failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

Result<JobId> JobQueue::schedule_with_dedupe_ttl(std::uint16_t kind, std::span<const std::uint8_t> args,
                                                 db::TimeMs when, std::string_view dedupe_key,
                                                 std::chrono::seconds dedupe_ttl) {
    if (spec_of(kind) == nullptr) { return fail(ErrorCode::ValidationFailed, "kind"); }
    if (args.size() > kMaxJobArgsBytes) { return fail(ErrorCode::PayloadTooLarge, "args"); }

    const db::TimeMs due = std::max(when, db::now_ms());
    const JobId id = uuid::generate_v4();
    const JobHeader header{
        .id = id,
        .not_before = due,
        .kind = kind,
        .args_len = static_cast<std::uint16_t>(args.size()),
        .attempt = 1,
        // Derived by the encoder from the link; see JobHeader::version.
        .version = kJobEnvelopeVersionUnlinked,
        .reserved = {},
        .linked_trace = http::trace_link_of(http::current_trace()),
    };

    std::string envelope;
    encode_envelope(header, args, envelope);

    const std::string idem = idem_key_of(idem_prefix_, dedupe_key);
    const std::string id_bytes = binary_of(id);
    const std::string score = std::to_string(due.time_since_epoch().count());
    const std::string ttl_ms = std::to_string(dedupe_ttl.count() * 1000);

    try {
        const std::string stored = redis::RedisClient::instance().eval<std::string>(
            view_of(kScheduleScript),
            {view_of(schedule_key_), view_of(payload_key_), view_of(idem),
             view_of(config_.prefix + ":idemof")},
            {view_of(id_bytes), view_of(score), view_of(envelope), view_of(ttl_ms),
             view_of(idem)});
        JobId existing{};
        if (!read_uuid(stored, existing)) { return fail(ErrorCode::Internal, "job"); }
        return existing;
    } catch (const std::exception& e) {
        LOG_ERROR << "job schedule failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "queue");
    }
}

bool JobQueue::hold_lease() noexcept {
    try {
        sw::redis::Redis& redis = redis::RedisClient::instance();
        const std::string ttl_ms = std::to_string(config_.lease_ttl.count() * 1000);

        if (leased_.load(std::memory_order_acquire)) {
            // Renewed by a heartbeat rather than set to the expected duration up
            // front: a sweep that outlives its lease would otherwise lose it
            // silently while still running (docs/10-timer-jobs.md §5).
            const long long renewed = redis.eval<long long>(
                view_of(kLeaseRenewScript), {view_of(lease_key_)},
                {view_of(lease_token_), view_of(ttl_ms)});
            if (renewed > 0) { return true; }
            leased_.store(false, std::memory_order_release);
        }

        const bool acquired = redis.set(
            view_of(lease_key_), view_of(lease_token_),
            std::chrono::milliseconds{config_.lease_ttl}, sw::redis::UpdateType::NOT_EXIST);
        leased_.store(acquired, std::memory_order_release);
        return acquired;
    } catch (const std::exception& e) {
        LOG_WARN << "promoter lease unavailable: " << e.what();
        leased_.store(false, std::memory_order_release);
        return false;
    }
}

void JobQueue::release_lease() noexcept {
    if (!leased_.exchange(false)) { return; }
    try {
        (void)redis::RedisClient::instance().eval<long long>(
            view_of(kLeaseReleaseScript), {view_of(lease_key_)}, {view_of(lease_token_)});
    } catch (const std::exception& e) {
        LOG_WARN << "promoter lease release failed: " << e.what();
    }
}

}  // namespace anvil::timer
