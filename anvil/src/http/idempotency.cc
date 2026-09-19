#include "anvil/http/idempotency.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>

#include <sw/redis++/redis++.h>
#include <trantor/utils/Logger.h>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/core/types.h"
#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/redis/redis_client.h"

namespace anvil::http {
namespace {

// --- the record -------------------------------------------------------------
//
// One Redis string per key, laid out so a Lua script can read the two fields it
// needs — the state and the fence — without parsing anything. Fixed offsets, big
// enough to be unambiguous against the empty string the claim script returns
// when it claimed.
//
//   0    version
//   1    state
//   2..3 HTTP status, big endian
//   4..19  fence token
//   20..51 request fingerprint
//   52..   response body
constexpr std::uint8_t kRecordVersion = 1;

constexpr std::uint8_t kStateInFlight = 1;
constexpr std::uint8_t kStateComplete = 2;
constexpr std::uint8_t kStateCompleteNoBody = 3;

constexpr std::size_t kVersionOffset = 0;
constexpr std::size_t kStateOffset = 1;
constexpr std::size_t kStatusOffset = 2;
constexpr std::size_t kTokenOffset = 4;
constexpr std::size_t kFingerprintOffset = kTokenOffset + kIdempotencyTokenBytes;
constexpr std::size_t kHeaderBytes = kFingerprintOffset + sizeof(crypto::Digest256);

static_assert(kHeaderBytes == 52);

// The Lua below addresses these fields by literal, 1-indexed position, because
// Lua has no way to take a constant from here. The assertion is what keeps the
// two spellings from drifting: a field moved in C++ without the script being
// edited would otherwise compare the wrong sixteen bytes and silently accept
// every fence.
static_assert(kStateOffset + 1 == 2, "the scripts read the state as byte 2");
static_assert(kTokenOffset + 1 == 5 && kTokenOffset + kIdempotencyTokenBytes == 20,
              "the scripts read the fence as bytes 5..20");

// --- the scripts ------------------------------------------------------------
//
// Each is one script for the reason every multi-step Redis mutation in this
// codebase is (anvil/timer/streams.cc): a crash between two of these calls leaves
// a state with no recovery path. A claim written without its marker is a record
// nothing will ever complete; a marker deleted by an attempt that no longer holds
// it is a second attempt admitted to work the first is still doing.
//
// Claim. KEYS: the record. ARGV: the in-flight envelope, the marker's TTL in ms.
//
// GET-then-SET without NX is safe precisely because the script is atomic, which
// is the same argument the job queue's schedule script makes. Returning the
// EXISTING record rather than a flag is what makes this one round trip: the
// caller needs the fingerprint, the state and possibly the body, and asking for
// them afterwards would be a second trip and a race with the record's own expiry.
constexpr std::string_view kClaimScript = R"lua(
local existing = redis.call('GET', KEYS[1])
if existing then return existing end
redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2])
return ''
)lua";

// Record. KEYS: the record. ARGV: the fence, the completed envelope, retention ms.
//
// The fence check is what stops a slow first attempt overwriting a newer one's
// record after its own marker expired — the classic lost fence, and here it
// would answer a client the response to a request that has since been superseded.
constexpr std::string_view kRecordScript = R"lua(
local existing = redis.call('GET', KEYS[1])
if not existing then return 0 end
if string.sub(existing, 5, 20) ~= ARGV[1] then return 0 end
redis.call('SET', KEYS[1], ARGV[2], 'PX', ARGV[3])
return 1
)lua";

// Release. KEYS: the record. ARGV: the fence.
//
// The state check is not redundant with the fence. A caller that releases after
// recording — a `catch` that runs on a path which already succeeded — would
// otherwise delete its own completed record and turn the store into a no-op for
// exactly the request that needed it. A completed record is never deleted here;
// it expires.
constexpr std::string_view kReleaseScript = R"lua(
local existing = redis.call('GET', KEYS[1])
if not existing then return 0 end
if string.byte(existing, 2) ~= 1 then return 0 end
if string.sub(existing, 5, 20) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1])
return 1
)lua";

static_assert(kStateInFlight == 1, "the release script compares the state against 1");

// The label index is a cast, and these are what make that safe. A state added to
// one enum and not the other would otherwise count into the wrong series, which
// is a metric that lies rather than a metric that breaks.
static_assert(static_cast<std::uint8_t>(IdempotencyState::Fresh) ==
              static_cast<std::uint8_t>(analytics::IdempotencyOutcome::Fresh));
static_assert(static_cast<std::uint8_t>(IdempotencyState::InFlight) ==
              static_cast<std::uint8_t>(analytics::IdempotencyOutcome::InFlight));
static_assert(static_cast<std::uint8_t>(IdempotencyState::Replay) ==
              static_cast<std::uint8_t>(analytics::IdempotencyOutcome::Replay));
static_assert(static_cast<std::uint8_t>(IdempotencyState::Completed) ==
              static_cast<std::uint8_t>(analytics::IdempotencyOutcome::Completed));
