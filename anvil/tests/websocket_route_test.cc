// Does Drogon give a WebSocket route a matched path pattern?
//
// `proposals/websocket-upgrade.md` could not answer this from the constraint
// signature, and the answer decides whether the whole design is buildable:
// `AccessFilter::doFilter` resolves a policy by `req->getMatchedPathPattern()`
// and a pattern the router never set is an empty string, which finds no policy,
// which takes the `policy == nullptr` branch. That branch is CLOSED and silent —
// so a WebSocket route would be denied with a stealth 404 and a log line about a
// missing registry entry, which is indistinguishable from the feature not being
// wired up at all. A design built on top of an assumption with that failure mode
// is a design that fails at the end.
//
// Reading Drogon 1.9.13 says it is set, in `HttpControllersRouter::routeWs`, for
// both the exact-match map and the regex vector. This file is here because
// reading a dependency's source answers the question for the version on this
// machine and for no other. anvil does not pin Drogon; a consuming application
// picks it, and the vcpkg baseline moves.
//
// So the property is asserted over a real handshake on a real socket, through a
// filter that records what it was given. It is deliberately NOT anvil's filter:
// what is under test is the ROUTER, and driving it through a filter that denies
// for its own reasons would answer a different question.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <string>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpFilter.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>

#include "anvil/accesscontrol/upgrade_gate.h"

#include "listener_fixture.h"

using namespace anvil::testfixture;  // NOLINT(google-build-using-namespace)

namespace anvil {
namespace {

// What the filter chain saw, and whether it ran at all. Two facts, because an
// empty pattern and a filter that never ran produce the same empty string.
std::string g_seen_pattern;
bool        g_filter_ran = false;

}  // namespace

// Namespace-scope and DrObject-derived, because Drogon resolves a filter by
// class name out of its object map and a lambda cannot be in one.
class PatternRecordingFilter final : public drogon::HttpFilter<PatternRecordingFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&&,
                  drogon::FilterChainCallback&& chain) override {
        g_filter_ran = true;
        g_seen_pattern = std::string{req->getMatchedPathPattern()};
        chain();
    }
};

class EchoSocket final : public drogon::WebSocketController<EchoSocket> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr&, std::string&&,
                          const drogon::WebSocketMessageType&) override {}
    void handleNewConnection(const drogon::HttpRequestPtr&,
                             const drogon::WebSocketConnectionPtr&) override {}
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override {}

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/echo", "anvil::PatternRecordingFilter");
    WS_PATH_LIST_END
};

