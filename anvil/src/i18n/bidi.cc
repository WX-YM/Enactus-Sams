#include "anvil/i18n/bidi.h"

#include <cstddef>

namespace anvil::i18n {
namespace {

// Both already yield unsigned int through integral promotion, so an explicit
// cast would be a no-op the compiler rightly flags as useless.
[[nodiscard]] constexpr std::uint32_t lead(char c, std::uint32_t mask) noexcept {
    return static_cast<unsigned char>(c) & mask;
}
[[nodiscard]] constexpr std::uint32_t cont(char c) noexcept {
    return static_cast<unsigned char>(c) & 0x3FU;
}

// Every character policed here is 3 bytes in UTF-8 except U+061C, which is 2.
// Decoding only the sequences that could match keeps this a single cheap pass.
[[nodiscard]] std::uint32_t decode(std::string_view text, std::size_t& i) noexcept {
    const auto byte = static_cast<unsigned char>(text[i]);

    if (byte < 0x80U) {
        i += 1;
        return byte;
    }
    if ((byte & 0xE0U) == 0xC0U && i + 1 < text.size()) {
        const std::uint32_t cp = (lead(text[i], 0x1FU) << 6) | cont(text[i + 1]);
        i += 2;
        return cp;
    }
    if ((byte & 0xF0U) == 0xE0U && i + 2 < text.size()) {
        const std::uint32_t cp = (lead(text[i], 0x0FU) << 12) | (cont(text[i + 1]) << 6) |
                                 cont(text[i + 2]);
        i += 3;
        return cp;
    }
    if ((byte & 0xF8U) == 0xF0U && i + 3 < text.size()) {
        i += 4;
        return 0x10000U;   // outside every policed range; exact value irrelevant
    }
    i += 1;
    return byte;
}

[[nodiscard]] constexpr bool is_zero_width(std::uint32_t cp) noexcept {
    return (cp >= 0x200BU && cp <= 0x200DU) ||   // ZWSP, ZWNJ, ZWJ
           cp == 0xFEFFU;                        // BOM used mid-string
}

[[nodiscard]] constexpr bool is_mark(std::uint32_t cp) noexcept {
    return cp == 0x200EU ||    // left-to-right mark
           cp == 0x200FU ||    // right-to-left mark
           cp == 0x061CU;      // Arabic letter mark
}

[[nodiscard]] constexpr bool is_embedding_or_override(std::uint32_t cp) noexcept {
    return cp >= 0x202AU && cp <= 0x202EU;   // LRE, RLE, PDF, LRO, RLO
}

[[nodiscard]] constexpr bool is_isolate(std::uint32_t cp) noexcept {
    return cp >= 0x2066U && cp <= 0x2069U;   // LRI, RLI, FSI, PDI
}

}  // namespace

BidiIssue check(std::string_view text, TextClass text_class) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = decode(text, i);

        // ZWNJ and ZWJ are orthographically meaningful in Persian and in some
        // Arabic ligatures, but not in an identifier — and in an identifier
        // they create two visually identical values that compare unequal.
        if (is_zero_width(cp)) {
            if (text_class == TextClass::Identifier) { return BidiIssue::ZeroWidth; }
            continue;
        }

        if (is_mark(cp)) {
            if (text_class == TextClass::Identifier) { return BidiIssue::Mark; }
            continue;
        }

        // Overrides are rejected everywhere. Nothing legitimate needs to force
        // the direction of text that already has an intrinsic direction.
        if (is_embedding_or_override(cp)) { return BidiIssue::Override; }

        if (is_isolate(cp) && text_class == TextClass::Identifier) {
            return BidiIssue::Override;
        }
    }
    return BidiIssue::Ok;
}

}  // namespace anvil::i18n
