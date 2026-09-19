#pragma once

// Assembling JSON response bodies by appending into one std::string.
//
// There is no JSON *writer* library here for the same reason there is no JSON
// parser library (anvil/input/json.h): the requirements are specific
// enough that a general one would have to be configured into exactly this
// behaviour anyway.
//
//   * ARABIC IS EMITTED AS RAW UTF-8, never as \uXXXX. Most writers escape
//     non-ASCII by default, and that turns every Arabic character from 2 bytes
//     into 6 — a 3x payload inflation on the hottest public endpoint in the
//     system, paid on every render.
//   * The escape set is exactly what RFC 8259 requires and nothing more, so a
//     response body is byte-predictable. Two responses that must be identical
//     (the stealth 404, a 304) are identical because the bytes were assembled
//     the same way, not because two library configurations agreed.
//   * Appending into a caller-owned string with a reserve() up front means one
//     allocation for a whole document instead of one per node.
//
// Input is assumed to be valid UTF-8: every string reaching here has already
// passed anvil/i18n/utf8.h at the trust boundary, or was read back out of BSON,
// where anvil/db/codec.cc revalidated it. This function does not repair, and it
// does not validate — it escapes.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace anvil::http {

inline constexpr std::string_view kJsonContentType = "application/json; charset=utf-8";

// Appends `text` as a JSON string INCLUDING its surrounding quotes.
void append_json_string(std::string& out, std::string_view text);

// The `"key":` prefix, quoted and colon-terminated. Keys in this codebase are
// always compile-time constants, but they go through the same escaper so that a
// future dynamic key cannot silently produce malformed JSON.
void append_json_key(std::string& out, std::string_view key);

void append_json_int(std::string& out, std::int64_t value);

// RFC 3339 in UTC with millisecond precision, e.g. "2026-08-10T18:00:00.000Z",
// quoted. Never a local time and never an offset other than Z: storage is UTC
// and conversion is the presentation layer's job (docs/03-i18n-utf8.md §8).
void append_json_time(std::string& out, std::int64_t epoch_ms);

// Hyphenated lowercase UUID, quoted. The 16 stored bytes never appear in JSON.
//
// A fixed-extent span rather than `const std::uint8_t (&)[16]`, which is what
// this took until it acquired a caller: `Uuid` is `std::array<std::uint8_t, 16>`
// throughout this library and does not convert to a reference to a C array, so
// the old spelling could only be called through a cast. The extent is part of
// the type, so a shorter buffer is still a compile error (ENGINEERING_RULES.md §3.1).
void append_json_uuid(std::string& out, std::span<const std::uint8_t, 16> id);

}  // namespace anvil::http
