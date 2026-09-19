#pragma once

// The refusal that must leave nothing behind it, taken before the framework has
// built the thing that would leave something.
//
// --- the four bytes ---------------------------------------------------------
//
// `WebSocketConnectionImpl`'s destructor calls `shutdown()`, which writes a
// WebSocket CLOSE FRAME — `88 02 03 e8`, opcode 8 and code 1000 — onto a socket
// that is still connected. Every refusal a FILTER can make arrives after that
// object exists, so a refused handshake went out as the byte-identical stealth
// 404 followed by four bytes, arriving after the response rather than in it,
// where no assertion about headers or bodies could see them
// (docs/04-access-control.md §8.3).
//
// `stealth.h` recorded that as a residual this library could not close, on the
// premise that the connection object is constructed before anything in anvil
// runs. **The premise was wrong by one step.** `HttpServer::onRequests` reads:
//
//     if (requestParser->firstReq() && requests.size() == 1 && isWebSocket(req))
//     {
//         if (passSyncAdvices(req, requestParser, false, false))
//         {
//             auto wsConn = std::make_shared<WebSocketConnectionImpl>(conn);
//             ...
//
// A SYNC ADVICE that returns a response short-circuits that branch, and the
// connection object is never constructed — no destructor, no frame, no race.
// It is the only such point: every pre-routing advice, every middleware and
// every filter runs after the `make_shared` above, and Drogon's own
// unmatched-upgrade path is past it too.
//
// So the decision moves to the one place where it can be made without leaving a
// trace, and Drogon's WebSocket router never sees a handshake this library is
// going to refuse.
//
// --- what the gate decides, and what it deliberately does not ---------------
//
// Only the refusals whose entire purpose is to be indistinguishable:
//
//   * an upgrade to a path that is not a registered WebSocket route — which
//     includes every ordinary HTTP route, and is the baseline the stealth claim
//     is measured against;
//   * every refusal on a `Stealth` upgrade route: the origin check, and any
//     denial `evaluate_token` can reach without I/O.
//
// A refusal on any other class keeps going through the filters
// (`upgrade_filter.h`, `access_filter.h`), with its real status code and its
// real request id. Two reasons, and the second is the load-bearing one. A
// trailing close frame on a route whose existence is not a secret discloses
// nothing — `upgrade_filter.cc` already said so. And a sync advice runs BEFORE
// `install_request_scope()`'s pre-routing advice has minted a request id and its
// response never reaches the pre-sending advice that emits one, so a 401 or a
// 403 answered from here would lose the id that joins a user saying "I cannot
// get in" to the row that says why. The stealth 404 must carry no id — that is
// the point of it — so for that class the gate loses nothing at all.
//
// --- it is not a second authorization decision ------------------------------
//
// It calls `evaluate_token`, the same pure function the filter calls, with the
// same policy out of the same table, the same keys and the same resolver. It
// answers only on `Step::Deny`, which is the branch the filter would take a
// moment later with the identical `Evaluation`. Nothing here can disagree with
// the filter, because there is nothing here to disagree WITH — the gate answers
// the same decision earlier, at the one point where the answer costs no frame.
//
// `Step::ResolveEpoch` falls through to the filter, because resolving an epoch
// touches Redis and a sync advice cannot wait. That is the one refusal on a
// stealth route the gate does not take, and it is not an oracle: reaching it
// means the caller presented a validly signed, unexpired token that CARRIES the
// route's permission bit, and a holder of the bit already knows the route is
// there.
//
// The cost is one extra token verify on a handshake that is going to be
// accepted — once per connection, on the only request that connection makes.
//
// --- what it costs every other request --------------------------------------
//
// A sync advice runs on every request the process handles, so the gate is on the
// hot path whether or not an upgrade ever arrives. It answers with a method
// compare for anything that is not a `GET`, and with one lookup in the request's
// header map for a `GET` that is not a handshake — the same lookup Drogon's own
// `isWebSocket` already makes per request, so the addition is a second one and
// nothing else. Everything past that point happens only on a handshake.
//
// --- and it is deny-by-default, which the router was not ---------------------
//
// An upgrade only passes for a path `register_websocket_route` registered. A
// WebSocket controller wired straight into Drogon — `WS_PATH_ADD`, or
// `registerWebSocketController` by hand — is a route with none of anvil's
// filters on it, which `route_registration.h` calls "a public route where every
// check reports green". Such a route is now unreachable rather than unguarded,
// and it fails the way ENGINEERING_RULES.md §5 asks: closed, and identically to a path that
// was never there.

#include <cstddef>
#include <string_view>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace anvil::accesscontrol {

// How many WebSocket routes one process may register.
//
// A fixed array rather than a growing container (ENGINEERING_RULES.md §2.1): the registry
// is scanned on every handshake, sixteen `string_view` compares fit in a couple
// of cache lines, and a bound that fails at BOOT with the number in the message
// is the shape this library uses everywhere an application supplies a table.
// Sixteen because a process with more than a handful of upgrade endpoints is
// multiplexing over one of them instead.
inline constexpr std::size_t kMaxUpgradePaths = 16;

// Records a pattern the gate will let an upgrade through to.
//
// It takes a `string_view` and STORES it, which is safe for exactly one kind of
// argument: the `pattern` field of a `RoutePolicy`, whose storage is the
// application's `constexpr` route table and therefore outlives the process's
// listeners. `register_websocket_route` passes `policy->pattern` for that
// reason, and passing a temporary here is a dangling view on every later
// handshake.
//
// Throws `std::logic_error` past `kMaxUpgradePaths`, and on a pattern that is
// not lowercase — Drogon lowercases both the key it registers and the path it
// looks up, so a registry entry that is not lowercase is an entry no handshake
// can ever match.
void register_upgrade_path(std::string_view pattern);

// The registered pattern this request path resolves to, or an empty view.
//
// ASCII case-insensitive, because `HttpControllersRouter::routeWs` lowercases
// `req->path()` before looking it up. Matching any other way would let the gate
// and the router disagree about whether a route exists, which is the one
// disagreement that reintroduces everything above.
[[nodiscard]] std::string_view upgrade_path_for(std::string_view request_path) noexcept;

[[nodiscard]] inline bool upgrade_path_registered(std::string_view request_path) noexcept {
    return !upgrade_path_for(request_path).empty();
}

// Installs the gate as a sync advice. Idempotent, and called by
// `register_websocket_route` rather than by an application.
//
// Registering the route IS installing the gate, for the reason
// `route_registration.h` builds the constraint list instead of letting a caller
// type a filter name: a control an application has to remember to wire is a
// control that is missing on the route that mattered. There is no route this can
// be forgotten on, because the only way to get a route is to call the function
// that installs it.
void install_upgrade_gate();

}  // namespace anvil::accesscontrol
