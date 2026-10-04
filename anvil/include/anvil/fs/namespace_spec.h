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

// Two bytes. It was one until the stored-file class needed a ninth type, and the
// assertion below is what made that a decision rather than a wrapped shift.
using MimeMask = std::uint16_t;

// `Mime::Unknown` is 0 and deliberately has NO bit. It is the absence of a
// recognised type rather than a type, so a mask can never accept it and
// `mime_accepted` fails closed on it without a special case at the call site.
[[nodiscard]] constexpr MimeMask mime_bit(Mime mime) noexcept {
    return mime == Mime::Unknown
               ? MimeMask{0}
               : static_cast<MimeMask>(1U << (static_cast<unsigned>(mime) - 1U));
}

// Every type of one class, DERIVED from the enum rather than listed, so adding
// a Mime widens the right one by itself and neither becomes the second list this
// mechanism exists to remove.
[[nodiscard]] constexpr MimeMask mimes_of(MimeClass mime_class_wanted) noexcept {
    MimeMask mask = 0;
    for (unsigned value = 1; value <= static_cast<unsigned>(kMaxMime); ++value) {
        const auto mime = static_cast<Mime>(value);
        if (mime_class(mime) == mime_class_wanted) {
            mask = static_cast<MimeMask>(mask | mime_bit(mime));
        }
    }
    return mask;
}

// Everything the image pipeline decodes, and the DEFAULT. It is the image class
// and not the whole enum: when the file class landed, a default derived from
// every Mime would have made every namespace that never stated a list start
// accepting PDFs and video overnight. A namespace takes files only by naming
// them.
inline constexpr MimeMask kDecodableMimes = mimes_of(MimeClass::Image);

// The stored-as-is types (fs/sniff.h). Never a default.
inline constexpr MimeMask kFileMimes = mimes_of(MimeClass::File);

// Ciphertext (fs/sniff.h). Never a default, never combined with anything: see
// namespace_is_well_formed below.
inline constexpr MimeMask kSealedMimes = mimes_of(MimeClass::Sealed);

static_assert(static_cast<unsigned>(kMaxMime) <= 16,
              "a MimeMask is two bytes; a seventeenth Mime needs a wider mask, not a wrap");

[[nodiscard]] constexpr bool mime_accepted(MimeMask mask, Mime mime) noexcept {
    return (mask & mime_bit(mime)) != 0;
}

// --- how far deduplication reaches -----------------------------------------
//
// Uploads are deduplicated by content hash (docs/07-filesystem.md §4 step 4),
// which saves a transcode and a copy. Across a whole namespace it is also an
// ORACLE: a hit skips probing, normalising and deriving variants, so it answers
// in milliseconds where new bytes take seconds, and uploading a guessed document
// tells the uploader whether somebody in this namespace already has those exact
// bytes. For a public namespace that is nothing; for a private one ("somebody
// here holds this contract") it is the disclosure. No response shape can hide
// it, because the clock is not in the response (docs/22-chat.md §6.3).
//
//   Namespace  every owner shares one copy. The default, and what every table
//              written before this field existed keeps.
//   Owner      only an owner's own earlier upload is reused, so a fast answer
//              can only ever say "you uploaded this before".
//   None       never reused. For bytes no two uploads share anyway, such as
//              ciphertext under a fresh key.
enum class Dedupe : std::uint8_t { Namespace = 0, Owner = 1, None = 2 };

// --- who may be served an object without a grant ----------------------------
//
//   Public   the application's own media handler decides, from the route and
//            the namespace (docs/07-filesystem.md §6). The default, and what
//            every table written before this field existed keeps.
//   Private  served ONLY on a grant minted by code that already checked the
//            caller against the object's owning document (media/grant.h). The
//            serving call refuses the namespace without one, so a handler that
//            has an id and no grant cannot serve it, however it got the id.
enum class Visibility : std::uint8_t { Public = 0, Private = 1 };

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

    Dedupe           dedupe{Dedupe::Namespace};

    Visibility       visibility{Visibility::Public};
};

// It was `sizeof(std::string_view)` until the mask landed, and the eight bytes
// after the view are stated here rather than discovered (the mask, the dedupe
// scope and the visibility live in them): this table is a handful of
// entries in `.rodata`, read at boot and at emit time and never per request, so
// the padding buys a per-namespace rule for nothing that matters. A struct on a
// request path would not get that answer.
static_assert(sizeof(NamespaceSpec) == sizeof(std::string_view) + alignof(std::string_view),
              "NamespaceSpec must not grow past one view and one word");

// The rules a namespace must keep whatever the application meant by it.
// anvil/fs/namespace.h static_asserts this over the application's table, so a
// breach is a compile error in the application's build. A function over one
// spec rather than over the table, so a refusal can be tested with a spec no
// real table should contain.
//
// A namespace that takes SEALED takes nothing else. The upload path for a
// sealed namespace sniffs nothing (UploadSink::finish_sealed), so a namespace
// that also took images would have two upload paths with opposite rules, and
// a client choosing between them would be choosing whether its bytes are
// inspected. It is also never deduplicated (every ciphertext is under a fresh
// key, so a lookup can only ever cost a round trip, and a hit would be a
// timing oracle on somebody else's blob — docs/22-chat.md §6.3) and never
// Public (an object an id alone can fetch is one a leaked id serves forever).
[[nodiscard]] constexpr bool namespace_is_well_formed(const NamespaceSpec& spec) noexcept {
    if ((spec.accepts & kSealedMimes) == 0) { return true; }
    return spec.accepts == kSealedMimes && spec.dedupe == Dedupe::None &&
           spec.visibility == Visibility::Private;
}

static_assert(kSealedMimes != 0 && (kSealedMimes & (kDecodableMimes | kFileMimes)) == 0,
              "the sealed class shares no type with anything the pipeline sniffs");

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
