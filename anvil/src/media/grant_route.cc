#include "anvil/media/grant_route.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/utils/Logger.h>

#include "anvil/accesscontrol/route_registration.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/thread_pools.h"
#include "anvil/db/codec.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/namespace_spec.h"
#include "anvil/fs/sniff.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/retry_after.h"
#include "anvil/media/record.h"
#include "anvil/media/serving.h"

namespace anvil::media {
namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;
using Responder = std::function<void(const HttpResponsePtr&)>;

[[nodiscard]] std::int64_t now_unix() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(db::now_ms().time_since_epoch())
        .count();
}

[[nodiscard]] HttpResponsePtr shed(const HttpRequestPtr& req) {
    std::string body;
    http::append_error_body(body, ErrorCode::ServiceUnavailable, http::request_id_of(req));
    HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k503ServiceUnavailable);
    response->setContentTypeString(std::string{http::kJsonContentType});
    response->setBody(std::move(body));
    http::apply_retry_after(*response, http::kShedRetryAfterSeconds);
    return response;
}

// The row, resolved to the response, or the stealth 404. db_pool.
[[nodiscard]] HttpResponsePtr serve(const MediaService& service, const MediaGrant& grant,
                                    fs::MediaRole role, std::string_view accept) {
    auto entry = db::MongoPool::instance().acquire();
    const Result<std::optional<MediaRecord>> found =
        service.find(*entry, grant.ns(), grant.id());
    if (!found) {
        LOG_ERROR << "grant route: media lookup failed";
        return accesscontrol::not_found_response();
    }
    const std::optional<MediaRecord>& row = found.value();
    // Held by nothing: the last message holding it was revoked or expired, and
    // a grant minted before that is no longer an entitlement to anything.
    if (!row.has_value() || (!is_pinned(*row) && attached_refs(*row) == 0)) {
        return accesscontrol::not_found_response();
    }
    const fs::VariantKey key =
        fs::mime_class(row->mime) == fs::MimeClass::Image
            ? resolve_role(row->variants, fs::role_width(grant.ns(), role),
                           negotiate_format(accept))
            : fs::kMasterVariant;
    HttpResponsePtr response = accel_redirect_response(grant, key, row->mime);
    // Never past the grant: the URL is the grant, and a cache keyed by it must
    // stop when the grant would have (see the header).
    const std::int64_t remaining = std::max<std::int64_t>(grant.expires_unix() - now_unix(), 0);
    response->removeHeader("Cache-Control");
    response->addHeader("Cache-Control", "private, max-age=" + std::to_string(remaining));
    return response;
}

[[nodiscard]] std::string pattern_of(std::span<const descriptor::RouteDescription> descriptions,
                                     std::string_view id) {
    for (const descriptor::RouteDescription& candidate : descriptions) {
        if (candidate.id != id) { continue; }
        if (std::count(candidate.pattern.begin(), candidate.pattern.end(), '{') != 2) {
            throw std::invalid_argument{"media grant route '" + std::string{id} +
                                        "' must carry two placeholders: the grant, then "
                                        "the role"};
        }
        return std::string{candidate.pattern};
    }
    throw std::invalid_argument{"media grant route id '" + std::string{id} +
                                "' is not in the route descriptions"};
}

}  // namespace

void install_media_grant_route(const MediaService& service, const GrantKeys& keys,
                               std::span<const accesscontrol::RoutePolicy> routes,
                               std::span<const descriptor::RouteDescription> descriptions,
                               std::string_view route_id) {
    const MediaService* media = &service;
    const GrantKeys* grants = &keys;
    accesscontrol::register_route(
        routes, pattern_of(descriptions, route_id), drogon::Get,
        [media, grants](const HttpRequestPtr& req, Responder&& callback,
                        const std::string& grant_text, const std::string& role_text) {
            // On the loop thread, because both are a length check and an AEAD
            // open with no I/O: a forged or stale URL costs no pool slot.
            const std::optional<MediaGrant> grant = open_grant(*grants, grant_text, now_unix());
            fs::MediaRole role{};
            if (!grant.has_value() || !fs::role_from_segment(role_text, role)) {
                callback(accesscontrol::not_found_response());
                return;
            }
            auto shared_callback = std::make_shared<Responder>(std::move(callback));
            const bool posted = Pools::db().try_post(anvil::guarded(
                "media-grant",
                [media, req, grant = *grant, role, shared_callback]() {
                    // Answered exactly once, whatever the lookup does.
                    try {
                        (*shared_callback)(
                            serve(*media, grant, role, req->getHeader("accept")));
                    } catch (const std::exception& e) {
                        LOG_ERROR << "grant route failed: " << e.what();
                        (*shared_callback)(accesscontrol::not_found_response());
                    }
                }));
            if (!posted) { (*shared_callback)(shed(req)); }
        });
}

}  // namespace anvil::media
