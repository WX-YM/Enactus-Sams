// The boot guard keys on what the filter keys on.
//
// `accesscontrol::declared()` checked `is_declared(routes, pattern)` while the
// filter resolves `(pattern, method)`. A handler registered under a method the
// registry does not declare booted CLEAN and then denied every request to
// itself, logging an error nobody is watching for. It fails closed, so this is
// availability rather than security — and it is the shape every outage of this
// kind has: one route totally dead, with the whole suite green.
//
// The guard lives here rather than in the foundation suite because it speaks
// Drogon's HttpMethod, and the registry it consults compiles into a library that
// does not link Drogon.
//
// The other half of the guard — "booting the full application registers every
// route without throwing" — is not asserted here and cannot be: a throw at
// registration aborts the process, so what proves it is a deployment that came
// up, not a case in this file.

#include <gtest/gtest.h>

#include "routes.h"

#include <functional>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/HttpConstraint.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/route_declaration.h"
#include "anvil/accesscontrol/route_registration.h"

namespace anvil::controllers {
namespace {

// Whether a constraint list names the access filter, which is the only question
// any of the registration cases below are asking.
[[nodiscard]] bool carries_filter(
    const std::vector<drogon::internal::HttpConstraint>& constraints) {
    for (const drogon::internal::HttpConstraint& constraint : constraints) {
        if (constraint.type() == drogon::internal::ConstraintType::HttpMiddleware &&
            constraint.getMiddlewareName() == accesscontrol::kAccessFilterName) {
            return true;
        }
    }
    return false;
}

// --- 1: the registration that used to boot clean and then 404 itself --------

TEST(RouteDeclaration, RegisteringAPatternUnderAnUndeclaredMethodThrowsAndNamesBoth) {
    // `/content/{id}` is declared for GET and DELETE, never POST. Registering a
    // POST handler on it is the mistake this guard exists for, and it used to
    // pass.
    try {
        const std::string registered = accesscontrol::declared(testapp::kRoutes, "/content/{id}", drogon::Post);
        FAIL() << "expected a throw, got '" << registered << "'";
    } catch (const std::logic_error& e) {
        const std::string message{e.what()};
        EXPECT_NE(message.find("/content/{id}"), std::string::npos) << message;
        EXPECT_NE(message.find("POST"), std::string::npos)
            << "the message must name the METHOD, or it sends the reader to count "
               "enum positions: " << message;
    }
}

TEST(RouteDeclaration, AnUnknownPatternStillThrows) {
    EXPECT_THROW((void)accesscontrol::declared(testapp::kRoutes, "/nope", drogon::Get), std::logic_error);
}

// --- 2: an Any entry satisfies every method ---------------------------------

TEST(RouteDeclaration, AnAnyEntryIsAcceptedUnderEveryMethod) {
    // Most patterns declare `Any`. If the guard rejected them, every such route
    // would fail to boot — the fix would be worse than the defect.
    for (const drogon::HttpMethod method :
         {drogon::Get, drogon::Post, drogon::Put, drogon::Patch, drogon::Delete}) {
        EXPECT_NO_THROW((void)accesscontrol::declared(testapp::kRoutes, "/session/logout", method));
    }
}

// --- 3: a method-specific entry satisfies its own method and no other -------

TEST(RouteDeclaration, AMethodSpecificEntryIsAcceptedOnlyForItsOwnMethod) {
    // /me is declared under Get and nothing else.
    EXPECT_NO_THROW((void)accesscontrol::declared(testapp::kRoutes, "/me", drogon::Get));
    EXPECT_THROW((void)accesscontrol::declared(testapp::kRoutes, "/me", drogon::Delete), std::logic_error);

    // One pattern split across two methods is declared for both and no others.
    EXPECT_NO_THROW((void)accesscontrol::declared(testapp::kRoutes, "/media/{ns}/{id}", drogon::Get));
    EXPECT_NO_THROW((void)accesscontrol::declared(testapp::kRoutes, "/media/{ns}/{id}", drogon::Delete));
    EXPECT_THROW((void)accesscontrol::declared(testapp::kRoutes, "/media/{ns}/{id}", drogon::Post), std::logic_error);
}

TEST(RouteDeclaration, AnUnmappedMethodIsRefusedRatherThanSilentlySatisfied) {
    // HEAD, OPTIONS and the WebDAV verbs map to no RouteMethod. They must NOT
    // resolve to `Any`, which would make the guard pass for a method the filter
    // will then find no policy for — the exact failure this task exists to close.
    EXPECT_EQ(accesscontrol::route_method_of(drogon::Head), accesscontrol::RouteMethod::Any);
    EXPECT_THROW((void)accesscontrol::declared(testapp::kRoutes, "/me", drogon::Head), std::logic_error);
    EXPECT_THROW((void)accesscontrol::declared(testapp::kRoutes, "/me", drogon::Options), std::logic_error);
}

// --- the guard returns what registerHandler needs ---------------------------

TEST(RouteDeclaration, AnAcceptedPatternIsReturnedUnchanged) {
    // It is passed straight into registerHandler, so it must be the pattern and
    // nothing else — no normalisation, no trailing slash, no prefix.
    EXPECT_EQ(accesscontrol::declared(testapp::kRoutes, "/content/{id}", drogon::Get), "/content/{id}");
    EXPECT_EQ(accesscontrol::declared(testapp::kRoutes, "/session/logout", drogon::Post), "/session/logout");
}

// --- the filter is attached by the library, never typed by the caller -------
//
// The registry proves a pattern DECLARES a policy. Nothing proved the filter
// that ENFORCES it was attached, and nothing could: getHandlersInfo() reports
// pattern, method and description, and no filter. An unguarded Stealth route
// passed the boot guard and the coverage test alike, because declaring a pattern
// is exactly what an unguarded route also does — so it was a public admin route
// with every check green.

// The pin that makes the string constant a checked one. kAccessFilterName is
// resolved by Drogon through DrObject's name map, so a drift between it and the
// class is not a compile error — it is every route registering without its
// filter.
TEST(RouteRegistration, TheFilterNameMatchesTheFilterClass) {
    EXPECT_EQ(accesscontrol::kAccessFilterName, accesscontrol::AccessFilter::classTypeName());
}

// The security property, over the whole reference table rather than over a case
// somebody chose. Every class but Public is denied by the filter and by nothing
// else, so for every one of them a missing filter is an authorization bypass.
TEST(RouteRegistration, EveryEnforcedRouteInTheTableGetsTheFilter) {
    for (const accesscontrol::RoutePolicy& route : testapp::kRoutes) {
        if (route.access == accesscontrol::RouteAccess::Public) { continue; }
        EXPECT_TRUE(carries_filter(accesscontrol::route_constraints(route, drogon::Get)))
            << route.pattern;
    }
}

TEST(RouteRegistration, PublicOmitsTheFilterUnlessTheRouteAsksForAContext) {
    const accesscontrol::RoutePolicy pub{PermSet{}, "/preview/{id}",
                                         accesscontrol::RouteAccess::Public};

    EXPECT_FALSE(carries_filter(accesscontrol::route_constraints(pub, drogon::Get)));
    EXPECT_TRUE(carries_filter(accesscontrol::route_constraints(
        pub, drogon::Get, accesscontrol::PublicContext::Attach)));
}

// Authenticated is included deliberately. The finding that prompted this named
// Guarded and Stealth; evaluate_token denies an absent token on Authenticated by
// exactly the same branch, so leaving it out would have been the same bug one
// class over.
TEST(RouteRegistration, FilterEnforcesEveryClassButPublic) {
    EXPECT_FALSE(accesscontrol::filter_enforces(accesscontrol::RouteAccess::Public));
    EXPECT_TRUE(accesscontrol::filter_enforces(accesscontrol::RouteAccess::Authenticated));
    EXPECT_TRUE(accesscontrol::filter_enforces(accesscontrol::RouteAccess::Guarded));
    EXPECT_TRUE(accesscontrol::filter_enforces(accesscontrol::RouteAccess::Stealth));
}

// The method constraint still has to survive, or the route answers every verb.
TEST(RouteRegistration, TheMethodIsConstrainedAlongsideTheFilter) {
    const accesscontrol::RoutePolicy guarded{PermSet{}, "/me",
                                             accesscontrol::RouteAccess::Authenticated};
    const std::vector<drogon::internal::HttpConstraint> constraints =
        accesscontrol::route_constraints(guarded, drogon::Post);

    ASSERT_EQ(constraints.size(), 2U);
    EXPECT_EQ(constraints[0].type(), drogon::internal::ConstraintType::HttpMethod);
    EXPECT_EQ(constraints[0].getHttpMethod(), drogon::Post);
    EXPECT_TRUE(carries_filter(constraints));
}

// register_route subsumes declared(), so the two failures read the same and are
// raised from one place rather than two that drift.
TEST(RouteRegistration, AnUndeclaredPatternAndMethodThrowsBeforeRegistering) {
    EXPECT_THROW(accesscontrol::register_route(testapp::kRoutes, "/content/{id}", drogon::Post,
                                               [](const drogon::HttpRequestPtr&,
                                                  std::function<void(const drogon::HttpResponsePtr&)>&&) {}),
                 std::logic_error);
}

}  // namespace
}  // namespace anvil::controllers
