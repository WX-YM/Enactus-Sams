#pragma once

// X25519 public-key validation (RFC 7748), for the device agreement keys in
// chat (docs/22-chat.md §7.3).
//
// The server never performs key agreement: it never holds a key that decrypts
// (§7). It stores the X25519 keys clients upload and hands them to other
// clients, so all it can do is refuse bytes that are malformed. It cannot judge
// whether a key is good, and a malformed key accepted here is a key some other
// client chokes on later.
//
// Key generation and derivation are here for tests and the reference
// application's vector emitter only. Nothing on a request path calls them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "anvil/crypto/secret.h"

namespace anvil::crypto {

inline constexpr std::size_t kX25519PublicKeyBytes = 32;
inline constexpr std::size_t kX25519PrivateKeyBytes = 32;
inline constexpr std::size_t kX25519SharedSecretBytes = 32;

using X25519PublicKey = std::array<std::uint8_t, kX25519PublicKeyBytes>;
using X25519PrivateKey = SecretBuffer<kX25519PrivateKeyBytes>;
using X25519SharedSecret = SecretBuffer<kX25519SharedSecretBytes>;

// True when the bytes are a canonical u-coordinate that is not of low order.
//
// Guarantees:
//   - Canonical: bit 255 is clear and u < p. RFC 7748 §5 has a receiver mask
//     bit 255 and reduce u mod p, so every key has up to four spellings that
//     agree with it. A client deriving with the key would not notice; a client
//     comparing keys as bytes, or computing a safety number over them, would.
//     WebCrypto exports canonical bytes, so an honest client never sends the
//     other spellings.
//   - Not low order. RFC 7748 §6.1 has each party check its shared secret is
//     not all zero; the server has no shared secret, so it refuses the inputs
//     that produce one instead. The table in x25519.cc lists them, compared
//     with bit 255 masked as X25519 itself masks it, so the table holds even
//     without the canonical check.
//
// Does NOT guarantee the key is in the prime-order subgroup or on the curve
// rather than its twist: X25519 is designed to be safe for every 32-byte input
// apart from the low-order ones, and RFC 7748 asks for no more.
[[nodiscard]] bool x25519_public_key_is_valid(
    std::span<const std::uint8_t, kX25519PublicKeyBytes> public_key) noexcept;

struct X25519Keypair final {
    X25519PrivateKey private_key;
    X25519PublicKey public_key;
};

// A fresh private key from the CSPRNG. Throws CryptoError on failure.
[[nodiscard]] X25519Keypair x25519_generate_keypair();

// X25519(private_key, peer_public), without validating the peer first, so RFC
// 7748's own vectors (one of which sets bit 255) can be reproduced. nullopt
// when OpenSSL refuses because the result is all zero, which is the low-order
// case. Throws CryptoError if OpenSSL cannot load a key.
[[nodiscard]] std::optional<X25519SharedSecret> x25519_derive(
    const X25519PrivateKey& private_key,
    std::span<const std::uint8_t, kX25519PublicKeyBytes> peer_public);

}  // namespace anvil::crypto
