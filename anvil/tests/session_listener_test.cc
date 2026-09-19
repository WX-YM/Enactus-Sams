// The holder-scoped route table, served.
//
// `append_reachable_routes` had a suite and no caller. Every case asserted the
// function's output as a string a test had built, which proves the projection
// and proves nothing about the response: no handler had ever run it, no socket
// had ever carried it, and the envelope it travels in — the status, the content
// type, the caching rules, the ETag the header file describes and delegates to
// "the caller" — existed only as a paragraph. A client generator built against a
// hand-written fixture asserts what we BELIEVE the server sends
// (docs/16-test-plan.md, fixtures).
//
// So this is the reference application serving it, over a real listener, through
// the real access filter, with the cookie a browser would send. What is recorded
// here is what a generated client parses.
//
// --- what is real, and what stands in --------------------------------------
//
// The filter is real, the route table is the reference application's, the
// projection is anvil's, and the credential is a token this file mints with
// anvil's own encoder and hands over as `__Host-at`. The epoch resolver is a
// stand-in, for the reason preview_listener_test.cc's credential store is one:
// `identity::AuthzService` lives in `anvil::app` while this binary links
// `anvil::platform` by design, and its own behaviour — the cache, the Redis
// mirror, the fail-closed denial — has a suite against a live cluster. What has
// never been exercised anywhere is the path between the socket and the table.

#include <array>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <thread>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <string_view>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>
#include <gtest/gtest.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/epoch_resolver.h"
#include "anvil/accesscontrol/route_registration.h"
#include "anvil/accesscontrol/route_projection.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/auth/token.h"
#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/http/errors.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/request_id.h"
#include "anvil/http/client_address.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/trace_context.h"
#include "anvil/http/response_writer.h"

#include "listener_fixture.h"
#include "perms.h"
#include "responses.h"
#include "route_descriptions.h"
#include "routes.h"

namespace anvil {
namespace {

namespace ac = accesscontrol;

using testapp::Perm;
using testfixture::Exchange;
using testfixture::get;

constexpr std::string_view kSessionPattern = "/session";
constexpr std::string_view kAuditPattern = "/audit";
constexpr std::string_view kFeedSocketPattern = "/ws/feed";
constexpr std::string_view kAuditSocketPattern = "/ws/audit";
constexpr std::string_view kAllowedOrigin = "https://example.test";
constexpr std::string_view kMePattern = "/me";

[[nodiscard]] std::int64_t now_unix() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// --- the credential ---------------------------------------------------------

// Drawn rather than fixed. A key constant in a test file is a key constant
// somebody copies into a deployment, and this one signs tokens the filter
// accepts.
[[nodiscard]] const std::shared_ptr<const auth::TokenKeys>& keys() {
    static const std::shared_ptr<const auth::TokenKeys> shared = [] {
        const std::array<std::uint8_t, auth::TokenKeys::kKeyBytes> key =
            crypto::random_array<auth::TokenKeys::kKeyBytes>();
        return std::make_shared<const auth::TokenKeys>(1, key);
    }();
    return shared;
}

// Every token this file mints is already the authority, so the cached answer is
// always Match. The interesting epoch behaviour is a database property and is
// tested where the database is; what is asserted here is that the epoch reaches
// the ETag.
class CachedEpochs final : public ac::EpochResolver {
public:
    [[nodiscard]] ac::EpochVerdict check_cached(const Uuid&, std::uint64_t) const noexcept override {
        return ac::EpochVerdict::Match;
    }

