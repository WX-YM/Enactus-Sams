#include "anvil/auth/token.h"

#include <optional>

#include <algorithm>
#include <array>
#include <stdexcept>

#include "anvil/crypto/base64url.h"
#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/digest.h"

namespace anvil::auth {
namespace {

// Explicit little-endian serialisation. Not memcpy of the struct: that would
// bake in this platform's padding and byte order, and these bytes are
// authenticated — a layout change would silently invalidate every live token.
void write_u32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

void write_u64(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::uint8_t> in,
                                     std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(in[offset + i]) << (i * 8);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::uint8_t> in,
                                     std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(in[offset + i]) << (i * 8);
    }
    return value;
}

// Field offsets, named once so encode and decode cannot drift apart.
constexpr std::size_t kOffVersion = 0;
constexpr std::size_t kOffKid = 1;
constexpr std::size_t kOffUserType = 2;
constexpr std::size_t kOffLang = 3;
constexpr std::size_t kOffExpiresAt = 4;
constexpr std::size_t kOffPermEpoch = 8;
constexpr std::size_t kOffUserId = 16;
constexpr std::size_t kOffSessionId = 32;
constexpr std::size_t kOffPermissions = 48;

void serialize_payload(const AccessClaims& claims, std::uint8_t kid,
                       std::span<std::uint8_t, kTokenPayloadBytes> out) noexcept {
    out[kOffVersion] = kTokenVersion;
    out[kOffKid] = kid;
    out[kOffUserType] = static_cast<std::uint8_t>(claims.user_type);
    out[kOffLang] = claims.locale.index();

    write_u32(out, kOffExpiresAt, claims.expires_at);
    write_u64(out, kOffPermEpoch, claims.perm_epoch);

    std::copy(claims.user_id.begin(), claims.user_id.end(), out.begin() + kOffUserId);
    std::copy(claims.session_id.begin(), claims.session_id.end(),
              out.begin() + kOffSessionId);

    const std::array<std::uint8_t, 16> perms = claims.permissions.to_bytes();
    std::copy(perms.begin(), perms.end(), out.begin() + kOffPermissions);
}

}  // namespace

TokenKeys::TokenKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key)
    : current_{}, previous_{}, current_kid_{current_kid}, previous_kid_{0},
      has_previous_{false} {
    if (current_key.size() != kKeyBytes) {
        throw std::invalid_argument{"TokenKeys: signing key must be 32 bytes"};
    }
    std::copy(current_key.begin(), current_key.end(), current_.data());
}

TokenKeys::TokenKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key,
                     std::uint8_t previous_kid, std::span<const std::uint8_t> previous_key)
    : current_{}, previous_{}, current_kid_{current_kid}, previous_kid_{previous_kid},
      has_previous_{true} {
    if (current_key.size() != kKeyBytes || previous_key.size() != kKeyBytes) {
        throw std::invalid_argument{"TokenKeys: signing key must be 32 bytes"};
    }
    // Two keys sharing a kid makes rotation ambiguous: a token would verify
    // against whichever happened to be checked first.
    if (current_kid == previous_kid) {
        throw std::invalid_argument{"TokenKeys: current and previous kid must differ"};
    }
    std::copy(current_key.begin(), current_key.end(), current_.data());
    std::copy(previous_key.begin(), previous_key.end(), previous_.data());
}

const crypto::SecretBuffer<TokenKeys::kKeyBytes>* TokenKeys::key_for(
    std::uint8_t kid) const noexcept {
    if (kid == current_kid_) { return &current_; }
    if (has_previous_ && kid == previous_kid_) { return &previous_; }
    return nullptr;
}

std::string encode(const AccessClaims& claims, const TokenKeys& keys) {
    std::array<std::uint8_t, kTokenRawBytes> raw{};

    const std::span<std::uint8_t, kTokenPayloadBytes> payload{raw.data(), kTokenPayloadBytes};
    serialize_payload(claims, keys.current_kid(), payload);

    const crypto::SecretBuffer<TokenKeys::kKeyBytes>* key = keys.key_for(keys.current_kid());
    // Unreachable: current_kid always resolves. Checked because a null deref
    // here would be a signing failure, not a verification failure.
    if (key == nullptr) { throw std::logic_error{"TokenKeys: current key missing"}; }

    const crypto::Digest256 tag = crypto::hmac_sha256(key->span(), payload);
    std::copy(tag.begin(), tag.end(), raw.begin() + kTokenPayloadBytes);

    return crypto::base64url_encode(raw);
}

