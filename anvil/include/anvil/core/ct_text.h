#pragma once

// Compile-time text checks: the constant-evaluation half of anvil's UTF-8 policy.
//
// anvil's runtime validator is simdutf-backed and therefore not `constexpr`, and
// every seam that takes literals from an application needs to prove those
// literals at build time rather than discover them at first render. A section's
// default copy, a notification template's Arabic string — each is reviewed source
// that reaches a reader with no validation step in between, so the validation
// happens where the mistake is made.
//
// These run in `static_assert`s and nowhere else. They are deliberately
// STRUCTURAL plus the two policy rules a literal can break on its own: no
// embedded NUL, no surrogate, no overlong form, nothing above U+10FFFF. The rest
// of the policy — bidi controls, zero-width characters, confusables — applies to
// REQUEST data, which arrives through anvil/i18n/utf8.h at runtime.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace anvil::ct {

[[nodiscard]] constexpr std::size_t count_code_points(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) { ++count; }
    }
    return count;
}

[[nodiscard]] constexpr bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if (b0 < 0x80U) {
            if (b0 == 0U) { return false; }
            length = 1;
            code_point = b0;
        } else if ((b0 & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = b0 & 0x1FU;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = b0 & 0x0FU;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = b0 & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) { return false; }
        for (std::size_t k = 1; k < length; ++k) {
            const auto bk = static_cast<unsigned char>(text[i + k]);
            if ((bk & 0xC0U) != 0x80U) { return false; }
            code_point = (code_point << 6U) | (bk & 0x3FU);
        }
        // Overlong encodings are rejected: `C0 80` is a second spelling of NUL,
        // and two spellings of one character is how a filter and a consumer
        // disagree about what a string says.
        if (length == 2 && code_point < 0x80U) { return false; }
        if (length == 3 && code_point < 0x800U) { return false; }
        if (length == 4 && code_point < 0x10000U) { return false; }
        if (code_point > 0x10FFFFU) { return false; }
        if (code_point >= 0xD800U && code_point <= 0xDFFFU) { return false; }
        i += length;
    }
    return true;
}

[[nodiscard]] constexpr bool is_non_empty_utf8(std::string_view text) noexcept {
    return !text.empty() && is_valid_utf8(text);
}

}  // namespace anvil::ct
