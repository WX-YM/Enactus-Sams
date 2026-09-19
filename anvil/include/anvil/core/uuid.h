#pragma once

// UUID generation and formatting.
//
// Two generators, and the choice between them is a security decision:
//
//   v7  internal document ids. Time-ordered, so B-tree inserts append instead
//       of splitting random pages. Leaks creation time — never use where the id
//       is a capability.
//   v4  public and capability ids — anything a client is handed and can quote
//       back. Fully random, unguessable, leaks nothing.
//
// Both draw from OpenSSL's CSPRNG. std::rand, std::mt19937 and time-seeded
// generators are banned for anything security-relevant, and UUIDv1 is banned
// outright: it encodes MAC address and timestamp (ENGINEERING_RULES.md §5).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "anvil/core/types.h"

namespace anvil::uuid {

// Random. Use where the id must be unguessable.
[[nodiscard]] Uuid generate_v4();

// Time-ordered (48-bit big-endian millisecond prefix + 74 random bits).
// Use for internal _id fields to get index locality.
[[nodiscard]] Uuid generate_v7();

// Milliseconds since the Unix epoch encoded in a v7 id. Meaningless for v4.
[[nodiscard]] std::int64_t v7_timestamp_ms(const Uuid& id) noexcept;

// The smallest UUIDv7 whose embedded millisecond is `epoch_ms`: the 48-bit
// big-endian prefix followed by zeroes.
//
// Used as a RANGE BOUND on `_id`. Because the prefix is big-endian and BSON
// compares equal-length BinData bytewise, `_id < v7_boundary(t)` is exactly
// "created before t" — answered by the index that already sorts by `_id`, with no
// second field in the filter and no separate `created_at` comparison to keep in
// step. `epoch_ms` outside the 48-bit range is clamped rather than truncated: a
// wrapped prefix would produce a bound that sorts BEFORE ids it should include.
//
// A bound may be PERSISTED where the stored value's only purpose is to be
// compared later — `notifications.clients.subs[].since` is one. What it must
// never be is an `_id`, or anything else read back as an identifier: it carries
// no version or variant nibble and identifies nothing. Prefer it to
// `generate_v7()` for every watermark, because two v7 ids minted in the same
// millisecond order by their random tails and so compare unpredictably.
[[nodiscard]] Uuid v7_boundary(std::int64_t epoch_ms) noexcept;

// Canonical 8-4-4-4-12 lowercase hex. For logs and JSON only — the stored form
// is always the 16 raw bytes.
[[nodiscard]] std::string to_string(const Uuid& id);

// Accepts the canonical hyphenated form only. Returns nullopt on any other
// length or on a non-hex digit; never throws, never allocates.
[[nodiscard]] std::optional<Uuid> parse(std::string_view text) noexcept;

// Unpadded base64url, 22 characters. Used in tokens and cursors, where 14 bytes
// per id matters and the hyphenated form does not.
[[nodiscard]] std::array<char, 22> to_base64url(const Uuid& id) noexcept;
[[nodiscard]] std::optional<Uuid> from_base64url(std::string_view text) noexcept;

}  // namespace anvil::uuid
