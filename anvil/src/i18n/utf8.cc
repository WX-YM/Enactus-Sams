#include "anvil/i18n/utf8.h"

#include <simdutf.h>

#include <cstdint>

namespace anvil::i18n {
namespace {

// Lead byte with its length marker removed, and continuation byte payload.
// Both already yield unsigned int through integral promotion, so an explicit
// cast here would be a no-op the compiler rightly flags as useless.
[[nodiscard]] constexpr std::uint32_t lead(char c, std::uint32_t mask) noexcept {
    return static_cast<unsigned char>(c) & mask;
}
[[nodiscard]] constexpr std::uint32_t cont(char c) noexcept {
    return static_cast<unsigned char>(c) & 0x3FU;
}

// Decodes the code point starting at `i`, assuming the sequence is already
// known to be structurally valid, and advances `i` past it.
[[nodiscard]] std::uint32_t decode_valid(std::string_view text, std::size_t& i) noexcept {
    const auto byte = static_cast<unsigned char>(text[i]);

    if (byte < 0x80U) {
        i += 1;
        return byte;
    }
    if ((byte & 0xE0U) == 0xC0U) {
        const std::uint32_t cp = (lead(text[i], 0x1FU) << 6) | cont(text[i + 1]);
        i += 2;
        return cp;
    }
    if ((byte & 0xF0U) == 0xE0U) {
        const std::uint32_t cp = (lead(text[i], 0x0FU) << 12) | (cont(text[i + 1]) << 6) |
                                 cont(text[i + 2]);
        i += 3;
        return cp;
    }
    const std::uint32_t cp = (lead(text[i], 0x07U) << 18) | (cont(text[i + 1]) << 12) |
                             (cont(text[i + 2]) << 6) | cont(text[i + 3]);
    i += 4;
    return cp;
}

[[nodiscard]] constexpr bool is_non_character(std::uint32_t cp) noexcept {
    // The Arabic Presentation Forms block sits just above U+FDEF, so the upper
    // bound here matters: getting it wrong would reject legitimate Arabic.
    if (cp >= 0xFDD0U && cp <= 0xFDEFU) { return true; }
    return (cp & 0xFFFEU) == 0xFFFEU;   // U+xFFFE and U+xFFFF on every plane
}

// Unicode combining marks that attach to the preceding character. Arabic
// tashkeel live in U+064B..U+065F and U+0670; splitting them from their base
// letter renders as a stray diacritic.
[[nodiscard]] constexpr bool is_combining(std::uint32_t cp) noexcept {
    return (cp >= 0x0300U && cp <= 0x036FU) ||   // combining diacritical marks
           (cp >= 0x064BU && cp <= 0x065FU) ||   // Arabic tashkeel
           (cp == 0x0670U) ||                    // Arabic superscript alef
           (cp >= 0x06D6U && cp <= 0x06DCU) ||   // Quranic annotation
           (cp >= 0x06DFU && cp <= 0x06E8U) ||
           (cp >= 0x06EAU && cp <= 0x06EDU) ||
           (cp >= 0x0E31U && cp <= 0x0E3AU) ||   // Thai
           (cp == 0x200DU);                      // ZWJ binds emoji sequences
}

}  // namespace

bool is_structurally_valid(std::string_view text) noexcept {
    if (text.empty()) { return true; }
    return simdutf::validate_utf8(text.data(), text.size());
}

Utf8Error validate(std::string_view text) noexcept {
    if (text.empty()) { return Utf8Error::Ok; }

    // Structure first: the decoder below assumes valid input, and checking
    // structure with simdutf is far cheaper than a scalar pass.
    if (!simdutf::validate_utf8(text.data(), text.size())) { return Utf8Error::Malformed; }

    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = decode_valid(text, i);
        if (cp == 0U) { return Utf8Error::EmbeddedNul; }
        if (is_non_character(cp)) { return Utf8Error::NonCharacter; }
    }
    return Utf8Error::Ok;
}

std::size_t count_code_points(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const char c : text) {
        // Continuation bytes are 10xxxxxx; everything else starts a code point.
        count += ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ? 1U : 0U;
    }
    return count;
}

bool within_code_point_bounds(std::string_view text, std::size_t min_cp,
                              std::size_t max_cp) noexcept {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++count;
            // Stop as soon as the limit is exceeded: a hostile body must not
            // cost a full scan before it is rejected.
            if (count > max_cp) { return false; }
        }
    }
    return count >= min_cp;
}

std::size_t code_point_boundary_before(std::string_view text, std::size_t byte_offset) noexcept {
    // The end of the string is always a boundary, and indexing at size() would
    // be an out-of-bounds read — this must return before touching text[offset].
    if (byte_offset >= text.size()) { return text.size(); }

    std::size_t offset = byte_offset;
    // At most 3 steps back: a UTF-8 sequence is never longer than 4 bytes.
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
        --offset;
    }
    return offset;
}

std::string_view truncate_to_code_points(std::string_view text, std::size_t max_cp) noexcept {
    if (max_cp == 0) { return {}; }

    std::size_t count = 0;
    std::size_t cut = text.size();
    std::size_t last_base = 0;   // start of the most recent non-combining code point

    for (std::size_t i = 0; i < text.size();) {
        const std::size_t start = i;
        const std::uint32_t cp = decode_valid(text, i);
        if (!is_combining(cp)) { last_base = start; }

        ++count;
        if (count > max_cp) {
            cut = start;
            break;
        }
    }

    if (cut == text.size()) { return text; }

    // If the cut would land immediately after a base character whose combining
    // marks are being dropped, back up to before the base too — a bare letter
    // that lost its tashkeel is wrong, and a bare tashkeel is worse.
    if (cut > 0 && cut > last_base) {
        std::size_t probe = cut;
        const std::uint32_t next = decode_valid(text, probe);
        if (is_combining(next)) { cut = last_base; }
    }
    return text.substr(0, cut);
}

}  // namespace anvil::i18n