    void resolve_async(const Uuid&, std::function<void(Result<std::uint64_t>)> done) override {
        // Unreachable while check_cached answers Match, and a denial rather than
        // an allow if it ever is: an unreadable revocation channel is not
        // permission to skip revocation checks.
        done(fail(ErrorCode::ServiceUnavailable));
    }
};

[[nodiscard]] CachedEpochs& epochs() {
    static CachedEpochs resolver{};
    return resolver;
}

[[nodiscard]] const Uuid& holder_id() {
    static const Uuid id = uuid::generate_v7();
    return id;
}

// One session for every cookie this file mints, so a case can assert the id the
// `/me` body carries without reaching into the token it sent.
[[nodiscard]] const Uuid& session_id() {
    static const Uuid id = uuid::generate_v7();
    return id;
}

// The `__Host-at` cookie a browser would send, carrying exactly the authority
// named here. Two holders differing by one bit are two calls to this.
[[nodiscard]] std::string session_cookie(const PermSet& held, UserType type,
                                         std::uint64_t perm_epoch) {
    const auth::AccessClaims claims{
        .user_id = holder_id(),
        .session_id = session_id(),
        .permissions = held,
        .perm_epoch = perm_epoch,
        .expires_at = static_cast<std::uint32_t>(now_unix() + 900),
        .user_type = type,
        .locale = *Locale::from_tag("en"),
        .reserved = {},
    };
    return std::string{ac::kAccessCookieName} + "=" + auth::encode(claims, *keys());
}

// --- the ETag ---------------------------------------------------------------

// The identity of the route TABLE this build spells, taken once.
//
// `perm_epoch` is the moving half of the tag and the one anvil documents: it
// bumps on every authority change, which is exactly when the set of routes a
// holder reaches can change, so a grant reaches the table at the next
// revalidation rather than at the next login.
//
// It is not the whole key, and the gap is a DEPLOY. A build that adds a route,
// moves one or re-authorises one changes what this holder reaches while their
// epoch stands still — so an epoch-only tag answers 304 and leaves the client
// calling yesterday's map until somebody happens to edit their permissions. The
// table's own identity closes it, and costs one hash at boot rather than one per
// request: keying on the response BYTES instead would mean projecting the table
// to answer a revalidation, which is the work the revalidation exists to skip.
[[nodiscard]] const std::string& table_id() {
    static const std::string id = [] {
        // Every route — public and bootstrap included, which is what the
        // trailing `true` buys — projected against the authority that reaches
        // all of them, so the hash covers the TABLE rather than one holder's
        // view of it.
        std::string whole;
        ac::append_reachable_routes(whole, testapp::kRoutes, testapp::kRouteDescriptions,
                                    PermSet{}, UserType::SuperAdmin, true);
        const crypto::Digest256 digest = crypto::sha256(whole);
        return crypto::base64url_encode(std::span{digest}.first(9));
    }();
    return id;
}

[[nodiscard]] std::string etag_for(std::uint64_t perm_epoch) {
    // to_chars into a stack buffer rather than std::to_string: this runs on every
    // request to the route, including the revalidations that are meant to be the
    // cheap ones.
    std::array<char, 20> digits{};
    const std::to_chars_result written =
        std::to_chars(digits.data(), digits.data() + digits.size(), perm_epoch);

    std::string tag;
    tag.reserve(table_id().size() + 24);
    tag += '"';
    tag += table_id();
    tag += '.';
    tag.append(digits.data(), static_cast<std::size_t>(written.ptr - digits.data()));
    tag += '"';
    return tag;
}

// --- the handlers -----------------------------------------------------------

// What the SERVER saw, recorded rather than returned.
//
// The trace a request arrived with cannot be asserted from the client's side:
// the object the test builds is not the object the server parses, and the one
// place the answer exists is inside a handler. Recording it here rather than
// writing it into a response keeps this suite from growing the exact code path
// the case above forbids — a response that carries a trace back.
//
// Published with a release store and read with an acquire load, because the
// handler runs on a loop thread and the assertion runs on the test's.
std::array<std::uint8_t, 16> g_seen_trace_id{};
std::atomic<bool>            g_seen_trace{false};

void record_trace(const drogon::HttpRequestPtr& req) {
    const http::TraceContext trace = http::trace_of(req);
    g_seen_trace_id = trace.trace_id;
    g_seen_trace.store(trace.present(), std::memory_order_release);
}

void session_handler(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    record_trace(req);
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    if (ctx == nullptr) {
        // Unreachable while the route is Authenticated — the filter denies before
        // this runs. Answered rather than asserted, because the day somebody
        // relaxes the access class this becomes a null dereference reachable from
        // the internet, and a 404 is the cheaper bug.
        callback(ac::not_found_response());
        return;
    }

    const std::string tag = etag_for(ctx->perm_epoch);

    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->addHeader("ETag", tag);
    // `private`, and deliberately not `no-store`: the document is per HOLDER, so
    // a shared cache serving one holder's copy to another would hand over exactly
    // the paths the projection filtered out. `no-cache` stores it and revalidates
    // it on every use, which is what makes carrying an ETag worth anything.
    response->addHeader("Cache-Control", "private, no-cache");
    // The credential is a cookie, so the cookie is what this varies by. A cache
    // keyed on the URL alone holds one entry for every holder of it.
    response->addHeader("Vary", "Cookie");

    if (req->getHeader("If-None-Match") == tag) {
        response->setStatusCode(drogon::k304NotModified);
        // No body and no type for one: a 304 that announces a content type it did
        // not send is describing bytes that are not there.
        response->setContentTypeCode(drogon::CT_NONE);
        callback(response);
        return;
    }

    // One envelope key rather than a bare object, so the document can grow a
    // sibling without a client having to decide what it is reading. No reserve
    // here: append_reachable_routes sizes the buffer for the whole table off what
    // is already in it.
    std::string body{R"({"routes":)"};
    ac::append_reachable_routes(body, testapp::kRoutes, testapp::kRouteDescriptions,
                                ctx->permissions, ctx->user_type);
    // The sibling the envelope was shaped for. A client renders routes from the
    // first key and everything that is NOT a route from this one — which is the
    // half a superadmin was previously handed nothing for, because their
    // permission set is deliberately not all-ones.
    body += R"(,"authority":)";
    ac::append_holder_authority(body, ctx->permissions, ctx->user_type, testapp::kPerms);
    body += '}';

    response->setStatusCode(drogon::k200OK);
    response->setContentTypeString(http::kJsonContentType);
    response->setBody(std::move(body));
    callback(response);
}

// `GET /me`, written through the response binder.
//
// This handler is the proof the binder is worth having, and the proof is a
// NEGATIVE one: there is no spelling of it that disagrees with
// `testapp::kMeResponse`. Writing a key that is not next, writing one of the
// wrong type, or stopping before the last field are `static_assert`s rather than
// a schema that quietly describes bytes nobody sends — so the `"response"` object
// the descriptor emits for `identity.me` is a description of these bytes rather
// than a claim about them.
//
// Everything it writes the filter already established, so it reads no database
// and invents nothing.
void me_handler(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    if (ctx == nullptr) {
        // Unreachable while the route is Authenticated, and a 404 rather than a
        // dereference for the reason the session handler gives.
        callback(ac::not_found_response());
        return;
    }

    // Bit order, not table order, and on the stack: the catalogue cannot hand
    // back more names than it holds, so the ceiling is a compile-time constant
    // and the p99 case is three or four (ENGINEERING_RULES.md §2.1).
    std::array<std::string_view, testapp::kPerms.size()> held{};
    std::size_t count = 0;
    testapp::kPerms.for_each_name(ctx->permissions,
                                  [&](std::string_view name) { held[count++] = name; });

    std::string body;
    body.reserve(256);

    // The chain is straight-line to the last field and then branches, which is
    // exactly what the writer allows: both arms start from the same declared
    // position and both are checked against it.
    const auto at_permissions = http::write_object<testapp::kMeResponse>(body)
                                    .uuid<"id">(ctx->user_id)
                                    .uuid<"session_id">(ctx->session_id)
                                    .text<"locale">(ctx->locale.tag());
    if (ac::is_superadmin(ctx->user_type)) {
        // NULL rather than the empty list, and the distinction is the reason the
        // field is declared nullable. A superadmin's permission set is
        // deliberately not all-ones, so an empty list here would read as "holds
        // no permissions" — which is the opposite of true, and is precisely the
        // defect `append_holder_authority` was added to close one response over.
        at_permissions.null_field<"permissions">().done();
    } else {
        at_permissions.strings<"permissions">(std::span{held}.first(count)).done();
    }

    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    response->setContentTypeString(http::kJsonContentType);
    response->addHeader("Cache-Control", "private, no-store");
    response->setBody(std::move(body));
    callback(response);
}


// A stealth route with something behind it, so "withheld from the table" and
// "refused at the door" can be asserted as the same answer over one socket
// rather than as two beliefs held in two suites.
void audit_handler(const drogon::HttpRequestPtr&,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeString(http::kJsonContentType);
    response->setBody(R"({"entries":[]})");
    callback(response);
}

}  // namespace

// `AutoCreation` is FALSE, which is the only correct setting for a controller
// anvil registers. With it on, Drogon calls the controller's own
// `initPathRouting` at startup and registers the path itself — with whatever
// constraints the `WS_PATH_ADD` macro carried and not the ones
// `register_websocket_route` builds. The route would exist twice, and the
// registration without the filters is the one nothing would report.
//
// Namespace scope and out-of-line member definitions, because Drogon resolves a
// controller by class name out of its object map: `DrObject<T>::alloc_` is a
// static member of a class template and registers the class in its constructor,
// so a class nothing ODR-uses is never in the map at all. Defining the members
// here is what instantiates it, the same way `AccessFilter`'s own out-of-line
// `doFilter` does.
class FeedSocket final : public drogon::WebSocketController<FeedSocket, false> {
public:
    // Never called, because AutoCreation above is false. It has to exist
    // anyway: Drogon's `pathRegistrator` guards the call with a runtime `if`
    // rather than an `if constexpr`, so it is compiled either way.
    static void initPathRouting() {}

    void handleNewMessage(const drogon::WebSocketConnectionPtr&, std::string&&,
                          const drogon::WebSocketMessageType&) override {}
    void handleNewConnection(const drogon::HttpRequestPtr&,
                             const drogon::WebSocketConnectionPtr&) override {}
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override {}
};

class AuditSocket final : public drogon::WebSocketController<AuditSocket, false> {
public:
    // Never called, because AutoCreation above is false. It has to exist
    // anyway: Drogon's `pathRegistrator` guards the call with a runtime `if`
    // rather than an `if constexpr`, so it is compiled either way.
    static void initPathRouting() {}

    void handleNewMessage(const drogon::WebSocketConnectionPtr&, std::string&&,
                          const drogon::WebSocketMessageType&) override {}
    void handleNewConnection(const drogon::HttpRequestPtr&,
                             const drogon::WebSocketConnectionPtr&) override {}
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override {}
};

namespace {

// Counts, and keeps the last row. Not a log of every denial: the binary makes
// many, the cases below assert a DELTA across one handshake, and a growing
// vector would be a second thing to reason about under `ctest -j`.
//
// Atomic because `record` is called from an event-loop thread and read from the
// case's own, which is the only cross-thread edge in this file.
struct CountingDenials final : ac::DenialSink {
    std::atomic<int>  count{0};
    std::atomic<bool> last_stealthed{false};
    std::atomic<int>  last_code{0};

