#pragma once

// The template seam: notification content as a template id plus parameters, never
// as rendered text.
//
// The obvious implementation stores `{title: {...}, body: {...}}` on every
// notification. For a broadcast to 20 000 subscribers under fan-out on write that
// is the same two strings copied 20 000 times — and the language is frozen at
// SEND time, so a reader who switches to Arabic still reads the English
// notifications they already received.
//
// Storing `{tpl: 4, params: {t: "Summer Hours"}}` and rendering at READ time from
// the table below means storage per notification drops from a few hundred bytes
// to a few dozen, the language is the reader's CURRENT locale, and a typo in a
// template is fixed by a deploy — retroactively, for every notification ever
// sent.
//
// --- substitution only, no expression language ------------------------------
//
// `{t}` is replaced by a parameter value and escaped for the output context.
// Anything richer — a conditional, a loop, a property path — is a
// template-injection surface, and a template that can read arbitrary properties
// can read ones the reader is not entitled to. So the grammar is exactly:
//
//     placeholder := '{' [A-Za-z] '}'
//
// One ASCII letter. Not a name, not a path, not an index. The scanner is thirty
// lines and there is nothing in it to exploit.
//
// Anything else containing `{` is a BUILD FAILURE rather than text that renders
// literally — `{a.b}`, `{0}`, `{total}`, `{{t}}` and a bare `{` all refuse to
// compile. That is the stronger of the two positions: "it renders as literal
// text" is a claim about the renderer, and the next change to the renderer could
// quietly stop it being true.
//
// A stray `}` is prose and is left alone. The asymmetry is deliberate: `}` is
// punctuation in somebody's copy, and `{` is the one character that opens the
// only construct there is.
//
// --- validated at compile time ----------------------------------------------
//
// The placeholder SET must be identical across every declared locale, its size
// must equal `param_count`, and every string must be present and valid UTF-8.
// All three are checked by `template_table_is_well_formed`, which an application
// static_asserts over its own table — so a template with a mismatched placeholder
// count FAILS TO COMPILE rather than rendering a literal `{t}` into somebody's
// inbox.
//
// The set is compared order-INSENSITIVELY on purpose: Arabic phrasing
// legitimately puts the placeholders in a different order from English, and a
// check that demanded the same order would force a translator to write an
// unnatural sentence.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "anvil/core/ct_text.h"
#include "anvil/core/locale.h"
#include "anvil/core/result.h"

namespace anvil::notifications {

// STORED on every notification row. NEVER renumber, and never repurpose a retired
// id — a row a year old referring to id 4 must still render what it meant when it
// was written.
using TemplateId = std::uint8_t;

// At most 8 parameters per notification, each at most 120 code points. Both are
// enforced at publish, so a render can size its output from the template without
// a second pass.
inline constexpr std::size_t kMaxParams = 8;
inline constexpr std::size_t kMaxParamCodePoints = 120;

enum class ParamType : std::uint8_t { Text = 0, Number = 1 };

// RESERVED on a coalescing topic, and bound at render to the row's `count`.
//
// Coalescing is an upsert whose `$setOnInsert` keeps the FIRST publish's
// parameters — a coalesced repeat is the same event happening again, and
// rewriting the params would make the row describe only the last one. So a count
// stored as a parameter is frozen at 1 while the row's own `count` climbs, and
// the sentence coalescing exists to produce — "3 new submissions" — would read
// "1 new submissions" forever.
//
// Binding it at render instead costs nothing and cannot go stale. A topic that
// does not coalesce leaves the letter to the application: its `count` is always
// 1, so there is nothing to bind and nothing to override.
inline constexpr char kCountParam = 'n';

// A borrowed parameter. Ordered largest-alignment-first; 32 bytes and trivially
// copyable, so a span of eight is 256 bytes of stack and no allocation.
struct Param final {
    std::string_view text;     // 16 — Text only
    std::int64_t     number;   //  8 — Number only
    char             name;     //  1 — the letter inside the braces
    ParamType        type;     //  1

