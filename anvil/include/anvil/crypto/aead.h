#pragma once

// AES-256-GCM sealed envelopes, for PII at rest (docs/13-dynamic-forms.md §6).
//
// The threat model is a database dump: an Egyptian National ID encodes date of
// birth, governorate and gender, so a leaked submissions collection discloses
// full identity records. The key lives outside the database credential, so
// holding the dump is not enough.
//
// Envelope layout, stored as a single BSON BinData blob:
//
//   [0]      version byte  — lets the scheme be rotated without a migration
//   [1..12]  96-bit IV     — fresh CSPRNG bytes per seal, NEVER reused
//   [13..28] 128-bit tag
//   [29..]   ciphertext
//
// A repeated IV under the same key destroys GCM's confidentiality and lets an
// attacker forge tags, so the IV is generated inside seal() and is not a
// parameter — a caller cannot supply one by mistake.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::crypto {

inline constexpr std::uint8_t kAeadVersion = 1;
inline constexpr std::size_t kAeadIvBytes = 12;
inline constexpr std::size_t kAeadTagBytes = 16;
inline constexpr std::size_t kAeadOverhead = 1 + kAeadIvBytes + kAeadTagBytes;

// `aad` is authenticated but not encrypted. Bind the envelope to its context —
// the form id and the field id — so a ciphertext moved to another field or
// another form fails to open instead of decrypting into the wrong record.
[[nodiscard]] std::vector<std::uint8_t> seal(std::span<const std::uint8_t> key,
                                             std::string_view plaintext,
                                             std::span<const std::uint8_t> aad);

// Returns nullopt when the tag does not verify: tampered, truncated, wrong key,
// or wrong aad. The caller cannot distinguish these, deliberately.
[[nodiscard]] std::optional<std::string> open(std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> envelope,
                                              std::span<const std::uint8_t> aad);

}  // namespace anvil::crypto
