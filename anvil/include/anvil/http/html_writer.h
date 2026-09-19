#pragma once

// Assembling HTML response bodies by appending into one std::string.
//
// It sits beside json_writer.h and csv_writer.h because it is the same kind of
// thing and shares their shape: append into one caller-owned string after a
// single reserve(), with every interpolated value escaped at the point it is
// written (docs/19-server-side-rendering.md §2). There is no template file, no
// grammar, no view catalogue and no page cache — the escaping is the part where
// a mistake is a vulnerability, and it is the only part this library holds.
//
// A page is one allocation: the handler reserves once, appends through these,
// and returns. No fragments, no per-field std::string, no operator+.
//
// --- the two contexts that do not exist -------------------------------------
//
// There is NO JavaScript escaper and NO CSS escaper here, not even a
// best-effort one. A correct JavaScript escaper has to know whether it is
// inside a string literal, a template literal, a regex or a comment, and a
// function that cannot be written correctly must not be offered. Data reaches
// client script through a <script type="application/json"> block written by
// json_writer.h, which is one escaper this library already has and already
// keeps byte-predictable — and a page assembling partially-trusted content
// serves a CSP with no script-src at all (anvil/http/content_headers.h), so the
// question does not arise. Remove the context, then the escaper is not needed.
//
// Input is assumed to be valid UTF-8: every string reaching here has already
// passed anvil/i18n/utf8.h at the trust boundary, or was read back out of BSON,
// where anvil/db/codec.cc revalidated it. These functions do not repair, and
// they do not validate — they escape.

#include <string>
#include <string_view>

#include "anvil/input/html.h"

namespace anvil::http {

inline constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";

// Element text. A server-rendered page is the other place this process writes
// untrusted text into a document, and every interpolation into one goes through
// this or through append_html_attr. `&` is escaped FIRST by construction — the
// switch handles one byte at a time, so no double-escaping window exists.
//
// The quote characters are NOT escaped here, and that is the difference between
// this and the attribute context rather than an omission: in element text a
// quote is an ordinary character, and there is no call shape that lands this
// inside an attribute, because the attribute writer emits its own quotes.
void append_html_text(std::string& out, std::string_view text);

// One whole attribute — a leading space, the name, the `=`, and the value
// INSIDE the quotes this function emits itself.
//
// It emits the quotes because nothing in a C++ append chain can check that the
// surrounding bytes were a quoted attribute. A template loader could have, and
// that was a real advantage of the engine this phase did not build; what
// replaces it is narrower and holds — there is no call shape that produces an
// unquoted attribute, and none that forgets the `=`.
//
// The escape set is the text set plus both quote characters, so a value can
// close neither the attribute nor the tag.
//
// `name` is a literal at every call site and is NOT escaped — escaping would
// not help, since `onload` is a perfectly well-formed attribute name. A name
// outside [A-Za-z0-9-] emits NOTHING, which fails closed against the one shape
// that would otherwise be an injection: an attribute name built from request
// data.
void append_html_attr(std::string& out, std::string_view name, std::string_view value);

// A URL-valued attribute, emitted as ` name="url"` or not at all.
//
// The accept/reject decision DELEGATES to input::is_safe_link_target rather
// than implementing a URL rule here. That function already exists, is already
// exercised by the sanitiser, and already rejects the case a fresh
// implementation forgets — the protocol-relative `//evil.test/x`, which
// inherits the page's scheme and is an absolute off-site link wearing a
// relative-looking prefix. A second implementation of "is this URL safe" is a
// second one to keep in agreement, and the one that drifts is the one with no
// attacker reading it.
//
// Delegating the DECISION is not delegating the ESCAPE. `is_safe_link_target`
// answers a question about schemes and origins, and `/x" onmouseover=...` is a
// site-relative path it correctly accepts — so the value still goes through the
// attribute escape set on the way out.
//
// Returns false when nothing was emitted, so a caller can render the text
// without the link rather than emit an element whose href silently vanished.
[[nodiscard]] bool append_url_attr(std::string& out, std::string_view name,
                                   std::string_view url);

// The ONLY overload that appends markup verbatim, and it does not take a
// string: it takes a value input::sanitize_rich_text produced and nothing else
// can mint (docs/19-server-side-rendering.md §3). A raw insertion site therefore
// stops being a discipline anybody has to remember or a comment anybody has to
// notice in review, and becomes a call the compiler checks. That is what
// replaces an AllowsRaw flag, a raw-site table and a boot-time raw count, all
// three of which were machinery for auditing a rule by hand.
//
// RE-SANITISE AT RENDER. The caller passes a FRESHLY sanitised value, not a
// stored one, and a verdict that is not Ok emits kWithheldContent instead of
// the markup. Rich text is already sanitised on write, and an earlier draft
// argued from that — sanitise where the value arrives, trust where it is
// emitted. That is wrong for one reason: a stored value that fails the second
// pass means something BYPASSED the write path, which is exactly what an
// attacker who has reached the database no longer has to go through.
void append_sanitized(std::string& out, const input::SanitizedHtml& safe);

// Visible, not silent. A blank region with no explanation is indistinguishable
// from a content bug, and gets "fixed" by whoever finds it next.
inline constexpr std::string_view kWithheldContent =
    "<!-- content withheld: failed re-sanitisation -->";

}  // namespace anvil::http
