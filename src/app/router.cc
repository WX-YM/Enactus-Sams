#include "app/router.h"

#include <string>
#include <utility>

#include <drogon/drogon.h>

#include "anvil/accesscontrol/route_registration.h"
#include "anvil/accounts/routes.h"
#include "app/applications.h"
#include "app/content.h"
#include "app/dashboard.h"
#include "app/forms.h"
#include "app/services.h"
#include "app/site.h"
#include "app/staff.h"
#include "app/teams.h"
#include "route_descriptions.h"
#include "routes.h"

namespace enactus {

namespace {

namespace ac = anvil::accesscontrol;
using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;
using Callback = std::function<void(const HttpResponsePtr&)>;
using H = void (*)(const HttpRequestPtr&, http::Responder&&);
using H1 = void (*)(const HttpRequestPtr&, http::Responder&&, const std::string&);
using H2 = void (*)(const HttpRequestPtr&, http::Responder&&, const std::string&, const std::string&);

void add(const char* pattern, drogon::HttpMethod method, H handler,
         ac::PublicContext context = ac::PublicContext::Omit) {
    ac::register_route(kRoutes, pattern, method,
                       [handler](const HttpRequestPtr& req, Callback&& respond) {
                           handler(req, std::move(respond));
                       },
                       context);
}

void add(const char* pattern, drogon::HttpMethod method, H1 handler,
         ac::PublicContext context = ac::PublicContext::Omit) {
    ac::register_route(kRoutes, pattern, method,
                       [handler](const HttpRequestPtr& req, Callback&& respond, const std::string& a) {
                           handler(req, std::move(respond), a);
                       },
                       context);
}

void add(const char* pattern, drogon::HttpMethod method, H2 handler) {
    ac::register_route(kRoutes, pattern, method,
                       [handler](const HttpRequestPtr& req, Callback&& respond, const std::string& a,
                                 const std::string& b) { handler(req, std::move(respond), a, b); });
}

}  // namespace

void install_routes() {
    using drogon::Delete;
    using drogon::Get;
    using drogon::Patch;
    using drogon::Post;
    using drogon::Put;

    // Salt, sign-in, refresh, sign-out and password change, straight from anvil.
    anvil::accounts::install_account_routes(services().accounts, kRoutes, kRouteDescriptions);

    add("/api/session", Get, &routes::session);
    add("/api/me", Get, &routes::me);

    // The public website.
    add("/api/site", Get, &routes::site_get);
    add("/api/teams", Get, &routes::teams_list);
    add("/api/applications", Post, &routes::applications_apply);
    // Attach: a signed-in form builder reads drafts through the same route.
    add("/api/forms/{id}", Get, &routes::forms_public, ac::PublicContext::Attach);
    add("/api/forms/{id}/responses", Post, &routes::forms_submit);
    add("/api/visits", Post, &routes::visits_record);
    ac::register_route(kRoutes, "/media/{ns}/{id}/{role}", Get,
                       [](const HttpRequestPtr& req, Callback&& respond, const std::string& ns,
                          const std::string& id, const std::string& role) {
                           routes::media_object(req, std::move(respond), ns, id, role);
                       });

    add("/api/dashboard", Get, &routes::dashboard_get);
    add("/api/audit", Get, &routes::audit_list);

    add("/api/applications", Get, &routes::applications_list);
    add("/api/applications/{id}", Patch, &routes::applications_update);
    add("/api/applications/{id}", Delete, &routes::applications_delete);

    add("/api/teams", Post, &routes::teams_create);
    add("/api/teams/{id}", Patch, &routes::teams_update);
    add("/api/teams/{id}", Delete, &routes::teams_delete);
    add("/api/teams/{id}/members", Get, &routes::members_list);
    add("/api/teams/{id}/members", Post, &routes::members_add);
    add("/api/teams/{id}/members/{member}", Patch, &routes::members_update);
    add("/api/teams/{id}/members/{member}", Delete, &routes::members_remove);
    add("/api/team-order", Put, &routes::teams_reorder);
    add("/api/team-leads", Get, &routes::team_leads);

    add("/api/sections", Get, &routes::sections_list);
    add("/api/sections/{key}", Put, &routes::sections_publish);
    add("/api/gallery/{kind}", Get, &routes::gallery_list);
    add("/api/gallery/{kind}", Post, &routes::gallery_add);
    add("/api/gallery/{kind}/{id}", Delete, &routes::gallery_remove);
    add("/api/gallery-order/{kind}", Put, &routes::gallery_reorder);
    add("/api/media", Post, &routes::media_upload);

    add("/api/forms", Get, &routes::forms_list);
    add("/api/forms", Post, &routes::forms_create);
    add("/api/forms/{id}", Put, &routes::forms_update);
    add("/api/forms/{id}", Delete, &routes::forms_delete);
    add("/api/forms/{id}/responses", Get, &routes::responses_list);
    add("/api/forms/{id}/responses/{response}", Delete, &routes::responses_delete);
    add("/api/forms/{id}/export", Get, &routes::responses_export);

    add("/api/staff", Get, &routes::staff_list);
    add("/api/staff", Post, &routes::staff_create);
    add("/api/staff/{id}", Patch, &routes::staff_update);
    add("/api/staff/{id}", Delete, &routes::staff_disable);
}

}  // namespace enactus