static_assert(static_cast<std::uint8_t>(IdempotencyState::Mismatch) ==
              static_cast<std::uint8_t>(analytics::IdempotencyOutcome::Mismatch));
static_assert(static_cast<std::size_t>(IdempotencyState::Mismatch) + 1 ==
                  static_cast<std::size_t>(analytics::IdempotencyOutcome::Unavailable),
              "the states are the first values of the label space, which is what makes the "
              "index above a cast rather than a second table");
static_assert(analytics::kIdempotencyOutcomeValues.size() ==
                  static_cast<std::size_t>(analytics::IdempotencyOutcome::Unavailable) + 1,
              "a claim outcome was added without a name for it in the scrape");

void count_outcome(IdempotencyState state) noexcept {
    analytics::count(analytics::Internal::IdempotencyClaims,
                     analytics::LabelIndex{static_cast<std::uint16_t>(state)});
}

[[nodiscard]] sw::redis::StringView view_of(std::string_view text) noexcept {
    return sw::redis::StringView{text.data(), text.size()};
}

// The three inputs are separated rather than concatenated, and the separator is
// a byte none of them may contain — a route id is an identifier, an identity is
// raw bytes of a fixed width, and the client's key is length-prefixed by being
// last. Without a separator ("ab", "c") and ("a", "bc") hash to one key, and a
// collision here replays one caller's response to another.
[[nodiscard]] std::array<char, kIdempotencyKeyChars> key_for(
    std::string_view route_id, std::span<const std::uint8_t> identity, std::string_view key) {
    constexpr std::array<std::uint8_t, 1> kSeparator{0x1F};

    crypto::Sha256Stream stream;
    stream.update(std::span{reinterpret_cast<const std::uint8_t*>(route_id.data()),
                            route_id.size()});
    stream.update(kSeparator);
    stream.update(identity);
    stream.update(kSeparator);
    stream.update(std::span{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
    const crypto::Digest256 digest = stream.finish();

    constexpr std::string_view kHex = "0123456789abcdef";
    std::array<char, kIdempotencyKeyChars> out{};
    out[0] = 'i';
    out[1] = 'd';
    out[2] = 'e';
    out[3] = 'm';
    out[4] = ':';
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[5 + (i * 2)] = kHex[digest[i] >> 4U];
        out[5 + (i * 2) + 1] = kHex[digest[i] & 0x0FU];
    }
    return out;
}

void append_header(std::string& out, std::uint8_t state, std::uint16_t status,
                   std::span<const std::uint8_t> token, const crypto::Digest256& fingerprint) {
    out.push_back(static_cast<char>(kRecordVersion));
    out.push_back(static_cast<char>(state));
    // Big endian, so a record is the same bytes on every machine that writes one.
    out.push_back(static_cast<char>((status >> 8U) & 0xFFU));
    out.push_back(static_cast<char>(status & 0xFFU));
    out.append(reinterpret_cast<const char*>(token.data()), token.size());
    out.append(reinterpret_cast<const char*>(fingerprint.data()), fingerprint.size());
}

// A record this build cannot read is treated as COMPLETE WITHOUT A BODY, which
// is the only safe reading during a rolling deploy: it says another process did
// this work and this one cannot report what it answered. Reading it as fresh
// would do the work twice, which is the single thing the store exists to prevent.
[[nodiscard]] IdempotencyClaim decode(const std::string& stored,
                                      const crypto::Digest256& fingerprint,
                                      const std::array<char, kIdempotencyKeyChars>& key) {
    IdempotencyClaim claim{.body = {},
                           .key = key,
                           .fingerprint = fingerprint,
                           .token = {},
                           .status = 0,
                           .state = IdempotencyState::Completed};

    if (stored.size() < kHeaderBytes) { return claim; }
    if (static_cast<std::uint8_t>(stored[kVersionOffset]) != kRecordVersion) { return claim; }

    // The fingerprint decides first, and before the state: a key reused for a
    // different request is a mismatch whether the first one has finished or not,
    // and reporting that as "in flight" would tell the client to retry into the
    // same refusal until the marker expires.
    const std::span<const std::uint8_t> stored_fingerprint{
        reinterpret_cast<const std::uint8_t*>(stored.data()) + kFingerprintOffset,
        sizeof(crypto::Digest256)};
    if (!crypto::secure_equal(stored_fingerprint, fingerprint)) {
        claim.state = IdempotencyState::Mismatch;
        return claim;
    }

    const auto state = static_cast<std::uint8_t>(stored[kStateOffset]);
    if (state == kStateInFlight) {
        claim.state = IdempotencyState::InFlight;
        return claim;
    }
    // Neither a state this build writes nor one it knows how to refuse. Same
    // reading as an unknown version, and for the same reason.
    if (state != kStateComplete && state != kStateCompleteNoBody) { return claim; }

    claim.status = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(stored[kStatusOffset])) << 8U) |
        static_cast<std::uint8_t>(stored[kStatusOffset + 1]));

    if (state == kStateCompleteNoBody) { return claim; }

    claim.body.assign(stored, kHeaderBytes, std::string::npos);
    claim.state = IdempotencyState::Replay;
    return claim;
}

}  // namespace

