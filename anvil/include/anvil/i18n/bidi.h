#pragma once

// Bidi control and invisible-character policy.
//
// This system legitimately contains right-to-left text, so "strip non-ASCII" is
// not an available defence. Instead the explicit bidi CONTROL characters are
// policed, and the policy differs by field class.
//
// U+202E (right-to-left override) reverses subsequent display, so
//     exe.<U+202E>gnp.evil
// renders in a dashboard as `evil.png` while remaining an executable. The same
// trick spoofs usernames and note titles.

#include <cstdint>
#include <string_view>

namespace anvil::i18n {

enum class TextClass : std::uint8_t {
    // Filenames, usernames, emails, slugs, keys. No bidi control of any kind,
    // no zero-width characters: these are compared, indexed and displayed in
    // lists where a spoofed value is indistinguishable from a real one.
    Identifier,

    // Note bodies, section copy, form labels. Isolates are genuinely required
    // to render mixed English/Arabic sentences correctly, so they are allowed;
    // overrides are not, because nothing legitimate needs them.
    Prose,
};

enum class BidiIssue : std::uint8_t {
    Ok = 0,
    Override,     // U+202A..U+202E, U+2066..U+2069 depending on class
    Mark,         // U+200E, U+200F, U+061C
    ZeroWidth,    // U+200B..U+200D, U+FEFF
};

// Assumes `text` is already valid UTF-8 (anvil/i18n/utf8.h).
[[nodiscard]] BidiIssue check(std::string_view text, TextClass text_class) noexcept;

[[nodiscard]] inline bool is_acceptable(std::string_view text, TextClass text_class) noexcept {
    return check(text, text_class) == BidiIssue::Ok;
}

}  // namespace anvil::i18n
