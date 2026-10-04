#pragma once

// AES-256-SIV, a deterministic AEAD (RFC 5297), for the chat media grant
// (docs/22-chat.md §6.1).
//
// Why deterministic: the grant is a URL. The same object in the same expiry
// bucket must be the same URL, or the browser's cache never hits and every
// scroll through a conversation re-downloads every image. A random-IV AEAD
// mints a new URL per call. The one thing deterministic encryption reveals is
// that two sealed values are equal, and that equality is exactly what a cache
// needs to see.
//
// Why SIV and not GCM with a derived nonce: deterministic GCM repeats its nonce
// for every repeated input, and a repeated GCM nonce leaks the XOR of the
// plaintexts and hands out the authentication key, so tags become forgeable.
// SIV derives its IV from the key, the associated data and the plaintext, so a
// repeat reveals equality and nothing else (it is nonce-misuse resistant).
//
// Layout: [0..15] the 128-bit synthetic IV, which is also the tag, then the
// ciphertext, exactly as long as the plaintext. There is no version byte: the
// grant is short-lived (minutes), so a scheme change needs no migration, and
// every byte is a byte of URL.
//
// Exactly one associated-data component is authenticated, always — including
// when it is empty. RFC 5297 treats "one empty component" and "no component" as
// different inputs, and the caller binding a grant to its context needs one,
// not a vector of them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace anvil::crypto {

// Two AES-256 keys: one for the S2V MAC, one for CTR.
inline constexpr std::size_t kSivKeyBytes = 64;
inline constexpr std::size_t kSivTagBytes = 16;

// Throws CryptoError on a key that is not kSivKeyBytes long, as seal() does:
// the key comes from configuration, so a wrong size is a deployment defect to
// surface at the first call, not a per-request outcome to return.
[[nodiscard]] std::vector<std::uint8_t> siv_seal(std::span<const std::uint8_t> key,
                                                 std::span<const std::uint8_t> plaintext,
                                                 std::span<const std::uint8_t> aad);

// nullopt on a wrong key, wrong aad, tampered or truncated input — the caller
// cannot tell which, deliberately: the input is a URL anyone can edit, and a
// distinguishable failure is an oracle. Throws only on a wrong-sized key.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> siv_open(
    std::span<const std::uint8_t> key, std::span<const std::uint8_t> sealed,
    std::span<const std::uint8_t> aad);

}  // namespace anvil::crypto
