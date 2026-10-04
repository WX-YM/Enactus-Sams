#pragma once

// Every route the backend serves, and the policy the anvil access filter
// enforces on it (anvil docs/04-access-control.md).
//
// A route with no entry here cannot be registered: `register_route` throws at
// boot, and it attaches the filter itself for every class but Public, so a
// Guarded route cannot be wired up without its permission check.
//
// Guarded routes answer a real 401/403 rather than the stealth 404: the admin
// panel's existence is public, and the 401 is what drives the session refresh
// in the browser.

#include <array>

#include "anvil/accesscontrol/route_registry.h"
#include "perms.h"

namespace enactus {

namespace ac = anvil::accesscontrol;

[[nodiscard]] constexpr anvil::PermSet need(Perm bit) noexcept { return anvil::perm_mask(bit); }

inline constexpr std::array<ac::RoutePolicy, 45> kRoutes{{
    // --- account flows (anvil/accounts) --------------------------------------
    {anvil::PermSet{}, "/api/auth/salt", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/auth/login", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/auth/refresh", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/auth/logout", ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/auth/password", ac::RouteAccess::Authenticated,
     ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/session", ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/me", ac::RouteAccess::Authenticated, ac::RouteMethod::Get},

    // --- the public website --------------------------------------------------
    {anvil::PermSet{}, "/api/site", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/teams", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/applications", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/forms/{id}", ac::RouteAccess::Public, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/api/forms/{id}/responses", ac::RouteAccess::Public,
     ac::RouteMethod::Post},
    {anvil::PermSet{}, "/api/visits", ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/media/{ns}/{id}/{role}", ac::RouteAccess::Public, ac::RouteMethod::Get},

    // --- dashboard; the audit log is for whoever manages staff --------------
    {need(Perm::Dashboard), "/api/dashboard", ac::RouteAccess::Guarded, ac::RouteMethod::Get},
    {need(Perm::Users), "/api/audit", ac::RouteAccess::Guarded, ac::RouteMethod::Get},

    // --- applications --------------------------------------------------------
    {need(Perm::Applications), "/api/applications", ac::RouteAccess::Guarded,
     ac::RouteMethod::Get},
    {need(Perm::Applications), "/api/applications/{id}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Patch},
    {need(Perm::Applications), "/api/applications/{id}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Delete},

    // --- teams ---------------------------------------------------------------
    {need(Perm::Teams), "/api/teams", ac::RouteAccess::Guarded, ac::RouteMethod::Post},
    {need(Perm::Teams), "/api/teams/{id}", ac::RouteAccess::Guarded, ac::RouteMethod::Patch},
    {need(Perm::Teams), "/api/teams/{id}", ac::RouteAccess::Guarded, ac::RouteMethod::Delete},
    {need(Perm::Teams), "/api/teams/{id}/members", ac::RouteAccess::Guarded,
     ac::RouteMethod::Get},
    {need(Perm::Teams), "/api/teams/{id}/members", ac::RouteAccess::Guarded,
     ac::RouteMethod::Post},
    {need(Perm::Teams), "/api/teams/{id}/members/{member}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Patch},
    {need(Perm::Teams), "/api/teams/{id}/members/{member}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Delete},
    {need(Perm::Teams), "/api/team-order", ac::RouteAccess::Guarded, ac::RouteMethod::Put},

    // --- content CMS and gallery ---------------------------------------------
    {need(Perm::Content), "/api/sections", ac::RouteAccess::Guarded, ac::RouteMethod::Get},
    {need(Perm::Content), "/api/sections/{key}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Put},
    {need(Perm::Gallery), "/api/gallery/{kind}", ac::RouteAccess::Guarded, ac::RouteMethod::Get},
    {need(Perm::Gallery), "/api/gallery/{kind}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Post},
    {need(Perm::Gallery), "/api/gallery/{kind}/{id}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Delete},
    {need(Perm::Gallery), "/api/gallery-order/{kind}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Put},
    {need(Perm::MediaUpload), "/api/media", ac::RouteAccess::Guarded, ac::RouteMethod::Post},

    // --- form maker ----------------------------------------------------------
    {need(Perm::FormMaker), "/api/forms", ac::RouteAccess::Guarded, ac::RouteMethod::Get},
    {need(Perm::FormMaker), "/api/forms", ac::RouteAccess::Guarded, ac::RouteMethod::Post},
    {need(Perm::FormMaker), "/api/forms/{id}", ac::RouteAccess::Guarded, ac::RouteMethod::Put},
    {need(Perm::FormMaker), "/api/forms/{id}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Delete},
    {need(Perm::FormRead), "/api/forms/{id}/responses", ac::RouteAccess::Guarded,
     ac::RouteMethod::Get},
    {need(Perm::FormRead), "/api/forms/{id}/responses/{response}", ac::RouteAccess::Guarded,
     ac::RouteMethod::Delete},
    {need(Perm::FormRead), "/api/forms/{id}/export", ac::RouteAccess::Guarded,
     ac::RouteMethod::Get},

    // --- access control ------------------------------------------------------
    {need(Perm::Users), "/api/staff", ac::RouteAccess::Guarded, ac::RouteMethod::Get},
    {need(Perm::Users), "/api/staff", ac::RouteAccess::Guarded, ac::RouteMethod::Post},
    {need(Perm::Users), "/api/staff/{id}", ac::RouteAccess::Guarded, ac::RouteMethod::Patch},
    {need(Perm::Users), "/api/staff/{id}", ac::RouteAccess::Guarded, ac::RouteMethod::Delete},
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

}  // namespace enactus
