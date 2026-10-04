#include "anvil/accesscontrol/access_filter.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <trantor/utils/Logger.h>

#include "denial.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/analytics/counters.h"
#include "anvil/core/user_context.h"
#include "anvil/http/client_address.h"
#include "anvil/http/errors.h"
#include "anvil/http/request_id.h"
#include "anvil/http/request_scope.h"

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

namespace anvil::accesscontrol {
namespace {

AccessControlDeps g_deps{};
bool              g_initialised = false;

// std::string, because Drogon's Attributes and getCookie are keyed by one. Both
// are short enough for the small-string optimisation, so neither touches the
// heap despite the type, and both are constructed once at load rather than per
// request.
//
// getCookie, and NOT getHeader("cookie"), which is what this filter used to do.
// Drogon's request parser special-cases the Cookie field: it splits the value
// into its own map and never stores the field itself, so getHeader("cookie")
// answers an empty string on every request that ever carried one. The filter
// therefore saw no credential on any request, and every authenticated and every
// stealth route in the application denied everything — a total authentication
// outage that no test could see, because every test drives evaluate() with a
// header string of its own rather than a Drogon request.
const std::string kAccessCookie{kAccessCookieName};

[[nodiscard]] std::int64_t now_unix() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::array<std::uint8_t, 16> peer_ip(const HttpRequestPtr& req) noexcept {
    // The CLIENT's address, not the socket's. Behind the Nginx front end this
    // repository ships, peerAddr() is 127.0.0.1 on every request, so every
    // denial record in the security log named the loopback as the source, and
    // the collection's whole forensic value was zero. The packing this used to
    // do privately moved into client_address, which every caller needing an
    // address now shares.
    return http::client_address(req);
}

// The non-stealth error body, through the one writer.
//
// It used to assemble `{"error":{"code":"X"}}` here with two string
// concatenations, under a comment claiming "a 401 from this filter carries no
// server-side detail worth correlating". That premise was wrong when it was
// written: `deny()` below has enqueued an audit row for every denial since phase
// 3, so there has always been something to correlate, and the id is what joins a
// user saying "I cannot get in" to the row that says why they could not. The
// concatenation was also the second writer of a shape
// docs/00-architecture.md §8 publishes, which is how two applications come to
// spell one failure two ways.
//
// No submitted value, ever, and no field map: `carries_field_detail()` decides
// that inside the writer, and none of the codes this filter produces is
// ValidationFailed.
[[nodiscard]] HttpResponsePtr plain_error(const HttpRequestPtr& req, ErrorCode code) {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(http::http_status(code)));
    response->setContentTypeString(http::kNotFoundContentType);

    // Reserved once for the longest body this can produce: the envelope is 44
    // bytes of punctuation and keys, the id is 26, and the longest wire name is
    // INSUFFICIENT_STORAGE at 20.
    std::string body;
    body.reserve(96);
    http::append_error_body(body, code, http::request_id_of(req));
    response->setBody(std::move(body));

    response->addHeader("Cache-Control", "no-store");
    // `X-Request-Id` is NOT set here. The pre-sending advice
    // `http::install_request_scope()` registers puts it on every response that
    // is not a 404, so a handler — and this filter — cannot forget it, and the
    // stealth path cannot acquire it by somebody adding a line to one branch.
    return response;
}

void deny(const HttpRequestPtr& req, drogon::FilterCallback& fcb, const RoutePolicy& policy,
          const Evaluation& evaluation, const std::optional<Uuid>& actor) {
    const bool stealth = policy.access == RouteAccess::Stealth;

    // Answer FIRST. The audit write below allocates and eventually reaches the
    // database; performing it before responding would put it inside the window
    // an attacker times, and the whole stealth property is that a denied route
    // and a nonexistent one are timed the same.
    //
    // An upgrade takes the SAME shared 404 as everything else, and the branch
    // that used to give it its own object is gone. That branch existed to stop
    // `~WebSocketConnectionImpl` appending a close frame to a refusal; it never
    // did, because the connection object exists by the time any filter runs and
    // `setCloseConnection` does not make it stop existing
    // (accesscontrol/stealth.h). The refusal that genuinely leaves nothing behind
    // it is taken a stage earlier, by `accesscontrol/upgrade_gate.h`, which is
    // why a Stealth route reaches this line only for the one denial the gate
    // cannot take without blocking an event loop.
    if (stealth) {
        fcb(not_found_response());
    } else {
        const drogon::HttpResponsePtr response = plain_error(req, evaluation.code);
        // Truthful rather than cosmetic on an upgrade: the connection object's
        // destructor calls `shutdown()` on the socket a moment from now, so a
        // refusal that did not say `Connection: close` would be describing a
        // connection the client cannot reuse as one it can.
        if (is_upgrade_request(req)) { response->setCloseConnection(true); }
        fcb(response);
    }

    detail::record_denial(req, stealth, evaluation.code, actor);
}

void allow(const HttpRequestPtr& req, drogon::FilterChainCallback& fccb,
           const Evaluation& evaluation, bool has_context) {
    if (has_context) {
        // Into the scope the pre-routing advice already allocated, rather than
        // into a second attribute of its own. This is the write the whole
        // one-attribute argument in core/user_context.h is about, and it now
        // costs no allocation at all — the scope was made when the id was.
        //
        // The return is checked rather than ignored: false means either that
        // `install_request_scope()` was never called, which would leave every
        // handler on this deployment reading a null context, or that something
        // filled the context before the filter did, which is a second authority
        // this one is about to overwrite. Both are boot-time mistakes and both
        // are silent, so the one place that can see them says so.
        if (!http::attach_user_context(req, evaluation.ctx)) {
            LOG_ERROR << "no request scope to attach a user context to; "
                         "call anvil::http::install_request_scope() before the first "
                         "listener starts, or a handler reads a null context";
        }
    }
    fccb();
}

}  // namespace

