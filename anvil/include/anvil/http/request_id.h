#pragma once

// The id that joins a failure a user can read out to the log line that explains
// it.
//
// docs/00-architecture.md §8 has published `{"error":{"code","request_id",
// "fields"}}` since phase 0 and §9's log line has named the same field for just
// as long. Until this header the string `request_id` appeared in no writer
// anywhere in the source, so neither contract was one this library kept — and a
// contract a client is generated against and nobody keeps is how two
// applications come to spell one failure two ways.
//
// --- Why sixteen bytes in this shape ----------------------------------------
//
// A 48-bit millisecond timestamp, then 80 CSPRNG bits. Each half is load-bearing:
//
//   * TIME FIRST, big-endian, so the rendered form sorts into occurrence order
//     as text. A log file is greppable and sortable without a parser, and two
//     ids from one incident sit next to each other.
//   * NO MAC ADDRESS AND NO HOST IDENTITY, which is why this is not UUIDv1.
//     An id a user quotes in a support ticket must not disclose the fleet.
//   * CSPRNG, not a counter. The id is quoted into channels anvil does not
//     control, and a guessable one lets somebody claim an incident that is not
//     theirs. It still AUTHORISES NOTHING — it names a log line and nothing
//     else, and no code path anywhere may treat holding one as evidence of
//     anything.
//
// Rendered as 26 Crockford base32 characters, which is the `01J…` docs/00 has
// always shown. The alphabet omits I, L, O and U, so a person reading an id
// down a phone line cannot turn it into a different valid id — transcription is
// the whole reason this is not hex or base64url, both of which have pairs that
// sound or look alike.
//
// --- Where it is NOT ---------------------------------------------------------
//
// Never on the stealth path. `kNotFoundBody` stays the `constexpr` it is: a
// correlatable value that a genuine 404 does not carry is one of the tells
// `accesscontrol/stealth.h` enumerates beside `WWW-Authenticate` and
// `Set-Cookie`, and a stealth 404 that carried an id would be separable from an
// unmatched route by reading the body.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace anvil::http {

// 128 bits at five bits per character is 25.6, so the rendering is 26 characters
// and the first one carries three bits rather than five. Two zero bits of
// padding lead, which is why a valid id's first character is never above '7'.
inline constexpr std::size_t kRequestIdChars = 26;

// The Crockford alphabet, in value order. I, L, O and U are absent.
inline constexpr std::string_view kCrockfordAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// A distinct type and not a `Uuid`, although both are sixteen bytes. A Uuid in
// this codebase names a THING — a user, a session, a media object — and several
// of them are capabilities (docs/07-filesystem.md §6). This names a log line and
// confers nothing, so the two must not be assignable to one another by accident.
struct RequestId final {
    std::array<std::uint8_t, 16> bytes;
};

static_assert(sizeof(RequestId) == 16);
static_assert(std::is_trivially_copyable_v<RequestId>);

// `now_unix_ms` is the LOCAL clock, never a client-supplied timestamp
// (docs/00-architecture.md §7 invariant 3). Throws `CryptoError` if the CSPRNG
// is unavailable, which `crypto::random_bytes` already guarantees rather than
// degrading to a weaker source.
[[nodiscard]] RequestId mint_request_id(std::int64_t now_unix_ms);

// 26 characters into the caller's storage, with no allocation. The array is
// returned by value because it is 26 bytes and every caller wants it on the
// stack; there is no allocating form, for the same reason
// `base64url_decode_into` exists (docs/04-access-control.md §4).
[[nodiscard]] std::array<char, kRequestIdChars> format_request_id(const RequestId& id) noexcept;

// The same 26 characters, appended. For the body writer and the log line, which
// are building a string either way.
void append_request_id(std::string& out, const RequestId& id);

}  // namespace anvil::http
