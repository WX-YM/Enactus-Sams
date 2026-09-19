#pragma once

// Serving a stored object without moving a single byte of it through this
// process.
//
// The handler authorises, sets headers, and returns an EMPTY body carrying
// X-Accel-Redirect. Nginx then serves the file with sendfile(), copying disk to
// socket inside the kernel. Reading a 4 MB image into a std::string to write it
// to a socket costs two copies, 4 MB of heap per concurrent request, and a
// thread held for the duration of a slow client's download — a hundred slow
// mobile clients would occupy a hundred threads (ENGINEERING_RULES.md §2.4).
//
// THE ENTIRE SECURITY BOUNDARY IS `internal;` ON THE NGINX LOCATION. Without
// that keyword every file under the storage root is reachable by direct URL, and
// nothing in this process can tell. A deployment smoke test that requests the
// internal prefix directly and asserts a 404 is the only thing that can.
//
// --- the public grammar names no width and no format ------------------------
//
// A client asks for a ROLE — thumb, card, hero, full — resolved server-side
// through the application's table. Making the client assemble `w640.avif` would
// require it to know the width ladder and the format set, which is exactly the
// metadata that is supposed to stay server-side. Format is negotiated from
// `Accept` and the response carries `Vary: Accept`, so a shared cache cannot
// hand an AVIF to a client that cannot decode one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/HttpResponse.h>

#include "anvil/core/types.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/namespace_spec.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/sniff.h"
#include "anvil/images/variants.h"

namespace anvil::media {

// The internal Nginx location objects are redirected into.
//
// A CONSTANT, never configuration: it must match the deployment's Nginx
// configuration exactly, and a mismatch between the two is a 404 on every image
// rather than something a deployment should be able to drift into by editing one
// of the two places.
inline constexpr std::string_view kAccelPrefix = "/protected_storage/";

// These routes are access-controlled, so the response is `private`: a shared
// cache holding one would serve it to the next requester.
inline constexpr std::string_view kMediaCacheControl = "private, max-age=31536000, immutable";

// Percent-encodes every byte that is not an RFC 3986 unreserved character,
// leaving '/' as a separator.
//
// Applied to the redirect value UNCONDITIONALLY, even though every component is
// server-generated hex. A CR or LF reaching a response header is response
// splitting, and "this value can never contain a newline" is exactly the
// assumption a refactor breaks — the path builder is three functions away and
// nothing links the two.
[[nodiscard]] std::string encode_accel_path(std::string_view path);

// Which format an `Accept` header asks for. AVIF when it is advertised, WebP
// otherwise — including when the header is absent or is `*/*`, which is what
// every non-browser client sends.
//
// A SUBSTRING SCAN, not a q-value parser. The header is attacker-controlled and
// unbounded, and the only question being asked is "does this client say it can
// decode AVIF" — a full RFC 7231 parse would be a request-path parser written to
// answer a boolean.
//
// Exposed because the negotiation is a decision rather than plumbing, and
// because the response's `Vary: Accept` is only correct if this is the whole of
// what varies with it.
[[nodiscard]] fs::Format negotiate_format(std::string_view accept) noexcept;

// The variant to serve for a role, in this preference order:
//
//   1. the WIDEST variant of `format` at or below the role's width;
//   2. the WIDEST variant of a format the client is guaranteed to decode, at or
//      below the role's width;
//   3. the NARROWEST variant of `format` above it;
//   4. the NARROWEST variant of a decodable format above it;
//   5. the master, and only when none of the above exists.
//
// A legitimate id must never 404 because a variant was not written; that turns
// an editorial choice about source resolution into a broken image on a public
// page. The pipeline does not upscale, so an 800 px master produces a 320 and a
// 640 and nothing else, and a `hero` request on that object serves the 640.
//
// --- Why the master is LAST and not second ---------------------------------
//
// This used to search one format and fall straight to the master, and both
// halves were right on their own terms. Together, on a build whose libvips has
// no libheif, they were severe: `negotiate_format` answers `Avif` for any client
// that advertises it, which is essentially every current browser; no object has
// an AVIF variant, because `generate_variants` correctly SKIPS a format it
// cannot encode; so the fallback fired and the client was served the normalised
// master — a multi-megabyte PNG, to a phone, in place of a 90 KB WebP. Nothing
// reported it. The upload succeeded, the variants that could be written were
// written, the role resolved, the response was a 200 with a correct
// `Content-Type`, and the only symptom was page weight.
//
// The master is the widest and least compressed object on disk, so reaching it
// is the WORST outcome available rather than a neutral default — which also
// makes the old "never fails upward" rule self-defeating, because falling to the
// master was already a failure upward, and a larger one than any variant.
//
// The question is object-local rather than build-local on purpose: it asks what
// this object HAS, not what this binary can encode, so the answer stays correct
// for an object stored while a delegate was available and read back by a build
// without one, and does not change meaning the day the delegate is installed.
//
// --- Degradation is DIRECTIONAL --------------------------------------------
//
// Steps 2 and 4 degrade AVIF to WebP and never the reverse. Every client that
// advertises AVIF decodes WebP; `negotiate_format` answers `Webp` precisely for
// the clients that did NOT say they decode AVIF, so serving one an AVIF is a
// broken image. A symmetric "try the other format" is the obvious spelling and
// is wrong in exactly the direction that reaches the oldest clients.
//
// `accel_redirect_response` derives the `Content-Type` from the KEY, so a
// degraded key carries a degraded type and the two cannot disagree.
//
// Reported by the first application built on this library, on a machine whose
// libvips had no libheif.
[[nodiscard]] fs::VariantKey resolve_role(const std::vector<images::VariantRecord>& variants,
                                          std::uint16_t role_width, fs::Format format) noexcept;

// The finished response: empty body, redirect header, and the four headers that
// make serving it safe.
//
// `mime` is the STORED enum, never anything from the request. A file cannot be
// allowed to choose how a browser interprets it, and the one place that could
// happen is here.
[[nodiscard]] drogon::HttpResponsePtr accel_redirect_response(fs::Ns ns, const Uuid& id,
                                                              fs::VariantKey key,
                                                              fs::Mime mime);

}  // namespace anvil::media
