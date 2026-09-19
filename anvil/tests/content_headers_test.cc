// The envelope, asserted on a real response rather than on a constant. A test
// that compares two string literals proves the two literals match; what has to
// be true is that a response carrying assembled HTML leaves with every one of
// these headers on it.

#include "anvil/http/content_headers.h"

#include <string>
#include <string_view>

#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include "anvil/http/html_writer.h"

namespace anvil::http {
namespace {

constexpr std::string_view kContentOrigin = "https://content.example.test";

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] drogon::HttpResponsePtr rendered_response() {
    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setBody("<!doctype html><p>body</p>");
    apply_content_headers(*response, content_security_policy(kContentOrigin));
    return response;
}

TEST(ContentHeaders, ThePolicyCarriesNoScriptSourceAtAll) {
    // Asserted by ABSENCE, not by a substring match on the rest. An
    // accidentally added `script-src 'none'` would pass a loose check while
    // changing what the policy means the moment it gains a source: with no
    // script-src, `default-src 'none'` governs script; with an explicit one, it
    // no longer does.
    const std::string policy = content_security_policy(kContentOrigin);
    EXPECT_FALSE(contains(policy, "script-src"));
    EXPECT_TRUE(contains(policy, "default-src 'none'"));
}

TEST(ContentHeaders, ThePolicyNamesTheContentOriginAsTheOnlyImageSource) {
    const std::string policy = content_security_policy(kContentOrigin);
    EXPECT_TRUE(contains(policy, "img-src https://content.example.test;"));
    EXPECT_TRUE(contains(policy, "frame-ancestors 'none'"));
    EXPECT_TRUE(contains(policy, "base-uri 'none'"));
    EXPECT_TRUE(contains(policy, "form-action 'none'"));
    EXPECT_TRUE(contains(policy, "style-src 'self'"));
    EXPECT_TRUE(contains(policy, "font-src 'self'"));
}

TEST(ContentHeaders, AnOriginThatCouldEndTheDirectiveFailsClosed) {
    // An operator typo that appended a directive would be a policy rewritten by
    // configuration. A page with no pictures is the failure to have.
    for (const std::string_view hostile :
         {std::string_view{"https://x.test; script-src *"},
          std::string_view{"https://x.test 'unsafe-inline'"},
          std::string_view{"https://x.test,https://y.test"}, std::string_view{""}}) {
        const std::string policy = content_security_policy(hostile);
        EXPECT_TRUE(contains(policy, "img-src 'none'")) << hostile;
        EXPECT_FALSE(contains(policy, "script-src")) << hostile;
        EXPECT_FALSE(contains(policy, "unsafe-inline")) << hostile;
    }
}

TEST(ContentHeaders, EveryHeaderIsOnTheResponse) {
    const drogon::HttpResponsePtr response = rendered_response();

    EXPECT_EQ(response->getHeader("Cache-Control"), "private, no-store");
    EXPECT_EQ(response->getHeader("X-Robots-Tag"), "noindex, nofollow");
    EXPECT_EQ(response->getHeader("Referrer-Policy"), "no-referrer");
    EXPECT_EQ(response->getHeader("X-Content-Type-Options"), "nosniff");
    EXPECT_FALSE(response->getHeader("Content-Security-Policy").empty());
    EXPECT_EQ(response->contentTypeString(), kHtmlContentType);
}

TEST(ContentHeaders, TheResponseIsNotStorableByAnyCache) {
    // no-store rather than no-cache: the content is unpublished and the URL is
    // a capability, so a shared cache must not hold either at all.
    const drogon::HttpResponsePtr response = rendered_response();
    const std::string cache_control = response->getHeader("Cache-Control");
    EXPECT_TRUE(contains(cache_control, "no-store"));
    EXPECT_TRUE(contains(cache_control, "private"));
}

}  // namespace
}  // namespace anvil::http
