// The parts of the request scope that need a request, and therefore the parts
// that cannot live in the foundation library.
//
// Same split as client_address_drogon.cc, and for the same reason: the struct
// and its assertions are two foundation types glued together, while minting into
// a Drogon attribute map needs the framework.

#include "anvil/http/request_scope.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include "anvil/http/client_address.h"
#include "anvil/http/request_id.h"
#include "anvil/http/trace_context.h"

namespace anvil::http {
namespace {

// std::string, because Drogon's Attributes map and addHeader are keyed by one.
// Both are short enough for the small-string optimisation, so neither touches
// the heap despite the type, and both are constructed once at load rather than
// per request.
const std::string kScopeKey{kRequestScopeKey};
const std::string kIdHeader{kRequestIdHeader};
// Lowercase, which is both the spelling the standard uses and the one Drogon
// stores a field name under.
const std::string kTraceHeader{kTraceparentHeader};

[[nodiscard]] std::int64_t now_unix_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The mutable handle, which only this translation unit ever holds.
//
// The attribute stores `shared_ptr<RequestScope>` and every accessor below hands
// out `shared_ptr<const RequestScope>`. `std::any` matches on exact type, so the
// non-const spelling is what has to be read back here; the const conversion at
// the boundary is a refcount bump and no allocation.
[[nodiscard]] std::shared_ptr<RequestScope> mutable_scope(
    const std::shared_ptr<drogon::HttpRequest>& req) noexcept {
    if (!req) { return nullptr; }
    const drogon::AttributesPtr& attributes = req->attributes();
    if (!attributes->find(kScopeKey)) { return nullptr; }
    return attributes->get<std::shared_ptr<RequestScope>>(kScopeKey);
}

// Written once by install_request_scope() before any listener starts and read on
// every request afterwards. A plain value rather than an atomic because the
// write happens before the threads that read it exist — the same lifetime the
// advices themselves have.
TraceIngest g_ingest = TraceIngest::Off;

void mint_scope(const std::shared_ptr<drogon::HttpRequest>& req) {
    // make_shared, so the 112 bytes and the control block are one allocation
    // rather than two. The context is zeroed here and filled by the filter; a
    // scope whose context is never filled is the ordinary shape of a public
    // route, not an error.
    auto scope = std::make_shared<RequestScope>();
    scope->request_id = mint_request_id(now_unix_ms());
    scope->has_context = false;
    // Under the default policy this is a comparison against `Off` and nothing
    // else: no header lookup, no proxy-list load, no parse. The short-circuit is
    // the whole reason the policy is checked before the peer is.
    if (g_ingest != TraceIngest::Off) {
        scope->trace = ingest_traceparent(g_ingest, peer_is_trusted_proxy(req),
                                          req->getHeader(kTraceHeader));
    }
    req->attributes()->insert(kScopeKey, std::move(scope));
}

// `X-Request-Id`, on every response but a 404.
//
// The exclusion is keyed on the STATUS and not on the identity of the shared
// not-found object, and the difference is the whole security argument. Stealth
// is the claim that a denied route and a nonexistent one are indistinguishable
// (accesscontrol/stealth.h), and any header one carries and the other does not
// is a tell. Drogon's `newNotFoundResponse()` hands back the custom 404 object
// itself on an event-loop thread and a COPY of it anywhere else, so a rule that
// recognised the shared object by address would hold until the first not-found
// built off the loop — and would then produce a 404 with an id beside a 404
// without one, on the same path. Excluding the status excludes every spelling of
// it, including ones this library never wrote.
//
// The cost is that a genuine 404 on a route whose existence is not secret also
// carries no header. That is the right side to be wrong on: the id is still in
// the body wherever `append_error_body` wrote it, and a 404 is the one status
// this library has promised is uniform.
void send_request_id(const std::shared_ptr<drogon::HttpRequest>& req,
                     const drogon::HttpResponsePtr& resp) {
    if (!resp || resp->statusCode() == drogon::k404NotFound) { return; }

    const std::shared_ptr<RequestScope> scope = mutable_scope(req);
    if (!scope) { return; }

    const std::array<char, kRequestIdChars> rendered = format_request_id(scope->request_id);
    resp->addHeader(kIdHeader, std::string{rendered.data(), rendered.size()});
}

}  // namespace

void install_request_scope(TraceIngest ingest) {
    // Function-local static: initialised exactly once, thread-safely, on first
    // use — the same shape stealth.h's shared response uses, and the reason the
    // second call is a no-op rather than a second advice.
    //
    // The policy is captured INSIDE the lambda, so a second call cannot change
    // it either. A boot function whose second call silently re-answers a
    // security question is worse than one that is unsafe to call twice, because
    // nothing fails.
    static const bool installed = [ingest] {
        g_ingest = ingest;
        // The OBSERVER overload, not the intercepting one. This advice answers
        // nothing and must not be able to: the intercepting form allocates a
        // shared_ptr for the continuation callback per request and puts a
        // branch on the response path, to buy the ability to refuse a request
        // that nothing here would ever refuse.
        drogon::app().registerPreRoutingAdvice(
            [](const drogon::HttpRequestPtr& req) { mint_scope(req); });
        // Pre-sending rather than post-handling: post-handling runs only where a
        // HANDLER produced the response, so every filter denial — which is every
        // response this wiring exists for — would go out without the header.
        drogon::app().registerPreSendingAdvice(send_request_id);
        return true;
    }();
    (void)installed;
}

std::shared_ptr<const RequestScope> request_scope(
    const std::shared_ptr<drogon::HttpRequest>& req) {
    return mutable_scope(req);
}

TraceContext trace_of(const std::shared_ptr<drogon::HttpRequest>& req) noexcept {
    const std::shared_ptr<RequestScope> scope = mutable_scope(req);
    if (!scope) { return TraceContext{}; }
    return scope->trace;
}

RequestId request_id_of(const std::shared_ptr<drogon::HttpRequest>& req) noexcept {
    const std::shared_ptr<RequestScope> scope = mutable_scope(req);
    if (!scope) { return RequestId{}; }
    return scope->request_id;
}

bool attach_user_context(const std::shared_ptr<drogon::HttpRequest>& req,
                         const UserContext& ctx) noexcept {
    const std::shared_ptr<RequestScope> scope = mutable_scope(req);
    if (!scope || scope->has_context) { return false; }
    scope->ctx = ctx;
    scope->has_context = true;
    return true;
}

}  // namespace anvil::http