TokenResult decode(std::string_view token, const TokenKeys& keys,
                   std::int64_t now_unix) noexcept {
    // Length first. A hostile 64 KB cookie must cost a compare, not a decode.
    if (token.size() != kTokenTextLength) {
        return TokenResult{.claims = {}, .error = TokenError::Malformed};
    }

    std::array<std::uint8_t, kTokenRawBytes> raw{};
    const std::optional<std::size_t> decoded = crypto::base64url_decode_into(token, raw);
    if (!decoded.has_value() || *decoded != kTokenRawBytes) {
        return TokenResult{.claims = {}, .error = TokenError::Malformed};
    }

    if (raw[kOffVersion] != kTokenVersion) {
        return TokenResult{.claims = {}, .error = TokenError::BadVersion};
    }

    // The kid is unauthenticated at this point, which is fine: it only selects
    // a key, and naming the wrong one simply fails the tag check below. It is
    // looked up rather than used as an index, so it cannot read out of bounds.
    const crypto::SecretBuffer<TokenKeys::kKeyBytes>* key = keys.key_for(raw[kOffKid]);
    if (key == nullptr) {
        return TokenResult{.claims = {}, .error = TokenError::UnknownKey};
    }

    const std::span<const std::uint8_t> payload{raw.data(), kTokenPayloadBytes};
    const crypto::Digest256 expected = crypto::hmac_sha256(key->span(), payload);
    const std::span<const std::uint8_t> actual{raw.data() + kTokenPayloadBytes, kTokenTagBytes};

    // Constant-time, and BEFORE any payload field is trusted. Checking expiry
    // first would branch on attacker-controlled bytes and leak through timing
    // whether a forged token was merely stale.
    if (!crypto::secure_equal(expected, actual)) {
        return TokenResult{.claims = {}, .error = TokenError::BadSignature};
    }

    const std::uint32_t expires_at = read_u32(payload, kOffExpiresAt);
    if (static_cast<std::int64_t>(expires_at) + kClockSkewToleranceSeconds < now_unix) {
        return TokenResult{.claims = {}, .error = TokenError::Expired};
    }

    AccessClaims claims{};
    std::copy(payload.begin() + kOffUserId, payload.begin() + kOffUserId + 16,
              claims.user_id.begin());
    std::copy(payload.begin() + kOffSessionId, payload.begin() + kOffSessionId + 16,
              claims.session_id.begin());

    std::array<std::uint8_t, 16> perm_bytes{};
    std::copy(payload.begin() + kOffPermissions, payload.begin() + kOffPermissions + 16,
              perm_bytes.begin());
    claims.permissions = PermSet::from_bytes(perm_bytes);

    claims.perm_epoch = read_u64(payload, kOffPermEpoch);
    claims.expires_at = expires_at;

    // Unknown enum values are rejected rather than clamped: a user_type outside
    // the enum would otherwise land in UserContext and be compared against
    // real values downstream.
    const std::uint8_t user_type = payload[kOffUserType];
    if (user_type > static_cast<std::uint8_t>(kMaxUserType)) {
        return TokenResult{.claims = {}, .error = TokenError::Malformed};
    }
    // Range-checked against the APPLICATION's locale table rather than against a
    // hardcoded ceiling. A byte naming a locale this build does not declare is a
    // malformed token, not a token to be answered in the default locale — and it
    // is checked here, after the tag has already been verified, so the extra
    // compare costs nothing an attacker can reach.
    const std::optional<Locale> locale = Locale::from_index(payload[kOffLang]);
    if (!locale.has_value()) {
        return TokenResult{.claims = {}, .error = TokenError::Malformed};
    }
    claims.user_type = static_cast<UserType>(user_type);
    claims.locale = *locale;

    return TokenResult{.claims = claims, .error = TokenError::Ok};
}

UserContext to_context(const AccessClaims& claims) noexcept {
    UserContext ctx{};
    ctx.user_id = claims.user_id;
    ctx.session_id = claims.session_id;
    ctx.permissions = claims.permissions;
    ctx.perm_epoch = claims.perm_epoch;
    ctx.user_type = claims.user_type;
    ctx.locale = claims.locale;
    return ctx;
}

}  // namespace anvil::auth
