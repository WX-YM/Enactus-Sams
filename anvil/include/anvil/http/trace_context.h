#pragma once

// The correlation a request carries INTO this process, and across every
// boundary inside it.
//
// docs/17-analytics.md §16 refused OpenTelemetry, and named three things to
// refuse: a tracing model, a context propagation format, and a wire protocol.
// Only one of the three is expensive. The line this header draws is that
// **anvil owns context and the application owns export** — the model and the
// protocol stay refused, and what ships is the 55 bytes that cross the one
// boundary an application cannot reach into from outside: `guarded()`, through
// which every task posted to every pool passes.
//
// An application that wants traces writes an exporter and reads one accessor.
// It does not write middleware to thread an id through anvil's thread pools,
// because it cannot: `guarded()` is inside the library and every pool task goes
// through it.
//
// --- `traceparent` only. `tracestate` is refused ----------------------------
//
//   traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
//                vv trace-id, 16 bytes              span-id, 8 bytes  flags
//
// `traceparent` is FIXED at 55 bytes: four fields, hex, with three hyphens at
// known offsets. There is no list, no quoting, no escaping and nothing to size,
// so it parses with a length compare and a table lookup per nibble and
// allocates nothing. That is the same shape of parser as `auth::decode`, which
// is why it is acceptable on a path where a JSON parser was not
// (docs/04-access-control.md §4).
//
// `tracestate` is a comma-separated list of up to 32 vendor entries with its own
// grammar, optional whitespace and per-vendor key rules. It is the only
// variable-length part of the standard, nothing here would read a byte of it,
// and parsing it would buy a class of parser bug on the request path in exchange
// for nothing. It is dropped on the floor and NOT forwarded. An application that
// federates with a vendor needing it can carry it itself.
//
// --- the parsed value is binary, never the string ---------------------------
//
// `traceparent` is attacker-supplied on any request that reaches the process
// directly. Once it is `std::array<std::uint8_t, 16>` there is no injection
// surface left anywhere downstream — no log injection, no Redis key separator,
// no BSON field name. Carrying the `string_view` and formatting it later is how
// that surface comes back, so there is no accessor anywhere that hands out the
// header's own bytes.
//
// Being harmless to hold is not the same as being safe to believe, so nothing
// in this header reads a request. Whether an inbound header is believed at all,
// and from which peer, is a decision the request path makes — and the same
// decision `client_address.h` already makes about `X-Forwarded-For`, for the
// same reason: a value a client chooses is a value an attacker chooses.
//
// --- nothing is ever written back on a response -----------------------------
//
// anvil writes no trace header on any response, and the rule is absolute rather
// than conditional. `accesscontrol/stealth.h` enumerates `WWW-Authenticate`,
// `Set-Cookie` and `X-Request-Id` as tells for the same reason: any header
// present on a denied route and absent on an unmatched one is an existence
// oracle, and an echoed `traceparent` is exactly that header. "Echo it except on
// stealth routes" is a rule that survives until the first handler that sets it
// directly.
//
// Trace ids flow INBOUND, and outbound to services the deployment controls. They
// do not flow back to the client.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace anvil::http {

// The rendered length, fixed by the standard. A header of any other length is
// not a `traceparent` of any version: the version field selects a grammar, and
// every grammar so far is this one.
inline constexpr std::size_t kTraceparentChars = 55;

// Lowercase, because Drogon lowercases a field name when it stores one and this
// is the spelling the standard uses on the wire in both directions.
inline constexpr std::string_view kTraceparentHeader = "traceparent";

// Bit 0 of the flags byte. The remaining seven are reserved by the standard.
inline constexpr std::uint8_t kTraceFlagSampled = 0x01;

// 25 bytes, alignment 1, trivially copyable. Small enough to sit in a
// per-request struct and to be captured by value into a pool task without a
// thought.
struct TraceContext final {
    std::array<std::uint8_t, 16> trace_id;  // 16
    std::array<std::uint8_t, 8>  span_id;   //  8
    // The raw byte rather than a bool, because the other seven bits are reserved
    // and a receiver that drops them cannot round-trip a context it did not
    // mint. A future flag would otherwise be erased by every hop running this.
    std::uint8_t                 flags;     //  1

