#include "anvil/accesscontrol/upgrade_filter.h"

#include <drogon/HttpResponse.h>

#include <string>
#include <string_view>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/types.h"
#include "anvil/http/errors.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/request_scope.h"

namespace anvil::accesscontrol {
namespace {

// The same body shape the access filter's denials carry, so a refused upgrade
// and a refused request spell one failure one way. Kept here rather than shared
// with `access_filter.cc` because sharing it would mean exporting a helper whose
// only two callers are filters in one library — and the duplication is four
// lines against a header that would be part of the ABI forever.
[[nodiscard]] drogon::HttpResponsePtr refusal(const drogon::HttpRequestPtr& req,
                                              ErrorCode code) {
    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(http::http_status(code)));
    response->setContentTypeString(http::kNotFoundContentType);

    std::string body;
    body.reserve(96);
    http::append_error_body(body, code, http::request_id_of(req));
    response->setBody(std::move(body));

    response->addHeader("Cache-Control", "no-store");
    // Truthful rather than cosmetic. `WebSocketConnectionImpl` was constructed
    // before this filter ran, and releasing it calls `shutdown()` on the socket —
    // so the connection is going away whatever this header says, and a refusal
    // that did not say so would be describing a connection the client cannot
    // reuse as one it can. It also carries four bytes of a protocol that was
    // never negotiated, which is nothing to disclose on a route whose existence
    // is not a secret and everything to disclose on one whose existence is: that
    // class is refused a stage earlier, where no connection object exists yet
    // (accesscontrol/upgrade_gate.h).
    response->setCloseConnection(true);
    return response;
}

}  // namespace

void UpgradeOriginFilter::doFilter(const drogon::HttpRequestPtr& req,
                                   drogon::FilterCallback&& fcb,
                                   drogon::FilterChainCallback&& fccb) {
    // `Always`, which is the whole reason this filter exists. The method would
    // answer `NotRequired` for every handshake, and that answer is a PASS — no
    // log line, no metric, nothing to notice (http/origin_check.h).
    const http::OriginVerdict verdict =
        http::check_request_origin(req, http::OriginRequirement::Always);
    if (!http::is_rejection(verdict)) {
        fccb();
        return;
    }

    // Which refusal, decided the way the access filter decides it: by the
    // route's own class. An unregistered pattern takes the stealth answer too —
    // it fails closed for the same reason `AccessFilter` does, and a route with
    // no policy is not a route whose existence is worth confirming.
    //
    // The `Stealth` half of this is UNREACHABLE while the upgrade gate is
    // installed, and it is installed by the only function that can register a
    // WebSocket route: the gate runs the same origin check a stage earlier and
    // answers there, because that is the one point where the refusal leaves no
    // close frame behind it. It is kept rather than deleted because the other
    // half is not unreachable — a pattern this table does not name has to fail
    // closed here too — and a branch that answers "deny" for one input and
    // nothing at all for another is not a branch worth splitting in two.
    const std::string_view pattern = req->getMatchedPathPattern();
    const RoutePolicy* policy =
        policy_for(AccessControl::deps().routes, pattern, RouteMethod::Get);
    if (policy == nullptr || policy->access == RouteAccess::Stealth) {
        fcb(not_found_response());
        return;
    }
    // Forbidden and never Unauthenticated: nothing here read a credential, and
    // an origin that is not ours is not a request that could be fixed by signing
    // in. Answering 401 would send a client into a refresh-and-retry loop
    // against a check no token can satisfy.
    fcb(refusal(req, ErrorCode::Forbidden));
}

}  // namespace anvil::accesscontrol
