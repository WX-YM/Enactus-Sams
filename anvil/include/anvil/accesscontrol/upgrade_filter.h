#pragma once

// The CSRF check on a connection upgrade, as a filter anvil attaches itself.
//
// `http/origin_check.h` explains why the method cannot decide this: a WebSocket
// handshake is an HTTP GET, same-origin policy does not constrain WebSockets,
// and the browser attaches the cookie anyway — so on an upgrade the origin check
// is the ONLY CSRF defence there is rather than defence in depth behind "GET
// does not mutate".
//
// That argument produced `OriginRequirement::Always`, and `Always` on its own
// changes nothing: it is a value nobody passes. This filter is the caller, and
// `register_websocket_route` is what attaches it, for exactly the reason
// `route_registration.h` builds the constraint list rather than letting an
// application type a filter name — a control an application has to remember to
// wire is a control that is missing on the route that mattered.
//
// --- it runs BEFORE the access filter ---------------------------------------
//
// Pipeline stage 2, before authentication (docs/00-architecture.md §4). It is a
// header compare with no I/O, so refusing here costs nothing, and a
// cross-origin handshake is refused without the process ever deciding whether
// the caller was signed in — which is one fewer thing the refusal can disclose.
//
// --- and it answers the way the access filter answers -----------------------
//
// A stealth route gets the byte-identical 404 from `stealth.h`, with no
// `Sec-WebSocket-*` header on it. A header present on a refused upgrade and
// absent on an unmatched route is the same existence oracle as
// `WWW-Authenticate` on a denied request, and the handshake response is where
// one would be easiest to add by accident. Every other class gets its real code,
// because its path is not a secret.

#include <drogon/HttpFilter.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>

namespace anvil::accesscontrol {

class UpgradeOriginFilter final : public drogon::HttpFilter<UpgradeOriginFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override;
};

}  // namespace anvil::accesscontrol
