#pragma once

// UTF-8 validation, code-point counting, and boundary-safe truncation.
//
// std::string is a byte sequence with no encoding guarantee. Everything that
// crosses the trust boundary is validated here first, and INVALID INPUT IS
// REJECTED, NEVER REPAIRED: substituting U+FFFD turns an attack into corrupted
// data and destroys the signal that an attack occurred.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::i18n {

enum class Utf8Error : std::uint8_t {
    Ok = 0,
    Malformed,      // overlong, truncated, lone surrogate, or > U+10FFFF
    EmbeddedNul,    // U+0000 — terminates C strings; log and path injection
    NonCharacter,   // U+FFFE, U+FFFF, U+FDD0..U+FDEF — no interchange meaning
};

// Structural validity only: rejects overlong encodings, surrogates (CESU-8),
// truncated sequences and code points above U+10FFFF. Backed by simdutf, which
// runs at roughly 1 GB/s and allocates nothing.
[[nodiscard]] bool is_structurally_valid(std::string_view text) noexcept;

// Full policy check: structural validity plus the NUL and non-character rules.
// This is what request handlers call.
[[nodiscard]] Utf8Error validate(std::string_view text) noexcept;

// Counts code points without decoding: every byte whose top bits are not 10
// starts one. Single pass, no allocation.
//
// THIS is the unit every length limit is expressed in. Arabic is 2 bytes per
// character in UTF-8, so a limit enforced in bytes silently gives Arabic users
// half the allowance English users get.
[[nodiscard]] std::size_t count_code_points(std::string_view text) noexcept;

// True when the text is within [min_cp, max_cp] code points. Short-circuits as
// soon as max_cp is exceeded, so a hostile 10 MB body does not pay for a full
// count before being rejected.
[[nodiscard]] bool within_code_point_bounds(std::string_view text, std::size_t min_cp,
                                            std::size_t max_cp) noexcept;

// Byte offset of the code-point boundary at or before `byte_offset`. Cutting a
// string at an arbitrary byte offset splits a multi-byte sequence and produces
// invalid UTF-8, which then fails validation everywhere downstream.
[[nodiscard]] std::size_t code_point_boundary_before(std::string_view text,
                                                     std::size_t byte_offset) noexcept;

// Truncates to at most max_cp code points, landing on a grapheme boundary so
// Arabic combining marks (tashkeel) are not orphaned from the letter they
// attach to. Returns a view into `text` — no allocation, and the caller must
// keep `text` alive.
[[nodiscard]] std::string_view truncate_to_code_points(std::string_view text,
                                                       std::size_t max_cp) noexcept;

}  // namespace anvil::i18n