    // An all-zero trace-id is invalid per the standard, so the type has a
    // natural empty state and `TraceContext{}` means "no trace". That is why
    // there is no `has_trace` flag beside it anywhere: a second spelling of
    // absent is a second thing to keep in agreement.
    //
    // The reason the standard forbids it is operational rather than pedantic. A
    // client sending zeros would otherwise mint one trace id shared by every
    // request in the fleet, and the first person to debug that would be
    // debugging the aggregator.
    [[nodiscard]] constexpr bool present() const noexcept {
        for (const std::uint8_t byte : trace_id) {
            if (byte != 0) { return true; }
        }
        return false;
    }

    [[nodiscard]] constexpr bool sampled() const noexcept {
        return (flags & kTraceFlagSampled) != 0;
    }
};

static_assert(sizeof(TraceContext) == 25);
static_assert(alignof(TraceContext) == 1);
static_assert(std::is_trivially_copyable_v<TraceContext>);
static_assert(std::is_standard_layout_v<TraceContext>);

// `nullopt` for absent, malformed, or all-zero in either id. Never throws, never
// allocates, and reads at most `kTraceparentChars` bytes of its argument.
//
// `std::optional` here and a plain `TraceContext` in storage is deliberate. A
// parser's honest answer is "a value or nothing", and at this one boundary the
// three ways to have nothing — no header, a bad header, a zeroed header — are
// worth being one answer. Past it, `present()` is the question everybody asks,
// and an `optional` in storage would spend a byte on a discriminant the
// standard already forbids the value from needing.
//
// An UNKNOWN version parses as version 00 over the first 55 bytes, which is what
// the standard requires and what keeps a future version from reading as absent
// on every hop running today's code. Version `ff` is the one reserved as
// invalid, and it is refused.
[[nodiscard]] std::optional<TraceContext> parse_traceparent(std::string_view header) noexcept;

// The inverse, into 55 bytes of the caller's storage. Returned by value because
// it is 55 bytes and every caller wants it on the stack; there is no allocating
// form, for the same reason `format_request_id` has none.
//
// anvil ships no caller for this, and that is the line at the top of this file
// holding rather than an omission: the only correct places to write a
// `traceparent` are an outbound call to a service the deployment controls and an
// exporter, and both of those are the application's. It is here so that the
// application's is the same 55 bytes anvil parsed, rather than a second
// formatter that can disagree with the parser about a leading zero.
[[nodiscard]] std::array<char, kTraceparentChars> format_traceparent(
    const TraceContext& ctx) noexcept;

// --- who may set it ---------------------------------------------------------
//
// `traceparent` is a header, so on any request that reaches this process
// directly it is whatever the client typed. Believing one unconditionally is
// not the harmless default it looks like: a chosen trace-id collides with a
// real trace in the aggregator, a chosen `sampled` bit forces export on traffic
// the deployment decided not to sample, and a fresh id per request is unbounded
// cardinality in whatever is storing them. None of those is a hole in THIS
// process, which is why the question is easy to skip — every one of them is a
// hole in the thing the traces are for.
//
// So it is the same rule `client_address.h` already applies to
// `X-Forwarded-For`, and for the same reason: the header is consulted only when
// the PEER is a configured proxy. A deployment with no trusted proxies IS the
// edge, and the edge believes nothing.
//
// There is deliberately no third policy for "believe any client". A deployment
// whose callers are its own services has those callers behind the proxy list by
// construction, so the only thing the third policy would add is the ability to
// spell the mistake.
enum class TraceIngest : std::uint8_t {
    // The default. The header is not read, and this is what a deployment that
    // exports nothing pays: one comparison per request.
    Off,
    // Read it, but only from a peer in the trusted-proxy list.
    TrustedPeer,
};

