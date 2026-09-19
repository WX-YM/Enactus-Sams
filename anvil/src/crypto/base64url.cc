#include "anvil/crypto/base64url.h"

#include <array>

namespace anvil::crypto {
namespace {

constexpr std::array<char, 64> kAlphabet{
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_'};

// Built at compile time into .rodata: no runtime initialisation, shared across
// every thread. 0xFF marks a character outside the alphabet, which includes
// '+', '/', '=' and all whitespace — all rejected deliberately.
constexpr std::array<std::uint8_t, 256> kReverse = [] {
    std::array<std::uint8_t, 256> table{};
    for (auto& slot : table) { slot = 0xFFU; }
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<std::size_t>(static_cast<unsigned char>(kAlphabet[i]))] =
            static_cast<std::uint8_t>(i);
    }
    return table;
}();

}  // namespace

std::string base64url_encode(std::span<const std::uint8_t> input) {
    std::string out;
    if (input.empty()) { return out; }

    // Exact final size, so no reallocation during the loop.
    out.reserve(((input.size() * 4) + 2) / 3);

    std::size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        const std::uint32_t group = (static_cast<std::uint32_t>(input[i]) << 16) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(input[i + 2]);
        out.push_back(kAlphabet[(group >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 6) & 0x3FU]);
        out.push_back(kAlphabet[group & 0x3FU]);
    }

    const std::size_t remaining = input.size() - i;
    if (remaining == 1) {
        const std::uint32_t group = static_cast<std::uint32_t>(input[i]) << 16;
        out.push_back(kAlphabet[(group >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12) & 0x3FU]);
    } else if (remaining == 2) {
        const std::uint32_t group = (static_cast<std::uint32_t>(input[i]) << 16) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 8);
        out.push_back(kAlphabet[(group >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 6) & 0x3FU]);
    }
    return out;
}

std::optional<std::size_t> base64url_decode_into(std::string_view input,
                                                 std::span<std::uint8_t> out) noexcept {
    const std::optional<std::size_t> needed = base64url_decoded_size(input.size());
    if (!needed.has_value() || *needed > out.size()) { return std::nullopt; }
    if (input.empty()) { return std::size_t{0}; }

    std::size_t written = 0;
    std::size_t i = 0;

    for (; i + 3 < input.size(); i += 4) {
        std::uint32_t group = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint8_t digit = kReverse[static_cast<unsigned char>(input[i + k])];
            if (digit == 0xFFU) { return std::nullopt; }
            group = (group << 6) | digit;
        }
        out[written++] = static_cast<std::uint8_t>((group >> 16) & 0xFFU);
        out[written++] = static_cast<std::uint8_t>((group >> 8) & 0xFFU);
        out[written++] = static_cast<std::uint8_t>(group & 0xFFU);
    }

    const std::size_t remaining = input.size() - i;
    if (remaining == 0) { return written; }

    std::uint32_t group = 0;
    for (std::size_t k = 0; k < remaining; ++k) {
        const std::uint8_t digit = kReverse[static_cast<unsigned char>(input[i + k])];
        if (digit == 0xFFU) { return std::nullopt; }
        group = (group << 6) | digit;
    }

    // The trailing group carries 2 or 4 bits that encode nothing. Requiring them
    // to be zero keeps the encoding canonical — otherwise several distinct
    // strings decode to the same bytes, and a hash lookup keyed on the string
    // form can be bypassed by re-spelling it.
    if (remaining == 2) {
        if ((group & 0x0FU) != 0U) { return std::nullopt; }
        out[written++] = static_cast<std::uint8_t>((group >> 4) & 0xFFU);
    } else {  // remaining == 3
        if ((group & 0x03U) != 0U) { return std::nullopt; }
        out[written++] = static_cast<std::uint8_t>((group >> 10) & 0xFFU);
        out[written++] = static_cast<std::uint8_t>((group >> 2) & 0xFFU);
    }
    return written;
}

std::optional<std::vector<std::uint8_t>> base64url_decode(std::string_view input) {
    const std::optional<std::size_t> needed = base64url_decoded_size(input.size());
    if (!needed.has_value()) { return std::nullopt; }

    std::vector<std::uint8_t> out(*needed);
    const std::optional<std::size_t> written = base64url_decode_into(input, out);
    if (!written.has_value()) { return std::nullopt; }
    out.resize(*written);
    return out;
}

}  // namespace anvil::crypto
