#pragma once

// The authorization decision, as a pure function.
//
// Everything that decides whether a request may proceed lives here, with no
// Drogon type, no socket, and no I/O. Two reasons, and the second is the
// important one:
//
//   1. It is testable directly. The stealth-404 timing property is
//      a claim about this function's cost, and measuring it through an HTTP
//      stack measures the HTTP stack.
//   2. It cannot accidentally acquire I/O. A database call added to an
//      authorization path is how the timing oracle comes back, and it cannot be
//      added to a function that has nothing to call.
//
// Steps, in this order (docs/04-access-control.md §3):
//
//   1. Extract the access token from the cookie. Absent -> deny.
//   2. Verify the tag and the expiry with the kid-selected key. Fail -> deny.
//      The tag is checked BEFORE any field is trusted, including the expiry.
//   3. (held & required) == required.
//   4. Compare the token's perm_epoch against the cached authority. Not cached
//      -> ResolveEpoch, and the caller finishes off the event loop. EXCEPT on a
//      Stealth route that already failed step 3, which denies here instead: a
//      denial that costs a Redis GET while a nonexistent route costs nothing is
//      the existence oracle stealth exists to close (docs/04-access-control.md
//      §3). Answering early cannot change the outcome — resume_after_epoch
//      re-checks the same token's mask, so the authority only ever turns an
//      allow into a denial.
//   5. Build UserContext.
//
// Steps 1, 2, 3 and 5 allocate nothing. Step 4 allocates nothing.

#include <cstdint>
#include <string_view>

#include "anvil/accesscontrol/epoch_resolver.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/auth/token.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"

