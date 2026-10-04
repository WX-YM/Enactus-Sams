#pragma once

// What the descriptor publishes about every route (anvil docs/01-seams.md §3):
// the stable id hammer's generated client calls it by, its rate-limit bucket,
// its paging cursor and whether it is safe to repeat. One description per
// route, asserted below.

#include <array>

#include "anvil/descriptor/route_description.h"
#include "routes.h"

namespace enactus {

namespace d = anvil::descriptor;
using M = ac::RouteMethod;

inline constexpr std::array<d::RouteDescription, kRoutes.size()> kRouteDescriptions{{
    {"auth.salt", "/api/auth/salt", "", "login", "", 0, M::Post, true},
    {"auth.login", "/api/auth/login", "", "login", "", 0, M::Post, false},
    {"auth.refresh", "/api/auth/refresh", "", "refresh", "", 0, M::Post, false},
    {"auth.logout", "/api/auth/logout", "", "", "", 0, M::Post, true},
    {"auth.password", "/api/auth/password", "", "", "", 0, M::Post, false},
    {"session.current", "/api/session", "", "", "", 0, M::Get, true, true},
    {"identity.me", "/api/me", "", "", "", 0, M::Get, true},

    {"site.get", "/api/site", "", "", "", 0, M::Get, true},
    {"teams.list", "/api/teams", "", "", "", 0, M::Get, true},
    {"applications.apply", "/api/applications", "", "apply", "", 0, M::Post, false},
    {"forms.public", "/api/forms/{id}", "", "", "", 0, M::Get, true},
    {"forms.submit", "/api/forms/{id}/responses", "", "submit", "", 0, M::Post, false},
    {"visits.record", "/api/visits", "", "visit", "", 0, M::Post, false},
    {"media.object", "/media/{ns}/{id}/{role}", "", "", "", 0, M::Get, true},

    {"dashboard.get", "/api/dashboard", "", "", "", 0, M::Get, true},
    {"audit.list", "/api/audit", "", "", "_id", 100, M::Get, true},

    {"applications.list", "/api/applications", "", "", "_id", 100, M::Get, true},
    {"applications.update", "/api/applications/{id}", "", "staff-write", "", 0, M::Patch, false},
    {"applications.delete", "/api/applications/{id}", "", "staff-write", "", 0, M::Delete,
     true},

    {"teams.create", "/api/teams", "", "staff-write", "", 0, M::Post, false},
    {"teams.update", "/api/teams/{id}", "", "staff-write", "", 0, M::Patch, false},
    {"teams.delete", "/api/teams/{id}", "", "staff-write", "", 0, M::Delete, true},
    {"members.list", "/api/teams/{id}/members", "", "", "", 0, M::Get, true},
    {"members.add", "/api/teams/{id}/members", "", "staff-write", "", 0, M::Post, false},
    {"members.update", "/api/teams/{id}/members/{member}", "", "staff-write", "", 0, M::Patch,
     false},
    {"members.remove", "/api/teams/{id}/members/{member}", "", "staff-write", "", 0, M::Delete,
     true},
    {"teams.reorder", "/api/team-order", "", "staff-write", "", 0, M::Put, true},

    {"sections.list", "/api/sections", "", "", "", 0, M::Get, true},
    {"sections.publish", "/api/sections/{key}", "", "staff-write", "", 0, M::Put, false},
    {"gallery.list", "/api/gallery/{kind}", "", "", "", 0, M::Get, true},
    {"gallery.add", "/api/gallery/{kind}", "", "staff-write", "", 0, M::Post, false},
    {"gallery.remove", "/api/gallery/{kind}/{id}", "", "staff-write", "", 0, M::Delete, true},
    {"gallery.reorder", "/api/gallery-order/{kind}", "", "staff-write", "", 0, M::Put, true},
    {"media.upload", "/api/media", "", "media", "", 0, M::Post, false},

    {"forms.list", "/api/forms", "", "", "", 0, M::Get, true},
    {"forms.create", "/api/forms", "", "staff-write", "", 0, M::Post, false},
    {"forms.update", "/api/forms/{id}", "", "staff-write", "", 0, M::Put, false},
    {"forms.delete", "/api/forms/{id}", "", "staff-write", "", 0, M::Delete, true},
    {"responses.list", "/api/forms/{id}/responses", "", "", "_id", 50, M::Get, true},
    {"responses.delete", "/api/forms/{id}/responses/{response}", "", "staff-write", "", 0,
     M::Delete, true},
    {"responses.export", "/api/forms/{id}/export", "", "", "", 0, M::Get, true},

    {"staff.list", "/api/staff", "", "", "", 0, M::Get, true},
    {"staff.create", "/api/staff", "", "staff-write", "", 0, M::Post, false},
    {"staff.update", "/api/staff/{id}", "", "staff-write", "", 0, M::Patch, false},
    {"staff.disable", "/api/staff/{id}", "", "staff-write", "", 0, M::Delete, true},
}};

static_assert(d::descriptions_match(kRoutes, kRouteDescriptions),
              "every route is described exactly once, with a unique id, a concrete method, "
              "and a cursor exactly where there is a page limit");

}  // namespace enactus