// The decision, with no request in it. `peer_is_trusted` is the caller's answer
// to "did this arrive from a hop I configured", which is the one fact the policy
// turns on.
//
// Pure, so the truth table is testable without a socket — the same split
// `resolve_client_address` uses, and for the same reason: the rule that decides
// whether to believe an attacker-supplied header is the part worth being able to
// falsify cheaply.
[[nodiscard]] TraceContext ingest_traceparent(TraceIngest policy, bool peer_is_trusted,
                                              std::string_view header) noexcept;

// --- deferred work: a link, never a parent ----------------------------------
//
// Work that leaves this request and runs later — a queued job above all — is not
// part of the span that asked for it. A job is at-least-once, may be reclaimed
// after a lease expiry, and may run days after it was enqueued; a span covering
// the publish and Thursday's retry is not a trace, it is a lie with a flame
// graph on it. So the deferred work carries the enqueuing trace's ID as a LINK
// and runs under a fresh ROOT of its own.
//
// Getting this backwards — making the job a child of the enqueuing span — is the
// single most common mistake in traced job systems, which is why the shape is
// stated here rather than left to whoever writes the next queue.

// The link a unit of work deferred from here should carry: the current trace's
// id, and nothing at all unless that trace is SAMPLED.
//
// An unsampled trace produces no span, so a link to it names a trace with
// nothing in it. Carrying it only when sampled also means a link's PRESENCE says
// the enqueuer was sampled, which is what lets `root_linked_to` decide sampling
// without a flags byte having to travel beside the id.
[[nodiscard]] constexpr std::array<std::uint8_t, 16> trace_link_of(
    const TraceContext& ctx) noexcept {
    if (!ctx.present() || !ctx.sampled()) { return {}; }
    return ctx.trace_id;
}

[[nodiscard]] constexpr bool has_trace_link(
    const std::array<std::uint8_t, 16>& link) noexcept {
    for (const std::uint8_t byte : link) {
        if (byte != 0) { return true; }
    }
    return false;
}

// A fresh root for one execution of deferred work, or an absent context when
// there is no link.
//
// Absent-when-unlinked is what keeps this free for a deployment that does not
// trace: no link means no CSPRNG call and no context. It also means a job
// enqueued by an untraced path runs untraced, which is what "off by default"
// has to mean at this seam — the link is the only evidence reaching this code
// that anybody wanted a trace at all.
//
// Throws whatever `crypto::random_bytes` throws if the CSPRNG is unavailable,
// which is the same refusal to degrade to a weaker source that `mint_request_id`
// makes. A caller on a pool thread catches it; it must not escape one.
[[nodiscard]] TraceContext root_linked_to(
    const std::array<std::uint8_t, 16>& link);

// --- the ambient context ----------------------------------------------------
//
// One thread-local `TraceContext`: 25 bytes per thread, no allocation, no
// synchronisation, and no map keyed by a thread id that would need one.
//
// It is what makes the propagation invisible to a caller. `guarded()` wraps the
// body of every task posted to every pool (`core/thread_pools.h`), so it is the
// single place a context has to be captured and restored — and because it is
// already mandatory there, there is no way to post a task that escapes it.

// The context installed on THIS thread, or an absent one.
[[nodiscard]] TraceContext current_trace() noexcept;

// Installs a context for the lifetime of the object and restores the previous
// value on the way out, INCLUDING when the scope is left by an exception.
//
// A pool thread runs many tasks in its life. Without the restore, a task that
// carries no trace inherits the last one that did, and the correlation is then
// worse than absent: it is confidently wrong, and it points at a request that
// finished.
class TraceScope final {
public:
    explicit TraceScope(const TraceContext& ctx) noexcept;
    ~TraceScope();

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
    TraceScope(TraceScope&&) = delete;
    TraceScope& operator=(TraceScope&&) = delete;

private:
    const TraceContext previous_;
};

}  // namespace anvil::http
