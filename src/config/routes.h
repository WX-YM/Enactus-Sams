#pragma once

#include <array>
#include "anvil/accesscontrol/route_registry.h"
#include "perms.h"

namespace enactus {
namespace ac = anvil::accesscontrol;

// Every route is `Public` at the anvil filter layer: this application does
// not use anvil's session store, so authentication and per-permission
// authorization are enforced inside each handler by `requireAdminAuth`
// (src/handlers/api.cc). A handler that changes data must call it.
inline constexpr std::array<ac::RoutePolicy, 22> kRoutes{{
    {anvil::PermSet{}, "/api/auth/login", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/auth/logout", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/auth/me", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/applications", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/applications_list", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/applications_update", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/teams", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/teams", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/content", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/content", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/users", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/users", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/users", ac::RouteAccess::Public, ac::RouteMethod::Delete},
    {anvil::PermSet{}, "/api/track_visit", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/analytics", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/logs", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/upload", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/form_schema", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/form_schema", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/form_submissions", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/form_submissions", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/form_submissions", ac::RouteAccess::Public, ac::RouteMethod::Delete},
}};

static_assert([] {
    for (std::size_t i = 0; i < kRoutes.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (kRoutes[j].pattern == kRoutes[i].pattern &&
                kRoutes[j].method == kRoutes[i].method) {
                return false;
            }
        }
    }
    return true;
}(), "two entries share a (pattern, method) pair");

} // namespace enactus
