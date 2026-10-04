#pragma once

// Access token codec.
//
// ---------------------------------------------------------------------------
// This is NOT a JWT, which docs/05-auth-sessions.md §1 originally specified.
// The deviation is recorded as; the reasoning is worth stating
// where the code lives.
//
// The spec already required a single hard-coded algorithm and a closed claim
// set. A JWT with those two constraints is a JSON envelope around a fixed
// struct — it keeps all of JSON's costs and none of JWT's benefits:
//
//   * Nobody else consumes this token. It lives in an httpOnly cookie, is read
//     by our own filter, and is never handed to a third party. The cookie is
//     httpOnly, so the SPA cannot read it either.
//   * The two headline JWT vulnerabilities — `alg: none` and algorithm
//     confusion — exist ONLY because the algorithm is named inside the token.
//     A fixed layout makes both structurally impossible rather than something
//     to defend against: there is no algorithm field to confuse, so the cases
//     that would have tested for one are a statement about the format instead
//     of a defence in it.
//   * A JSON parser on the hottest security path is attack surface and
//     allocation. The budget is < 15 us with at most one allocation
//     (docs/04-access-control.md §4); this decodes with zero.
//   * A closed claim set becomes a property of the LAYOUT. An unknown claim is
//     not rejected by validation, it is unrepresentable.
//   * 128 bytes on the wire against roughly 280 for the equivalent JWT, on
//     every single request.
//
// What is preserved exactly: stateless verification with no database read,
// HMAC-SHA256 authentication, `kid`-selected key rotation, and the perm_epoch
// revocation channel.
// ---------------------------------------------------------------------------
//
// Wire layout — 96 bytes, base64url encoded to exactly 128 characters.
// All multi-byte integers are little-endian and written explicitly, so a token
// minted by one build reads identically on any platform.
//
//   off  size  field
//     0     1  version
//     1     1  kid            selects the signing key
//     2     1  user_type
//     3     1  lang
//     4     4  expires_at     unix seconds, u32
//     8     8  perm_epoch
//    16    16  user_id
//    32    16  session_id
//    48    16  permissions
//   ----------  64 bytes of payload
//    64    32  tag            HMAC-SHA256 over bytes 0..63
//
// The payload is authenticated, not encrypted — exactly as a JWT would be. It
// carries a user id, a session id and a permission bitset, none of which is a
// secret from the user it was issued to, and it never leaves TLS.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"
#include "anvil/crypto/secret.h"

namespace anvil::auth {

inline constexpr std::uint8_t kTokenVersion = 1;
inline constexpr std::size_t kTokenPayloadBytes = 64;
inline constexpr std::size_t kTokenTagBytes = 32;
inline constexpr std::size_t kTokenRawBytes = kTokenPayloadBytes + kTokenTagBytes;
inline constexpr std::size_t kTokenTextLength = 128;   // base64url of 96 bytes, unpadded

// Clock skew tolerance. Instances disagree by small amounts and a token minted
// a moment ago on another machine must not read as already expired
// (docs/00-architecture.md §7 invariant 3).
inline constexpr std::int64_t kClockSkewToleranceSeconds = 60;

// 64 bytes: the same content as UserContext, in wire order.
struct AccessClaims final {
    Uuid          user_id;
    Uuid          session_id;
    PermSet       permissions;
    std::uint64_t perm_epoch;
    std::uint32_t expires_at;   // unix seconds; u32 is valid until 2106
    UserType      user_type;
    Locale          locale;
    std::array<std::uint8_t, 2> reserved;
};
static_assert(sizeof(AccessClaims) == 64);
static_assert(std::is_trivially_copyable_v<AccessClaims>);

enum class TokenError : std::uint8_t {
    Ok = 0,
    Malformed,      // wrong length, or not valid base64url
    BadVersion,     // a format we do not speak
    UnknownKey,     // kid names no key we hold
    BadSignature,   // tag does not verify
    Expired,
};

struct TokenResult final {
    AccessClaims claims;   // meaningful only when error == Ok
    TokenError   error;

    [[nodiscard]] constexpr bool ok() const noexcept { return error == TokenError::Ok; }
};

// Signing keys. Immutable after construction and cheap to share as a
// shared_ptr<const TokenKeys>, swapped atomically on rotation so readers never
// take a lock (CLAUDE.md §4).
//
// Two keys at a time is the whole rotation story: mint with CURRENT, accept
// CURRENT or PREVIOUS. Sessions survive a rotation because they are anchored in
// the database, not in the key.
class TokenKeys final {
public:
    static constexpr std::size_t kKeyBytes = 32;

    // Throws std::invalid_argument on a wrong-sized key or duplicate kid.
    TokenKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key);
    TokenKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key,
              std::uint8_t previous_kid, std::span<const std::uint8_t> previous_key);

    [[nodiscard]] std::uint8_t current_kid() const noexcept { return current_kid_; }

    // nullptr when the kid is unknown. Callers must not distinguish "unknown
    // kid" from "bad signature" to the client.
    [[nodiscard]] const crypto::SecretBuffer<kKeyBytes>* key_for(
        std::uint8_t kid) const noexcept;

    TokenKeys(const TokenKeys&) = delete;
    TokenKeys& operator=(const TokenKeys&) = delete;

private:
    crypto::SecretBuffer<kKeyBytes> current_;
    crypto::SecretBuffer<kKeyBytes> previous_;
    std::uint8_t                    current_kid_;
    std::uint8_t                    previous_kid_;
    bool                            has_previous_;
};

// Mints a token signed with the current key. One allocation, for the returned
// string. Minting happens at login and refresh, never on the request path.
[[nodiscard]] std::string encode(const AccessClaims& claims, const TokenKeys& keys);

// Verifies and decodes. Zero allocation: everything lands in fixed stack
// buffers, so a hostile 64 KB cookie is rejected on length before any work.
//
// The tag is verified BEFORE any field is trusted, including the expiry — never
// branch on unauthenticated data.
[[nodiscard]] TokenResult decode(std::string_view token, const TokenKeys& keys,
                                 std::int64_t now_unix) noexcept;

// Convenience for the access filter, which needs a UserContext and not claims.
[[nodiscard]] UserContext to_context(const AccessClaims& claims) noexcept;

}  // namespace anvil::auth