    [[nodiscard]] static constexpr Param of(char name, std::string_view value) noexcept {
        return Param{value, 0, name, ParamType::Text};
    }
    [[nodiscard]] static constexpr Param of(char name, std::int64_t value) noexcept {
        return Param{{}, value, name, ParamType::Number};
    }
};

static_assert(sizeof(Param) == 32);
static_assert(std::is_trivially_copyable_v<Param>);

namespace detail {

// The placeholder letters a string uses, in first-seen order. Fixed capacity, no
// allocation, fully constexpr — this runs at compile time and nowhere else.
struct PlaceholderSet final {
    std::array<char, kMaxParams> names;
    std::uint8_t                 count;
    // False on a malformed placeholder: an unclosed brace, a non-letter inside
    // one, or more than kMaxParams distinct letters. A template that trips this
    // fails to compile rather than rendering a literal `{`.
    bool                         well_formed;
};

[[nodiscard]] constexpr bool is_ascii_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool contains(const PlaceholderSet& set, char name) noexcept {
    for (std::uint8_t i = 0; i < set.count; ++i) {
        if (set.names[i] == name) { return true; }
    }
    return false;
}

[[nodiscard]] constexpr PlaceholderSet placeholders_of(std::string_view text) noexcept {
    PlaceholderSet set{{}, 0, true};
    for (std::size_t i = 0; i < text.size(); ++i) {
        // A stray '}' is not an error: it is a literal brace in prose, and copy in
        // any language legitimately contains punctuation anvil does not police.
        if (text[i] != '{') { continue; }
        if (i + 2 >= text.size() || !is_ascii_letter(text[i + 1]) || text[i + 2] != '}') {
            set.well_formed = false;
            return set;
        }
        const char name = text[i + 1];
        if (!contains(set, name)) {
            if (set.count >= kMaxParams) {
                set.well_formed = false;
                return set;
            }
            set.names[set.count] = name;
            ++set.count;
        }
        i += 2;
    }
    return set;
}

[[nodiscard]] constexpr PlaceholderSet merged(const PlaceholderSet& a,
                                              const PlaceholderSet& b) noexcept {
    PlaceholderSet out = a;
    if (!a.well_formed || !b.well_formed) {
        out.well_formed = false;
        return out;
    }
    for (std::uint8_t i = 0; i < b.count; ++i) {
        if (contains(out, b.names[i])) { continue; }
        if (out.count >= kMaxParams) {
            out.well_formed = false;
            return out;
        }
        out.names[out.count] = b.names[i];
        ++out.count;
    }
    return out;
}

// Set equality, order-insensitive. See the header comment.
[[nodiscard]] constexpr bool same_set(const PlaceholderSet& a, const PlaceholderSet& b) noexcept {
    if (!a.well_formed || !b.well_formed || a.count != b.count) { return false; }
    for (std::uint8_t i = 0; i < a.count; ++i) {
        if (!contains(b, a.names[i])) { return false; }
    }
    return true;
}

}  // namespace detail

// One message, in every declared locale.
//
// `Localized<>` rather than a pair, for the same reason a section's field label
// is: the locale table is the application's, and a two-language struct would make
// a third locale a rewrite rather than a row.
struct TemplateSpec final {
    Localized<>  title;        // 16 * kLocaleCount
    Localized<>  body;         // 16 * kLocaleCount
    TemplateId   id;           // STORED. Append only
    std::uint8_t param_count;
};

// Stated as a formula rather than as a number, because the number is the
// application's: two locales make this 72 bytes and three make it 104. What must
// not change is that there is no interior padding.
static_assert(sizeof(TemplateSpec) ==
                  (2 * kLocaleCount * sizeof(std::string_view)) + sizeof(std::size_t),
              "TemplateSpec must not grow padding");

// --- table conformance ------------------------------------------------------

// Every string present and valid UTF-8, the placeholder set identical across
// every locale, and its size equal to param_count. static_assert this over your
// own table: a mismatch here is a notification that renders wrong for every
// reader who ever receives it, and the render path cannot recover from it.
[[nodiscard]] constexpr bool template_table_is_well_formed(
    std::span<const TemplateSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        const TemplateSpec& tpl = table[i];
        if (!tpl.title.complete() || !tpl.body.complete()) { return false; }
        if (tpl.param_count > kMaxParams) { return false; }

        detail::PlaceholderSet reference{{}, 0, true};
        for (std::size_t locale = 0; locale < kLocaleCount; ++locale) {
            // A literal reaches a reader with no validation step in between, so
            // the validation happens where the mistake is made.
            if (!ct::is_non_empty_utf8(tpl.title.values[locale])) { return false; }
            if (!ct::is_non_empty_utf8(tpl.body.values[locale])) { return false; }

            const detail::PlaceholderSet here =
                detail::merged(detail::placeholders_of(tpl.title.values[locale]),
                               detail::placeholders_of(tpl.body.values[locale]));
            if (!here.well_formed) { return false; }
            if (locale == 0) {
                reference = here;
            } else if (!detail::same_set(reference, here)) {
                return false;
            }
        }
        if (reference.count != tpl.param_count) { return false; }

        for (std::size_t j = 0; j < i; ++j) {
            if (table[j].id == tpl.id) { return false; }
        }
    }
    return true;
}

[[nodiscard]] constexpr bool templates_are_dense_from_zero(
    std::span<const TemplateSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].id != static_cast<TemplateId>(i)) { return false; }
    }
    return true;
}

// nullptr for an id this build does not declare. Same rolling-deploy state, and
// the same correct direction to fail, as an unknown topic code.
[[nodiscard]] constexpr const TemplateSpec* template_of(std::span<const TemplateSpec> table,
                                                        TemplateId id) noexcept {
    const std::size_t index = id;
    if (index < table.size() && table[index].id == id) { return &table[index]; }
    for (const TemplateSpec& spec : table) {
        if (spec.id == id) { return &spec; }
    }
    return nullptr;
}

// --- rendering --------------------------------------------------------------

struct Rendered final {
    std::string title;
    std::string body;
};

// Substitutes into the template for `locale`.
//
// A placeholder with no matching parameter renders as NOTHING AT ALL, never as
// the literal `{t}`: a stored row whose params were written by an older build
// must degrade to a shorter sentence rather than leak the template's internals
// into a reader's inbox.
//
// Returns ValidationFailed when a parameter is unusable — not valid UTF-8, over
// the code-point cap, or carrying a bidi override or a zero-width character.
// Checked at render as well as at publish, because a row written by an earlier
// build is data from another process and gets the treatment a request would.
[[nodiscard]] Result<Rendered> render(std::span<const TemplateSpec> table, TemplateId id,
                                      Locale locale, std::span<const Param> params);

// Whether a parameter value is acceptable, exposed so a publish can refuse at the
// boundary instead of storing something that will not render.
[[nodiscard]] Status check_param(const Param& param) noexcept;

}  // namespace anvil::notifications