    void record(const ac::DenialRecord& denial) noexcept override {
        last_stealthed.store(denial.stealthed, std::memory_order_relaxed);
        last_code.store(static_cast<int>(denial.code), std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_release);
    }
};

[[nodiscard]] CountingDenials& denials() {
    static CountingDenials sink;
    return sink;
}

void install_session_routes() {
    ac::AccessControl::init(ac::AccessControlDeps{
        .keys = keys(),
        .epochs = &epochs(),
        // A counting sink rather than none, and it is here for one property no
        // other binary can assert: a refused stealth UPGRADE is answered by the
        // gate, before the filter that used to answer it and therefore before
        // the line that used to record it. Moving a decision earlier is exactly
        // how the forensic record silently stops being written, so the record is
        // pinned where the decision moved.
        .denials = &denials(),
        .routes = testapp::kRoutes,
    });

    // Registered THROUGH register_route, which is how an application registers.
    // It refuses a handler the filter would then deny every request to, the way
    // the boot guard inside it always did — and it ATTACHES the filter, which
    // the boot guard never could. The filter name used to be spelled here, in
    // every consumer, and a route that omitted it was a route with no
    // authorization that nothing anywhere reported
    // (anvil/accesscontrol/route_registration.h).
    ac::register_route(testapp::kRoutes, std::string{kSessionPattern}, drogon::Get,
                       &session_handler);
    ac::register_route(testapp::kRoutes, std::string{kAuditPattern}, drogon::Get,
                       &audit_handler);
    ac::register_route(testapp::kRoutes, std::string{kMePattern}, drogon::Get, &me_handler);

    // The origins this process accepts. Installed here rather than in the shared
    // fixture because the upgrade filter is the only thing in this binary that
    // consults the list, and a list installed for every file would be a
    // configuration two other files did not ask for.
    auto origins = std::make_shared<http::AllowedOrigins>();
    if (!origins->parse(kAllowedOrigin)) {
        throw std::logic_error{"the origin list this suite installs is malformed"};
    }
    http::install_allowed_origins(std::move(origins));

    // Registered THROUGH register_websocket_route, which is what attaches both
    // the access filter and the origin check. An application that registered the
    // controller itself would get neither, and nothing would say so.
    ac::register_websocket_route(testapp::kRoutes, std::string{kFeedSocketPattern},
                                 FeedSocket::classTypeName());
    ac::register_websocket_route(testapp::kRoutes, std::string{kAuditSocketPattern},
                                 AuditSocket::classTypeName());
}

const testfixture::RouteRegistrar kRegistrar{&install_session_routes};

// --- driving it -------------------------------------------------------------

[[nodiscard]] Exchange get_session(std::string_view cookie, std::string_view if_none_match) {
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(std::string{kSessionPattern});
    if (!cookie.empty()) { req->addHeader("Cookie", std::string{cookie}); }
    if (!if_none_match.empty()) {
        req->addHeader("If-None-Match", std::string{if_none_match});
    }
    return testfixture::send(req);
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// The authority the reference application's own cases are written against: a
// content editor, who reaches the content read and nothing above it.
[[nodiscard]] std::string editor_cookie(std::uint64_t perm_epoch = 1) {
    return session_cookie(perm_mask(Perm::ContentRead), UserType::Staff, perm_epoch);
}

// --- the cases --------------------------------------------------------------

TEST(SessionListener, TheRouteIsAuthenticatedAndRequiresNoBit) {
    // Both halves matter. Authenticated rather than Public, because the table
    // names the paths a bundle is not allowed to carry; unpermissioned, because
    // a holder of no bits still has to be able to learn that they hold none.
    const ac::RoutePolicy* policy =
        ac::policy_for(testapp::kRoutes, kSessionPattern, ac::RouteMethod::Get);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->access, ac::RouteAccess::Authenticated);
    EXPECT_FALSE(policy->required.any());
}

TEST(SessionListener, TheBodyIsOneObjectOfRouteIdToMethodAndPath) {
    // This case is the recording. A client generator is built against these
    // bytes, so the shape is asserted literally rather than by parsing it into
    // something forgiving.
    const Exchange exchange = get_session(editor_cookie(), "");

    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_NE(exchange.response, nullptr);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);
    EXPECT_EQ(exchange.response->contentTypeString(), http::kJsonContentType);

    const std::string_view body = exchange.response->body();
    EXPECT_TRUE(body.starts_with(R"({"routes":{)")) << body;
    EXPECT_TRUE(body.ends_with("}}")) << body;

    // Keyed by the opaque ID and never by the path, which is the property the
    // whole descriptor exists for: a client that names routes by path publishes
    // the path.
    EXPECT_TRUE(contains(body, R"("identity.me":"GET /me")")) << body;
    // The method travels with the path, in one string, because a client needs
    // both and never one.
    EXPECT_TRUE(contains(body, R"("content.get":"GET /content/{id}")")) << body;