Result<IdempotencyClaim> IdempotencyStore::claim(std::string_view route_id,
                                                 std::span<const std::uint8_t> identity,
                                                 std::string_view key,
                                                 const crypto::Digest256& request_fingerprint) {
    // An empty route id merges every route into one namespace and an empty
    // identity merges every caller into one — see the header. Both are refusals
    // rather than defaults, because both are what a caller reaches by passing a
    // value it forgot to fill in. They are Internal rather than
    // ValidationFailed: neither can be caused by a request, so a 400 would send
    // the client to look for a mistake it did not make.
    if (route_id.empty()) { return fail(ErrorCode::Internal, "route"); }
    if (identity.empty()) { return fail(ErrorCode::Internal, "identity"); }
    if (key.empty() || key.size() > kMaxIdempotencyKeyBytes) {
        return fail(ErrorCode::ValidationFailed, "idempotency_key");
    }

    const std::array<char, kIdempotencyKeyChars> record_key = key_for(route_id, identity, key);
    const std::array<std::uint8_t, kIdempotencyTokenBytes> token =
        crypto::random_array<kIdempotencyTokenBytes>();

    std::string marker;
    marker.reserve(kHeaderBytes);
    append_header(marker, kStateInFlight, 0, token, request_fingerprint);

    try {
        const std::string existing = redis::RedisClient::instance().eval<std::string>(
            view_of(kClaimScript),
            {sw::redis::StringView{record_key.data(), record_key.size()}},
            {view_of(marker), std::to_string(config_.in_flight_ttl.count())});

        if (existing.empty()) {
            count_outcome(IdempotencyState::Fresh);
            return IdempotencyClaim{.body = {},
                                    .key = record_key,
                                    .fingerprint = request_fingerprint,
                                    .token = token,
                                    .status = 0,
                                    .state = IdempotencyState::Fresh};
        }

        IdempotencyClaim found = decode(existing, request_fingerprint, record_key);
        count_outcome(found.state);
        return found;
    } catch (const std::exception& e) {
        // No degraded mode — see the header. A process-local answer here is not a
        // weaker guarantee, it is a confidently wrong one, and what it produces
        // is the duplicate write this class exists to prevent.
        //
        // Counted, because this fires once per protected request and a line per
        // request is what docs/00-architecture.md §9 forbids. record() and
        // release() log rather than count for a reason that only holds because
        // of the ordering: an outage refuses every claim, so nothing ever
        // reaches them at request rate.
        analytics::count(analytics::Internal::IdempotencyClaims,
                         analytics::LabelIndex{static_cast<std::uint16_t>(
                             analytics::IdempotencyOutcome::Unavailable)});
        LOG_ERROR << "idempotency store unavailable: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "idempotency");
    }
}

Status IdempotencyStore::record(const IdempotencyClaim& claim, std::uint16_t status,
                                std::string_view body) {
    // Dropped rather than truncated. A truncated JSON document is a parse error
    // the client cannot tell from a corrupt response, where a body-less record
    // is a refusal that says exactly what happened.
    const bool retain = body.size() <= config_.max_body_bytes;

    std::string record;
    record.reserve(kHeaderBytes + (retain ? body.size() : 0));
    append_header(record, retain ? kStateComplete : kStateCompleteNoBody, status, claim.token,
                  claim.fingerprint);
    if (retain) { record.append(body); }

    try {
        const long long stored = redis::RedisClient::instance().eval<long long>(
            view_of(kRecordScript),
            {sw::redis::StringView{claim.key.data(), claim.key.size()}},
            {sw::redis::StringView{reinterpret_cast<const char*>(claim.token.data()),
                                   claim.token.size()},
             view_of(record), std::to_string(config_.retention.count())});
        if (stored == 0) {
            // The marker expired under a request that outran its deadline, or
            // another attempt holds the key now. The caller still answers its own
            // request; what is lost is the replay for a later retry.
            return fail(ErrorCode::Conflict, "idempotency");
        }
        return ok();
    } catch (const std::exception& e) {
        LOG_ERROR << "idempotency record failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "idempotency");
    }
}

void IdempotencyStore::release(const IdempotencyClaim& claim) noexcept {
    // Only a claim that owns a marker has anything to release. Calling this on a
    // Replay would present that record's token — which the claim does not
    // carry — and delete nothing, so the guard is here rather than in the script
    // where it would be a round trip to learn the same thing.
    if (claim.state != IdempotencyState::Fresh) { return; }

    try {
        (void)redis::RedisClient::instance().eval<long long>(
            view_of(kReleaseScript),
            {sw::redis::StringView{claim.key.data(), claim.key.size()}},
            {sw::redis::StringView{reinterpret_cast<const char*>(claim.token.data()),
                                   claim.token.size()}});
    } catch (const std::exception& e) {
        // Best effort by construction: a release that does not land costs the
        // client a wait until the marker expires, which is the same outcome as
        // this process having been killed a moment earlier.
        LOG_WARN << "idempotency release failed: " << e.what();
    }
}

}  // namespace anvil::http
