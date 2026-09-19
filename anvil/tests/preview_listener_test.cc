// The end-to-end case, and the reason this binary exists.
//
// The preview route's render path once required a UserContext derived from
// `__Host-at`. That cookie is host-only, the preview URL points at
// CONTENT_ORIGIN, and config FORCES that to be a different host — so the cookie
// never arrived, the context was null, and every preview answered the stealth
// 404. The feature had never worked in the documented deployment, and every
// test at the time was correct: they drove the repository or the service, and
// neither of those sees a cookie.
//
// So the assertion has to be made against a REAL listener, over a real socket,
// with the credentials a browser would and would not send
// (docs/19-server-side-rendering.md §6, docs/16-test-plan.md phase 10).
//
// --- what is simulated, and what is not -------------------------------------
//
// One process cannot hold two registrable hosts, and it does not need to: the
// only thing the server observes about the origin split is WHICH COOKIES ARRIVE.
// A request to the content origin carries the path-scoped capability cookie and
// no session cookie, and a request that carries a session cookie instead is the
// production defect in its original shape. Both are sent here exactly as a
// browser would send them.
//
// The credential store is a stand-in rather than identity::CapabilityService,
// for two reasons: that service lives in anvil::app and this binary links
// anvil::platform by design, and its own behaviour — the single find_one_and_update,
// the scope and subject binding, the expiry filter — already has a suite against
// a live cluster. What has never been exercised anywhere is the path between the
// socket and it.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <drogon/Cookie.h>
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>

#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/types.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/http/content_headers.h"
#include "anvil/http/html_writer.h"
#include "anvil/input/html.h"

#include "listener_fixture.h"
#include "routes.h"

namespace anvil {
namespace {

namespace ac = accesscontrol;

constexpr std::string_view kContentOrigin = "https://content.example.test";
constexpr std::string_view kPreviewPattern = "/preview/{id}";

[[nodiscard]] std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// --- the credential ---------------------------------------------------------

// What a capability row holds: a digest, never the token. A raw token in a
// collection is a credential anybody with a dump can replay.
struct StoredCapability final {
    crypto::Digest256 hash;
    Uuid              subject;
    std::int64_t      expires_at_ms;
};

// Deliberately NOT a map keyed by the token: a lookup by digest is what the real
// repository does, and the binding that matters — the subject — is checked
// rather than assumed, so a token for draft A presented against draft B matches
// nothing.
class CredentialStore final {
public:
    [[nodiscard]] std::string issue(const Uuid& subject, std::int64_t expires_at_ms) {
        std::string token = crypto::random_token();
        rows_.push_back(StoredCapability{crypto::sha256_with_pepper(token, pepper_), subject,
                                         expires_at_ms});
        return token;
    }

    // The explicit expiry predicate. A TTL index is a garbage collector and not
    // an access control: the monitor runs roughly every 60 seconds, so an
    // expired row is still readable and a query that leaves the comparison to
    // the reaper is a credential that outlives its lifetime by up to a minute
    // (docs/09-mongodb.md §6).
    [[nodiscard]] bool verify(std::string_view token, const Uuid& subject,
                              std::int64_t now) const {
        if (token.empty() || token.size() > 128) { return false; }
        const crypto::Digest256 presented = crypto::sha256_with_pepper(token, pepper_);
        for (const StoredCapability& row : rows_) {
            if (!crypto::secure_equal(row.hash, presented)) { continue; }
            if (row.subject != subject) { return false; }
            return row.expires_at_ms > now;
        }
        return false;
    }

private:
    std::vector<StoredCapability> rows_;
    crypto::Digest256             pepper_ = crypto::random_array<32>();
};

// --- what the process under test is serving ---------------------------------

struct Fixture final {
    CredentialStore store;
    Uuid            subject;
    std::string     token;
    std::string     expired_token;
    std::string     policy;
};

[[nodiscard]] Fixture& fixture() {
    static Fixture state = [] {
        Fixture created{};
        created.subject = uuid::generate_v4();
        created.token = created.store.issue(created.subject, now_ms() + 600000);
        // Expired by an hour: far enough past that no clock skew explains it,
        // and still PRESENT in the store, so a test that passes proves the
        // predicate refused it rather than the row having been reaped.
        created.expired_token = created.store.issue(created.subject, now_ms() - 3600000);
        created.policy = http::content_security_policy(kContentOrigin);
        return created;
    }();
    return state;
}

// The stored draft, as it would come back out of the database: already
// sanitised on write, and re-sanitised here on the way out.
[[nodiscard]] std::string stored_body() { return R"(<p>Draft <strong>body</strong></p>)"; }

[[nodiscard]] drogon::HttpResponsePtr render(std::string_view title, std::string_view body) {
    const input::SanitizedHtml safe =
        input::sanitize_rich_text(body, input::HtmlPolicy{kContentOrigin, 20000, 16});

    std::string page;
    page.reserve(512 + body.size());
    page.append("<!doctype html><html><head><title>");
    http::append_html_text(page, title);
    page.append("</title></head><body><h1");
    http::append_html_attr(page, "class", "preview");
    // The same value through both escapers, which is how a page proves the two
    // sets differ where it matters rather than only in a unit test.
    http::append_html_attr(page, "title", title);
    page.append(">");
    http::append_html_text(page, title);
    page.append("</h1>");
    http::append_sanitized(page, safe);
    page.append("</body></html>");

    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setBody(std::move(page));
    http::apply_content_headers(*response, fixture().policy);
    return response;
}

void preview_handler(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id) {
    // Everything below answers with the SAME response object on failure, so a
    // leaked id, a missing credential and an expired one are byte-identical to
    // a route that does not exist.
    const std::optional<Uuid> subject = uuid::parse(id);
    if (!subject.has_value()) {
        callback(ac::not_found_response());
        return;
    }

    // getCookie, and NOT getHeader("cookie"): Drogon's parser splits the Cookie
    // field into its own map and never stores the field itself, so the header
    // reads empty on every request that carried one. That mistake was once a
    // total authentication outage, and it is exactly the kind of thing only a
    // real request can catch.
    const std::string& token = req->getCookie(std::string{ac::kPreviewCookieName});

    // A request carrying no credential is refused on a length compare, before
    // anything touches a store — so a leaked id gets the byte-identical 404 at
    // the same cost as a random one.
    if (token.empty()) {
        callback(ac::not_found_response());
        return;
    }
    if (!fixture().store.verify(token, *subject, now_ms())) {
        callback(ac::not_found_response());
        return;
    }

    callback(render("Ali's <draft>", stored_body()));
}

// The link handed out at creation. Following it sets the cookie and 303s to the
// clean URL, so the token leaves the address bar on the first load rather than
// sitting in browser history and in every copied URL.
void preview_link_handler(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          std::string id) {
    const std::string& token = req->getParameter(std::string{ac::kPreviewTokenParameter});
    const std::optional<Uuid> subject = uuid::parse(id);
    if (token.empty() || !subject.has_value() ||
        !fixture().store.verify(token, *subject, now_ms())) {
        callback(ac::not_found_response());
        return;
    }

    const std::string path = "/preview/" + id;
    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k303SeeOther);
    response->addHeader("Location", path);
    response->addHeader("Cache-Control", "private, no-store");

