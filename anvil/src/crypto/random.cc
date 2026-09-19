#include "anvil/crypto/random.h"

#include <openssl/rand.h>

#include <limits>

#include "anvil/crypto/base64url.h"
#include "anvil/crypto/errors.h"

namespace anvil::crypto {

void random_bytes(std::span<std::uint8_t> out) {
    if (out.empty()) { return; }

    // RAND_bytes takes an int. A span longer than INT_MAX would silently
    // truncate, leaving the tail of the buffer as uninitialised stack.
    if (out.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw CryptoError{"random_bytes: request too large"};
    }

    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        throw CryptoError{"RAND_bytes failed"};
    }
}

std::uint32_t random_below(std::uint32_t bound) {
    if (bound < 2) { return 0; }

    // The largest multiple of `bound` that fits in 32 bits. Draws at or above it
    // are discarded, which removes the bias entirely at the cost of an expected
    // handful of extra draws in the worst case.
    const std::uint32_t limit =
        std::numeric_limits<std::uint32_t>::max() -
        (std::numeric_limits<std::uint32_t>::max() % bound);

    std::array<std::uint8_t, 4> raw{};
    while (true) {
        random_bytes(raw);
        const std::uint32_t draw =
            static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8U) |
            (static_cast<std::uint32_t>(raw[2]) << 16U) |
            (static_cast<std::uint32_t>(raw[3]) << 24U);
        if (draw < limit) { return draw % bound; }
    }
}

std::string random_token() {
    SecretBuffer<32> raw = random_secret<32>();
    return base64url_encode(raw.span());
}

}  // namespace anvil::crypto
