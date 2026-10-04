// The application's tables and the small pure helpers the handlers lean on.
// The tables are also static_asserted where they are declared; these tests
// cover what a static_assert cannot say.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "app/applications.h"
#include "app/teams.h"
#include "perms.h"
#include "route_descriptions.h"
#include "routes.h"

namespace {

using namespace enactus;
namespace ac = anvil::accesscontrol;

TEST(Permissions, ImpliedBitsFollowTheirGrants) {
    EXPECT_TRUE(holds(with_implied(anvil::perm_mask(Perm::Content)), Perm::MediaUpload));
    EXPECT_TRUE(holds(with_implied(anvil::perm_mask(Perm::Gallery)), Perm::MediaUpload));
    EXPECT_TRUE(holds(with_implied(anvil::perm_mask(Perm::FormMaker)), Perm::FormRead));
    EXPECT_TRUE(holds(with_implied(anvil::perm_mask(Perm::Applications)), Perm::FormRead));
    EXPECT_FALSE(holds(with_implied(anvil::perm_mask(Perm::Users)), Perm::FormRead));
    EXPECT_FALSE(holds(kGrantable, Perm::MediaUpload));
    EXPECT_FALSE(holds(kGrantable, Perm::FormRead));
}

TEST(Routes, EveryWriteToStaffDataIsGuardedOrAuthenticated) {
    // The public surface, spelled out: anything else Public is a mistake.
    const std::set<std::pair<std::string, ac::RouteMethod>> kPublic{
        {"/api/auth/salt", ac::RouteMethod::Post},
        {"/api/auth/login", ac::RouteMethod::Post},
        {"/api/auth/refresh", ac::RouteMethod::Post},
        {"/api/site", ac::RouteMethod::Get},
        {"/api/teams", ac::RouteMethod::Get},
        {"/api/applications", ac::RouteMethod::Post},
        {"/api/forms/{id}", ac::RouteMethod::Get},
        {"/api/forms/{id}/responses", ac::RouteMethod::Post},
        {"/api/visits", ac::RouteMethod::Post},
        {"/media/{ns}/{id}/{role}", ac::RouteMethod::Get},
    };
    for (const ac::RoutePolicy& route : kRoutes) {
        if (route.access == ac::RouteAccess::Public) {
            EXPECT_TRUE(kPublic.contains({std::string{route.pattern}, route.method})) << route.pattern;
        }
        if (route.access == ac::RouteAccess::Guarded) {
            EXPECT_FALSE(route.required.none()) << route.pattern << " is Guarded with no permission";
        }
    }
}

TEST(Routes, TheAuditLogNeedsTheUsersPermission) {
    for (const ac::RoutePolicy& route : kRoutes) {
        if (route.pattern == "/api/audit") { EXPECT_TRUE(holds(route.required, Perm::Users)); }
    }
}

TEST(Applications, TheSlugIsAStableDigestOfTheEmail) {
    const std::string a = application_slug("someone@example.com");
    EXPECT_EQ(a.size(), 32U);
    EXPECT_EQ(a, application_slug("someone@example.com"));
    EXPECT_NE(a, application_slug("someone.else@example.com"));
    EXPECT_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(Teams, NamesCompareCaseInsensitivelyAndTrimmed) {
    EXPECT_TRUE(same_team_name("  Human Resources ", "human resources"));
    EXPECT_FALSE(same_team_name("HR", "Human Resources"));
    EXPECT_FALSE(same_team_name("", ""));
}

TEST(Teams, SlugsAreLowercaseHyphenated) {
    EXPECT_EQ(slugify("Project Management"), "project-management");
    EXPECT_EQ(slugify("  R&D -- Lab 2 "), "r-d-lab-2");
    EXPECT_EQ(slugify("العلاقات"), "");
}

}  // namespace
