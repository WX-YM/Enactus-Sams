#pragma once

// The section registry: the SHAPE of a CMS, as an application declares it.
//
// --- why this is C++ and not a collection ----------------------------------
//
// Two requirements only conflict if "section" means one thing: nothing on the
// site should be hardcoded, and the section table should be a developer's
// concern. Splitting shape from content resolves it. Developers own the SHAPE,
// and it lives in a `constexpr` table, changed by a deploy. Staff own the
// CONTENT, and it lives in MongoDB, changed by an authenticated write.
//
// That split also produces the single most important security property of the
// sections endpoint: THE REGISTRY IS AN ALLOW-LIST. A key that is not in the
// table cannot be written, so there is no mass-assignment surface and no way to
// inject an unexpected field into a document a renderer will later trust. Never
// add a passthrough, an "extra" map, or a wildcard field.
//
// --- why constexpr and not a parsed JSON blob -------------------------------
//
// A table declared here lives in `.rodata`: zero start-up cost, zero heap,
// shared across every thread and — after a fork — across every process. A
// `static const std::string kJson` of the same content would allocate at
// start-up, parse per process, and move every error from compile time to first
// use.
//
// Lookup is a binary search over a sorted array of `string_view`. No map, no
// hash, no runtime construction; a miss costs a handful of compares and touches
// two cache lines.
//
// --- what anvil ships and what you ship -------------------------------------
//
// anvil ships the vocabulary below, the `ct::` constant-evaluation validators,
// and the lookups — all taking the table as a `std::span`, because a section
// table is only ever LOOKED UP and never dimensions anything anvil compiles
// (docs/01-seams.md §6). You ship `kSections`, and `static_assert` the three
// well-formedness checks over it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/core/ct_text.h"
#include "anvil/core/locale.h"

namespace anvil::sections {

// Stored nowhere — this is a compile-time classification only, so the
// enumerators may be reordered freely. Values are never persisted.
enum class FieldType : std::uint8_t {
    // Plain text. Rendered escaped, never as markup.
    Text,
    // Sanitised HTML (anvil/input/html.h). Sanitised on WRITE, so every renderer
    // downstream is safe by construction and one that forgets is not a hole.
    RichText,
    // Reserved: images are declared in SectionSpec::images and stored in the
    // document's `media` map, never as a data field. A FieldSpec using this
    // fails fields_fit_buffers() and is therefore a build error — a field whose
    // value is a media id would bypass the ImageSpec dimension check entirely.
    Image,
    // Integer. Never a float: the two things a section carries a number for are
    // counts and prices, and neither survives binary floating point.
    Number,
    Bool,
    // Site-relative, or absolute https/mailto. An unvalidated URL in a section
    // is a stored open redirect, and with `javascript:` it is stored XSS.
    Url,
    // `#rrggbb`, lowercase hex.
    Color,
    // A value from the field's OWN allow-list, and nothing else.
    //
    // This is the generalisation of what an icon name was: a field whose only
    // failure mode is otherwise silent. A free-text icon name renders nothing,
    // the dashboard shows a saved value, and no error is raised anywhere. The
    // allow-list makes a wrong value a validation error at the moment it is
    // typed — and because the choices travel with the field on the registry
    // endpoint, the editor renders a picker rather than holding a second copy of
    // the list that drifts the first time one is added.
    //
    // The choice list is the APPLICATION'S: a vocabulary of icon names, badge
    // styles or layout variants is one product's, and a table anvil populated
    // would be a table anvil had to guess. The empty string is always a legal
    // value and means "none"; whether a field may be blank is `required`.
    Choice,
};

// The name the registry endpoint puts on the wire. Unlike a stored enum, this
// value is not persisted anywhere, so a name costs nothing and reads — a number
// would make every consumer carry a copy of an enum whose values only matter
// inside this process.
//
// `Image` is unreachable through the endpoint because a FieldSpec declaring one
// fails the registry's own static_assert, but it is named here rather than left
// to a default so that adding a type and forgetting this switch is a -Wswitch
// error rather than an empty string on the wire.
[[nodiscard]] constexpr std::string_view field_type_name(FieldType type) noexcept {
    switch (type) {
        case FieldType::Text:     return "Text";
        case FieldType::RichText: return "RichText";
        case FieldType::Image:    return "Image";
        case FieldType::Number:   return "Number";
        case FieldType::Bool:     return "Bool";
        case FieldType::Url:      return "Url";
        case FieldType::Color:    return "Color";
        case FieldType::Choice:   return "Choice";
    }
    return "Text";
}

// The widest field list anvil will walk with a stack array.
//
// index_content() builds one pointer per declared field and per declared slot in
// a `std::array`, which turns canonicalise, serialize and content_etag from
// O(fields²) linear searches into one pass. That array is per-request scratch,
// so the bound is what keeps it on the stack — and `fields_fit_buffers` is what
// makes exceeding it a build failure rather than a quiet overflow.
//
// Worth knowing what raising it costs before raising it: the buffers are sized
// for every section including the four-field ones. At 64 fields the pair is
// 640 bytes, well inside a page.
inline constexpr std::size_t kMaxFieldsPerSection = 64;
inline constexpr std::size_t kMaxImagesPerSection = 16;

// One editable field.
//
// Ordered largest-alignment-first so the row packs: the views, then the 2-byte
// bound, then the three single-byte members (ENGINEERING_RULES.md §2.3).
struct FieldSpec final {
    std::string_view key;                        // 16