    // The table does NOT name the route that produced it, and that is the
    // bootstrap flag working rather than a hole. `session.current` is
    // Authenticated, so every holder here reaches it — but its path is compiled
    // into the client bundle, because the address of the session cannot arrive
    // with the session. Sending it back would be bytes on the one response every
    // signed-in tab asks for, to tell a client something it already had.
    //
    // This line read `"session.current":"GET /session"` until the path and the
    // authority stopped being one question.
    EXPECT_FALSE(contains(body, "session.current")) << body;
    EXPECT_FALSE(contains(body, R"("GET /session")")) << body;
    // `/session/logout`'s POLICY is `Any` — one handler that dispatches
    // internally — and what a client is handed here is POST, because ANY is not
    // a method anything can send. This line read `"ANY /session/logout"` for as
    // long as a description was allowed to carry `Any`, which is to say for as
    // long as the recorded bytes a client generator is built from named a call
    // no generated client could make.
    EXPECT_TRUE(contains(body, R"("auth.logout":"POST /session/logout")")) << body;
    EXPECT_FALSE(contains(body, "ANY")) << body;
}

TEST(SessionListener, TheEnvelopeCarriesTheHoldersAuthorityBesideItsRoutes) {
    // The sibling key the envelope was shaped for. A client renders routes from
    // the first and everything that is NOT a route from the second.
    const Exchange exchange = get_session(editor_cookie(), "");
    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    const std::string_view body = exchange.response->body();
    EXPECT_TRUE(contains(body, R"("authority":{"superadmin":false,"perms":["ContentRead"]})"))
        << body;
}

TEST(SessionListener, ASuperAdminIsToldSoOverTheWire) {
    // The defect this closed, end to end: a superadmin's permission set is
    // deliberately not all-ones, so before the authority key existed the one
    // account that reaches everything was handed an empty list and a client
    // rendering affordances from it hid every control from them.
    const Exchange exchange =
        get_session(session_cookie(PermSet{}, UserType::SuperAdmin, 1), "");
    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    const std::string_view body = exchange.response->body();
    EXPECT_TRUE(contains(body, R"("authority":{"superadmin":true,"perms":[]})")) << body;

    // And the routes half is unaffected, because the server built it with
    // satisfies() — which is why this was only ever about affordances that are
    // not routes.
    EXPECT_TRUE(contains(body, "audit.list")) << body;
}

TEST(SessionListener, AHolderIsHandedOnlyTheRoutesTheyReach) {
    const Exchange exchange = get_session(editor_cookie(), "");
    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    const std::string_view body = exchange.response->body();
    EXPECT_TRUE(contains(body, R"("content.get":"GET /content/{id}")")) << body;

    // Held by nobody here, so neither the id nor the path reaches the wire. The
    // path is the whole point: a bundle is a public file and a lazily-loaded
    // chunk is a public URL, so a path withheld from one is withheld from
    // neither unless the server withholds it.
    EXPECT_FALSE(contains(body, "content.delete")) << body;
    EXPECT_FALSE(contains(body, "audit.list")) << body;
    EXPECT_FALSE(contains(body, "/audit")) << body;
}

TEST(SessionListener, ThePublicPathsAreNotResentWithEverySession) {
    const Exchange exchange = get_session(editor_cookie(), "");
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    // Already compiled into the bundle, because a route reachable with no
    // credential discloses nothing by naming itself. Sending them again is bytes
    // on a response every signed-in tab asks for.
    const std::string_view body = exchange.response->body();
    EXPECT_FALSE(contains(body, "auth.login")) << body;
    EXPECT_FALSE(contains(body, "auth.refresh")) << body;
    EXPECT_FALSE(contains(body, "content.preview")) << body;
}

TEST(SessionListener, NoCredentialIsARefusalAndNeverAPartialTable) {
    const Exchange exchange = get_session("", "");

    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    // A REAL 401, not the stealth 404: a single-page client has to be able to
    // tell "re-authenticate" from "route gone", and this is the route it asks
    // when it is trying to find out.
    EXPECT_EQ(exchange.response->statusCode(), drogon::k401Unauthorized);
    EXPECT_FALSE(contains(exchange.response->body(), "routes")) << exchange.response->body();
    EXPECT_TRUE(exchange.response->getHeader("ETag").empty());
}

TEST(SessionListener, TheTableAndTheDoorAgreeOverTheSocket) {
    // The projection calls satisfies() and so does the filter, and a unit test
    // can assert they return the same answer. What it cannot assert is that the
    // two answers are the same over a socket, for a real holder, on a real
    // route — which is the form the disagreement would actually take.
    const std::string without = editor_cookie();
    const Exchange withheld = get_session(without, "");
    ASSERT_EQ(withheld.response->statusCode(), drogon::k200OK);
    ASSERT_FALSE(contains(withheld.response->body(), "audit.list"));

    // Withheld from the table, and refused at the door with the byte-identical
    // not-found: a route listed but denied is an affordance that fails, and one
    // denied here but served there is a stealth route with a public index.
    const Exchange refused = get(kAuditPattern, without);
    EXPECT_EQ(refused.response->statusCode(), drogon::k404NotFound);
    EXPECT_EQ(refused.response->body(), get("/no-such-path-at-all", "").response->body());

    // And the other direction, which is the one a client notices: listed, and
    // served.
    const std::string with =
        session_cookie(perm_mask(Perm::ContentRead, Perm::AuditRead), UserType::Staff, 1);
    const Exchange listed = get_session(with, "");
    ASSERT_EQ(listed.response->statusCode(), drogon::k200OK);
    EXPECT_TRUE(contains(listed.response->body(), R"("audit.list":"GET /audit")"))
        << listed.response->body();

    const Exchange served = get(kAuditPattern, with);
    EXPECT_EQ(served.response->statusCode(), drogon::k200OK);
}

TEST(SessionListener, TheResponseIsPrivateAndVariesByTheCredential) {
    const Exchange exchange = get_session(editor_cookie(), "");
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    // Per holder, so a shared cache holding one copy would serve one holder's
    // map to another — which is the disclosure the projection exists to prevent,
    // reintroduced downstream of it.
    EXPECT_EQ(exchange.response->getHeader("Cache-Control"), "private, no-cache");
    EXPECT_EQ(exchange.response->getHeader("Vary"), "Cookie");
    EXPECT_FALSE(exchange.response->getHeader("ETag").empty());
}

TEST(SessionListener, AnUnchangedTableRevalidatesWithoutTheBytes) {
    const std::string cookie = editor_cookie();
    const Exchange first = get_session(cookie, "");
    ASSERT_EQ(first.response->statusCode(), drogon::k200OK);

    const std::string tag = first.response->getHeader("ETag");
    ASSERT_FALSE(tag.empty());

    const Exchange revalidated = get_session(cookie, tag);
    EXPECT_EQ(revalidated.response->statusCode(), drogon::k304NotModified);
    EXPECT_TRUE(revalidated.response->body().empty());
    // The same tag comes back, so a client that stored it keeps it rather than
    // dropping to an unconditional request on the next load.
    EXPECT_EQ(revalidated.response->getHeader("ETag"), tag);
    EXPECT_EQ(revalidated.response->getHeader("Cache-Control"), "private, no-cache");
}

TEST(SessionListener, AGrantReachesTheTableAtTheNextRevalidation) {
    // The claim the ETag is keyed to perm_epoch FOR. A grant bumps the epoch, the
    // next access token carries the new one, and the tag the client is holding
    // stops matching — so the table is re-fetched at the next revalidation
    // rather than at the next login.
    const std::string before = editor_cookie(1);
    const Exchange granted_nothing = get_session(before, "");
    ASSERT_EQ(granted_nothing.response->statusCode(), drogon::k200OK);
    const std::string old_tag = granted_nothing.response->getHeader("ETag");
    ASSERT_FALSE(contains(granted_nothing.response->body(), "audit.list"));

    const std::string after =
        session_cookie(perm_mask(Perm::ContentRead, Perm::AuditRead), UserType::Staff, 2);
    const Exchange revalidated = get_session(after, old_tag);

    // Not a 304. The client offered the tag it had and the server refused it,
    // which is the only reason the new route ever reaches a long-lived tab.
    ASSERT_EQ(revalidated.response->statusCode(), drogon::k200OK);
    EXPECT_NE(revalidated.response->getHeader("ETag"), old_tag);
    EXPECT_TRUE(contains(revalidated.response->body(), "audit.list"))
        << revalidated.response->body();
}

// --- the described response -------------------------------------------------
//
// `identity.me` is the one route in the reference table whose success body is
// DESCRIBED, and these cases are the recording of it. What makes the recording
// worth anything is that the handler cannot disagree with the declaration:
// `write_object<kMeResponse>` walks `testapp::kMeResponse` and a handler that
// wrote a different key, a different type or a different number of keys would
// not compile. So a case asserting the bytes is asserting the schema too.

[[nodiscard]] Exchange get_me(std::string_view cookie) {
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(std::string{kMePattern});
    if (!cookie.empty()) { req->addHeader("Cookie", std::string{cookie}); }
    return testfixture::send(req);
}

TEST(DescribedResponse, TheBodyIsTheDeclaredFieldsInTheDeclaredOrder) {
    const Exchange exchange = get_me(editor_cookie());
    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);
    EXPECT_EQ(exchange.response->contentTypeString(), http::kJsonContentType);

    // Literal bytes, in order, because the ORDER is half of what the declaration
    // promises: a client generated from `"fields":[…]` reads them positionally in
    // some languages and by name in others, and a server free to reorder them
    // would break the first kind silently.
    const std::string expected = std::string{R"({"id":")"} + uuid::to_string(holder_id()) +
                                 R"(","session_id":")" + uuid::to_string(session_id()) +
                                 R"(","locale":"en","permissions":["ContentRead"]})";
    EXPECT_EQ(exchange.response->body(), expected);
}

TEST(DescribedResponse, TheDeclarationAndTheBytesNameTheSameKeysInTheSameOrder) {
    // The property the binder exists for, asserted rather than assumed: the keys
    // that reach the wire are the declared ones, read out of the table a client
    // generator is handed. A handler that drifted from the table could not
    // compile, so what this pins is the other direction — that the table a client
    // reads is the table the handler was checked against.
    const Exchange exchange = get_me(editor_cookie());
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    const std::string_view body = exchange.response->body();
    std::size_t at = 0;
    for (const http::ResponseField& field : testapp::kMeResponse) {
        const std::string key = "\"" + std::string{field.name} + "\":";
        const std::size_t found = body.find(key, at);
        ASSERT_NE(found, std::string_view::npos)
            << "declared key '" << field.name << "' is not in the body: " << body;
        EXPECT_GE(found, at) << field.name;
        at = found + key.size();
    }
}

TEST(DescribedResponse, ANullableFieldIsNullAndNeverAnEmptyList) {
    // The branch the nullable flag exists for. A superadmin's permission set is
    // deliberately not all-ones, so the honest answer to "which bits does this
    // holder hold" is nothing at all — and an empty list would read as "holds no
    // permissions", which is the opposite of true and is the same defect
    // `append_holder_authority` closed one response over.
    const Exchange exchange = get_me(session_cookie(PermSet{}, UserType::SuperAdmin, 1));
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    const std::string_view body = exchange.response->body();
    EXPECT_TRUE(contains(body, R"("permissions":null)")) << body;
    EXPECT_FALSE(contains(body, R"("permissions":[])")) << body;
}

