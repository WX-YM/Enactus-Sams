#pragma once

// The response envelope for a page assembled from partially-trusted content
// (docs/19-server-side-rendering.md §7).
//
// Every header here is set on EVERY such response, together, from one place.
// Five headers spread across the handlers that happen to remember them is four
// chances to ship a page without one, and the one that is missing is invisible
// until it matters.
//
// --- no script-src, because there is no script -------------------------------
//
// A page that renders content for review needs no JavaScript, so the strongest
// possible policy is also the correct one — and a policy with no script source
// needs no nonce, which is why nothing here has one. A nonce is a mechanism for
// pages that run scripts, and it forbids caching besides.
//
// The directive is ABSENT rather than `script-src 'none'`. With no script-src,
// `default-src 'none'` governs script the moment anything is added; with an
// explicit one, a later edit that gives it a source silently stops inheriting
// the default. Both spellings block script today and they differ in what the
// next change does.
//
// --- no-store is not a caching decision to revisit ---------------------------
//
// The content is unpublished and the URL is a capability; a shared cache
// holding either is the leak the whole envelope exists to prevent.
// `no-referrer` keeps the capability out of a third party's logs.

#include <string>
#include <string_view>

#include <drogon/HttpResponse.h>

namespace anvil::http {

// Built ONCE at boot and held by the application: the only variable in it is
// CONTENT_ORIGIN, which is deployment configuration, so a per-response rebuild
// would be a string assembled thousands of times to produce the same bytes.
//
// `content_origin` is boot configuration and never request data. It is still
// checked here, because a value carrying a `;` or a quote would not be a broken
// origin — it would be an appended directive, and a policy an operator typo can
// rewrite is not a policy. A value that fails emits `img-src 'none'`, which
// fails closed: a review page with no pictures, rather than a review page with
// no policy.
[[nodiscard]] std::string content_security_policy(std::string_view content_origin);

// Content-Type, the policy, and the four headers that keep the page out of
// shared caches, out of search results, out of a third party's referrer log and
// out of a sniffing parser.
void apply_content_headers(drogon::HttpResponse& response, std::string_view policy);

}  // namespace anvil::http