namespace detail {

void record_denial(const HttpRequestPtr& req, bool stealth, ErrorCode code,
                   const std::optional<Uuid>& actor) noexcept {
    // The TRUE reason, even where the client saw a byte-identical 404. Coarsened
    // to three values rather than carrying ErrorCode, because ErrorCode is
    // append-only and a label space that grows with it is a label space nobody
    // reviews.
    //
    // NOT labelled by source network. The value space of that label is the
    // internet — the cardinality explosion docs/17 §6 exists to refuse — and the
    // coarsened network is already on the audit row below, where retention
    // bounds it instead of resident memory.
    const analytics::DenialReason reason =
        code == ErrorCode::Unauthenticated ? analytics::DenialReason::Unauthenticated
        : code == ErrorCode::Forbidden     ? analytics::DenialReason::Forbidden
                                           : analytics::DenialReason::Other;
    analytics::count(analytics::Internal::StealthDenials, reason,
                     stealth ? analytics::Stealthed::Yes : analytics::Stealthed::No);

    if (g_deps.denials != nullptr) {
        g_deps.denials->record(DenialRecord{
            .actor = actor,
            .ip = peer_ip(req),
            .code = code,
            .stealthed = stealth,
        });
    }
}

}  // namespace detail

void AccessControl::init(AccessControlDeps deps) {
    if (!deps.keys) { throw std::invalid_argument{"AccessControl: token keys must not be null"}; }
    if (deps.epochs == nullptr) {
        throw std::invalid_argument{"AccessControl: epoch resolver must not be null"};
    }
    g_deps = std::move(deps);
    g_initialised = true;
}

bool AccessControl::initialised() noexcept { return g_initialised; }

const AccessControlDeps& AccessControl::deps() {
    if (!g_initialised) { throw std::logic_error{"AccessControl::init has not been called"}; }
    return g_deps;
}

void AccessControl::reset() noexcept {
    g_deps = AccessControlDeps{};
    g_initialised = false;
}

void AccessFilter::doFilter(const HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                            drogon::FilterChainCallback&& fccb) {
    const AccessControlDeps& deps = AccessControl::deps();

    const std::string_view pattern = req->getMatchedPathPattern();
    // Keyed by pattern AND method: one pattern can carry a public GET and a
    // permission-gated DELETE, and collapsing them would have to weaken one of
    // the two (see route_registry.h).
    const RoutePolicy* policy =
        policy_for(deps.routes, pattern, method_from_string(req->getMethodString()));
    if (policy == nullptr) {
        // A registered route with no registry entry is a REGISTRATION BUG, and
        // it fails closed. The route-coverage test exists so this is caught in
        // CI rather than in production, but a deny-by-default runtime is what
        // makes that test a safety net instead of the only defence
        // (CLAUDE.md §5).
        LOG_ERROR << "route '" << pattern
                  << "' declares no access policy; denying. Add it to route_registry.h";
        fcb(not_found_response());
        return;
    }

    // A reference into the request's own cookie map, so the token is not copied
    // and the deny path still allocates nothing.
    const std::string& access_token = req->getCookie(kAccessCookie);
    const Evaluation evaluation =
        evaluate_token(access_token, *policy, *deps.keys, *deps.epochs, now_unix());

    switch (evaluation.step) {
        case Step::Allow:
            // A public route yields a zeroed context when nobody is signed in;
            // attaching that would let a handler read user_id 0 as a user.
            allow(req, fccb, evaluation,
                  policy->access != RouteAccess::Public || !is_nil(evaluation.ctx.user_id));
            return;

        case Step::Deny:
            deny(req, fcb, *policy, evaluation, std::nullopt);
            return;

        case Step::ResolveEpoch:
            break;
    }

    // The epoch authority is not cached. Everything from here is off the event
    // loop, because it may touch Redis and, rarely, MongoDB. Captured by value:
    // the request and both callbacks are shared_ptr-backed and this frame is
    // gone the moment resolve_async returns (CLAUDE.md §3.3).
    deps.epochs->resolve_async(
        evaluation.ctx.user_id,
        [req, fcb = std::move(fcb), fccb = std::move(fccb), evaluation,
         policy](Result<std::uint64_t> authority) mutable {
            if (!authority) {
                // Fail CLOSED. A revocation channel we cannot read is not
                // permission to skip revocation checks.
                const Evaluation denied{.ctx = {},
                                        .code = ErrorCode::Unauthenticated,
                                        .step = Step::Deny,
                                        .token_epoch = 0};
                deny(req, fcb, *policy, denied, evaluation.ctx.user_id);
                return;
            }

            const Evaluation resumed =
                resume_after_epoch(evaluation, *policy, authority.value());
            if (resumed.step == Step::Allow) {
                allow(req, fccb, resumed, true);
            } else {
                deny(req, fcb, *policy, resumed, evaluation.ctx.user_id);
            }
        });
}

std::shared_ptr<const UserContext> user_context(const HttpRequestPtr& req) {
    const std::shared_ptr<const http::RequestScope> scope = http::request_scope(req);
    if (!scope || !scope->has_context) { return nullptr; }
    // The ALIASING constructor: one more reference on the scope's existing
    // control block, pointing at the member. No allocation, and the scope
    // outlives every copy of this pointer — which is what makes a context that
    // travels to a thread pool safe without the copy CLAUDE.md §2.2 would
    // otherwise demand.
    return std::shared_ptr<const UserContext>{scope, &scope->ctx};
}

}  // namespace anvil::accesscontrol
