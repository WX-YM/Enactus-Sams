#pragma once

// The only source of randomness in this codebase.
//
// std::rand, std::mt19937 and anything time-seeded are banned for security
// purposes (CLAUDE.md §5). A predictable capability id, session token or IV is
// indistinguishable from having no protection at all.
//
// A CSPRNG failure throws. It must never fall back to a weaker source: silent
// degradation here produces guessable tokens that look completely normal.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "anvil/crypto/secret.h"

namespace anvil::crypto {

// Fills the whole span. Throws CryptoError if the CSPRNG is unavailable.
void random_bytes(std::span<std::uint8_t> out);

template <std::size_t N>
[[nodiscard]] std::array<std::uint8_t, N> random_array() {
    std::array<std::uint8_t, N> out{};
    random_bytes(out);
    return out;
}

template <std::size_t N>
[[nodiscard]] SecretBuffer<N> random_secret() {
    SecretBuffer<N> out;
    random_bytes(out.mutable_span());
    return out;
}

// Uniform in [0, bound). Returns 0 for a bound of 0 or 1.
//
// Rejection sampling, not `% bound`. The modulo of a uniform 32-bit draw is
// biased towards the low values whenever `bound` does not divide 2^32, and the
// two places this is used — retry backoff jitter and recurring-schedule jitter —
// exist specifically to spread work out. Jitter that clusters is jitter that
// does not do its job (docs/10-timer-jobs.md §7, §8).
[[nodiscard]] std::uint32_t random_below(std::uint32_t bound);

// 32 CSPRNG bytes rendered as unpadded base64url: 43 characters, 256 bits.
// Used for refresh tokens, capability tokens and webhook secrets. Returned once
// to the caller; only SHA-256(token ‖ pepper) is ever stored.
[[nodiscard]] std::string random_token();

}  // namespace anvil::crypto
