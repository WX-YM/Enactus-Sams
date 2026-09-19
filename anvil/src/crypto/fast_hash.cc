#include "anvil/crypto/fast_hash.h"

#include <xxhash.h>

#include <cstddef>

namespace anvil::crypto {
namespace {

constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

// Little-endian, written out explicitly. XXH64_hash_t is a native-endian
// integer, and this value is PERSISTED in `sections.etag` — a byte order that
// follows the host would make a document written on one machine compare
// unequal on another (the same rule PermSet::to_bytes follows).
[[nodiscard]] FastDigest to_bytes(std::uint64_t value) noexcept {
    FastDigest out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
    }
    return out;
}

}  // namespace

FastDigest xxh3_64(std::span<const std::uint8_t> data) noexcept {
    return to_bytes(XXH3_64bits(data.data(), data.size()));
}

FastDigest xxh3_64(std::string_view data) noexcept {
    return to_bytes(XXH3_64bits(data.data(), data.size()));
}

std::array<char, 18> etag_of(const FastDigest& digest) noexcept {
    std::array<char, 18> out{};
    out[0] = '"';
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[1 + (i * 2)] = kHex[(digest[i] >> 4U) & 0x0FU];
        out[2 + (i * 2)] = kHex[digest[i] & 0x0FU];
    }
    out[17] = '"';
    return out;
}

}  // namespace anvil::crypto