namespace anvil::accesscontrol {

enum class Step : std::uint8_t {
    Allow,
    Deny,
    // The token verified but the authority for its perm_epoch is not cached.
    // The caller must call EpochResolver::resolve_async off the event loop and
    // then finish with resume_after_epoch().
    ResolveEpoch,
};

struct Evaluation final {
    // Meaningful when step is Allow, and also when step is ResolveEpoch — the
    // claims are already authenticated at that point, so the caller carries the
    // context forward rather than re-verifying the token.
    UserContext ctx;
    // The TRUE code, even when the caller is about to answer with a stealth
    // 404. This is what the audit row records.
    ErrorCode   code;
    Step        step;
    // The epoch the token carried, so the caller can compare it against a
    // resolved authority without re-decoding.
    std::uint64_t token_epoch;
};

// `access_token` is the value of the access cookie, already separated from
// whatever carried it. Empty when the caller had none. `now_unix` is seconds
// since the epoch from the LOCAL clock — never a client-supplied timestamp
// (docs/00-architecture.md §7 invariant 3).
//
// This is what the filter calls, and it takes a TOKEN rather than a header
// because Drogon never hands the filter a header to read. `HttpRequestImpl`
// special-cases `Cookie` while parsing: it splits the value into its own map and
// does not store the field, so `getHeader("cookie")` answers an empty string on
// every request that ever carried a cookie. Reading the raw header therefore
// denied every authenticated and every stealth route in the application, and no
// test saw it because every test calls this function directly.
[[nodiscard]] Evaluation evaluate_token(std::string_view access_token, const RoutePolicy& policy,
                                        const auth::TokenKeys& keys, const EpochResolver& epochs,
                                        std::int64_t now_unix) noexcept;

// The same decision, from a raw `Cookie:` header. Nothing in the server can call
// this — see above — so it exists for callers that hold a real header, and for
// the tests that pin read_cookie() and the decision together.
[[nodiscard]] Evaluation evaluate(std::string_view cookie_header, const RoutePolicy& policy,
                                  const auth::TokenKeys& keys, const EpochResolver& epochs,
                                  std::int64_t now_unix) noexcept;

// Finishes an evaluation that returned ResolveEpoch, once the authority is
// known. Split out so the asynchronous continuation cannot drift from the
// synchronous path: the permission check below is the same code either way.
[[nodiscard]] Evaluation resume_after_epoch(const Evaluation& pending, const RoutePolicy& policy,
                                            std::uint64_t authoritative_epoch) noexcept;

// --- an authorization that outlives the request that made it ----------------
//
// Everything above decides a REQUEST. A long-lived connection is decided once
// and then lives for hours, and two mechanisms that work because requests are
// short stop working the moment the connection outlives the decision:
//
//   * The epoch. `EpochCache`'s TTL IS the revocation latency
//     (docs/04-access-control.md §5), and for an open connection it is
//     meaningless — the permission was checked at the handshake and nothing
//     checks it again. A staff member dismissed at 09:00 keeps their live feed
//     until they close the laptop.
//   * The token's expiry. A connection that outlives it is a session with no
//     end.
//
// This is not a WebSocket concern waiting on a WebSocket. `notifications/sse.h`
// already ships a registry of connections that are held open across the same
// gap, and anvil owns neither its HTTP nor its authorization — so the re-check
// has to be a function an application can call on whatever it is holding open.
//
// It lives HERE, beside `evaluate_token`, for the reason `resume_after_epoch`
// does: two authorization paths that can disagree will. The verdicts below map
// onto `Step` one for one, and the case that matters is the table driving both
// against the same context, policy and epoch.

enum class ConnectionVerdict : std::uint8_t {
    // Still authorized. Nothing to do until the next sweep.
    Keep,
    // No longer authorized, or the credential that opened it has expired. The
    // connection closes and the client reconnects — which is SSE's "a dropped
    // client reconnects and reads its inbox" applied unchanged, and is a
    // condition every client of one of these already handles.
    Close,
    // The authority for this context's epoch is not cached. The caller resolves
    // off the event loop — in a BATCH for every connection due at once, never
    // one `resolve_async` per connection — and finishes with
    // `resume_connection_after_epoch`. A resolve that FAILS closes the
    // connection: an unreadable revocation channel is not permission to skip
    // revocation.
    ResolveEpoch,
};

// The re-check. `expires_at_unix` is the `expires_at` of the token the handshake
// verified, kept by the connection because `UserContext` deliberately does not
// carry it — that field would push the context to 72 bytes and across a second
// cache line, and no request-path reader wants it.
//
// There is no `token_epoch` parameter, although the design that produced this
// had one: `ctx.perm_epoch` IS the token's epoch, `auth::to_context` is what
// puts it there, and `evaluate_token` reads the same value out of both. A second
// parameter carrying it would be a second spelling of one number, and the two
// would be passed inconsistently by the first caller that had only one of them.
//
// The expiry is checked FIRST and for every access class, including `Public`.
// Agreement with `evaluate_token` is not "both allow" — it is "both produce the
// same authority". A public route with an expired token evaluates to an EMPTY
// context, so a connection still holding a populated one past that instant
// disagrees with what a fresh request would build, whatever either of them
// answers about access.
//
// Pure, allocation-free and safe on an event-loop thread: `check_cached` is the
// only thing it consults and that is a cache read.
[[nodiscard]] ConnectionVerdict still_authorized(const UserContext& ctx,
                                                 std::uint32_t expires_at_unix,
                                                 const RoutePolicy& policy,
                                                 const EpochResolver& epochs,
                                                 std::int64_t now_unix) noexcept;

// Finishes a `ResolveEpoch`, once the authority is known. The mirror of
// `resume_after_epoch`, and split out for the same reason: the permission check
// is the same code on both the cached and the resolved path.
[[nodiscard]] ConnectionVerdict resume_connection_after_epoch(
    const UserContext& ctx, const RoutePolicy& policy,
    std::uint64_t authoritative_epoch) noexcept;

// The short-circuit, as one expression.
//
// Superadmin is an explicit utype check and NOT an all-ones mask, so that
// "holds every permission" and "is superadmin" stay distinguishable in the
// audit log, and so no bit-fiddling accident can synthesise superadmin
// (docs/04-access-control.md §2).
//
// It is a named function rather than an inline comparison because it now has a
// SECOND reader: append_holder_authority tells a client whether this holder is
// one, and a client that learned it from a different test than the filter uses
// would render affordances the server then denies — or, the way it actually
// failed, hide every affordance from the one account that reaches them all.
[[nodiscard]] constexpr bool is_superadmin(UserType type) noexcept {
    return type == UserType::SuperAdmin;
}

// Whether `held` satisfies `required`, including the superadmin short-circuit.
[[nodiscard]] constexpr bool satisfies(const PermSet& held, UserType type,
                                       const PermSet& required) noexcept {
    if (is_superadmin(type)) { return true; }
    return held.contains_all(required);
}

}  // namespace anvil::accesscontrol
