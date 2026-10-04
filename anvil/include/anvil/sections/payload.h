#pragma once

// Binding and rendering a section's content: JSON in, SectionContent out, and
// pre-serialised JSON bytes back out again.
//
// Both directions live here rather than in the service because more than one
// caller needs exactly this logic and they must not drift: the section write and
// whatever draft or preview path an application builds on top of it. A draft
// that validates differently from the section write it will eventually become is
// a draft that cannot be committed, and the staff member discovers that after
// previewing.
//
// --- binding is registry-driven, and that is the security property ----------
//
// Every accepted key comes from the compile-time registry. A key that is not
// there is a validation error, NEVER a silent drop: dropping hides client bugs
// and hides probing, and it is how a client discovers that some other endpoint
// accepts the field it just tried.
//
// --- serialisation is done ONCE, at cache fill, not per request -------------
//
// The output of `serialize` is what the process-local cache stores. A cache hit
// is then a pointer read and a write() of bytes that already exist: zero JSON
// serialisation, zero BSON decode, zero allocation. Text is emitted as raw
// UTF-8, never \uXXXX, or every non-Latin character costs six bytes instead of
// two.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/locale.h"
#include "anvil/crypto/fast_hash.h"
#include "anvil/input/fields.h"
#include "anvil/input/json.h"
#include "anvil/sections/content.h"
#include "anvil/sections/defaults.h"
#include "anvil/sections/registry.h"

namespace anvil::sections {

struct BindPolicy final {
    // Passed through to the rich-text sanitiser: the only absolute origin an
    // `<img src>` inside a RichText field may name.
    std::string_view content_origin;
};

// A field name plus a reason, both of which are server-chosen constants. The
// name is a registry key, never a key taken from the request — an unknown key is
// reported with an EMPTY name precisely so a client cannot choose what appears
// in a response or a log line.
using BindError = input::FieldError;

// Binds the `data` object of a section write. `data` may be null, which is a
// legitimate write that changes only images.
//
// Rich text is SANITISED here, so what lands in `out` is what will be stored:
// sanitising on write means every renderer downstream is safe by construction,
// and a renderer that forgets is not a hole.
[[nodiscard]] std::optional<BindError> bind_data(const SectionSpec& spec,
                                                 const input::JsonValue* data,
                                                 const BindPolicy& policy, SectionContent& out);

// Binds the `images` object: slot name from the registry, value a UUID string.
// Whether the media exists, is in the right namespace, and satisfies the slot's
// ImageSpec is checked by the service, which is the layer that can ask the
// database.
[[nodiscard]] std::optional<BindError> bind_images(const SectionSpec& spec,
                                                   const input::JsonValue* images,
                                                   SectionContent& out);

// `required` is enforced against the MERGED result, never against the patch, or
// the first partial update makes the section invalid.
[[nodiscard]] std::optional<BindError> check_required(const SectionSpec& spec,
                                                      const SectionContent& merged);

// The compile-time defaults as a SectionContent, ready to be stored or
// serialised. Pure: no I/O, and no allocation beyond the strings it copies out
// of `.rodata`.
//
// It lives beside the binder rather than beside the bootstrap that first uses it
// because anything that RESTORES a section needs the same bytes, and "what a
// restore produces" and "what a fresh database gets" must be the same content by
// construction rather than by review.
[[nodiscard]] SectionContent default_content(const SectionSpec& spec,
                                             const SectionDefaults& defaults);

// --- serialisation ----------------------------------------------------------

// What the process-local cache holds, and what a hit writes straight to the
// socket. `etag_header` is the quoted strong entity tag, precomputed so a
// conditional GET is one comparison and no formatting.
struct SerializedSection final {
    std::string          json;
    crypto::FastDigest   etag;
    std::array<char, 18> etag_header;
    std::int64_t         version;
};

// ONE locale only. Returning every declared locale multiplies the payload by the
// locale count for content all but one of which is discarded on arrival.
//
// `image_url_base` is the whole prefix an image id is appended to, built once by
// the caller — origin, path and namespace segment. Every component of the result
// is server-generated: the origin comes from frozen configuration, the namespace
// from a compile-time table, and the id from a parsed UUID. No request byte
// contributes a character.
[[nodiscard]] SerializedSection serialize(const SectionSpec& spec,
                                          const SectionContent& content, std::int64_t version,
                                          Locale locale, std::string_view image_url_base);

// The etag of the CONTENT rather than of a rendered payload: it is stored in the
// document and is what a writer compares to decide whether anything actually
// changed. Per-locale etags come from `serialize`.
[[nodiscard]] crypto::FastDigest content_etag(const SectionSpec& spec,
                                              const SectionContent& content);

// The bilingual upload requirements an editor renders next to each slot.
// Requirements that live only in documentation get ignored; requirements
// returned by the API get rendered.
[[nodiscard]] std::string serialize_image_specs(const SectionSpec& spec);

// One section's shape — `{key, path, fields, images}` — exactly as
// serialize_registry() writes each element of its array. Public so that anything
// else describing section-shaped content to an editor (anvil/entries) emits the
// same bytes the section editor already reads, rather than a second writer that
// drifts the first time a FieldSpec member is added.
void append_shape_json(std::string& out, const SectionSpec& spec);

// The whole schema, so a staff editor is GENERATED rather than written twice. A
// hard-coded field list is a second copy that drifts the first time a field is
// added, and the failure lands on a staff member typing into a box the server
// rejects as unknown.
//
// Serialised on every call rather than cached: it is a staff-only route read
// once when the editor opens, not the per-render path `serialize` sits on, and a
// cache here would be a copy of a constexpr table.
[[nodiscard]] std::string serialize_registry(std::span<const SectionSpec> registry);

}  // namespace anvil::sections