// --- the request id, over the same socket -----------------------------------
//
// These cases live in this file rather than in one of their own because the
// property needs all four shapes of response at once — an allowed one, a real
// 401 from the filter, a stealth drop and an unmatched route — and this is the
// only file in the binary that owns a credential, a filter, and a Stealth route
// with something behind it. A second file would either re-initialise
// `AccessControl` and take the keys out from under this one, or assert against
// routes it did not register.
//
// What is under test is the WIRING, not the encoding. `errors_test.cc` pins the
// alphabet, the padding and the two extreme vectors against hand-computed
// strings; nothing there can see whether an id ever reaches a socket, and until
// this phase none of them did.

[[nodiscard]] bool is_well_formed_id(std::string_view rendered) {
    if (rendered.size() != http::kRequestIdChars) { return false; }
    // 128 bits do not divide into fives, so two zero bits lead and the first
    // character carries three real bits — a valid id never begins above '7'.
    if (rendered.front() > '7') { return false; }
    for (const char c : rendered) {
        if (http::kCrockfordAlphabet.find(c) == std::string_view::npos) { return false; }
    }
    return true;
}

TEST(RequestIdListener, AnAllowedResponseCarriesTheIdAsAHeader) {
    const Exchange exchange = get_session(editor_cookie(), "");
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    const std::string id = exchange.response->getHeader(std::string{http::kRequestIdHeader});
    EXPECT_TRUE(is_well_formed_id(id)) << id;
}

TEST(RequestIdListener, EveryRequestGetsItsOwnId) {
    // A CSPRNG tail rather than a counter, so two ids from two requests differ
    // even inside one millisecond — and a person quoting one names exactly one
    // log line rather than every request that second.
    const std::string cookie = editor_cookie();
    const std::string first =
        get_session(cookie, "").response->getHeader(std::string{http::kRequestIdHeader});
    const std::string second =
        get_session(cookie, "").response->getHeader(std::string{http::kRequestIdHeader});
    ASSERT_TRUE(is_well_formed_id(first)) << first;
    EXPECT_NE(first, second);
}

