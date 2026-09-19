#pragma once

// The byte-identical 404 (docs/04-access-control.md §4).
//
// Stealth is not "return 404 instead of 403". It is the claim that a DENIED
// admin route and a NONEXISTENT one are indistinguishable, and that claim is
// defeated by any observable difference at all:
//
//   Timing         handled structurally, in decision.h: the deny path is a
//                  token verify and a bitset AND, with no database and no Redis
//                  on the hit path. Jitter is not a fix — averaging removes it.
//   Response bytes handled here. Drogon's default 404 and Nginx's default 404
//                  differ in body, Content-Type, Content-Length and header
//                  order. One response object serves all three cases so they
//                  cannot drift.
//   Headers        no WWW-Authenticate, no Set-Cookie, no X-Request-Id. Any
//                  header the real 404 lacks is a tell.
//
// The SAME HttpResponsePtr is used for the framework's unmatched-route page and
// for every stealth drop, so "byte-identical" is a property of construction
// rather than of two code paths agreeing. Drogon serialises a response into the
// socket buffer without mutating it, which is what makes sharing one safe — and
// what setCustom404Page relies on already.
//
// Deployment obligation: Nginx's `error_page 404` must serve exactly these
// bytes. A 404 from Nginx that differs from a 404 from the application
// reintroduces the distinction at the edge, where this code cannot see it.

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace anvil::accesscontrol {

// Process-wide, built once. Safe to share across threads and across requests.
[[nodiscard]] const drogon::HttpResponsePtr& not_found_response();

// Whether this request is a connection upgrade.
//
// Keyed on `Sec-WebSocket-Key`, because that is what Drogon's own router keys on
// — `routeWs` answers NotFound without it, whatever the `Upgrade` header says.
// Asking the same question the router asks is what keeps a refusal and a route
// from disagreeing about what kind of request this is.
[[nodiscard]] bool is_upgrade_request(const drogon::HttpRequestPtr& req) noexcept;

// The same 404 as a FRESH object, for the one refusal the framework rewrites.
//
// `accesscontrol/upgrade_gate.h` answers a refused upgrade from a Drogon sync
// advice, which is the only point in the pipeline before the framework builds
// the WebSocket connection object whose destructor writes a close frame. The
// framework treats a response returned from there as its own to finish:
// `passSyncAdvices` calls `setVersion` and `setCloseConnection` on it before it
// is buffered. Handing it the object every HTTP 404 shares would be a write to
// process-wide state from an event-loop thread while other loops are serialising
// the same object — the same class of defect as putting a per-request value on a
// shared response (http/request_id.h), and a data race besides.
//
// So the gate gets a copy, and every other 404 in this library keeps sharing
// one. The copy costs an allocation on a path that is refusing a request.
//
// --- what used to be here, and why it is worth knowing it was wrong ---------
//
// This function used to also call `setCloseConnection(true)`, on the theory that
// a connection already marked closing makes `connected()` false by the time
// `~WebSocketConnectionImpl` runs, so the frame is never written. The measurement
// that supported it read the socket ONCE, and a single `recv` sees the frame only
// when TCP coalesces it into the same segment as the response — so the same
// server behaviour measured 0 of 40 times and 12 of 40 times depending on
// segmentation, and the difference was read as a property of the two paths.
//
// Reading until the peer goes quiet (`raw_handshake_all` in
// tests/listener_fixture.h) says what actually happens: the flag changes nothing,
// the frame follows every refusal a FILTER makes, and it follows none the gate
// makes — because there is no connection object on that path to destroy.
[[nodiscard]] drogon::HttpResponsePtr not_found_upgrade_response();

// Installs the same object as the framework's 404 page, so an unmatched route
// answers with the bytes a stealth drop answers with. Called once from main(),
// before the first listener starts.
void install_as_framework_404();

}  // namespace anvil::accesscontrol
