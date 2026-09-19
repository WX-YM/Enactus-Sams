#include "anvil/i18n/digits.h"

#include <cstdint>

namespace anvil::i18n {
namespace {

// Both ranges encode as three-byte UTF-8 sequences:
//   U+0660..U+0669  ->  D9 A0 .. D9 A9
//   U+06F0..U+06F9  ->  DB B0 .. DB B9
// Matching on the byte pattern avoids decoding to code points entirely, which
// is what keeps this a single pass with no allocation.
constexpr unsigned char kArabicIndicLead = 0xD9U;
constexpr unsigned char kArabicIndicFirst = 0xA0U;
constexpr unsigned char kArabicIndicLast = 0xA9U;

constexpr unsigned char kExtendedLead = 0xDBU;
constexpr unsigned char kExtendedFirst = 0xB0U;
constexpr unsigned char kExtendedLast = 0xB9U;

// Returns the ASCII digit for a two-byte sequence at `i`, or 0 if it is not a
// non-ASCII digit.
[[nodiscard]] char digit_at(std::string_view text, std::size_t i) noexcept {
    if (i + 1 >= text.size()) { return '\0'; }

    const auto lead = static_cast<unsigned char>(text[i]);
    const auto trail = static_cast<unsigned char>(text[i + 1]);

    if (lead == kArabicIndicLead && trail >= kArabicIndicFirst && trail <= kArabicIndicLast) {
        return static_cast<char>('0' + (trail - kArabicIndicFirst));
    }
    if (lead == kExtendedLead && trail >= kExtendedFirst && trail <= kExtendedLast) {
        return static_cast<char>('0' + (trail - kExtendedFirst));
    }
    return '\0';
}

}  // namespace

bool has_non_ascii_digits(std::string_view text) noexcept {
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (digit_at(text, i) != '\0') { return true; }
    }
    return false;
}

std::string fold_digits(std::string_view text) {
    // The overwhelmingly common case is all-ASCII input; do not pay for a copy
    // and a scan when there is nothing to fold.
    if (!has_non_ascii_digits(text)) { return std::string{text}; }

    std::string out;
    out.reserve(text.size());   // folding only ever shrinks: 2 bytes -> 1

    for (std::size_t i = 0; i < text.size();) {
        const char folded = digit_at(text, i);
        if (folded != '\0') {
            out.push_back(folded);
            i += 2;
        } else {
            out.push_back(text[i]);
            i += 1;
        }
    }
    return out;
}

std::optional<std::size_t> fold_digits_into(std::string_view text, std::span<char> out) noexcept {
    std::size_t written = 0;

    for (std::size_t i = 0; i < text.size();) {
        if (written >= out.size()) { return std::nullopt; }

        const char folded = digit_at(text, i);
        if (folded != '\0') {
            out[written++] = folded;
            i += 2;
        } else {
            out[written++] = text[i];
            i += 1;
        }
    }
    return written;
}

}  // namespace anvil::i18n
