#pragma once

// Recognising a repeated request, so a client may retry one that is not safe to
// repeat (docs/00-architecture.md §4 stage 9, docs/01-seams.md §7).
//
// --- the gap this closes ----------------------------------------------------
//
// `descriptor::RouteDescription::idempotent` tells a generated client whether
// repeating a call is safe. For the routes where it is false, a client that
// loses a response has no good option: retrying risks a second write, and not
// retrying turns a dropped packet into a user-visible failure on an operation
// that may well have succeeded. So a generated client does not retry them at
// all, and every network blip on a POST surfaces as an error the user has to
// resolve by hand.
//
// The server is the only participant that can settle it, because it is the only
// one that knows whether the first attempt landed. This is that mechanism: the
// client sends a key it chooses, the server records the response against it, and
// a repeat is answered from the record rather than performed again.
//
// --- what it guarantees, and what it does not -------------------------------
//
// It makes a retry WITHIN the retention window safe. It does not make an
// operation exactly-once forever: past the window the record is gone and a
// repeat is a fresh request, which is the correct trade — a store that never
// forgot would grow without bound, and an unbounded store keyed by
// attacker-chosen strings is a memory-exhaustion vector rather than a
// correctness guarantee.
//
// A process killed mid-request leaves its in-flight marker behind. It expires
// after `in_flight_ttl`, which is therefore the REQUEST DEADLINE and not a
// tuning knob: set longer than a request can run and a client is blocked from
// retrying work that will never finish; set shorter and two attempts overlap.
//
// --- what bounds it ---------------------------------------------------------
//
// The keys are attacker-chosen, so the number of records is too, and every one
// of them occupies Redis memory until its TTL runs out. What bounds it is the
// STAGE BEFORE IT: a claim happens at stage 9 of the request pipeline
// (docs/00-architecture.md §4) and the per-IP limiter refuses at stage 3, so the
// records one caller can create in a retention window is that caller's budget
// and not the bandwidth they have. A deployment that protects a route with this
// and gives it no rate-limit rule has an unbounded store, which is why the two
// are documented in the same section of docs/01-seams.md.
//
// A marker is 52 bytes. A completed record is 52 plus a bounded body, and it is
// only ever written by a request that actually ran.
//
// --- why it does not degrade ------------------------------------------------
//
// anvil/http/rate_limit.h falls back to a per-process bucket when Redis is
// unreachable, because a weaker limit is better than none. This does the
// OPPOSITE and fails the request, and the asymmetry is deliberate: a
// process-local idempotency store answers "this is a fresh request" for every
// retry that reaches a different instance, which is not a weaker guarantee but a
// confidently wrong one — and the failure it produces is a duplicate write
// rather than an extra request. There is no degraded mode that is better than
// refusing.
//
// BLOCKING. Every call here talks to Redis and must run on a worker pool, never
// on a Trantor event-loop thread.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/crypto/secret.h"

namespace anvil::http {

// How a claim ended. FIVE states rather than a pair of flags, because each one
// has exactly one correct response and a flag combination does not: a caller
// branching on `completed && !has_body` is a caller one negation away from
// replaying a body it does not have.
enum class IdempotencyState : std::uint8_t {
    // Nothing was stored under this key. The caller owns the work, and owes the
    // store either a record() or a release().
    Fresh = 0,
    // An earlier attempt at THIS request is still running. Answer `Conflict`;
    // do not do the work. The client retries after the first attempt finishes or
    // its marker expires.
    InFlight = 1,
    // An earlier attempt finished and its response is here. Send `status` and
    // `body` — the same bytes the first attempt sent.
    Replay = 2,
    // An earlier attempt finished, but its response was not retained: too large
    // for the store, or written by a build whose record format this one cannot
    // read. Answer `Conflict`. The work happened exactly once, which is the
    // property; the body is what is gone.
    Completed = 3,
    // This key was used before for a DIFFERENT request. Answer `Conflict`, and
    // neither perform the work nor replay: replaying would answer one request
    // with another request's response, which is worse than either.
    Mismatch = 4,
};

// The ceiling anvil enforces on a retained response body, whatever an
// application configures.
//
// A record is one Redis string, and Redis serves it from one thread: a
// multi-megabyte value is head-of-line latency for every other client on that
// server, including the rate limiter two stages earlier in the same request
// pipeline. A replay cache is for the JSON a write route answers with; a route
// whose response is a document belongs behind `X-Accel-Redirect`, not in here
// (ENGINEERING_RULES.md §2.4).
inline constexpr std::size_t kMaxRetainedBodyBytes = 64 * 1024;

// The bound on the client's key. It is attacker-supplied and it is hashed, so
// its CONTENT needs no opinion — any bytes produce a valid Redis key. Its LENGTH
// does: without a bound, one request can make the server hash a megabyte.
inline constexpr std::size_t kMaxIdempotencyKeyBytes = 255;

// The fence that ties a record to the attempt that claimed it. Random rather
// than sequential so two processes cannot mint the same one; never sent to a
// client, which is why the comparison that checks it does not need to be
// constant-time and could not be — it happens inside a Lua script.
inline constexpr std::size_t kIdempotencyTokenBytes = 16;

// "idem:" and a 32-byte digest in hex. A fixed length, so the claim carries the
// key in an array rather than in a heap string charged to every protected
// request (ENGINEERING_RULES.md §2.1) — which is also why it is hex and not the base64url
// the rate limiter's keys use: base64url_encode returns a std::string, and the
// spelling of a key that is never read by a human is worth nothing next to the
// allocation.
inline constexpr std::size_t kIdempotencyKeyChars = 5 + 64;

struct IdempotencyConfig final {
    // How long a completed record answers a repeat. A deployment decision: it is
    // how long a client may sensibly still be retrying.
    std::chrono::milliseconds retention;

