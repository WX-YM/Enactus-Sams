#pragma once

// Unpadded base64url (RFC 4648 §5).
//
// Unpadded because '=' is not URL-safe in every position and adds nothing: the
// output length already determines the input length. Decoding rejects padding,
// whitespace, and the standard-alphabet '+' and '/' rather than accepting them
// leniently — a decoder that accepts several spellings of the same value is a
// parser-differential vector when one component canonicalises and another does
// not.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::crypto {

[[nodiscard]] std::string base64url_encode(std::span<const std::uint8_t> input);

// Decodes into a caller-provided buffer. Returns the number of bytes written,
// or nullopt if the input is malformed or does not fit. No allocation, so this
// is the form used on the token verification path where the budget is
// < 15 us with at most one allocation (docs/04-access-control.md §4).
[[nodiscard]] std::optional<std::size_t> base64url_decode_into(
    std::string_view input, std::span<std::uint8_t> out) noexcept;

// Allocating convenience form. Not for the request path.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64url_decode(
    std::string_view input);

// Exact decoded size for a given encoded length, or nullopt if that length is
// impossible. Lets callers size a stack buffer before decoding.
[[nodiscard]] constexpr std::optional<std::size_t> base64url_decoded_size(
    std::size_t encoded_len) noexcept {
    const std::size_t remainder = encoded_len % 4;
    // A remainder of 1 cannot occur: one base64 character carries 6 bits, which
    // is not enough to encode any whole byte.
    if (remainder == 1) { return std::nullopt; }
    const std::size_t full_groups = encoded_len / 4;
    const std::size_t tail = (remainder == 0) ? 0 : remainder - 1;
    return full_groups * 3 + tail;
}

}  // namespace anvil::crypto
