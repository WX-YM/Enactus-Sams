// The one part of origin_check that needs a request, and therefore the one part
// that cannot live in the foundation library.
//
// Same split as client_address_drogon.cc, and the installed list is held the
// same way and for the same reason: parsed and validated once, at boot, by the
// parser the request path uses, rather than by a second one that can disagree
// with it.

#include "anvil/http/origin_check.h"

#include <drogon/HttpRequest.h>

#include <atomic>
#include <memory>
#include <string>

namespace anvil::http {
namespace {

// std::string because getHeader is keyed by one; short enough for the
// small-string optimisation, and constructed once at load rather than per
// request. Lowercase, because Drogon lowercases a field name when it stores one.
const std::string kOriginHeader{"origin"};

// shared_ptr<const>, swapped atomically: a request that started under the old
// list finishes under it rather than seeing a half-updated one.
std::shared_ptr<const AllowedOrigins> g_allowed_origins;

}  // namespace

void install_allowed_origins(std::shared_ptr<const AllowedOrigins> origins) noexcept {
    std::atomic_store_explicit(&g_allowed_origins, std::move(origins),
                               std::memory_order_release);
}

OriginVerdict check_request_origin(const std::shared_ptr<drogon::HttpRequest>& req,
                                   OriginRequirement requirement) noexcept {
    if (!req) { return OriginVerdict::Mismatched; }

    const std::shared_ptr<const AllowedOrigins> allowed =
        std::atomic_load_explicit(&g_allowed_origins, std::memory_order_acquire);
    if (!allowed) {
        // Nothing installed, so nothing is on the list. Deliberately NOT the
        // `client_address` answer, where an absent proxy list means "this
        // process is the edge" and is a real deployment: an absent origin list
        // means nobody said which origins are ours, and there is no safe way to
        // guess. It still runs the method rule first, so a route that did not
        // require an Origin is not turned into a rejection by a missing list.
        static const AllowedOrigins kNone;
        return check_origin(req->getMethodString(), req->getHeader(kOriginHeader), kNone,
                            requirement);
    }
    return check_origin(req->getMethodString(), req->getHeader(kOriginHeader), *allowed,
                        requirement);
}

}  // namespace anvil::http
