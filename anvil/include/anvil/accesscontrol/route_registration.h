#pragma once

// Registering a handler WITH the filter its policy requires.
//
// --- The hole this closes ---------------------------------------------------
//
// The route registry guarantees that every registered pattern DECLARES a policy.
// It does not, and cannot, guarantee that the filter which ENFORCES that policy
// was attached to the handler.
//
// Drogon resolves filters by class NAME in the `registerHandler` argument list,
// so attaching one is a string an application has to remember to type. Nothing
// can check that it did: `drogon::app().getHandlersInfo()` returns
// `std::tuple<std::string, HttpMethod, std::string>` — pattern, method and
// description — and carries no filter information at all. `declared()` throws
// for a pattern the table does not name, and a boot-time sweep comparing the
// framework's routes against the application's table compares those same two
// sets; an unguarded `Stealth` route passes both, because declaring a pattern is
// exactly what an unguarded route also does.
//
// In the plainest terms: **a `Stealth` route registered without the filter is a
// public route, and every check in this library and in an application built on
// it reports green.** The first `/admin` anybody writes is where this happens.
//
// So the unguarded spelling is made unreachable rather than discouraged. The
// constraint list is built HERE, from the policy, and a caller never types the
// filter name — the same move `append_sanitized` makes for raw markup
// (docs/19-server-side-rendering.md §3): the dangerous call becomes one the API
// does not offer.
//
// Reported by the first application built on this library, which found it while
// reasoning about a route it had not yet written.
//
// --- Why this is not in route_declaration.h ---------------------------------
//
// That header needs only `<drogon/HttpTypes.h>`, and it is included by every
// controller that registers anything. Registering needs `HttpAppFramework.h`,
// which is the heaviest header Drogon publishes. Keeping them apart means a
// caller that only wants the boot guard does not pay for the framework
// (CLAUDE.md §8, explicit includes).

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/HttpConstraint.h>

#include <span>

#include "anvil/accesscontrol/route_declaration.h"
#include "anvil/accesscontrol/upgrade_filter.h"
#include "anvil/accesscontrol/upgrade_gate.h"
#include "anvil/accesscontrol/route_registry.h"

