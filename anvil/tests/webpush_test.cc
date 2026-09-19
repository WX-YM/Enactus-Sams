// Web Push: the VAPID token and the aes128gcm payload.
//
// Two kinds of assertion here, and the difference matters.
//
// The KDF is checked against RFC 5869's published vectors. A test that only
// compared our HKDF against our own expectation would prove the function is
// deterministic and nothing else — and "self-consistent but interoperates with
// nothing" is the exact failure mode of every hand-rolled Web Push
// implementation.
//
// The encryption is checked by DECRYPTING with the subscription's private key,
// which is the browser's side of the exchange. Encrypting and then re-deriving
// the key the same way would pass with the two public keys in the wrong order,
// with the trailing NUL missing from every label, and with the record delimiter
// dropped — all three of which produce a body no browser can read.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/crypto/base64url.h"
#include "anvil/crypto/random.h"
#include "anvil/notifications/webpush.h"

namespace {

using anvil::ErrorCode;
namespace n = anvil::notifications;

[[nodiscard]] anvil::db::TimeMs at_seconds(std::int64_t seconds) {
    return anvil::db::TimeMs{std::chrono::seconds{seconds}};
}

[[nodiscard]] std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    const auto value = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') { return static_cast<std::uint8_t>(c - '0'); }
        return static_cast<std::uint8_t>((c | 0x20) - 'a' + 10);
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>((value(hex[i]) << 4U) | value(hex[i + 1])));
    }
    return out;
}

[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4U]);
        out.push_back(kDigits[byte & 0x0FU]);
    }
    return out;
}

// A browser's subscription: a P-256 key pair it generated plus a 16-byte auth
// secret. The private half is kept so the test can do what the browser does.
struct FakeBrowser final {
    n::VapidKey                                   keys;
    std::array<std::uint8_t, n::kAuthSecretBytes> auth;

    [[nodiscard]] n::PushSubscription subscription(std::string_view endpoint) const {
        return n::PushSubscription{endpoint, keys.public_key, auth};
    }
};

[[nodiscard]] FakeBrowser browser() {
    const anvil::Result<n::VapidKey> keys = n::generate_vapid_key();
    EXPECT_TRUE(keys.ok());
    return FakeBrowser{keys.ok() ? keys.value() : n::VapidKey{},
                       anvil::crypto::random_array<n::kAuthSecretBytes>()};
}

constexpr std::string_view kEndpoint = "https://fcm.googleapis.com/fcm/send/abc123";

}  // namespace

// --- HKDF against RFC 5869 ---------------------------------------------------

