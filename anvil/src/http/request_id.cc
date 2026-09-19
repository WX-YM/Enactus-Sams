#include "anvil/http/request_id.h"

#include <cstdint>
#include <span>

#include "anvil/crypto/random.h"

namespace anvil::http {
namespace {

// Bit `n` counted from the most significant bit of the whole sixteen-byte value,
// so the loop below can read the 128 bits as one big-endian string and not care
// where byte boundaries fall.
[[nodiscard]] std::uint8_t bit_at(const std::array<std::uint8_t, 16>& bytes,
                                  std::size_t n) noexcept {
    const std::uint8_t byte = bytes[n / 8U];
    return static_cast<std::uint8_t>((byte >> (7U - (n % 8U))) & 1U);
}

// Two zero bits lead, so that 26 groups of five cover the 128 real bits exactly.
inline constexpr std::size_t kPadBits = 2;

}  // namespace

RequestId mint_request_id(std::int64_t now_unix_ms) {
    RequestId id{};

    // The low 48 bits of the millisecond, big-endian. Six bytes reaches the year
    // 10889, so there is no wrap worth handling and no branch here to get wrong.
    const std::uint64_t ms = static_cast<std::uint64_t>(now_unix_ms);
    for (std::size_t i = 0; i < 6U; ++i) {
        id.bytes[i] = static_cast<std::uint8_t>((ms >> (40U - 8U * i)) & 0xFFU);
    }

    // The remaining ten bytes. Written straight into the tail rather than into a
    // temporary, so the entropy exists in exactly one place.
    crypto::random_bytes(std::span<std::uint8_t>{id.bytes.data() + 6, 10});
    return id;
}

std::array<char, kRequestIdChars> format_request_id(const RequestId& id) noexcept {
    std::array<char, kRequestIdChars> out{};
    for (std::size_t i = 0; i < kRequestIdChars; ++i) {
        std::uint8_t value = 0;
        for (std::size_t j = 0; j < 5U; ++j) {
            const std::size_t padded = i * 5U + j;
            // The first two positions are the padding, and they are zero. Every
            // other position maps onto a real bit.
            const std::uint8_t bit =
                padded < kPadBits ? std::uint8_t{0} : bit_at(id.bytes, padded - kPadBits);
            value = static_cast<std::uint8_t>((value << 1U) | bit);
        }
        out[i] = kCrockfordAlphabet[value];
    }
    return out;
}

void append_request_id(std::string& out, const RequestId& id) {
    const std::array<char, kRequestIdChars> rendered = format_request_id(id);
    out.append(rendered.data(), rendered.size());
}

}  // namespace anvil::http