namespace anvil::accesscontrol {

// The name Drogon resolves the filter by.
//
// It is a string because DrObject's object map is keyed by one, and it lives
// here because every application used to type it at every call site. A typo in
// it is not a compile error — it is a route that registers without its filter,
// which is the defect this header exists to make unreachable.
inline constexpr std::string_view kAccessFilterName = "anvil::accesscontrol::AccessFilter";

// Whether a Public route wants the filter anyway.
//
// Public is the only class the filter does not ENFORCE — `evaluate_token`
// allows an absent or invalid token on it and never denies — so attaching it
// there is a question about CONTEXT rather than about authority, and it is
// genuinely the application's to answer.
enum class PublicContext : std::uint8_t {
    // The route does not read `user_context()`. No filter, so a request costs no
    // token decode at all. The default, because it is the cheaper answer and
    // because a handler that silently gained a context it does not read is a
    // handler nobody notices paying for one.
    Omit,
    // The route renders differently for a signed-in visitor. The filter attaches
    // a context when a valid token happens to be present and allows the request
    // either way. The epoch is deliberately not consulted on a public route, so
    // this costs a decode and never a round trip (docs/04-access-control.md §3).
    Attach,
};

// Whether the filter ENFORCES this class, as opposed to merely decorating it.
//
// Every class but Public is denied by the filter and by nothing else, so for
// every class but Public a missing filter is an authorization bypass. Stated as
// a predicate rather than inline so the one place that decides it is the one
// place a reader has to check.
[[nodiscard]] constexpr bool filter_enforces(RouteAccess access) noexcept {
    return access != RouteAccess::Public;
}

// The constraint list a registration needs.
//
// Exposed rather than buried inside `register_route` because it is the only way
// this decision can be TESTED: Drogon reports no filter through
// `getHandlersInfo()`, so a test that registered a route could never read back
// whether the right thing was attached. Nothing can check it after the fact, so
// it is checked before the fact.
[[nodiscard]] inline std::vector<drogon::internal::HttpConstraint> route_constraints(
    const RoutePolicy& policy, drogon::HttpMethod method,
    PublicContext public_context = PublicContext::Omit) {
    std::vector<drogon::internal::HttpConstraint> constraints;
    constraints.reserve(2);
    constraints.emplace_back(method);
    if (filter_enforces(policy.access) || public_context == PublicContext::Attach) {
        constraints.emplace_back(std::string{kAccessFilterName});
    }
    return constraints;
}

// Register `handler` for `pattern` under `method`, with the filter its policy
// requires.
//
// Throws the same way `declared()` does when the table names no entry that would
// answer for this registration — the two failures are one failure, so they read
// the same and are raised from one place.
//
// The pattern is taken by value and moved: Drogon stores it, and every caller
// has a `std::string_view` constant to build one from.
template <typename FUNCTION>
void register_route(std::span<const RoutePolicy> routes, std::string pattern,
                    drogon::HttpMethod method, FUNCTION&& handler,
                    PublicContext public_context = PublicContext::Omit) {
    const RoutePolicy* policy = policy_for(routes, pattern, route_method_of(method));
    if (policy == nullptr) {
        throw std::logic_error{"route '" + pattern + "' has no entry in the route table for " +
                               std::string{method_name(method)} +
                               " — the access filter resolves (pattern, method) and would "
                               "deny every request to this handler"};
    }
    std::vector<drogon::internal::HttpConstraint> constraints =
        route_constraints(*policy, method, public_context);
    drogon::app().registerHandler(std::move(pattern), std::forward<FUNCTION>(handler),
                                  constraints);
}

// Whether a pattern survives Drogon's WebSocket router unchanged.
//
// `registerWebSocketController` keys `wsCtrlMap_` by the LOWERCASED path, and
// `routeWs` reports that key as the matched pattern. So a route declared with an
// uppercase letter is registered under one spelling and reported under another,
// `policy_for` compares the reported one against the table byte for byte, finds
// nothing, and the access filter takes its `policy == nullptr` branch — which is
// closed, silent, and indistinguishable from the feature not being wired up.
//
// This is NOT how anvil's HTTP routes behave and the difference is easy to
// assume away. `register_route` goes through `registerHandler`, which stores the
// pattern as given and reports it back unchanged; only the WebSocket map lowers.
// So the rule is stated here, where it is true, rather than as a claim about
// route tables in general.
[[nodiscard]] constexpr bool upgrade_pattern_survives_routing(std::string_view pattern) noexcept {
    for (const char c : pattern) {
        if (c >= 'A' && c <= 'Z') { return false; }
    }
    return true;
}

// Register a WebSocket controller for `pattern`, with the filters its policy
// requires and the CSRF check an upgrade cannot do without.
//
// `controller_name` is the class name Drogon resolves the controller by. Pass
// `YourSocket::classTypeName()` rather than a literal: it cannot go stale under
// a rename, and it ODR-uses the static member whose constructor is what puts the
// class in Drogon's object map in the first place.
//
// **The controller must not also declare its own paths.** `WS_PATH_ADD`
// registers the path with whatever constraints the macro carried, which are not
// these — and Drogon's registration merges into `wsCtrlMap_[path]`, so a
// controller that does both ends up with a binder that has no filters on it and
// nothing anywhere reports the fact. Derive from
// `drogon::WebSocketController<T, false>` so the framework does not register it
// either, and give the class an empty `static void initPathRouting() {}`:
// Drogon's `pathRegistrator` guards its call with a runtime `if` rather than an
// `if constexpr`, so the member has to exist even where it is never called.
//
// An upgrade is a GET, always, so the policy is looked up under `Get` and there
// is no method parameter to pass inconsistently. It throws the same way
// `register_route` does when the table names no entry, and for the same reason:
// a route the filter would deny every request to is a registration bug, not a
// runtime condition.
//
// Two filters, in this order: the origin check first, because it is a header
// compare with no I/O and refusing there costs nothing and discloses nothing
// about whether the caller was signed in; then the access filter, under the same
// rule `route_constraints` applies to every other route.
inline void register_websocket_route(std::span<const RoutePolicy> routes, std::string pattern,
                                     const std::string& controller_name,
                                     PublicContext public_context = PublicContext::Omit) {
    if (!upgrade_pattern_survives_routing(pattern)) {
        throw std::logic_error{
            "websocket route '" + pattern +
            "' contains an uppercase letter — Drogon registers and reports a "
            "websocket path lowercased, so the access filter would look up a "
            "pattern this table does not contain and deny every upgrade"};
    }

    const RoutePolicy* policy = policy_for(routes, pattern, RouteMethod::Get);
    if (policy == nullptr) {
        throw std::logic_error{"websocket route '" + pattern +
                               "' has no entry in the route table for GET — an upgrade is a "
                               "GET, and the access filter resolves (pattern, method)"};
    }

    // Before the registration, because both of these are things a handshake to
    // this pattern needs in place before the first one arrives, and because
    // `register_upgrade_path` is the second half of the boot check above: a
    // pattern the gate does not hold is a pattern every upgrade is refused on,
    // whatever the router thinks.
    //
    // `policy->pattern` and not the local `pattern`, and that is a lifetime
    // decision rather than a style one. The gate STORES the view it is given, so
    // it has to be the one whose storage is the application's `constexpr` table
    // — the local is moved into Drogon two statements below, and a view into it
    // would dangle on every handshake this route ever sees.
    register_upgrade_path(policy->pattern);
    install_upgrade_gate();

    std::vector<drogon::internal::HttpConstraint> constraints =
        route_constraints(*policy, drogon::Get, public_context);
    // In front of the access filter, and in front of the method constraint's
    // position in the list only incidentally — Drogon runs the filters in the
    // order they appear, so this insert is the ordering.
    // The TYPE's own name, never a string constant, and this is load-bearing
    // twice over. Drogon resolves a filter by class name out of its object map,
    // and `DrObject<T>::alloc_` — the static member whose constructor registers
    // the class — is instantiated only when something ODR-uses it. A name typed
    // as a literal here would leave nothing referencing the class, so the
    // filter's object file would not even be pulled out of the static library,
    // and Drogon's answer to a middleware it cannot find is a log line and a
    // chain that runs WITHOUT it: the route registers, the handshake succeeds,
    // and the CSRF check silently is not there. Naming the type makes that
    // unrepresentable, and it cannot go stale under a rename.
    constraints.insert(
        constraints.begin() + 1,
        drogon::internal::HttpConstraint{UpgradeOriginFilter::classTypeName()});

    drogon::app().registerWebSocketController(std::move(pattern), controller_name, constraints);
}

}  // namespace anvil::accesscontrol