namespace {

// The name the filter is resolved by, taken from the type rather than typed.
//
// `DrObject<T>::alloc_` is a static data member of a class template, so it is
// instantiated only when something ODR-uses it — and the constructor of that
// member is what puts the class in Drogon's object map. A filter nothing touches
// is therefore a filter that is never registered, and Drogon's answer is
// `middleware ... not found` on a log line and a chain that runs without it.
// That is the same silent-pass shape this file exists to rule out, met on the
// way to ruling it out.
[[nodiscard]] const std::string& recording_filter_name() {
    return PatternRecordingFilter::classTypeName();
}

void install_websocket_route() {
    // `WS_PATH_ADD` above IS the registration: it defines `initPathRouting`,
    // which calls `registerSelf__` with the path and the constraint list.
    // Calling it here rather than leaving it to the framework's own sweep keeps
    // the route installed at the same point in boot as every other route in this
    // binary — and the filter has to exist in the map before the route names it.
    (void)recording_filter_name();
    EchoSocket::initPathRouting();
    // And the line this route needs BECAUSE it is registered the raw way.
    //
    // `accesscontrol/upgrade_gate.h` refuses an upgrade to any path
    // `register_websocket_route` did not register, before Drogon's router is
    // consulted at all — which is the point of it: a WebSocket controller wired
    // straight into the framework carries none of anvil's filters, and a route
    // like that is now unreachable rather than unguarded. This file registers one
    // deliberately, because what is under test is the ROUTER, so it opts the path
    // in by hand. An application never writes this line; it calls
    // `register_websocket_route`, which writes it.
    //
    // A literal, so the view the gate stores points at static storage.
    anvil::accesscontrol::register_upgrade_path("/ws/echo");
}

const RouteRegistrar kRegistrar{&install_websocket_route};

TEST(WebSocketRouting, TheFilterNameInTheRouteIsTheOneDrogonResolves) {
    // Typed in `WS_PATH_ADD` and derived from the type here, so a rename that
    // moved one and not the other fails as a comparison rather than as a route
    // that quietly registered with no filter on it.
    EXPECT_EQ(recording_filter_name(), "anvil::PatternRecordingFilter");
}

TEST(WebSocketRouting, AnUpgradeCarriesTheMatchedPatternIntoTheFilterChain) {
    g_filter_ran = false;
    g_seen_pattern.clear();

    const std::string status{status_line(raw_handshake("/ws/echo"))};
    ASSERT_FALSE(status.empty()) << "the handshake got no response at all";

    ASSERT_TRUE(g_filter_ran)
        << "the filter chain did not run for a WebSocket upgrade, so a policy "
           "could not be enforced on one at all";
    EXPECT_FALSE(g_seen_pattern.empty())
        << "the router left the matched pattern empty, so AccessFilter would "
           "find no policy and deny every upgrade with a stealth 404 — the "
           "failure the design had to rule out before being built on";
    EXPECT_EQ(g_seen_pattern, "/ws/echo");

    // And the upgrade was accepted, which is what says the filter ran as part of
    // routing the handshake rather than as part of refusing it.
    EXPECT_NE(status.find("101"), std::string::npos) << status;
}

TEST(WebSocketRouting, ThePatternIsTheLowercasedRegisteredPath) {
    // The spelling matters as much as the presence. `policy_for` compares the
    // pattern byte for byte against the application's table, so a router that
    // reported a different spelling than the one a route was declared under
    // would find no policy — the same closed, silent branch as an empty one.
    //
    // Drogon lowercases: `wsCtrlMap_` is keyed by the lowered path and the
    // pattern it reports is that key.
    //
    // This is specific to WebSocket routes, and assuming otherwise is the easy
    // mistake. `register_route` goes through `registerHandler`, which stores the
    // pattern as given and reports it back unchanged — only the WebSocket map
    // lowers. So an uppercase HTTP pattern is fine and an uppercase WebSocket
    // pattern denies every upgrade, which is why `register_websocket_route`
    // refuses one and this case is what that refusal is built on.
    g_filter_ran = false;
    g_seen_pattern.clear();

    const std::string status{status_line(raw_handshake("/WS/Echo"))};
    ASSERT_FALSE(status.empty());
    ASSERT_TRUE(g_filter_ran) << "a differently-cased path did not reach the route at all";
    EXPECT_EQ(g_seen_pattern, "/ws/echo");
}

TEST(WebSocketRouting, AGetWithNoWebSocketKeyIsNotRoutedAsAnUpgrade) {
    // The other half of the router's rule, and the reason an upgrade cannot be
    // recognised by method: `routeWs` keys on `sec-websocket-key` and answers
    // NotFound without it. So the same path, requested as an ordinary GET, is
    // an unmatched route — which is what the framework 404 answers.
    g_filter_ran = false;
    g_seen_pattern.clear();

    const Exchange exchange = get("/ws/echo", "");
    ASSERT_NE(exchange.response, nullptr);
    EXPECT_EQ(exchange.response->statusCode(), drogon::k404NotFound);
    EXPECT_FALSE(g_filter_ran)
        << "a plain GET reached the WebSocket route's filter, which would mean "
           "the upgrade and the ordinary request share one policy decision";
}

}  // namespace
}  // namespace anvil