    // The REQUEST DEADLINE — see the header comment.
    std::chrono::milliseconds in_flight_ttl;

    // Responses above this are not retained; the record still marks the work as
    // done and a repeat is answered `Completed`. Never above
    // kMaxRetainedBodyBytes.
    std::uint32_t             max_body_bytes;
};

// Each condition is a configuration that ships as a store which does not store,
// and none of them is visible in a review of the numbers themselves.
[[nodiscard]] constexpr bool idempotency_config_is_well_formed(
    const IdempotencyConfig& config) noexcept {
    // A zero retention writes a record that has already expired, so every
    // repeat is a fresh request and the store is an elaborate no-op.
    if (config.retention.count() <= 0) { return false; }
    // A zero in-flight TTL is a marker that never blocks a concurrent attempt,
    // which is the one thing the marker exists to do.
    if (config.in_flight_ttl.count() <= 0) { return false; }
    // A marker that outlives the retention window blocks a retry for longer than
    // a completed record would have answered it — the failure mode is a client
    // locked out of an operation that already succeeded.
    if (config.in_flight_ttl > config.retention) { return false; }
    if (config.max_body_bytes > kMaxRetainedBodyBytes) { return false; }
    return true;
}

// What a claim found, and what the caller needs to finish it.
//
// Ordered largest-alignment-first (ENGINEERING_RULES.md §2.3).
struct IdempotencyClaim final {
    // The first attempt's response body. Non-empty only for Replay.
    std::string                                     body;

    // The Redis key this claim resolved to, so record() and release() do not
    // re-derive it — and cannot derive it differently.
    std::array<char, kIdempotencyKeyChars>          key;

    // The fingerprint this claim was made against, carried so that record()
    // writes the completed record with the SAME one the marker held. A record
    // that lost it would answer the next repeat `Mismatch` — the client would be
    // told its own retry was a different request.
    crypto::Digest256                               fingerprint;

    // The fence. Meaningful only for Fresh; record() and release() present it
    // and the store refuses if it is no longer the attempt that holds the key.
    std::array<std::uint8_t, kIdempotencyTokenBytes> token;

    // The first attempt's HTTP status. Meaningful only for Replay.
    std::uint16_t                                   status;

    IdempotencyState                                state;
};

class IdempotencyStore final {
public:
    explicit IdempotencyStore(IdempotencyConfig config) noexcept : config_{config} {}

    // Claims the key, or reports what is already under it.
    //
    // `route_id` is `descriptor::RouteDescription::id` — the stable, opaque name
    // a client already knows the route by. It is part of the key because two
    // routes must never share a record: a client that reuses one key across a
    // create and a delete would otherwise be answered the create's response to
    // its delete.
    //
    // `identity` is the caller: the user id on an authenticated route, the
    // client address on one that is not. It is part of the key, and an EMPTY one
    // is refused rather than treated as "anonymous" — an empty identity merges
    // every anonymous caller into one namespace, where the first client to use
    // the key "1" is replayed to every other client that picks it. That is the
    // worst failure this class has, and it is the one a caller reaches by
    // passing a default-constructed span.
    //
    // `request_fingerprint` is a digest of the request the caller is about to
    // perform — the body, or for a streamed upload whatever the caller hashed
    // as it streamed. It is what separates a RETRY from a key reused for
    // something else.
    //
    // Whether a request with NO key reaches here at all is the caller's decision,
    // not this class's: anvil never reads the request, and "refuse a
    // non-idempotent route that sent no key" and "run it unprotected" are both
    // defensible depending on who the clients are. An EMPTY key passed here is a
    // refusal, because it is what a caller reaches by forwarding a header that
    // was not there.
    //
    // Fails with ValidationFailed for a key that is not usable, Internal for a
    // route or identity that is not, and ServiceUnavailable when Redis cannot
    // answer — never with a Fresh claim it is not entitled to.
    [[nodiscard]] Result<IdempotencyClaim> claim(std::string_view route_id,
                                                 std::span<const std::uint8_t> identity,
                                                 std::string_view key,
                                                 const crypto::Digest256& request_fingerprint);

    // Stores the response against a claim, so a repeat replays it.
    //
    // Call this whether the work SUCCEEDED or FAILED. A failure recorded here is
    // replayed as that same failure, which is correct: the client learns the
    // same thing it would have learnt from the first response, and the work is
    // not repeated. Only call release() instead when nothing was written.
    //
    // A failure return does not invalidate the response the caller is about to
    // send. It means the record was not stored — the claim expired, or another
    // attempt now holds the key — so a later retry will be treated as fresh.
    // The caller answers its own request either way.
    [[nodiscard]] Status record(const IdempotencyClaim& claim, std::uint16_t status,
                                std::string_view body);

    // Releases a claim so the client may retry immediately.
    //
    // ONLY when nothing was written. A handler that failed halfway through has
    // side effects the client's retry would repeat, and for that case record()
    // with the error response is the correct call — the retry then gets the
    // error back rather than a second half-application.
    //
    // Best effort and never throws: a release that does not land costs the
    // client a wait until the marker expires, which is the same outcome as the
    // process having been killed a moment earlier.
    void release(const IdempotencyClaim& claim) noexcept;

    [[nodiscard]] const IdempotencyConfig& config() const noexcept { return config_; }

    IdempotencyStore(const IdempotencyStore&) = delete;
    IdempotencyStore& operator=(const IdempotencyStore&) = delete;

private:
    const IdempotencyConfig config_;
};

}  // namespace anvil::http
