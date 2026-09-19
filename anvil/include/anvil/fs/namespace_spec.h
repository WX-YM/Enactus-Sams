#pragma once

// The vocabulary an application needs to declare its storage namespaces, with no
// dependency on the application's own configuration header.
//
// Same shape, and same reason, as anvil/core/locale_spec.h: <anvil_app_config.h>
// includes THIS to spell its tables, and anvil/fs/namespace.h includes the
// application's header to read them. Without a type both sides can name
// independently, those two includes are a cycle.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "anvil/fs/sniff.h"

namespace anvil::fs {

// --- which types a namespace accepts ---------------------------------------
//
// The ALLOW-LIST is anvil's, because it is the pipeline's capability: these are
// the formats it can sniff, decode and re-encode, and no application can widen
// that by declaring something. NARROWING it is the application's, because which
// types a particular namespace should take is a fact about that namespace.
//
// It exists because the accepted set was enforced in the upload path and
// published nowhere, so a client's file picker carried a second copy of a server
// rule — and the copy that drifts is the one offering a format the server
// rejects after the bytes are already on the wire.
//
// PER NAMESPACE rather than one global list, and the difference is the whole
// point: a global list would close the copy and not the case that reopens it.
// The moment one namespace takes no AVIF, the narrower list gets written into
// the client by hand again.

using MimeMask = std::uint8_t;

// `Mime::Unknown` is 0 and deliberately has NO bit. It is the absence of a
// recognised type rather than a type, so a mask can never accept it and
// `mime_accepted` fails closed on it without a special case at the call site.
[[nodiscard]] constexpr MimeMask mime_bit(Mime mime) noexcept {
    return mime == Mime::Unknown
               ? MimeMask{0}
               : static_cast<MimeMask>(1U << (static_cast<unsigned>(mime) - 1U));
}

// Everything the pipeline decodes, DERIVED from the enum rather than listed.
// Adding a Mime widens this by itself, which is what stops the default from
// becoming the second list this whole mechanism exists to remove.
inline constexpr MimeMask kDecodableMimes = []() constexpr {
    MimeMask mask = 0;
    for (unsigned value = 1; value <= static_cast<unsigned>(kMaxMime); ++value) {
        mask = static_cast<MimeMask>(mask | mime_bit(static_cast<Mime>(value)));
    }
    return mask;
}();

static_assert(static_cast<unsigned>(kMaxMime) <= 8,
              "a MimeMask is one byte; a ninth Mime needs a wider mask, not a wrap");

[[nodiscard]] constexpr bool mime_accepted(MimeMask mask, Mime mime) noexcept {
    return (mask & mime_bit(mime)) != 0;
}

// One storage namespace: which API owns a stored object.
//
// The directory name is also the path segment that appears in a media URL, so
// there is one string for both. Two spellings would be two places for them to
// disagree, and the disagreement would be a 404 on a path the handler had just
// authorised.
struct NamespaceSpec final {
    std::string_view dir;

    // Defaults to everything the pipeline decodes, so a table written before
    // this field existed keeps its exact behaviour and a namespace that has no
    // opinion states none.
    MimeMask         accepts{kDecodableMimes};
};

// It was `sizeof(std::string_view)` until the mask landed, and the eight bytes
// of padding are stated here rather than discovered: this table is a handful of
// entries in `.rodata`, read at boot and at emit time and never per request, so
// the padding buys a per-namespace rule for nothing that matters. A struct on a
// request path would not get that answer.
static_assert(sizeof(NamespaceSpec) == sizeof(std::string_view) + alignof(std::string_view),
              "NamespaceSpec must not grow past one view and one word");

// --- roles: what a client asks for instead of a width ----------------------
//
// The public grammar is `GET /media/{ns}/{id}/{role}`, and a role is what the
// image is FOR. Making the client assemble `w640.avif` would require it to know
// the width ladder and the format set, which is exactly the metadata that is
// supposed to stay server-side — and a client that knows the ladder is a client
// that will start building paths from it again.
//
// The role VOCABULARY is anvil's, because the grammar is. Which width each role
// resolves to is the application's, because that is a fact about its design.
enum class MediaRole : std::uint8_t { Thumb = 0, Card = 1, Hero = 2, Full = 3 };

inline constexpr std::size_t kRoleCount = 4;

inline constexpr std::array<std::string_view, kRoleCount> kRoleNames{
    {"thumb", "card", "hero", "full"}};

// What `GET /media/{ns}/{id}` with no role segment means. It is also what a
// caller that FORGETS the role segment gets, which is why it is the middle of
// the ladder rather than the top: a mistake here costs resolution rather than
// bandwidth, and the opposite mistake serves megabytes to a phone.
inline constexpr MediaRole kDefaultRole = MediaRole::Card;

// The URL segment back to a role. A miss is a MISS — never a fallback to the
// master, because a typo that silently serves 4 MB to a phone is the failure
// this grammar exists to prevent.
[[nodiscard]] constexpr bool role_from_segment(std::string_view segment,
                                               MediaRole& out) noexcept {
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (kRoleNames[i] == segment) {
            out = static_cast<MediaRole>(i);
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr std::string_view role_name(MediaRole role) noexcept {
    return kRoleNames[static_cast<std::size_t>(role)];
}

// --- encoded formats -------------------------------------------------------
// anvil's, not the application's: these are the formats the image pipeline can
// actually write, which is a property of the pipeline.
enum class Format : std::uint8_t { Avif = 0, Webp = 1 };

inline constexpr std::array<Format, 2> kVariantFormats{{Format::Avif, Format::Webp}};

[[nodiscard]] constexpr std::string_view format_extension(Format format) noexcept {
    return format == Format::Avif ? "avif" : "webp";
}

[[nodiscard]] constexpr std::string_view format_mime(Format format) noexcept {
    return format == Format::Avif ? "image/avif" : "image/webp";
}

}  // namespace anvil::fs
