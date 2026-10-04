#pragma once

// Timing-safe comparison.
//
// operator== and std::memcmp short-circuit on the first differing byte, so the
// time they take reveals how many leading bytes matched. Against a token or a
// MAC that is an oracle: an attacker recovers the value one byte at a time.
// Every secret comparison in this codebase goes through here (CLAUDE.md §5).

#include <openssl/crypto.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anvil::crypto {

// Length-independent by construction: differing lengths return false without
// comparing contents, since a length difference is not itself secret.
[[nodiscard]] inline bool secure_equal(std::span<const std::uint8_t> a,
                                       std::span<const std::uint8_t> b) noexcept {
    if (a.size() != b.size()) { return false; }
    if (a.empty()) { return true; }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

template <std::size_t N>
[[nodiscard]] inline bool secure_equal(const std::array<std::uint8_t, N>& a,
                                       const std::array<std::uint8_t, N>& b) noexcept {
    return CRYPTO_memcmp(a.data(), b.data(), N) == 0;
}

}  // namespace anvil::crypto
