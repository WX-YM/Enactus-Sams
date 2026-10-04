#pragma once

// A fixed-size buffer for secret material that is guaranteed to be zeroed.
//
// std::memset before scope exit is legally removed by the optimiser: the writes
// are dead stores to an object whose lifetime is ending, so the compiler is
// entitled to delete them. OPENSSL_cleanse is written specifically to survive
// that (CLAUDE.md §5).
//
// Move-only. A copyable secret is a secret that ends up in a container, gets
// reallocated, and leaves a plaintext copy behind in freed memory.

#include <openssl/crypto.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace anvil::crypto {

template <std::size_t N>
class SecretBuffer final {
public:
    static constexpr std::size_t kSize = N;

    SecretBuffer() noexcept : bytes_{} {}

    ~SecretBuffer() { OPENSSL_cleanse(bytes_.data(), bytes_.size()); }

    SecretBuffer(const SecretBuffer&) = delete;
    SecretBuffer& operator=(const SecretBuffer&) = delete;

    SecretBuffer(SecretBuffer&& other) noexcept : bytes_{other.bytes_} {
        // The moved-from buffer still holds the plaintext; wipe it now rather
        // than waiting for its destructor, which may be far away.
        OPENSSL_cleanse(other.bytes_.data(), other.bytes_.size());
    }

    SecretBuffer& operator=(SecretBuffer&& other) noexcept {
        if (this != &other) {
            OPENSSL_cleanse(bytes_.data(), bytes_.size());
            bytes_ = other.bytes_;
            OPENSSL_cleanse(other.bytes_.data(), other.bytes_.size());
        }
        return *this;
    }

    [[nodiscard]] std::uint8_t* data() noexcept { return bytes_.data(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return N; }

    [[nodiscard]] std::span<std::uint8_t, N> mutable_span() noexcept { return bytes_; }
    [[nodiscard]] std::span<const std::uint8_t, N> span() const noexcept { return bytes_; }

    // Deliberately absent: operator==, operator<<, and any conversion to string.
    // Secrets are compared with secure_equal and never formatted.

private:
    std::array<std::uint8_t, N> bytes_;
};

using Key256 = SecretBuffer<32>;
using Digest256 = std::array<std::uint8_t, 32>;

}  // namespace anvil::crypto