TEST(WebPushHkdf, MatchesTheRfc5869TestVectors) {
    // A.1: basic case with SHA-256.
    {
        const std::vector<std::uint8_t> ikm = from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
        const std::vector<std::uint8_t> salt = from_hex("000102030405060708090a0b0c");
        const std::vector<std::uint8_t> info = from_hex("f0f1f2f3f4f5f6f7f8f9");
        EXPECT_EQ(to_hex(n::hkdf_sha256(salt, ikm, info, 42)),
                  "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                  "34007208d5b887185865");
    }
    // A.2: longer inputs and outputs, which is what exercises the expand loop
    // past a single block.
    {
        const std::vector<std::uint8_t> ikm = from_hex(
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
            "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
            "404142434445464748494a4b4c4d4e4f");
        const std::vector<std::uint8_t> salt = from_hex(
            "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
            "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
            "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
        const std::vector<std::uint8_t> info = from_hex(
            "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
            "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
            "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        EXPECT_EQ(to_hex(n::hkdf_sha256(salt, ikm, info, 82)),
                  "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
                  "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
                  "cc30c58179ec3e87c14c01d5c1f3434f1d87");
    }
    // A.3: zero-length salt and info. The extract step must still run with an
    // all-zero key rather than skipping the HMAC.
    {
        const std::vector<std::uint8_t> ikm = from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
        EXPECT_EQ(to_hex(n::hkdf_sha256({}, ikm, {}, 42)),
                  "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
                  "9d201395faa4b61a96c8");
    }
}

// --- VAPID -------------------------------------------------------------------

TEST(WebPushVapid, TheAudienceIsTheEndpointsOriginAndNotItsPath) {
    // The single most common VAPID failure, and it surfaces as an opaque 401.
    EXPECT_EQ(n::push_origin(kEndpoint), "https://fcm.googleapis.com");
    // The port is part of the origin. A token signed over the host alone is
    // refused by a service on a non-default port.
    EXPECT_EQ(n::push_origin("https://push.example.com:8443/x/y"),
              "https://push.example.com:8443");
    EXPECT_TRUE(n::push_origin("http://push.example.com/x").empty());
    EXPECT_TRUE(n::push_origin("not a url").empty());
}

TEST(WebPushVapid, TheTokenIsThreeBase64UrlSegments) {
    const anvil::Result<n::VapidKey> key = n::generate_vapid_key();
    ASSERT_TRUE(key.ok());
    const anvil::Result<std::string> token = n::vapid_token(
        key.value(), n::VapidIdentity{"mailto:ops@example.com"}, "https://fcm.googleapis.com",
        at_seconds(1'700'000'000));
    ASSERT_TRUE(token.ok()) << static_cast<int>(token.code());

    const std::string& jwt = token.value();
    const std::size_t first = jwt.find('.');
    ASSERT_NE(first, std::string::npos);
    const std::size_t second = jwt.find('.', first + 1);
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(jwt.find('.', second + 1), std::string::npos);
    // Unpadded base64url throughout: a '=' anywhere means a padded encoder crept
    // in, and a JWS with padding is rejected by every verifier.
    EXPECT_EQ(jwt.find('='), std::string::npos);
    EXPECT_EQ(jwt.find('+'), std::string::npos);
    EXPECT_EQ(jwt.find('/'), std::string::npos);

    // The signature is raw r||s, 64 bytes — never the DER OpenSSL produces.
    const std::optional<std::vector<std::uint8_t>> signature =
        anvil::crypto::base64url_decode(jwt.substr(second + 1));
    ASSERT_TRUE(signature.has_value());
    EXPECT_EQ(signature->size(), n::kEcdsaSignatureBytes);
}

TEST(WebPushVapid, TheSignatureIsSixtyFourBytesEveryTime) {
    // BN_bn2binpad left-pads, and a DER integer whose leading byte is below 0x80
    // is one byte shorter than 32. Passing the DER bytes through would produce a
    // signature that verifies nowhere, roughly one time in 256 — so this needs
    // enough iterations to catch a short integer rather than one.
    const anvil::Result<n::VapidKey> key = n::generate_vapid_key();
    ASSERT_TRUE(key.ok());
    for (int i = 0; i < 300; ++i) {
        const anvil::Result<std::string> token =
            n::vapid_token(key.value(), n::VapidIdentity{"mailto:ops@example.com"},
                           "https://fcm.googleapis.com", at_seconds(1'700'000'000 + i));
        ASSERT_TRUE(token.ok());
        const std::size_t last = token.value().rfind('.');
        ASSERT_NE(last, std::string::npos);
        const std::optional<std::vector<std::uint8_t>> signature =
            anvil::crypto::base64url_decode(token.value().substr(last + 1));
        ASSERT_TRUE(signature.has_value()) << i;
        ASSERT_EQ(signature->size(), n::kEcdsaSignatureBytes) << i;
    }
}

TEST(WebPushVapid, TheClaimsCarryTheAudienceTheSubjectAndAnExpiry) {
    const anvil::Result<n::VapidKey> key = n::generate_vapid_key();
    ASSERT_TRUE(key.ok());
    const std::int64_t signed_at = 1'700'000'000;
    const anvil::Result<std::string> token = n::vapid_token(
        key.value(), n::VapidIdentity{"mailto:ops@example.com"}, "https://fcm.googleapis.com",
        at_seconds(signed_at));
    ASSERT_TRUE(token.ok());

    const std::size_t first = token.value().find('.');
    const std::size_t second = token.value().find('.', first + 1);
    const std::optional<std::vector<std::uint8_t>> claims = anvil::crypto::base64url_decode(
        token.value().substr(first + 1, second - first - 1));
    ASSERT_TRUE(claims.has_value());
    const std::string json{reinterpret_cast<const char*>(claims->data()), claims->size()};

    EXPECT_NE(json.find(R"("aud":"https://fcm.googleapis.com")"), std::string::npos) << json;
    EXPECT_NE(json.find(R"("sub":"mailto:ops@example.com")"), std::string::npos) << json;
    // A captured token sends notifications in our name until it expires, which is
    // the only reason the lifetime is bounded at all.
    const std::string expected_exp =
        R"("exp":)" + std::to_string(signed_at + n::kVapidLifetime.count());
    EXPECT_NE(json.find(expected_exp), std::string::npos) << json;
}

TEST(WebPushVapid, ASubjectThatCouldForgeAClaimIsRefused) {
    const anvil::Result<n::VapidKey> key = n::generate_vapid_key();
    ASSERT_TRUE(key.ok());
    // The subject reaches the JSON. A quote would close the string and let the
    // rest forge claims, so it is refused rather than escaped — a `mailto:` or
    // `https:` address contains neither a quote nor a backslash.
    for (const std::string_view subject :
         {std::string_view{R"(mailto:a@b","aud":"https://evil.example.com)"},
          std::string_view{"mailto:a@b\\"}, std::string_view{"mailto:a@b\nc"},
          std::string_view{}}) {
        EXPECT_EQ(n::vapid_token(key.value(), n::VapidIdentity{subject},
                                 "https://fcm.googleapis.com", at_seconds(1))
                      .code(),
                  ErrorCode::ValidationFailed)
            << subject;
    }
}

TEST(WebPushVapid, TheAuthorizationHeaderCarriesTheTokenAndThePublicKey) {
    const anvil::Result<n::VapidKey> key = n::generate_vapid_key();
    ASSERT_TRUE(key.ok());
    const anvil::Result<std::string> header = n::vapid_authorization(
        key.value(), n::VapidIdentity{"mailto:ops@example.com"}, kEndpoint, at_seconds(1));
    ASSERT_TRUE(header.ok()) << static_cast<int>(header.code());
    EXPECT_EQ(header.value().compare(0, 8, "vapid t="), 0);
    // The `k=` parameter is what the browser pinned at subscribe time.
    EXPECT_NE(header.value().find(", k=" + n::vapid_public_key_for_browser(key.value())),
              std::string::npos);
}

TEST(WebPushVapid, AnEndpointWithNoUsableOriginIsRefusedRatherThanSigned) {
    const anvil::Result<n::VapidKey> key = n::generate_vapid_key();
    ASSERT_TRUE(key.ok());
    // Refusing here says which of the two went wrong. Signing anyway produces an
    // opaque 401 from the push service that names nothing.
    EXPECT_EQ(n::vapid_authorization(key.value(), n::VapidIdentity{"mailto:ops@example.com"},
                                     "http://push.example.com/x", at_seconds(1))
                  .code(),
              ErrorCode::ValidationFailed);
}

TEST(WebPushVapid, TheBrowserFacingKeyIsUnpaddedBase64UrlOfTheUncompressedPoint) {
    const anvil::Result<n::VapidKey> key = n::generate_vapid_key();
    ASSERT_TRUE(key.ok());
    const std::string encoded = n::vapid_public_key_for_browser(key.value());
    const std::optional<std::vector<std::uint8_t>> decoded =
        anvil::crypto::base64url_decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), n::kP256PublicBytes);
    // 0x04 is the uncompressed-point marker. A browser rejects anything else.
    EXPECT_EQ(decoded->front(), 0x04);
}

TEST(WebPushVapid, TwoGeneratedKeysDiffer) {
    const anvil::Result<n::VapidKey> one = n::generate_vapid_key();
    const anvil::Result<n::VapidKey> two = n::generate_vapid_key();
    ASSERT_TRUE(one.ok());
    ASSERT_TRUE(two.ok());
    EXPECT_NE(one.value().private_key, two.value().private_key);
    EXPECT_NE(one.value().public_key, two.value().public_key);
}

// --- the encrypted payload ---------------------------------------------------

TEST(WebPushPayload, TheBrowserCanDecryptWhatWeEncrypt) {
    const FakeBrowser ua = browser();
    const std::string message = R"({"title":"New sign-in","body":"Firefox on Linux"})";

    const anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(ua.subscription(kEndpoint), message);
    ASSERT_TRUE(body.ok()) << static_cast<int>(body.code());

    // The browser's side of the exchange, with the private key we never have.
    // Re-deriving the key our own way instead would pass with the two public keys
    // in the wrong order and with every label's trailing NUL missing.
    const anvil::Result<std::string> recovered =
        n::decrypt_payload(ua.keys.private_key, ua.auth, body.value());
    ASSERT_TRUE(recovered.ok()) << static_cast<int>(recovered.code());
    EXPECT_EQ(recovered.value(), message);
}

TEST(WebPushPayload, NonLatinTextSurvivesIntact) {
    const FakeBrowser ua = browser();
    const std::string message = R"({"title":"تسجيل دخول جديد"})";
    const anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(ua.subscription(kEndpoint), message);
    ASSERT_TRUE(body.ok());
    const anvil::Result<std::string> recovered =
        n::decrypt_payload(ua.keys.private_key, ua.auth, body.value());
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value(), message);
}

TEST(WebPushPayload, TheHeaderIsSaltRecordSizeAndOurEphemeralPublicKey) {
    const FakeBrowser ua = browser();
    const anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(ua.subscription(kEndpoint), "hello");
    ASSERT_TRUE(body.ok());
    ASSERT_GT(body.value().size(), n::kHeaderBytes);

    const std::uint32_t record_size =
        (static_cast<std::uint32_t>(body.value()[n::kSaltBytes]) << 24U) |
        (static_cast<std::uint32_t>(body.value()[n::kSaltBytes + 1]) << 16U) |
        (static_cast<std::uint32_t>(body.value()[n::kSaltBytes + 2]) << 8U) |
        static_cast<std::uint32_t>(body.value()[n::kSaltBytes + 3]);
    EXPECT_EQ(record_size, n::kRecordSize);
    EXPECT_EQ(body.value()[n::kSaltBytes + 4], n::kP256PublicBytes);
    EXPECT_EQ(body.value()[n::kSaltBytes + 5], 0x04);

    // plaintext + delimiter + tag.
    EXPECT_EQ(body.value().size(), n::kHeaderBytes + 5 + 1 + n::kGcmTagBytes);
}

TEST(WebPushPayload, EverySendUsesAFreshSaltAndAFreshEphemeralKey) {
    const FakeBrowser ua = browser();
    const anvil::Result<std::vector<std::uint8_t>> first =
        n::encrypt_payload(ua.subscription(kEndpoint), "same message");
    const anvil::Result<std::vector<std::uint8_t>> second =
        n::encrypt_payload(ua.subscription(kEndpoint), "same message");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());

    // AES-GCM under a repeated key and nonce leaks the XOR of the two plaintexts
    // AND the authentication key. Both inputs that feed the nonce are generated
    // inside encrypt_payload precisely so no caller can supply a reused one.
    EXPECT_NE(first.value(), second.value());
    const std::vector<std::uint8_t> salt_one{first.value().begin(),
                                             first.value().begin() + n::kSaltBytes};
    const std::vector<std::uint8_t> salt_two{second.value().begin(),
                                             second.value().begin() + n::kSaltBytes};
    EXPECT_NE(salt_one, salt_two);

    const std::vector<std::uint8_t> key_one{
        first.value().begin() + n::kSaltBytes + 5,
        first.value().begin() + n::kSaltBytes + 5 + n::kP256PublicBytes};
    const std::vector<std::uint8_t> key_two{
        second.value().begin() + n::kSaltBytes + 5,
        second.value().begin() + n::kSaltBytes + 5 + n::kP256PublicBytes};
    EXPECT_NE(key_one, key_two);
}

TEST(WebPushPayload, ATamperedBodyFailsTheTagRatherThanDecrypting) {
    const FakeBrowser ua = browser();
    anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(ua.subscription(kEndpoint), "hello");
    ASSERT_TRUE(body.ok());

    std::vector<std::uint8_t> corrupted = body.value();
    corrupted[corrupted.size() - n::kGcmTagBytes - 1] ^= 0x01U;
    EXPECT_EQ(n::decrypt_payload(ua.keys.private_key, ua.auth, corrupted).code(),
              ErrorCode::ValidationFailed);

    // And the header is covered too, not by the tag but by the key derivation:
    // changing the salt derives a different key, so the tag check fails anyway.
    std::vector<std::uint8_t> reheaded = body.value();
    reheaded[0] ^= 0x01U;
    EXPECT_EQ(n::decrypt_payload(ua.keys.private_key, ua.auth, reheaded).code(),
              ErrorCode::ValidationFailed);
}

TEST(WebPushPayload, AnotherSubscriptionCannotDecryptIt) {
    const FakeBrowser mine = browser();
    const FakeBrowser theirs = browser();
    const anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(mine.subscription(kEndpoint), "private");
    ASSERT_TRUE(body.ok());

    // The push service relays ciphertext it cannot read, and so does anybody else
    // who obtains it. That is what makes it acceptable to put a notification's
    // text through somebody else's infrastructure.
    EXPECT_EQ(n::decrypt_payload(theirs.keys.private_key, theirs.auth, body.value()).code(),
              ErrorCode::ValidationFailed);
    // The right key and the wrong auth secret also fails: the auth secret is the
    // HKDF salt for the IKM, not decoration.
    EXPECT_EQ(n::decrypt_payload(mine.keys.private_key, theirs.auth, body.value()).code(),
              ErrorCode::ValidationFailed);
}

TEST(WebPushPayload, AKeyThatIsNotOnTheCurveIsRefused) {
    FakeBrowser ua = browser();
    // Not a point on P-256. Multiplying our ephemeral scalar by it is an
    // invalid-curve attack that leaks the scalar a few bits at a time.
    ua.keys.public_key[40] ^= 0xFFU;
    EXPECT_EQ(n::encrypt_payload(ua.subscription(kEndpoint), "hello").code(),
              ErrorCode::ValidationFailed);

    // And a compressed or otherwise mis-tagged point is refused rather than
    // reinterpreted.
    FakeBrowser tagged = browser();
    tagged.keys.public_key[0] = 0x02;
    EXPECT_EQ(n::encrypt_payload(tagged.subscription(kEndpoint), "hello").code(),
              ErrorCode::ValidationFailed);
}

TEST(WebPushPayload, AnOverLongMessageIsRefusedRatherThanSplit) {
    const FakeBrowser ua = browser();
    const std::string oversized(n::kMaxPlaintextBytes + 1, 'x');
    // A browser is not obliged to reassemble records, so a message that does not
    // fit in one is not sent rather than silently truncated.
    EXPECT_EQ(n::encrypt_payload(ua.subscription(kEndpoint), oversized).code(),
              ErrorCode::PayloadTooLarge);

    const std::string largest(n::kMaxPlaintextBytes, 'x');
    const anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(ua.subscription(kEndpoint), largest);
    ASSERT_TRUE(body.ok());
    const anvil::Result<std::string> recovered =
        n::decrypt_payload(ua.keys.private_key, ua.auth, body.value());
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().size(), n::kMaxPlaintextBytes);
}

TEST(WebPushPayload, AnEmptyMessageRoundTrips) {
    const FakeBrowser ua = browser();
    const anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(ua.subscription(kEndpoint), "");
    ASSERT_TRUE(body.ok());
    const anvil::Result<std::string> recovered =
        n::decrypt_payload(ua.keys.private_key, ua.auth, body.value());
    ASSERT_TRUE(recovered.ok());
    EXPECT_TRUE(recovered.value().empty());
}

TEST(WebPushPayload, ATruncatedBodyIsRefusedRatherThanRead) {
    const FakeBrowser ua = browser();
    const anvil::Result<std::vector<std::uint8_t>> body =
        n::encrypt_payload(ua.subscription(kEndpoint), "hello");
    ASSERT_TRUE(body.ok());

    for (const std::size_t length : {std::size_t{0}, std::size_t{10}, n::kHeaderBytes}) {
        const std::vector<std::uint8_t> truncated{body.value().begin(),
                                                  body.value().begin() +
                                                      static_cast<std::ptrdiff_t>(length)};
        EXPECT_EQ(n::decrypt_payload(ua.keys.private_key, ua.auth, truncated).code(),
                  ErrorCode::ValidationFailed)
            << length;
    }
}