TEST(RequestIdListener, TheFiltersOwnDenialCarriesTheSameIdInTheBodyAndTheHeader) {
    // The whole point of the pair. A user reads the id out of a browser or out
    // of a response body and support finds the audit row `deny()` wrote — which
    // it can only do if the two values are one value. Independently generated
    // ids correlate to nothing.
    const Exchange exchange = get_session("", "");
    ASSERT_EQ(exchange.response->statusCode(), drogon::k401Unauthorized);

    const std::string id = exchange.response->getHeader(std::string{http::kRequestIdHeader});
    ASSERT_TRUE(is_well_formed_id(id)) << id;

    // Asserted as the literal bytes, because this is the recording a generated
    // client is built from: one envelope, the code, then the id, and no `fields`
    // on a code that does not carry one.
    const std::string expected =
        std::string{R"({"error":{"code":"UNAUTHENTICATED","request_id":")"} + id + R"("}})";
    EXPECT_EQ(exchange.response->body(), expected);
}

TEST(RequestIdListener, TheStealthDropCarriesNeitherAnIdNorAHeader) {
    // The tell `stealth.h` enumerates beside `WWW-Authenticate` and
    // `Set-Cookie`, asserted over a socket rather than as a claim about the one
    // response object: a correlatable value that a genuine 404 does not have
    // makes a denied route separable from a nonexistent one by reading a header.
    const Exchange refused = get(kAuditPattern, editor_cookie());
    ASSERT_EQ(refused.response->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(refused.response->getHeader(std::string{http::kRequestIdHeader}).empty());
    EXPECT_EQ(refused.response->body(), http::kNotFoundBody);

    const Exchange absent = get("/no-such-path-at-all", "");
    ASSERT_EQ(absent.response->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(absent.response->getHeader(std::string{http::kRequestIdHeader}).empty());

    // Not just the body, which the case above this file already pinned: the
    // HEADERS too. Adding `X-Request-Id` to one path and not the other is
    // precisely the shape of regression this pair exists to catch, and it is
    // invisible to a body comparison.
    EXPECT_EQ(refused.response->headers(), absent.response->headers());
}

TEST(RequestIdListener, TheNotFoundBytesAreTheSameOnEveryRequest) {
    // `not_found_response()` is ONE object, and Drogon caches its RENDERED form:
    // once a not-found has been serialised, later ones are sent from the buffer
    // that serialisation produced. That is what makes the status-keyed exclusion
    // in request_scope.cc structural rather than tidy, and it was found by
    // deleting the guard and reading the wire — the leak is not the one the
    // design anticipated. Two consecutive drops came back carrying the SAME id,
    // the first request's, because the second request never re-rendered. A
    // per-request value written onto this object is therefore not merely a
    // stealth tell: it is one visitor's correlation id handed to every later
    // 404 for the life of the process.
    //
    // What this case can assert from the client's side is the constancy that
    // makes the leak possible, and therefore the property that has to hold: the
    // bytes of a not-found do not depend on the request that asked for it. That
    // the guard is what keeps a per-request value off them is asserted by the
    // case above, which fails when it is removed.
    const std::string cookie = editor_cookie();
    const Exchange first = get(kAuditPattern, cookie);
    const Exchange second = get(kAuditPattern, cookie);
    ASSERT_EQ(first.response->statusCode(), drogon::k404NotFound);
    ASSERT_EQ(second.response->statusCode(), drogon::k404NotFound);
    EXPECT_EQ(first.response->headers(), second.response->headers());
    EXPECT_EQ(first.response->body(), second.response->body());
}

// --- and the header that never goes back out ---------------------------------

TEST(TraceContextListener, NoResponseCarriesATraceHeaderEvenWhenOneWasSent) {
    // The rule `trace_context.h` states as absolute, asserted over a socket
    // rather than as a claim about the code that would have to write one. An
    // echoed `traceparent` is the same existence oracle as an `X-Request-Id` on
    // a stealth 404 — and unlike that one it would be the CLIENT'S own value
    // coming back, so a request that gets it echoed and a request that does not
    // are distinguishable by a client that sent the same header to both.
    //
    // All four shapes, because "no response carries it" is only worth asserting
    // if it covers the one a handler produced, the one a filter refused, the one
    // stealth dropped and the one the framework answered by itself.
    const std::string trace_header{http::kTraceparentHeader};
    const std::string sent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

    drogon::HttpRequestPtr allowed = drogon::HttpRequest::newHttpRequest();
    allowed->setMethod(drogon::Get);
    allowed->setPath("/session");
    allowed->addHeader("Cookie", editor_cookie());
    allowed->addHeader(trace_header, sent);
    const Exchange served = testfixture::send(allowed);
    ASSERT_EQ(served.response->statusCode(), drogon::k200OK);
    EXPECT_TRUE(served.response->getHeader(trace_header).empty());

    drogon::HttpRequestPtr denied = drogon::HttpRequest::newHttpRequest();
    denied->setMethod(drogon::Get);
    denied->setPath("/session");
    denied->addHeader(trace_header, sent);
    const Exchange refused = testfixture::send(denied);
    ASSERT_EQ(refused.response->statusCode(), drogon::k401Unauthorized);
    EXPECT_TRUE(refused.response->getHeader(trace_header).empty());

    drogon::HttpRequestPtr stealth = drogon::HttpRequest::newHttpRequest();
    stealth->setMethod(drogon::Get);
    stealth->setPath(std::string{kAuditPattern});
    stealth->addHeader("Cookie", editor_cookie());
    stealth->addHeader(trace_header, sent);
    const Exchange dropped = testfixture::send(stealth);
    ASSERT_EQ(dropped.response->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(dropped.response->getHeader(trace_header).empty());

    drogon::HttpRequestPtr unmatched = drogon::HttpRequest::newHttpRequest();
    unmatched->setMethod(drogon::Get);
    unmatched->setPath("/no-such-path-at-all");
    unmatched->addHeader(trace_header, sent);
    const Exchange absent = testfixture::send(unmatched);
    ASSERT_EQ(absent.response->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(absent.response->getHeader(trace_header).empty());

    // And the pair that carries the stealth claim: the two 404s stay
    // byte-identical in their headers with a trace header on the way in, which
    // is the property a conditional echo would break first.
    EXPECT_EQ(dropped.response->headers(), absent.response->headers());
}

// The two sides of the ingest decision, over a real socket.
//
// `install_request_scope` was given `TrustedPeer`, so the policy is not what
// distinguishes these two cases — the PEER is. The pair matters because either
// half alone is satisfied by a broken build: a wiring that never parses passes
// the first, and one that never checks the peer passes the second.

// The proxy list is process-wide, so it is put back whatever the case does.
// A case that left loopback trusted would make every later per-IP assertion in
// this binary read `X-Forwarded-For` from a client that supplied it.
class TrustLoopback final {
public:
    TrustLoopback() {
        auto proxies = std::make_shared<http::TrustedProxies>();
        EXPECT_TRUE(proxies->parse("127.0.0.1/32, ::1"));
        http::install_trusted_proxies(std::move(proxies));
    }
    ~TrustLoopback() { http::install_trusted_proxies(nullptr); }

    TrustLoopback(const TrustLoopback&) = delete;
    TrustLoopback& operator=(const TrustLoopback&) = delete;
};

[[nodiscard]] Exchange get_session_carrying_a_trace(std::string_view traceparent) {
    g_seen_trace.store(false, std::memory_order_release);
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(std::string{kSessionPattern});
    req->addHeader("Cookie", editor_cookie());
    req->addHeader(std::string{http::kTraceparentHeader}, std::string{traceparent});
    return testfixture::send(req);
}

constexpr std::string_view kInboundTrace =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

TEST(TraceContextListener, AnEdgeProcessBelievesNoTraceItIsSent) {
    // No proxy list installed, which is the ordinary state of this binary and
    // the state a genuinely edge-facing deployment runs in. The header is
    // whatever the client typed, and a chosen trace id collides with a real
    // trace in whatever is storing them.
    const Exchange exchange = get_session_carrying_a_trace(kInboundTrace);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);
    EXPECT_FALSE(g_seen_trace.load(std::memory_order_acquire))
        << "a client-chosen trace id was believed by a process with no hop to "
           "have heard it from";
}

TEST(TraceContextListener, ATraceFromATrustedHopReachesTheHandler) {
    const TrustLoopback trusted;

    const Exchange exchange = get_session_carrying_a_trace(kInboundTrace);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);
    ASSERT_TRUE(g_seen_trace.load(std::memory_order_acquire))
        << "nothing was ingested from a hop the policy trusts, so the parse is "
           "wired to nothing";

    // The bytes, not just the flag. A wiring that minted its own id would set
    // the flag and correlate to nothing, which is the failure that looks most
    // like success.
    const std::array<std::uint8_t, 16> expected = {0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3,
                                                   0x4d, 0xa6, 0xa3, 0xce, 0x92, 0x9d,
                                                   0x0e, 0x0e, 0x47, 0x36};
    EXPECT_EQ(g_seen_trace_id, expected);

    // And it still does not come back. The one case where a response could
    // plausibly echo is the one where there was something to echo.
    EXPECT_TRUE(
        exchange.response->getHeader(std::string{http::kTraceparentHeader}).empty());
}

TEST(TraceContextListener, AMalformedHeaderFromATrustedHopIsStillRefused) {
    // A hop being trusted is a statement about who sent the header, not about
    // whether it is a `traceparent`. The parser is the second gate and it stays
    // the second gate.
    const TrustLoopback trusted;

    const Exchange exchange = get_session_carrying_a_trace("00-nonsense-nonsense-zz");
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);
    EXPECT_FALSE(g_seen_trace.load(std::memory_order_acquire));
}

// --- an upgrade, through the filters anvil attaches to one -------------------
//
// These are here rather than in websocket_route_test.cc for the reason the
// request-id cases are: this is the only file in the binary that owns a
// credential, a filter and a Stealth route, and the properties below need all
// three. That file asserts what Drogon's ROUTER does with an upgrade; this one
// asserts what anvil does with it.

[[nodiscard]] std::string header_block(std::string_view origin, std::string_view cookie) {
    std::string block;
    if (!origin.empty()) { block.append("Origin: ").append(origin).append("\r\n"); }
    if (!cookie.empty()) { block.append("Cookie: ").append(cookie).append("\r\n"); }
    return block;
}

// Everything but the `Date:` line, which moves between two responses a second
// apart and is the only part of a refusal that is allowed to.
[[nodiscard]] std::string without_the_clock(std::string_view response) {
    std::string kept;
    kept.reserve(response.size());
    std::size_t at = 0;
    while (at < response.size()) {
        const std::size_t end = response.find("\r\n", at);
        const std::string_view line =
            response.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (line.size() < 5 || (line.substr(0, 5) != "Date:" && line.substr(0, 5) != "date:")) {
            kept.append(line);
            kept.append("\n");
        }
        if (end == std::string_view::npos) { break; }
        at = end + 2;
    }
    return kept;
}

// Where the HTTP message ends: the head, plus exactly the body its
// `Content-Length` declares. Anything past it is not part of the response.
[[nodiscard]] std::size_t end_of_message(std::string_view response) {
    const std::size_t head_end = response.find("\r\n\r\n");
    if (head_end == std::string_view::npos) { return response.size(); }

    const std::string_view head = response.substr(0, head_end);
    const std::size_t at = head.find("content-length: ");
    if (at == std::string_view::npos) { return response.size(); }

    const std::string_view rest = head.substr(at + 16);
    const std::string digits{rest.substr(0, rest.find("\r\n"))};
    if (digits.empty()) { return response.size(); }
    return std::min(response.size(), head_end + 4 + std::stoul(digits));
}

// What the server sent AFTER the response it declared. Nothing, or a tell.
//
// This is the assertion that was impossible while `raw_handshake` read the
// socket once: a four-byte WebSocket close frame written by
// `~WebSocketConnectionImpl` is visible to a single `recv` only when TCP
// coalesced it into the same segment, so the same server behaviour measured
// 0 of 40 and 12 of 40 depending on segmentation. Reading until the peer goes
// quiet makes it a fact rather than a frequency.
[[nodiscard]] std::string_view bytes_after_the_message(std::string_view response) {
    return response.substr(end_of_message(response));
}

// Hex, because the thing this file asserts the absence of is `88 02 03 e8` and a
// failure that printed it as text would print two replacement characters.
[[nodiscard]] std::string as_hex(std::string_view bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(kDigits[byte >> 4U]);
        out.push_back(kDigits[byte & 0x0FU]);
        out.push_back(' ');
    }
    return out;
}

// The HTTP message alone, with the clock line dropped: the only part of a
// refusal that is allowed to differ between two responses a second apart.
[[nodiscard]] std::string http_message_only(std::string_view response) {
    const std::size_t head_end = response.find("\r\n\r\n");
    if (head_end == std::string_view::npos) { return std::string{response}; }

    const std::string_view head = response.substr(0, head_end);
    return std::string{without_the_clock(head)} + "\n" +
           std::string{response.substr(head_end + 4, end_of_message(response) - head_end - 4)};
}

[[nodiscard]] bool carries_a_websocket_header(std::string_view response) {
    // Case-insensitively, because the tell is the FIELD being present and a
    // framework may spell it however it likes.
    std::string lowered{response};
    for (char& c : lowered) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return lowered.find("sec-websocket-") != std::string::npos;
}

TEST(UpgradeListener, AHandshakeFromAnAllowedOriginWithACredentialIsAccepted) {
    // The positive case, and without it every refusal below is satisfied by a
    // route that refuses everything.
    const std::string response =
        testfixture::raw_handshake(kFeedSocketPattern, header_block(kAllowedOrigin, editor_cookie()));
    ASSERT_FALSE(response.empty()) << "no response to the handshake at all";
    EXPECT_NE(testfixture::status_line(response).find("101"), std::string_view::npos) << response;
}

TEST(UpgradeListener, AHandshakeFromAnotherOriginIsRefusedEvenWithAValidCredential) {
    // The hole `OriginRequirement::Always` exists to close, over a socket. The
    // cookie here is GOOD — that is the point. Same-origin policy does not
    // constrain WebSockets and the browser attaches the cookie anyway, so
    // without this check a page on any origin could open an authenticated socket
    // against this route.
    const std::string response = testfixture::raw_handshake(
        kFeedSocketPattern, header_block("https://evil.test", editor_cookie()));
    ASSERT_FALSE(response.empty());
    EXPECT_NE(testfixture::status_line(response).find("403"), std::string_view::npos) << response;
    EXPECT_EQ(testfixture::status_line(response).find("101"), std::string_view::npos)
        << "a cross-origin handshake was upgraded";
}

TEST(UpgradeListener, AnAbsentOriginIsARefusalAndNotAPass) {
    // "Absent means not a browser, so it cannot be CSRF" is false, and treating
    // absence as safe turns the whole check into an opt-out an attacker takes by
    // stripping a header.
    const std::string response =
        testfixture::raw_handshake(kFeedSocketPattern, header_block("", editor_cookie()));
    ASSERT_FALSE(response.empty());
    EXPECT_NE(testfixture::status_line(response).find("403"), std::string_view::npos) << response;
}

TEST(UpgradeListener, TheOriginCheckRunsBeforeAnythingReadsACredential) {
    // Pipeline order, asserted by its observable consequence: a cross-origin
    // handshake with NO cookie is refused as 403 and not as 401. If the access
    // filter ran first it would answer 401, which tells a cross-origin caller
    // that signing in is the thing standing between it and the socket — and it
    // is not.
    const std::string response =
        testfixture::raw_handshake(kFeedSocketPattern, header_block("https://evil.test", ""));
    ASSERT_FALSE(response.empty());
    EXPECT_NE(testfixture::status_line(response).find("403"), std::string_view::npos) << response;
}

TEST(UpgradeListener, AGoodOriginStillLeavesTheAccessFilterToDecide) {
    // The origin check is a CSRF control and not an authorization one. A
    // handshake from an allowed origin with no credential must still be refused,
    // or the filter anvil attached is decorative.
    const std::string response =
        testfixture::raw_handshake(kFeedSocketPattern, header_block(kAllowedOrigin, ""));
    ASSERT_FALSE(response.empty());
    EXPECT_NE(testfixture::status_line(response).find("401"), std::string_view::npos) << response;
}

TEST(UpgradeListener, ARefusedStealthUpgradeIsTheUnmatchedAnswerExactly) {
    // The rule that costs one extra line and is the whole reason a stealth
    // upgrade route exists in this table. A `Sec-WebSocket-*` header on a
    // refused handshake and none on an unmatched path is an existence oracle,
    // exactly like `WWW-Authenticate` on a denied request — and a handshake
    // response is the easiest place in the system to add one by accident.
    //
    // Read to the end of what the server says rather than to the end of one
    // segment, because the tell this case missed for a whole phase arrived after
    // the response rather than in it.
    const std::string refused = testfixture::raw_handshake_all(
        kAuditSocketPattern, header_block("https://evil.test", editor_cookie()));
    ASSERT_FALSE(refused.empty());
    EXPECT_NE(testfixture::status_line(refused).find("404"), std::string_view::npos) << refused;
    EXPECT_FALSE(carries_a_websocket_header(refused)) << refused;

    // Denied by the access filter rather than by the origin check — a good
    // origin, a valid credential, and no `AuditRead` bit — must answer the same
    // bytes, or the two refusals are separable from each other.
    const std::string denied = testfixture::raw_handshake_all(
        kAuditSocketPattern, header_block(kAllowedOrigin, editor_cookie()));
    ASSERT_FALSE(denied.empty());
    EXPECT_NE(testfixture::status_line(denied).find("404"), std::string_view::npos) << denied;
    EXPECT_FALSE(carries_a_websocket_header(denied)) << denied;

    // And an upgrade to a path that does not exist at all.
    const std::string absent = testfixture::raw_handshake_all(
        "/ws/no-such-socket", header_block(kAllowedOrigin, editor_cookie()));
    ASSERT_FALSE(absent.empty());
    EXPECT_NE(testfixture::status_line(absent).find("404"), std::string_view::npos) << absent;
    EXPECT_FALSE(carries_a_websocket_header(absent)) << absent;

    // Byte for byte, not merely the same status. A refusal that differed from an
    // unmatched route by a header, a body or a reason phrase would separate the
    // two just as surely as a `Sec-WebSocket-*` field would.
    EXPECT_EQ(http_message_only(refused), http_message_only(absent));
    EXPECT_EQ(http_message_only(denied), http_message_only(absent));

    // And the whole stream, which is the assertion the message comparison above
    // cannot make: `88 02 03 e8` after a refusal and nothing after an unmatched
    // path is the same oracle one layer down.
    EXPECT_EQ(refused.size(), absent.size())
        << "refused: " << as_hex(refused) << "\nabsent:  " << as_hex(absent);
    EXPECT_EQ(denied.size(), absent.size())
        << "denied: " << as_hex(denied) << "\nabsent: " << as_hex(absent);
}

TEST(UpgradeListener, NothingFollowsARefusedUpgradeThatHadToBeIndistinguishable) {
    // The four bytes, asserted directly rather than as a size comparison, so a
    // failure names them.
    //
    // `~WebSocketConnectionImpl` writes a WebSocket close frame onto a socket
    // that is still connected, and every refusal a FILTER makes arrives after
    // that object exists. `accesscontrol/upgrade_gate.h` answers these three
    // from a sync advice instead — the one point in Drogon's pipeline before
    // `make_shared<WebSocketConnectionImpl>` — so there is no object to destroy
    // and nothing to write. That is a property of construction, not a race, and
    // this case is allowed to be exact about it.
    const std::array<std::pair<std::string_view, std::string>, 3> cases{{
        {"a cross-origin handshake to a stealth route",
         testfixture::raw_handshake_all(kAuditSocketPattern,
                                        header_block("https://evil.test", editor_cookie()))},
        {"a handshake to a stealth route without the bit",
         testfixture::raw_handshake_all(kAuditSocketPattern,
                                        header_block(kAllowedOrigin, editor_cookie()))},
        {"a handshake to a path that does not exist",
         testfixture::raw_handshake_all("/ws/no-such-socket",
                                        header_block(kAllowedOrigin, editor_cookie()))},
    }};

    for (const auto& [what, response] : cases) {
        ASSERT_FALSE(response.empty()) << what;
        EXPECT_EQ(bytes_after_the_message(response), std::string_view{})
            << what << " was followed by " << as_hex(bytes_after_the_message(response));
    }
}

TEST(UpgradeListener, ARefusedStealthUpgradeIsStillOnTheSecurityLog) {
    // The half of a denial that is not the response, asserted where the response
    // moved. `AccessFilter::deny` answers and then records; the gate answers by
    // RETURNING, so it cannot record in the same order and queues the row to run
    // once the event loop has written the refusal. A queue nobody drains and a
    // branch that forgot to record look identical from the client's side, which
    // is the whole reason this case exists.
    const int before = denials().count.load(std::memory_order_acquire);

    const std::string denied = testfixture::raw_handshake_all(
        kAuditSocketPattern, header_block(kAllowedOrigin, editor_cookie()));
    ASSERT_NE(testfixture::status_line(denied).find("404"), std::string_view::npos) << denied;

    // The handshake helper already waits out 250 ms of silence, which is orders
    // of magnitude more than a queued functor needs — the poll is here so a
    // loaded machine fails slowly rather than falsely.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (denials().count.load(std::memory_order_acquire) > before) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    EXPECT_EQ(denials().count.load(std::memory_order_acquire), before + 1)
        << "a stealth upgrade was refused without a record of it";
    EXPECT_TRUE(denials().last_stealthed.load(std::memory_order_relaxed))
        << "the row must say the client saw a 404, or the log cannot be read";
    // The TRUE code, which is the entire value of the row: the client was told
    // the route does not exist and the record has to say it does and why.
    EXPECT_EQ(denials().last_code.load(std::memory_order_relaxed),
              static_cast<int>(ErrorCode::Forbidden));
}

TEST(UpgradeListener, AnUpgradeToAnOrdinaryRouteIsTheUnmatchedAnswer) {
    // Deny by default, applied to the transport. Before the gate this went to
    // Drogon's WebSocket router, which found no entry and answered its own
    // not-found branch — a branch that mutates the per-IO-thread copy of the
    // shared 404 on its way past, so the FIRST unmatched upgrade a thread
    // handled gave every later 404 from that thread a `Connection: close` no
    // other thread's 404 had.
    //
    // `/audit` is a real, registered, Stealth HTTP route. An upgrade to it is
    // not a route this library serves, and it answers exactly as a path that was
    // never there does.
    const std::string upgraded = testfixture::raw_handshake_all(
        kAuditPattern, header_block(kAllowedOrigin, editor_cookie()));
    const std::string absent = testfixture::raw_handshake_all(
        "/ws/no-such-socket", header_block(kAllowedOrigin, editor_cookie()));
    ASSERT_FALSE(upgraded.empty());
    ASSERT_FALSE(absent.empty());

    EXPECT_EQ(http_message_only(upgraded), http_message_only(absent));
    EXPECT_EQ(bytes_after_the_message(upgraded), std::string_view{})
        << as_hex(bytes_after_the_message(upgraded));
}

TEST(UpgradeListener, ARefusalOnARouteThatIsNotSecretKeepsItsRequestId) {
    // The scoping decision in `upgrade_gate.h`, asserted by its consequence.
    //
    // A sync advice runs before `install_request_scope()` has minted an id and
    // its response never reaches the advice that emits one, so a 401 answered
    // from the gate would lose the value that joins a user saying "I cannot get
    // in" to the row that says why. That is why the gate takes only the refusals
    // that must carry NO id — the stealth 404s — and leaves every other class to
    // the filters. A gate that quietly widened to cover this route would show up
    // here as an id that stopped being issued.
    const std::string refused =
        testfixture::raw_handshake_all(kFeedSocketPattern, header_block(kAllowedOrigin, ""));
    ASSERT_FALSE(refused.empty());
    EXPECT_NE(testfixture::status_line(refused).find("401"), std::string_view::npos) << refused;
    EXPECT_TRUE(contains(refused, "x-request-id")) << refused;
    EXPECT_TRUE(contains(refused, "\"request_id\":\"")) << refused;
}

TEST(UpgradeListener, TheOriginRefusalOnAStealthRouteIsAlsoTheUnmatchedAnswer) {
    // The case that isolates the origin filter's own stealth branch. Every
    // refusal above would have been a 404 anyway, because the access filter
    // denies that holder — so none of them proves the ORIGIN check answers the
    // stealth way.
    //
    // A superadmin satisfies the route, so the access filter would let this
    // handshake through. The only thing refusing it is the origin, and if that
    // refusal were the ordinary 403 the route's existence would be disclosed by
    // any cross-origin page that asked.
    const std::string cookie = session_cookie(PermSet{}, UserType::SuperAdmin, 1);
    const std::string allowed_through =
        testfixture::raw_handshake(kAuditSocketPattern, header_block(kAllowedOrigin, cookie));
    ASSERT_FALSE(allowed_through.empty());
    ASSERT_NE(testfixture::status_line(allowed_through).find("101"), std::string_view::npos)
        << "this holder must be one the access filter admits, or the case below "
           "proves nothing about the origin check: " << allowed_through;

    const std::string refused = testfixture::raw_handshake_all(
        kAuditSocketPattern, header_block("https://evil.test", cookie));
    ASSERT_FALSE(refused.empty());
    EXPECT_NE(testfixture::status_line(refused).find("404"), std::string_view::npos) << refused;
    EXPECT_EQ(testfixture::status_line(refused).find("403"), std::string_view::npos)
        << "a cross-origin refusal disclosed that the stealth route exists";
    EXPECT_FALSE(carries_a_websocket_header(refused)) << refused;

    const std::string absent = testfixture::raw_handshake_all(
        "/ws/no-such-socket", header_block("https://evil.test", cookie));
    ASSERT_FALSE(absent.empty());
    EXPECT_EQ(http_message_only(refused), http_message_only(absent));
    EXPECT_EQ(bytes_after_the_message(refused), std::string_view{})
        << as_hex(bytes_after_the_message(refused));
}

TEST(UpgradeRegistration, ARouteTheTableDoesNotNameIsRefusedAtBoot) {
    // The same rule `register_route` enforces, and for the same reason: a route
    // the filter would deny every request to is a registration bug, and a boot
    // failure naming the pattern is cheaper than a socket nobody can open.
    EXPECT_THROW(ac::register_websocket_route(testapp::kRoutes, "/ws/undeclared",
                                              FeedSocket::classTypeName()),
                 std::logic_error);
}

TEST(UpgradeRegistration, AnUppercasePatternIsRefusedAtBoot) {
    // Drogon keys its WebSocket map by the lowercased path and reports that
    // spelling back as the matched pattern, so a pattern with a capital in it is
    // registered under one name and looked up under another — which denies every
    // upgrade through the filter's closed, silent `policy == nullptr` branch.
    // Refused here, where the message can say why, rather than discovered there.
    //
    // It is refused BEFORE the table lookup, so it fails for the reason it is
    // wrong rather than because the lowercase spelling happens to be absent.
    EXPECT_THROW(ac::register_websocket_route(testapp::kRoutes, "/WS/Feed",
                                              FeedSocket::classTypeName()),
                 std::logic_error);
    EXPECT_FALSE(ac::upgrade_pattern_survives_routing("/WS/Feed"));
    EXPECT_TRUE(ac::upgrade_pattern_survives_routing(std::string{kFeedSocketPattern}));
}

TEST(SessionListener, TheTagIsNotKeyedToTheEpochAlone) {
    // Two holders at the same epoch share the epoch half of the tag, so the
    // table half is what stops a build that moved a route from serving a 304
    // against yesterday's map. Asserted as a property of the tag rather than by
    // rebuilding: the table half is present, stable across calls, and not empty.
    const std::string first = etag_for(1);
    EXPECT_EQ(first, etag_for(1)) << "a tag that is not stable is a revalidation that never hits";
    EXPECT_NE(first, etag_for(2));
    EXPECT_FALSE(table_id().empty());
    EXPECT_TRUE(contains(first, table_id()));
}

}  // namespace
}  // namespace anvil
