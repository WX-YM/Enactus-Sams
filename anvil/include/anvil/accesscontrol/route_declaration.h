#pragma once

// The boot-time assertion that a route being registered is a route the access
// filter can find.
//
// It was a lambda copy-pasted into twelve controllers, and it checked
// `is_declared(pattern)` while the filter resolves `(pattern, method)`. A
// handler registered under a method the registry does not declare therefore
// booted CLEAN and then denied every request to itself, logging an error nobody
// is watching for. It fails closed, so this is availability rather than
// security — but it is the same shape as the cookie defect recorded in
// src/accesscontrol/access_filter.cc: a total outage that every test passed,
// because every test drove the layer underneath the one that was wrong.
//
// Twelve copies of a guard is twelve chances for one of them to be weakened, so
// there is now one.
//
// --- Why this is not in route_registry.h -----------------------------------
//
// It cannot go there. `accesscontrol/decision.h` includes the registry and is
// compiled without Drogon — the decision function is deliberately free of every
// framework type — while this guard has to speak Drogon's HttpMethod to be given
// the method at the call site. The METHOD-AWARE half of the fix — `is_declared`
// taking a RouteMethod — does live in the registry beside `policy_for`, which is
// what makes the two key on the same thing. Only the Drogon adaptor lives here,
// one layer up, where Drogon is already a dependency.

#include <stdexcept>
#include <string>
#include <string_view>

#include <drogon/HttpTypes.h>

#include <span>

#include "anvil/accesscontrol/route_registry.h"

namespace anvil::accesscontrol {

[[nodiscard]] constexpr accesscontrol::RouteMethod route_method_of(
    drogon::HttpMethod method) noexcept {
    switch (method) {
        case drogon::Get:    return accesscontrol::RouteMethod::Get;
        case drogon::Post:   return accesscontrol::RouteMethod::Post;
        case drogon::Put:    return accesscontrol::RouteMethod::Put;
        case drogon::Patch:  return accesscontrol::RouteMethod::Patch;
        case drogon::Delete: return accesscontrol::RouteMethod::Delete;
        default:             break;
    }
    // Head, Options and the WebDAV verbs. `Any` is deliberately NOT the answer:
    // an unmapped method must not silently satisfy the guard, so it resolves to
    // a value no entry declares and the registration throws.
    return accesscontrol::RouteMethod::Any;
}

// Drogon exposes no HttpMethod-to-string helper, and the message has to NAME the
// method or it sends the reader back to count enum positions.
[[nodiscard]] constexpr std::string_view method_name(drogon::HttpMethod method) noexcept {
    switch (method) {
        case drogon::Get:     return "GET";
        case drogon::Post:    return "POST";
        case drogon::Head:    return "HEAD";
        case drogon::Put:     return "PUT";
        case drogon::Delete:  return "DELETE";
        case drogon::Options: return "OPTIONS";
        case drogon::Patch:   return "PATCH";
        default:              return "an unsupported method";
    }
}

// Returns `pattern` so it can be passed straight to registerHandler, and throws
// if the registry has nothing that would answer for it under `method`.
//
// The METHOD is the whole point: `declared("/x")` used to pass merely because
// some entry mentioned "/x", even when the entry was for a different verb.
[[nodiscard]] inline std::string declared(std::span<const RoutePolicy> routes,
                                          std::string pattern, drogon::HttpMethod method) {
    if (!is_declared(routes, pattern, route_method_of(method))) {
        throw std::logic_error{"route '" + pattern + "' has no entry in the route table for " +
                               std::string{method_name(method)} +
                               " — the access filter resolves (pattern, method) and would "
                               "deny every request to this handler"};
    }
    return pattern;
}

}  // namespace anvil::accesscontrol