    // Not `__Host-`: that prefix requires Path=/, and the path scoping is worth
    // more here. The browser will not attach this to a request for any OTHER
    // subject, so a second layer sits under the server-side subject binding.
    drogon::Cookie cookie{std::string{ac::kPreviewCookieName}, token};
    cookie.setPath(path);
    cookie.setSecure(true);
    cookie.setHttpOnly(true);
    // Converted from the shipped constant rather than named again here, so the
    // attribute that lands is the one anvil declares.
    cookie.setSameSite(drogon::Cookie::convertString2SameSite(ac::kPreviewCookieSameSite));
    response->addCookie(std::move(cookie));
    callback(response);
}

// --- the listener -----------------------------------------------------------

// The routes this file contributes to the binary's one listener. It boots lazily
// from the first case that asks for a port, so a registrar at namespace scope is
// early enough (tests/listener_fixture.h).
void install_preview_routes() {
    drogon::app().registerHandler(std::string{kPreviewPattern}, &preview_handler,
                                  {drogon::Get});
    drogon::app().registerHandler("/preview-link/{id}", &preview_link_handler,
                                  {drogon::Get});
}

const testfixture::RouteRegistrar kRegistrar{&install_preview_routes};

// --- driving it -------------------------------------------------------------

using testfixture::Exchange;
using testfixture::get;

[[nodiscard]] std::string preview_path() {
    return "/preview/" + uuid::to_string(fixture().subject);
}

[[nodiscard]] std::string capability_cookie(std::string_view token) {
    return std::string{ac::kPreviewCookieName} + "=" + std::string{token};
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Every header plus the status and the body, so "byte-identical" is asserted
// against the whole response rather than against the part that was convenient.
[[nodiscard]] bool same_response(const drogon::HttpResponsePtr& a,
                                 const drogon::HttpResponsePtr& b) {
    return a->statusCode() == b->statusCode() && a->body() == b->body() &&
           a->contentTypeString() == b->contentTypeString() &&
           std::map<std::string, std::string>{a->headers().begin(), a->headers().end()} ==
               std::map<std::string, std::string>{b->headers().begin(), b->headers().end()};
}

// --- the cases --------------------------------------------------------------

TEST(PreviewListener, TheRouteIsDeclaredPublicAndEnforcedByItsHandler) {
    // The one route allowed to be Public with no permission bit, because the
    // credential it takes is not the one the access filter knows how to read.
    const ac::RoutePolicy* policy =
        ac::policy_for(testapp::kRoutes, kPreviewPattern, ac::RouteMethod::Get);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->access, ac::RouteAccess::Public);
    EXPECT_FALSE(policy->required.any());
}

TEST(PreviewListener, ACapabilityCookieAndNoSessionCookieRendersThePage) {
    // The case the production failure could not pass. No `__Host-at` here, and
    // there never would be one: it is host-only and this request is to a
    // different host.
    const Exchange exchange = get(preview_path(), capability_cookie(fixture().token));

    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_NE(exchange.response, nullptr);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    const std::string_view body = exchange.response->body();
    EXPECT_TRUE(contains(body, "<p>Draft <strong>body</strong></p>"));
    // One value, two contexts. In element text a quote is an ordinary
    // character; inside the attribute the writer quoted itself, it is not.
    EXPECT_TRUE(contains(body, ">Ali's &lt;draft&gt;</h1>"));
    EXPECT_TRUE(contains(body, R"HTML(title="Ali&#x27;s &lt;draft&gt;")HTML"));
    EXPECT_FALSE(contains(body, "<draft>"));
}

TEST(PreviewListener, TheRenderedPageArrivesInsideItsEnvelope) {
    const Exchange exchange = get(preview_path(), capability_cookie(fixture().token));
    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k200OK);

    EXPECT_EQ(exchange.response->getHeader("Cache-Control"), "private, no-store");
    EXPECT_EQ(exchange.response->getHeader("X-Robots-Tag"), "noindex, nofollow");
    EXPECT_EQ(exchange.response->getHeader("Referrer-Policy"), "no-referrer");
    EXPECT_EQ(exchange.response->getHeader("X-Content-Type-Options"), "nosniff");

    const std::string policy = exchange.response->getHeader("Content-Security-Policy");
    EXPECT_FALSE(policy.empty());
    EXPECT_FALSE(contains(policy, "script-src"));
}

TEST(PreviewListener, NoCredentialIsTheSameBytesAsNoRoute) {
    const Exchange no_cookie = get(preview_path(), "");
    const Exchange no_route = get("/no-such-path-at-all", "");

    ASSERT_EQ(no_cookie.result, drogon::ReqResult::Ok);
    ASSERT_EQ(no_route.result, drogon::ReqResult::Ok);
    EXPECT_EQ(no_cookie.response->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(same_response(no_cookie.response, no_route.response));
}

TEST(PreviewListener, ASessionCookieAloneAuthenticatesNothingHere) {
    // The production defect, in its original shape. A render path that wanted a
    // UserContext would have accepted this and rejected the one above — and the
    // deployment sends the opposite pair, which is why it never worked.
    const Exchange exchange =
        get(preview_path(), std::string{ac::kAccessCookieName} + "=" + fixture().token);

    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    EXPECT_EQ(exchange.response->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(same_response(exchange.response, get("/no-such-path-at-all", "").response));
}

TEST(PreviewListener, AnExpiredCapabilityIsRefusedByThePredicateNotTheReaper) {
    // The row is still in the store — nothing reaped it — so the refusal came
    // from the explicit expiry comparison. A TTL index lags by up to a minute,
    // which is a minute of a credential that has expired and still works.
    const Exchange exchange =
        get(preview_path(), capability_cookie(fixture().expired_token));

    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    EXPECT_EQ(exchange.response->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(same_response(exchange.response, get("/no-such-path-at-all", "").response));

    // And the same token, against a clock before its expiry, would have passed:
    // it is the predicate that refused it and not the token being unknown.
    EXPECT_TRUE(fixture().store.verify(fixture().expired_token, fixture().subject,
                                       now_ms() - 7200000));
}

TEST(PreviewListener, ACapabilityForOneSubjectDoesNotOpenAnother) {
    const Uuid other = uuid::generate_v4();
    const Exchange exchange =
        get("/preview/" + uuid::to_string(other), capability_cookie(fixture().token));

    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    EXPECT_EQ(exchange.response->statusCode(), drogon::k404NotFound);
}

TEST(PreviewListener, TheLinkSetsThePathScopedCookieAndRedirectsWithoutTheToken) {
    const std::string path =
        "/preview-link/" + uuid::to_string(fixture().subject) + "?" +
        std::string{ac::kPreviewTokenParameter} + "=" + fixture().token;
    const Exchange exchange = get(path, "");

    ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
    ASSERT_EQ(exchange.response->statusCode(), drogon::k303SeeOther);

    // The token leaves the address bar on the first load, so it does not sit in
    // browser history or in a copied URL.
    const std::string location = exchange.response->getHeader("Location");
    EXPECT_EQ(location, preview_path());
    EXPECT_FALSE(contains(location, fixture().token));

    // Read back through the parser rather than as a header substring: what has
    // to hold is what a client makes of the attributes, and Drogon splits
    // Set-Cookie out of the header map on the way in exactly as it does Cookie.
    const drogon::Cookie& cookie =
        exchange.response->getCookie(std::string{ac::kPreviewCookieName});
    EXPECT_EQ(cookie.getValue(), fixture().token);
    // Path-scoped to the ONE subject, which is what was traded for the
    // `__Host-` prefix: the browser will not attach it to any other preview.
    EXPECT_EQ(cookie.path(), preview_path());
    EXPECT_TRUE(cookie.isSecure());
    EXPECT_TRUE(cookie.isHttpOnly());
    EXPECT_EQ(cookie.sameSite(), drogon::Cookie::SameSite::kLax);
    EXPECT_FALSE(ac::kPreviewCookieName.starts_with("__Host-"));
}

}  // namespace
}  // namespace anvil
