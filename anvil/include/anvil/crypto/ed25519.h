#pragma once

// Ed25519 (RFC 8032), for the device-link signature in chat
// (docs/22-chat.md §7.3).
//
// The server verifies one thing with this: that a new device was admitted by a
// signature from an existing device's identity key, which is what makes a
// stolen session cookie insufficient to add a device. It also checks keys as
// bytes before storing them. It cannot judge whether a key is good; it can
// refuse one that is malformed, and a malformed key accepted here is a key some
// other client chokes on later.
//
// Signing exists for tests and for the reference application's vector emitter.
// The server never holds a device's signing key, so nothing on a request path
// signs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "anvil/crypto/secret.h"

namespace anvil::crypto {

inline constexpr std::size_t kEd25519PublicKeyBytes = 32;
inline constexpr std::size_t kEd25519SignatureBytes = 64;
// RFC 8032's "private key" is this 32-byte seed; the expanded scalar is derived
// from it on every use and never leaves OpenSSL.
inline constexpr std::size_t kEd25519SeedBytes = 32;

using Ed25519PublicKey = std::array<std::uint8_t, kEd25519PublicKeyBytes>;
using Ed25519Signature = std::array<std::uint8_t, kEd25519SignatureBytes>;
using Ed25519Seed = SecretBuffer<kEd25519SeedBytes>;

// True when the bytes are a strict RFC 8032 §5.1.3 encoding of a curve point
// that is not of small order.
//
// Guarantees, beyond the length the type already fixes:
//   - y < p. OpenSSL reduces a non-canonical y silently, so two byte strings
//     would name one key; a client comparing keys as bytes, or showing a
//     safety number computed over them, would disagree with one that does not.
//   - The x recovered from y exists, and x = 0 is not encoded with the sign
//     bit set (the other non-canonical spelling OpenSSL accepts).
//   - Not one of the eight small-order points, under any of their encodings —
//     the identity among them. A small-order key makes a "signature" that
//     verifies for many messages, which is not what a device identity is.
//
// Does NOT guarantee the point is in the prime-order subgroup: a mixed-order
// point (a valid key plus a small-order component) passes. Excluding those
// needs a scalar multiplication by the group order, which OpenSSL does not
// expose for Ed25519 and which this library will not hand-write. Such a key
// can only be chosen deliberately by its own holder, and it does not help
// anyone forge a signature under somebody else's key.
[[nodiscard]] bool ed25519_public_key_is_valid(
    std::span<const std::uint8_t, kEd25519PublicKeyBytes> public_key) noexcept;

// RFC 8032 pure Ed25519 verification, after ed25519_public_key_is_valid. False
// for an invalid key, a non-canonical S, a wrong signature, or an allocation
// failure inside OpenSSL; never throws. The caller cannot tell these apart and
// has no reason to: every one of them means "not admitted".
//
// OpenSSL checks the cofactorless equation [S]B = R + [k]A and rejects S >= L.
// Its R check is a byte comparison against a canonical re-encoding, so a
// non-canonical R fails as well.
[[nodiscard]] bool ed25519_verify(std::span<const std::uint8_t, kEd25519PublicKeyBytes> public_key,
                                  std::span<const std::uint8_t> message,
                                  std::span<const std::uint8_t, kEd25519SignatureBytes> signature) noexcept;

// Throws CryptoError if OpenSSL cannot load the seed or sign. Ed25519 signing
// is deterministic, which is what lets a test reproduce RFC 8032's bytes.
[[nodiscard]] Ed25519PublicKey ed25519_public_key_from_seed(const Ed25519Seed& seed);
[[nodiscard]] Ed25519Signature ed25519_sign(const Ed25519Seed& seed,
                                            std::span<const std::uint8_t> message);

struct Ed25519Keypair final {
    Ed25519Seed seed;
    Ed25519PublicKey public_key;
};

// A fresh seed from the CSPRNG. For tests and vector emitters only.
[[nodiscard]] Ed25519Keypair ed25519_generate_keypair();

}  // namespace anvil::crypto
