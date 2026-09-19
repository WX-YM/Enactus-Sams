#include "anvil/accesscontrol/stealth.h"

#include <string>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>

#include "anvil/http/errors.h"

namespace anvil::accesscontrol {
namespace {

[[nodiscard]] drogon::HttpResponsePtr build() {
    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k404NotFound);
    // setContentTypeString rather than setContentTypeCode: the code form appends
    // a charset parameter, and the exact Content-Type string is part of what has
    // to match Nginx's error_page byte for byte.
    response->setContentTypeString(http::kNotFoundContentType);
    response->setBody(std::string{http::kNotFoundBody});

    // no-store, not no-cache: a shared cache must not hold this at all. It is
    // also what the genuine 404 carries, and the two must agree.
    response->addHeader("Cache-Control", "no-store");
    // The body is JSON and will never be anything else, so sniffing has nothing
    // to discover — but its presence or absence is itself observable, so it is
    // set unconditionally and identically on both paths.
    response->addHeader("X-Content-Type-Options", "nosniff");

    // Deliberately absent, each one a tell that the route exists:
    //   WWW-Authenticate  announces that authentication would have helped
    //   Set-Cookie        only a route that ran would clear or set one
    //   X-Request-Id      correlatable, and absent from an unmatched route

    // Never cached by the framework's own response cache: a shared response
    // whose expiry the framework manages could acquire a Date or Age header
    // that the unmatched-route path does not have.
    response->setExpiredTime(0);
    return response;
}

}  // namespace

const drogon::HttpResponsePtr& not_found_response() {
    // Function-local static: initialised exactly once, thread-safely, on first
    // use — which is at boot, from install_as_framework_404().
    static const drogon::HttpResponsePtr response = build();
    return response;
}

bool is_upgrade_request(const drogon::HttpRequestPtr& req) noexcept {
    // std::string because getHeader is keyed by one; constructed once at load
    // rather than per request, and lowercase because Drogon stores field names
    // that way.
    static const std::string kKeyHeader{"sec-websocket-key"};
    return req != nullptr && !req->getHeader(kKeyHeader).empty();
}

drogon::HttpResponsePtr not_found_upgrade_response() { return build(); }

void install_as_framework_404() {
    drogon::app().setCustom404Page(not_found_response());
}

}  // namespace anvil::accesscontrol
