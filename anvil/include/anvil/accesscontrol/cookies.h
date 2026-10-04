#pragma once

// Cookie names, attributes, and an allocation-free reader.
//
// The reader exists because Drogon's `req->getCookie(name)` takes a
// `const std::string&`: calling it means constructing a std::string per lookup,
// on a path whose whole budget is 15 microseconds and whose DENY path must
// allocate nothing at all (docs/04-access-control.md §4). Scanning the raw
// header is a pointer walk over bytes we already have.
//
// --- On the `__Host-` prefix (docs/05-auth-sessions.md §2) ---
//
// The design scopes the refresh cookie to `Path=/auth` while also naming it
// `__Host-rt`. Those two are mutually exclusive: RFC 6265bis §4.1.3.2 requires a
// `__Host-` cookie to carry `Secure`, to carry NO `Domain`, and to have
// `Path=/`. A browser rejects `__Host-rt; Path=/auth` outright, so the design as
// written produces a refresh cookie that is never stored and a login flow that
// silently cannot refresh.
//
// The intent behind each half is real, and they cannot both be had:
//
//   `__Host-` prefix   host-only scope. A compromised sibling subdomain cannot
//                      SET this cookie. Losing it means an attacker who
//                      controls any subdomain can plant their own refresh token
//                      in a victim's browser — session fixation, where the
//                      victim ends up operating inside the attacker's account.
//   `Path=/auth`       the refresh token is not transmitted on the hundreds of
//                      ordinary API calls that cannot use it. This is exposure
//                      reduction and a bytes-per-request saving.
//
// Security decides (CLAUDE.md, priority order). The prefix is kept and the path
// scoping is given up: `__Host-rt` with `Path=/`. The refresh token remains
// HttpOnly and Secure, so the cost is that it accompanies requests that will
// not use it — not that it becomes readable.

#include <cstddef>
#include <string_view>

namespace anvil::accesscontrol {

// Both carry the __Host- prefix, so both are host-only and neither can be set
// by a subdomain.
inline constexpr std::string_view kAccessCookieName = "__Host-at";
inline constexpr std::string_view kRefreshCookieName = "__Host-rt";

// SameSite=Lax on the access cookie so an ordinary top-level navigation to a
// server-rendered page still authenticates; Strict on refresh, which is only
// ever used by the SPA's own fetch.
inline constexpr std::string_view kAccessCookieSameSite = "Lax";
inline constexpr std::string_view kRefreshCookieSameSite = "Strict";

// --- the preview credential ---
//
// The preview route required a UserContext, which comes from `__Host-at`.
// That cookie is host-only and the preview URL points at CONTENT_ORIGIN, which
// config FORCES to differ from SITE_ORIGIN. The cookie never arrived, `ctx` was
// null, and every preview render answered the stealth 404 — the feature has
// never worked in the documented deployment.
//
// The separate origin is not the defect and is not the fix: it is what contains
// the blast radius, since a preview renders staff-authored content that has not
// been through the publish path. What was missing is a credential that can
// reach that origin.
//
// Deliberately NOT `__Host-`, and that is the one attribute here worth arguing
// about. `__Host-` requires `Path=/`, and this cookie's entire value is that it
// is scoped to ONE draft: a path covering that subject alone means the browser
// will not attach it to a request for any other preview, so a second layer sits
// under the server-side subject binding. The property `__Host-` buys — that a sibling
// subdomain cannot set the cookie — is worth less here than the path scoping,
// because this cookie carries no permission bits and authorises reads of a
// single draft that the holder was handed a link to anyway.
//
// Named for the MECHANISM. The old name was one application's abbreviation for
// one of its features, in a cookie this library sets on every deployment built
// on it (CLAUDE.md §1). A browser holding it is no longer recognised, and the
// recovery is the link the holder was handed — the token travels in the URL and
// the cookie is only what the redirect leaves behind.
//
// It was `hv-preview`.  // ban-exempt: named so a stranded deployment can grep it
inline constexpr std::string_view kPreviewCookieName = "preview-cap";

// Lax, not Strict. The preview link is followed from the admin dashboard, which
// is a DIFFERENT origin by construction, and the redirect that sets this cookie
// is the tail of that cross-site navigation. Strict would decline to send it on
// the very next request and the preview would 404 once, then work on reload —
// which is worse than either consistent outcome.
inline constexpr std::string_view kPreviewCookieSameSite = "Lax";

// The query parameter carrying the token on the link handed out at creation.
// The redirect that consumes it strips it from the address bar, so the token
// does not sit in browser history or in a copied URL after the first load.
inline constexpr std::string_view kPreviewTokenParameter = "k";

// A hostile client can send a megabyte of Cookie header. Nothing here is
// permitted to be proportional to that: the scan stops at this many bytes and
// the token itself is length-checked before it is decoded.
inline constexpr std::size_t kMaxCookieHeaderBytes = 8192;

// Returns a view INTO `cookie_header`, valid exactly as long as it is. Empty
// when the cookie is absent, when the header is over the cap, or when the value
// is empty.
//
// Deliberately strict: no unescaping, no quoted-string handling, no
// whitespace-inside-value tolerance. Both cookies this reads are base64url
// alphabets we generated, so any input needing lenient parsing is not one of
// ours, and a lenient cookie parser is a parser-differential vector.
[[nodiscard]] std::string_view read_cookie(std::string_view cookie_header,
                                           std::string_view name) noexcept;

}  // namespace anvil::accesscontrol
