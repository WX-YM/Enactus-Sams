#pragma once

#include <array>
#include "anvil/descriptor/route_description.h"
#include "routes.h"

namespace enactus {
namespace d = anvil::descriptor;

inline constexpr std::array<d::RouteDescription, 11> kRouteDescriptions{{
    {"auth.login", "/api/auth/login", "", "", "", 0, ac::RouteMethod::Post, false},
    {"auth.me", "/api/auth/me", "", "", "", 0, ac::RouteMethod::Get, true},
    
    {"applications.apply", "/api/applications", "", "", "", 0, ac::RouteMethod::Post, false},
    {"applications.list", "/api/applications_list", "", "", "_id", 100, ac::RouteMethod::Get, true},
    
    {"teams.list", "/api/teams", "", "", "_id", 100, ac::RouteMethod::Get, true},
    {"teams.create", "/api/teams", "", "", "", 0, ac::RouteMethod::Post, false},
    {"content.get", "/api/content", "", "", "", 0, ac::RouteMethod::Get, true},
    {"content.set", "/api/content", "", "", "", 0, ac::RouteMethod::Post, false},
    {"users.list", "/api/users", "", "", "", 100, ac::RouteMethod::Get, true},
    {"users.create", "/api/users", "", "", "", 0, ac::RouteMethod::Post, false},
    {"users.delete", "/api/users", "", "", "", 0, ac::RouteMethod::Delete, false},
}}};

static_assert(d::descriptions_match(kRoutes, kRouteDescriptions),
              "every route is described exactly once");

} // namespace enactus