    // The editor generates one control per field and has to put a word next to
    // it. Without these the only string available is `key`, so the box reads
    // `cta_href`.
    //
    // These are labels for the STAFF editor, not public copy: the public site
    // renders values, never field names. One per declared locale, because a
    // staff member's editor is in their language too.
    Localized<>      label;                      // 16 * kLocaleCount

    // The allow-list for a Choice field, SORTED, and empty for every other type.
    // Sorted because is_choice binary-searches it, and asserted by
    // choices_are_well_formed() so a value added out of order is a build failure
    // rather than a lookup that quietly misses.
    std::span<const std::string_view> choices;   // 16

    // CODE POINTS, never bytes. A byte limit silently halves the allowance for
    // any non-Latin script (ENGINEERING_RULES.md §8).
    std::uint16_t    max_cp;                     //  2
    FieldType        type;                       //  1

    // Requires a complete value in EVERY declared locale. A localised field with
    // one locale missing is a validation error, never a fallback to another
    // locale — a fallback shows the wrong language to a reader who cannot tell
    // it is wrong.
    bool             localized;                  //  1

    // Enforced against the MERGED document, never against the patch — otherwise
    // the first partial update makes the section invalid.
    bool             required;                   //  1
};

// Stated in terms of kLocaleCount rather than as a number, because the number is
// the application's: two locales make this 72 bytes, three make it 88. What must
// not change is that there is no interior padding.
static_assert(sizeof(FieldSpec) ==
                  ((2 + kLocaleCount) * sizeof(std::string_view)) + sizeof(std::size_t),
              "FieldSpec must not grow padding");

// One image slot. The editor renders these requirements next to each upload
// control, in the staff member's language: requirements that live only in
// documentation get ignored, and requirements returned by the API get rendered.
struct ImageSpec final {
    std::string_view slot;          // 16
    Localized<>      label;         // 16 * kLocaleCount

    // MINIMUM source dimensions. A hero slot that needs 1920x1080 rejects a
    // 200x200 upload rather than letting the public site render broken.
    std::uint16_t    min_width;     //  2
    std::uint16_t    min_height;    //  2

    // Enforced aspect ratio as a fraction, compared with integer cross
    // multiplication and a tolerance — never as a double.
    //
    // Either half zero means NO constraint, and the endpoints serialise that as
    // a null aspect rather than as `{"num":0,"den":0}`: an object is always
    // truthy, so a slot with no constraint printed "shaped 0:0" beside it, which
    // a staff member can neither satisfy nor recognise as nothing being asked.
    std::uint16_t    aspect_num;    //  2
    std::uint16_t    aspect_den;    //  2
};

static_assert(sizeof(ImageSpec) ==
                  ((1 + kLocaleCount) * sizeof(std::string_view)) + sizeof(std::size_t),
              "ImageSpec must not grow padding");

struct SectionSpec final {
    std::string_view           key;
    // The page of the public site this section appears on. A preview URL has to
    // land the reader on the page the change is visible on; deriving it from the
    // key would be a second, guessing implementation of knowledge the registry
    // already holds. Sections rendered in the chrome name the page a reader will
    // see them on first.
    std::string_view           site_path;
    std::span<const FieldSpec> fields;
    std::span<const ImageSpec> images;
};

// --- compile-time text checks ----------------------------------------------
//
// The three constant-evaluation validators moved to anvil/core/ct_text.h when the
// notification template table needed the same proof over its own literals. They
// are still spelled `sections::ct::` here, because that is what an application's
// table and the conformance checks below already say.

namespace ct {

using anvil::ct::count_code_points;
using anvil::ct::is_non_empty_utf8;
using anvil::ct::is_valid_utf8;

}  // namespace ct

// --- keys -------------------------------------------------------------------

// Section keys are compile-time identifiers, and this is what keeps them that
// way: a request-supplied key is compared against the registry and either
// matches an entry or is rejected. It never contributes a character to a
// collection name, a Redis key, or a filesystem path.
[[nodiscard]] constexpr bool is_wellformed_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > 48) { return false; }
    bool previous_was_dot = true;   // a leading dot is invalid
    for (const char c : key) {
        const bool is_lower = c >= 'a' && c <= 'z';
        const bool is_digit = c >= '0' && c <= '9';
        if (c == '.') {
            if (previous_was_dot) { return false; }
            previous_was_dot = true;
            continue;
        }
        if (!is_lower && !is_digit && c != '_') { return false; }
        previous_was_dot = false;
    }
    return !previous_was_dot;
}

// True when `key` is exactly `prefix` or begins with `prefix.`. The dot is
// required, so a request for "home" cannot also match a future "homepage.x".
[[nodiscard]] constexpr bool key_has_prefix(std::string_view key,
                                            std::string_view prefix) noexcept {
    if (key == prefix) { return true; }
    return key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0 &&
           key[prefix.size()] == '.';
}

