// The access token codec.

#include <gtest/gtest.h>

#include "perms.h"

#include <array>
#include <chrono>
#include <set>
#include <string>

#include "anvil/auth/token.h"
#include "anvil/core/locale.h"
#include "anvil/crypto/random.h"

namespace anvil::auth {

using testapp::Perm;
namespace {

constexpr std::int64_t kNow = 1'800'000'000;   // a fixed instant, so tests do not drift

std::array<std::uint8_t, 32> key_of(std::uint8_t fill) {
    std::array<std::uint8_t, 32> key{};
    key.fill(fill);
    return key;
}

AccessClaims sample_claims(std::int64_t expires_at = kNow + 900) {
    AccessClaims claims{};
    claims.user_id.fill(0xA1);
    claims.session_id.fill(0xB2);
    claims.permissions = perm_mask(Perm::ContentWrite, Perm::MediaUpload, Perm::FormPii);
    claims.perm_epoch = 7;
    claims.expires_at = static_cast<std::uint32_t>(expires_at);
    claims.user_type = UserType::Staff;
    claims.locale = *Locale::from_tag("ar");
    return claims;
}

}  // namespace

// --- 1-3: the vulnerability classes the format removes --------------------

TEST(Token, FormatCarriesNoAlgorithmField) {
    // `alg: none` and algorithm confusion are JWT vulnerabilities that exist
    // only because the algorithm is named inside the token. There is no field
    // here to attack: byte 0 is a version, byte 1 selects a key, and the MAC
    // algorithm is a compile-time constant.
    const TokenKeys keys{1, key_of(0x11)};
    const std::string token = encode(sample_claims(), keys);

    EXPECT_EQ(token.size(), kTokenTextLength);
    // No JWT structure at all — nothing to re-interpret.
    EXPECT_EQ(token.find('.'), std::string::npos);
    EXPECT_EQ(token.find("eyJ"), std::string::npos) << "no base64 JSON header";
}

TEST(Token, ClaimSetIsClosedByTheLayout) {
    // An unknown claim is not rejected by validation, it is unrepresentable:
    // the payload is exactly 64 bytes with every byte spoken for.
    static_assert(sizeof(AccessClaims) == kTokenPayloadBytes);
    const TokenKeys keys{1, key_of(0x11)};
    // Every token is the same length regardless of content — no field can be
    // added by a caller, and length leaks nothing about the claims.
    EXPECT_EQ(encode(sample_claims(), keys).size(), kTokenTextLength);

    AccessClaims wide = sample_claims();
    wide.permissions = ~PermSet{};
    wide.perm_epoch = UINT64_MAX;
    EXPECT_EQ(encode(wide, keys).size(), kTokenTextLength);
}

// --- round trip -----------------------------------------------------------

TEST(Token, RoundTripsEveryField) {
    const TokenKeys keys{1, key_of(0x11)};
    const AccessClaims original = sample_claims();

    const TokenResult result = decode(encode(original, keys), keys, kNow);
    ASSERT_TRUE(result.ok()) << static_cast<int>(result.error);

    EXPECT_EQ(result.claims.user_id, original.user_id);
    EXPECT_EQ(result.claims.session_id, original.session_id);
    EXPECT_EQ(result.claims.permissions, original.permissions);
    EXPECT_EQ(result.claims.perm_epoch, original.perm_epoch);
    EXPECT_EQ(result.claims.expires_at, original.expires_at);
    EXPECT_EQ(result.claims.user_type, original.user_type);
    EXPECT_EQ(result.claims.locale, original.locale);
}

TEST(Token, RoundTripsAllPermissionBits) {
    const TokenKeys keys{1, key_of(0x11)};
    for (std::size_t bit = 0; bit < PermSet::kBits; ++bit) {
        AccessClaims claims = sample_claims();
        claims.permissions = PermSet{}.set(bit);

        const TokenResult result = decode(encode(claims, keys), keys, kNow);
        ASSERT_TRUE(result.ok()) << "bit=" << bit;
        EXPECT_TRUE(result.claims.permissions.test(bit)) << "bit=" << bit;
        EXPECT_EQ(result.claims.permissions.count(), 1U) << "bit=" << bit;
    }
}

TEST(Token, IsUnforgeableWithoutTheKey) {
    const TokenKeys signer{1, key_of(0x11)};
    const TokenKeys attacker{1, key_of(0x22)};

    const std::string token = encode(sample_claims(), signer);
    EXPECT_EQ(decode(token, attacker, kNow).error, TokenError::BadSignature);
}

// --- 4: key rotation ------------------------------------------------------

TEST(Token, PreviousKeyStillVerifies) {
    // Rotation must not log everyone out: a token minted before the rotation
    // carries the old kid and must keep working until it expires.
    const TokenKeys old_keys{1, key_of(0x11)};
    const std::string minted_before_rotation = encode(sample_claims(), old_keys);

    const TokenKeys rotated{2, key_of(0x22), 1, key_of(0x11)};
    EXPECT_TRUE(decode(minted_before_rotation, rotated, kNow).ok());

    // New tokens are minted with the current key.
    const std::string fresh = encode(sample_claims(), rotated);
    EXPECT_TRUE(decode(fresh, rotated, kNow).ok());
    EXPECT_EQ(decode(fresh, old_keys, kNow).error, TokenError::UnknownKey);
}

TEST(Token, UnknownKeyIdIsRejected) {
    const TokenKeys signer{9, key_of(0x11)};
    const std::string token = encode(sample_claims(), signer);

    const TokenKeys other{1, key_of(0x11)};
    EXPECT_EQ(decode(token, other, kNow).error, TokenError::UnknownKey);
}

TEST(Token, KeyIdsMustDifferAndKeysMustBeThirtyTwoBytes) {
    EXPECT_THROW((TokenKeys{1, key_of(0x11), 1, key_of(0x22)}), std::invalid_argument);

    const std::array<std::uint8_t, 16> too_short{};
    EXPECT_THROW((TokenKeys{1, too_short}), std::invalid_argument);
}

TEST(Token, EveryKeyIdValueIsUsable) {
    // kid is a byte and is looked up, never used as an index — 0 and 255 must
    // behave like any other value and must not read out of bounds.
    for (const std::uint8_t kid : {std::uint8_t{0}, std::uint8_t{127}, std::uint8_t{255}}) {
        const TokenKeys keys{kid, key_of(0x11)};
        EXPECT_TRUE(decode(encode(sample_claims(), keys), keys, kNow).ok())
            << "kid=" << static_cast<int>(kid);
    }
}

// --- 5: expiry and clock skew --------------------------------------------

TEST(Token, ExpiredTokenIsRejected) {
    const TokenKeys keys{1, key_of(0x11)};
    const std::string token = encode(sample_claims(kNow - 3600), keys);
    EXPECT_EQ(decode(token, keys, kNow).error, TokenError::Expired);
}

TEST(Token, ClockSkewToleranceIsSixtySecondsAndNoMore) {
    // Instances disagree by small amounts; a token minted a moment ago
    // elsewhere must not read as already expired. But the window is bounded.
    const TokenKeys keys{1, key_of(0x11)};

    const std::string just_expired = encode(sample_claims(kNow - 30), keys);
    EXPECT_TRUE(decode(just_expired, keys, kNow).ok()) << "within tolerance";

    const std::string well_expired = encode(sample_claims(kNow - 61), keys);
    EXPECT_EQ(decode(well_expired, keys, kNow).error, TokenError::Expired);
}

TEST(Token, ExpiryIsCheckedOnlyAfterTheSignature) {
    // An expired token whose tag is also forged must report BadSignature, not
    // Expired. Reporting Expired would mean the expiry was read — and branched
    // on — before the payload was authenticated.
    const TokenKeys signer{1, key_of(0x11)};
    const TokenKeys attacker{1, key_of(0x22)};

    const std::string token = encode(sample_claims(kNow - 3600), signer);
    EXPECT_EQ(decode(token, attacker, kNow).error, TokenError::BadSignature);
}

// --- 6: malformed input ---------------------------------------------------

TEST(Token, RejectsMalformedInputWithoutThrowing) {
    const TokenKeys keys{1, key_of(0x11)};

    EXPECT_EQ(decode("", keys, kNow).error, TokenError::Malformed);
    EXPECT_EQ(decode("short", keys, kNow).error, TokenError::Malformed);
    EXPECT_EQ(decode(std::string(kTokenTextLength, '!'), keys, kNow).error,
              TokenError::Malformed);
    EXPECT_EQ(decode(std::string(kTokenTextLength, 'A') + "A", keys, kNow).error,
              TokenError::Malformed);
    // A 64 KB cookie must cost a length compare, not a decode.
    EXPECT_EQ(decode(std::string(65536, 'A'), keys, kNow).error, TokenError::Malformed);
}

TEST(Token, RejectsUnknownVersion) {
    const TokenKeys keys{1, key_of(0x11)};
    std::string token = encode(sample_claims(), keys);

    // Byte 0 is the version; flipping it changes the first base64url character.
    token[0] = (token[0] == 'A') ? 'B' : 'A';
    const TokenError error = decode(token, keys, kNow).error;
    EXPECT_TRUE(error == TokenError::BadVersion || error == TokenError::BadSignature);
}

TEST(Token, RejectsOutOfRangeEnumValues) {
    // A user_type outside the enum would otherwise reach UserContext and be
    // compared against real values downstream.
    const TokenKeys keys{1, key_of(0x11)};
    AccessClaims claims = sample_claims();

    // Forge by signing with the real key so only the enum check can reject it.
    claims.user_type = static_cast<UserType>(99);
    const TokenResult result = decode(encode(claims, keys), keys, kNow);
    EXPECT_EQ(result.error, TokenError::Malformed);
}

// --- 7: tamper detection --------------------------------------------------

TEST(Token, AnySinglePayloadByteFlipIsDetected) {
    // Walks every byte of the payload, not just a sample: a MAC that covered
    // only part of the payload would pass a spot check.
    const TokenKeys keys{1, key_of(0x11)};
    const std::string original = encode(sample_claims(), keys);

    int detected = 0;
    for (std::size_t i = 0; i < original.size(); ++i) {
        std::string tampered = original;
        tampered[i] = (tampered[i] == 'A') ? 'B' : 'A';
        if (tampered == original) { continue; }
        if (!decode(tampered, keys, kNow).ok()) { ++detected; }
    }
    EXPECT_EQ(detected, static_cast<int>(original.size()))
        << "every byte of the token must be authenticated";
}

TEST(Token, PrivilegeEscalationByTamperingIsDetected) {
    // The attack this format exists to stop: flip permission bits in a token
    // you legitimately hold.
    const TokenKeys keys{1, key_of(0x11)};

    AccessClaims low = sample_claims();
    low.permissions = perm_mask(Perm::MediaDelete);
    const std::string honest = encode(low, keys);

    AccessClaims elevated = low;
    elevated.permissions = perm_mask(Perm::StaffManage, Perm::FormRead);
    const std::string forged = encode(elevated, keys);

    // Splicing the elevated payload onto the honest token's tag must fail.
    const std::string spliced = forged.substr(0, 86) + honest.substr(86);
    EXPECT_FALSE(decode(spliced, keys, kNow).ok());
}

// --- 9: context conversion ------------------------------------------------

TEST(Token, ToContextPreservesEveryAuthorizationField) {
    const AccessClaims claims = sample_claims();
    const UserContext ctx = to_context(claims);

    EXPECT_EQ(ctx.user_id, claims.user_id);
    EXPECT_EQ(ctx.session_id, claims.session_id);
    EXPECT_EQ(ctx.permissions, claims.permissions);
    EXPECT_EQ(ctx.perm_epoch, claims.perm_epoch);
    EXPECT_EQ(ctx.user_type, claims.user_type);
    EXPECT_EQ(ctx.locale, claims.locale);
}

// --- size ----------------------------------------------------------------

TEST(Token, IsSubstantiallySmallerThanAnEquivalentJwt) {
    // 128 characters against roughly 280 for a JWT carrying the same claims,
    // on every request that presents a cookie.
    const TokenKeys keys{1, key_of(0x11)};
    EXPECT_EQ(encode(sample_claims(), keys).size(), 128U);
}

TEST(Token, DistinctClaimsProduceDistinctTokens) {
    const TokenKeys keys{1, key_of(0x11)};
    std::set<std::string> seen;
    for (std::uint64_t epoch = 0; epoch < 200; ++epoch) {
        AccessClaims claims = sample_claims();
        claims.perm_epoch = epoch;
        seen.insert(encode(claims, keys));
    }
    EXPECT_EQ(seen.size(), 200U);
}

}  // namespace anvil::auth
