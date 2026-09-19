#pragma once

// xxh3-64, for etags and other NON-SECURITY digests.
//
// The distinction matters and is enforced by naming rather than by hope:
// anything an attacker benefits from forging goes through anvil/crypto/digest.h
// (SHA-256, HMAC). This is for cache keys and entity tags, where the only
// requirement is that different content produce different bytes with
// overwhelming probability, and where SHA-256 over a 200 KB section payload on
// every write would be pure waste.
//
// An etag is computed ONCE, at write time, and stored (docs/12-sections-cms.md
// §2). A conditional GET is then a comparison against 8 stored bytes with no
// serialisation and no hashing at all — which is what makes the 304 path cost
// under 40 µs.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace anvil::crypto {

// 8 bytes, little-endian, exactly as stored in BSON BinData subtype 0.
using FastDigest = std::array<std::uint8_t, 8>;

[[nodiscard]] FastDigest xxh3_64(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] FastDigest xxh3_64(std::string_view data) noexcept;

// The digest as a quoted, 16-character-hex strong ETag: `"a1b2c3d4e5f60718"`.
// Quoted here rather than at the call site because an unquoted ETag is invalid
// per RFC 9110 and caches respond to it by simply ignoring the header — a
// failure that is completely silent and shows up only as a missing 304.
[[nodiscard]] std::array<char, 18> etag_of(const FastDigest& digest) noexcept;

}  // namespace anvil::crypto