// --- table conformance ------------------------------------------------------
//
// Each of these is a mistake that would otherwise ship. static_assert all three
// over your own table (docs/01-seams.md §6).

// Sorted strictly ascending by key, which also rules out a duplicate, and every
// key spelled the way is_wellformed_key requires. The sort is what find_section
// binary-searches; a duplicate key would be ambiguous.
//
// Keys are dotted identifiers, so sorting by the whole key puts every `home.*`
// entry in one contiguous run — which is what lets a request for a whole page
// answer from one scan of a range rather than a walk of the table.
[[nodiscard]] constexpr bool registry_is_sorted(std::span<const SectionSpec> registry) noexcept {
    for (std::size_t i = 0; i < registry.size(); ++i) {
        if (!is_wellformed_key(registry[i].key)) { return false; }
        if (i > 0 && !(registry[i - 1].key < registry[i].key)) { return false; }
    }
    return true;
}

// Every section's field and image lists fit the stack arrays index_content()
// builds, and no field declares an Image type.
[[nodiscard]] constexpr bool fields_fit_buffers(std::span<const SectionSpec> registry) noexcept {
    for (const SectionSpec& section : registry) {
        if (section.fields.size() > kMaxFieldsPerSection) { return false; }
        if (section.images.size() > kMaxImagesPerSection) { return false; }
        for (const FieldSpec& field : section.fields) {
            // Images are declared in `images`, never as a data field.
            if (field.type == FieldType::Image) { return false; }
        }
    }
    return true;
}

// A Choice field has a sorted, duplicate-free, non-empty allow-list; every other
// type has none. A choice list nobody sorted is a binary search that misses, and
// a choice list on a Text field is a picker the validator does not enforce.
[[nodiscard]] constexpr bool choices_are_well_formed(
    std::span<const SectionSpec> registry) noexcept {
    for (const SectionSpec& section : registry) {
        for (const FieldSpec& field : section.fields) {
            if (field.type != FieldType::Choice) {
                if (!field.choices.empty()) { return false; }
                continue;
            }
            if (field.choices.empty()) { return false; }
            for (std::size_t i = 0; i < field.choices.size(); ++i) {
                // The empty string is "none" and is accepted everywhere, so
                // listing it is a duplicate of a value that already exists.
                if (field.choices[i].empty()) { return false; }
                if (i > 0 && !(field.choices[i - 1] < field.choices[i])) { return false; }
            }
        }
    }
    return true;
}

// --- lookup -----------------------------------------------------------------

// nullptr for an unknown key. The caller turns that into a stealth 404 WITHOUT
// creating a cache entry — a miss that populated a cache would be an
// unbounded-growth vector keyed on attacker input.
[[nodiscard]] constexpr const SectionSpec* find_section(std::span<const SectionSpec> registry,
                                                        std::string_view key) noexcept {
    std::size_t low = 0;
    std::size_t high = registry.size();
    while (low < high) {
        const std::size_t mid = low + ((high - low) / 2);
        if (registry[mid].key < key) {
            low = mid + 1;
        } else if (key < registry[mid].key) {
            high = mid;
        } else {
            return &registry[mid];
        }
    }
    return nullptr;
}

// Index into the registry, for the process-local cache: the cache is one slot
// per (section, locale), so a lookup is one bounds-checked load and no hashing.
[[nodiscard]] constexpr std::size_t section_index(std::span<const SectionSpec> registry,
                                                  const SectionSpec* spec) noexcept {
    return static_cast<std::size_t>(spec - registry.data());
}

[[nodiscard]] constexpr const FieldSpec* find_field(const SectionSpec& section,
                                                    std::string_view key) noexcept {
    for (const FieldSpec& field : section.fields) {
        if (field.key == key) { return &field; }
    }
    return nullptr;
}

[[nodiscard]] constexpr const ImageSpec* find_image(const SectionSpec& section,
                                                    std::string_view slot) noexcept {
    for (const ImageSpec& image : section.images) {
        if (image.slot == slot) { return &image; }
    }
    return nullptr;
}

// Membership in a Choice field's allow-list. The EMPTY STRING IS NOT A MEMBER:
// "none" is a legitimate stored value and is handled by the caller, because the
// two questions — "is this a value I know" and "is this field allowed to be
// blank" — have different answers per field.
[[nodiscard]] constexpr bool is_choice(std::span<const std::string_view> choices,
                                       std::string_view value) noexcept {
    std::size_t low = 0;
    std::size_t high = choices.size();
    while (low < high) {
        const std::size_t mid = low + ((high - low) / 2);
        if (choices[mid] < value) { low = mid + 1; } else { high = mid; }
    }
    return low < choices.size() && choices[low] == value;
}

// --- document state ---------------------------------------------------------

// The `s` half of the compound `_id`. Two documents per key rather than one
// document with two subtrees, so a public read cannot leak an unpublished string
// through a missing projection.
enum class SectionState : std::int32_t { Published = 0, Draft = 1 };

}  // namespace anvil::sections
