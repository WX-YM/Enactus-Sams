#pragma once

// Image edits on the wire (docs/21-image-edits.md §6): one handler that makes an
// edit and one that says what an editor needs to reopen one, installed at the
// patterns the APPLICATION declared for the route ids it names — the same
// arrangement install_account_routes makes.
//
// anvil ships the handlers because every application needs the same four
// stages on the same three pools, and each would get the order wrong once. The
// application declares the routes, the permission and the budget, because
// those are facts about the application.
//
//     POST <edit pattern ending {ns}/{id}>    {"recipe":"<b64url>","detach":false}
//         201 {id,width,height}   a new object
//         200 {id,width,height}   the same edit of the same source, already made
//     GET  <state pattern ending {ns}/{id}>
//         200 {source,width,height,recipe}
//
// The state route answers for ANY object: for an edit it names the source, the
// SOURCE's size and the recipe, so an editor always opens on the original; for
// anything else the object is its own source and the recipe is null.
//
// Both patterns must carry `{ns}` and `{id}` as their LAST two segments, in that
// order. They cannot share `/media/{ns}/{id}/…` with the public role route: any
// GET there is a role, and a role that happened to be named `edits` would be
// answered by the wrong handler.
//
// Available only when ANVIL_WITH_VIPS is on, like media/pipeline.h: making an
// edit is a render, and a build that cannot encode has nothing to render with.

#ifdef ANVIL_HAS_VIPS

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "anvil/accesscontrol/route_registry.h"
#include "anvil/descriptor/route_description.h"
#include "anvil/http/rate_limit.h"
#include "anvil/media/edit_shapes.h"
#include "anvil/media/service.h"

// Declared, not included, as origin_check.h does: an observer reads the request
// only to hand it to the application's own audit helper.
namespace drogon {
class HttpRequest;
}  // namespace drogon

namespace anvil::media {

// One answered edit, for the application's audit log.
//
// anvil owns the handler and the application owns the audit table, so without
// this an edit is the one write an application cannot record — an upload
// passes through the application's own handler, and an edit never does.
struct EditOutcome final {
    // For the client address and the request id; the body is not the
    // observer's to read.
    const std::shared_ptr<drogon::HttpRequest>& request;
    Uuid                actor;
    fs::Ns              ns;
    // As the path named it, so it may name nothing: a NotFound reports the
    // address that was tried.
    Uuid                source;
    // The object the caller was handed: new when `created`, an earlier render of
    // this exact edit otherwise. nullopt on every failure.
    std::optional<Uuid> edit;
    bool                created;
    // Ok on success. NotFound is the stealth answer, reported as itself.
    ErrorCode           code;
};

// Called once per answered edit, AFTER the response is handed back, on
// whichever thread answered: an event loop or a db/cpu pool thread. It must not
// block — buffer the row, as AuditService::write_async does — and must not
// throw; an exception is swallowed so a failed audit cannot fail an edit that
// already succeeded.
//
// Not called for a request the access filter refused — the filter's own
// DenialSink records those, and a second row would count them twice — nor for a
// path whose namespace or id does not parse, which names no object to record.
using EditObserver = std::function<void(const EditOutcome&)>;

struct EditRoutes final {
    std::string_view     edit_route_id;
    std::string_view     state_route_id;
    // Per ACCOUNT, and in the application's decode budget: an edit is one
    // libvips render on cpu_pool, the same cost as an upload.
    http::RateLimitRule  budget;
    // Empty records nothing.
    EditObserver         on_edit{};
};

// Throws std::invalid_argument at boot when either id is not described, or its
// pattern does not end `{ns}/{id}`.
//
// `service` and `limiter` must outlive HTTP serving; the handlers hold both by
// reference, as every handler in an application holds its services.
void install_media_edit_routes(const MediaService& service, http::RateLimiter& limiter,
                               std::span<const accesscontrol::RoutePolicy> routes,
                               std::span<const descriptor::RouteDescription> descriptions,
                               const EditRoutes& config);

}  // namespace anvil::media

#endif  // ANVIL_HAS_VIPS
