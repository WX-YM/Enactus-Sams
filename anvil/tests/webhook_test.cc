// The signed webhook envelope and the SSRF refusal.
//
// Every property here is a pure function of its inputs, which is the point of
// keeping the HTTP out of the module: a receiver in another language can be
// written from the header, and this is what it has to agree with.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

#include "anvil/notifications/webhook.h"

namespace {

namespace n = anvil::notifications;

[[nodiscard]] anvil::db::TimeMs at_seconds(std::int64_t seconds) {
    return anvil::db::TimeMs{std::chrono::seconds{seconds}};
}

// A fixed secret, so the vectors below are reproducible rather than merely
// self-consistent. A generated one would let sign() and verify() agree with each
// other while both disagreeing with every other implementation.
[[nodiscard]] n::WebhookSecret fixed_secret() {
    n::WebhookSecret secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) {
        secret[i] = static_cast<std::uint8_t>(i);
    }
    return secret;
}

constexpr std::int64_t kSignedAt = 1'700'000'000;
constexpr std::string_view kBody = R"({"id":"abc","kind":3})";

}  // namespace

// --- the signed string -------------------------------------------------------

TEST(WebhookSigning, TheSignedStringIsTheTimestampADotAndTheRawBody) {
    // Spelled out rather than described, because a receiver has to build the
    // identical bytes and prose is something two implementations can disagree
    // about.
    EXPECT_EQ(n::signing_string(kSignedAt, kBody), "1700000000." + std::string{kBody});
}

TEST(WebhookSigning, TheSeparatorMakesTheEncodingUnambiguous) {
    // Without the dot, (1, "23x") and (12, "3x") would sign the same bytes — a
    // length-extension of the cheapest kind, against a field the attacker
    // supplies.
    EXPECT_NE(n::signing_string(1, "23x"), n::signing_string(12, "3x"));
}

TEST(WebhookSigning, ASignatureIsTheVersionPrefixAndSixtyFourLowercaseHex) {
    const std::string signature = n::sign(fixed_secret(), kSignedAt, kBody);
    ASSERT_EQ(signature.size(), n::kSignatureVersion.size() + 1 + n::kSignatureHexChars);
    EXPECT_EQ(signature.compare(0, n::kSignatureVersion.size(), n::kSignatureVersion), 0);
    EXPECT_EQ(signature[n::kSignatureVersion.size()], '=');
    for (std::size_t i = n::kSignatureVersion.size() + 1; i < signature.size(); ++i) {
        const char c = signature[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << signature;
    }
}

TEST(WebhookSigning, SigningIsDeterministicAndKeyed) {
    const n::WebhookSecret secret = fixed_secret();
    EXPECT_EQ(n::sign(secret, kSignedAt, kBody), n::sign(secret, kSignedAt, kBody));

    n::WebhookSecret other = secret;
    other[0] = static_cast<std::uint8_t>(other[0] ^ 0x01U);
    // A plain hash of a structured body would be forgeable by anybody who can
    // guess the body. The construction is keyed, so it is not.
    EXPECT_NE(n::sign(secret, kSignedAt, kBody), n::sign(other, kSignedAt, kBody));
}

// --- verification ------------------------------------------------------------

TEST(WebhookVerify, AGenuineDeliveryVerifies) {
    const n::WebhookSecret secret = fixed_secret();
    const std::string signature = n::sign(secret, kSignedAt, kBody);
    EXPECT_TRUE(n::verify(secret, signature, kSignedAt, kBody, at_seconds(kSignedAt)));
}

TEST(WebhookVerify, ATamperedBodyFails) {
    const n::WebhookSecret secret = fixed_secret();
    const std::string signature = n::sign(secret, kSignedAt, kBody);
    EXPECT_FALSE(n::verify(secret, signature, kSignedAt, R"({"id":"abc","kind":4})",
                           at_seconds(kSignedAt)));
}

TEST(WebhookVerify, ATamperedTimestampFails) {
    const n::WebhookSecret secret = fixed_secret();
    const std::string signature = n::sign(secret, kSignedAt, kBody);
    // Moving the timestamp to stay inside the window changes the signed string,
    // so the attacker cannot move the window without invalidating the signature.
    // That is the entire reason the instant is inside it.
    EXPECT_FALSE(n::verify(secret, signature, kSignedAt + 10, kBody,
                           at_seconds(kSignedAt + 10)));
}

TEST(WebhookVerify, TheWrongSecretFails) {
    n::WebhookSecret other = fixed_secret();
    other[31] = static_cast<std::uint8_t>(other[31] ^ 0xFFU);
    const std::string signature = n::sign(fixed_secret(), kSignedAt, kBody);
    EXPECT_FALSE(n::verify(other, signature, kSignedAt, kBody, at_seconds(kSignedAt)));
}

TEST(WebhookVerify, AReplayOutsideTheWindowFails) {
    const n::WebhookSecret secret = fixed_secret();
    const std::string signature = n::sign(secret, kSignedAt, kBody);
    const std::int64_t tolerance = n::kReplayTolerance.count();

    // The edges, asserted rather than approached: inside is accepted and one
    // second past is not.
    EXPECT_TRUE(n::verify(secret, signature, kSignedAt, kBody,
                          at_seconds(kSignedAt + tolerance)));
    EXPECT_FALSE(n::verify(secret, signature, kSignedAt, kBody,
                           at_seconds(kSignedAt + tolerance + 1)));
    // A captured delivery resent a year later carries a perfectly valid
    // signature. The window is the only thing that refuses it.
    EXPECT_FALSE(n::verify(secret, signature, kSignedAt, kBody,
                           at_seconds(kSignedAt + 31'536'000)));
}

TEST(WebhookVerify, ATimestampFromTheFutureIsAlsoRefused) {
    const n::WebhookSecret secret = fixed_secret();
    const std::int64_t tolerance = n::kReplayTolerance.count();
    const std::string signature = n::sign(secret, kSignedAt, kBody);

    EXPECT_TRUE(n::verify(secret, signature, kSignedAt, kBody,
                          at_seconds(kSignedAt - tolerance)));
    // An unbounded future tolerance hands an attacker a signature that stays
    // valid for as long as they chose when they made it.
    EXPECT_FALSE(n::verify(secret, signature, kSignedAt, kBody,
                           at_seconds(kSignedAt - tolerance - 1)));
}

TEST(WebhookVerify, AMalformedSignatureFailsRatherThanThrowing) {
    const n::WebhookSecret secret = fixed_secret();
    const anvil::db::TimeMs now = at_seconds(kSignedAt);
    for (const std::string_view candidate :
         {std::string_view{}, std::string_view{"v1="}, std::string_view{"garbage"},
          std::string_view{"v2=0000000000000000000000000000000000000000000000000000000000000000"}}) {
        EXPECT_FALSE(n::verify(secret, candidate, kSignedAt, kBody, now)) << candidate;
    }
}

TEST(WebhookSecretGeneration, TwoSecretsDiffer) {
    // A CSPRNG, never a hash of anything the operator chose. Two consecutive
    // secrets being equal would mean the generator is not one.
    EXPECT_NE(n::generate_secret(), n::generate_secret());
}

// --- where a webhook may point -----------------------------------------------

TEST(WebhookUrl, APublicHttpsUrlIsAccepted) {
    EXPECT_EQ(n::check_webhook_url("https://hooks.example.com/anvil"), n::UrlVerdict::Ok);
    EXPECT_EQ(n::check_webhook_url("https://hooks.example.com:8443/anvil?x=1"),
              n::UrlVerdict::Ok);
    EXPECT_EQ(n::check_webhook_url("https://203.0.113.10/anvil"), n::UrlVerdict::Ok);
}

TEST(WebhookUrl, PlaintextIsRefused) {
    // The signature proves who sent a delivery, not that nobody else read it.
    EXPECT_EQ(n::check_webhook_url("http://hooks.example.com/anvil"), n::UrlVerdict::NotHttps);
    EXPECT_EQ(n::check_webhook_url("ftp://hooks.example.com/"), n::UrlVerdict::NotHttps);
    EXPECT_EQ(n::check_webhook_url("//hooks.example.com/"), n::UrlVerdict::NotHttps);
}

TEST(WebhookUrl, TheMetadataAddressIsRefused) {
    // The highest-value SSRF target there is: it hands out instance credentials
    // to anything that can make a plain GET.
    EXPECT_EQ(n::check_webhook_url("https://169.254.169.254/latest/meta-data/"),
              n::UrlVerdict::NotPublic);
}

TEST(WebhookUrl, LoopbackAndPrivateRangesAreRefused) {
    for (const std::string_view url : {
             std::string_view{"https://127.0.0.1/x"},
             std::string_view{"https://10.0.0.5/x"},
             std::string_view{"https://172.16.4.1/x"},
             std::string_view{"https://172.31.255.254/x"},
             std::string_view{"https://192.168.1.1/x"},
             std::string_view{"https://100.64.0.1/x"},
             std::string_view{"https://0.0.0.0/x"},
             std::string_view{"https://[::1]/x"},
             std::string_view{"https://[fe80::1]/x"},
             std::string_view{"https://[fc00::1]/x"},
         }) {
        EXPECT_EQ(n::check_webhook_url(url), n::UrlVerdict::NotPublic) << url;
    }
}

TEST(WebhookUrl, APrivateAddressWearingAnIpv6SpellingIsRefused) {
    // ::ffff:127.0.0.1. Without the mapped-address branch, every private v4
    // address reaches the network by being written the other way round.
    EXPECT_TRUE(n::is_private_address("::ffff:127.0.0.1"));
    EXPECT_TRUE(n::is_private_address("::ffff:169.254.169.254"));
    EXPECT_EQ(n::check_webhook_url("https://[::ffff:10.0.0.1]/x"), n::UrlVerdict::NotPublic);
}

TEST(WebhookUrl, AHostnameIsNotDecidableHereAndSaysSo) {
    // is_private_address answers about LITERALS. A hostname that resolves to a
    // private address is only caught after resolution — which is why the function
    // is exposed for the HTTP client to call again, and why this is not the last
    // line of defence.
    EXPECT_FALSE(n::is_private_address("localhost"));
    EXPECT_FALSE(n::is_private_address("metadata.google.internal"));
    EXPECT_EQ(n::check_webhook_url("https://localhost/x"), n::UrlVerdict::Ok);
}

TEST(WebhookUrl, AnArbitraryPortIsRefused) {
    // Otherwise a webhook is a port scanner whose results are visible in the
    // delivery log.
    EXPECT_EQ(n::check_webhook_url("https://hooks.example.com:22/x"),
              n::UrlVerdict::PortNotAllowed);
    EXPECT_EQ(n::check_webhook_url("https://hooks.example.com:6379/x"),
              n::UrlVerdict::PortNotAllowed);
}

TEST(WebhookUrl, CredentialsAFragmentAndControlCharactersAreRefused) {
    // Several HTTP clients read the userinfo form differently from the host that
    // ends up in a log, and a check that disagrees with the client about which
    // host it is checking is not a check.
    EXPECT_EQ(n::check_webhook_url("https://evil.example.com@10.0.0.1/x"),
              n::UrlVerdict::Malformed);
    EXPECT_EQ(n::check_webhook_url("https://hooks.example.com/x#frag"),
              n::UrlVerdict::Malformed);
    // A newline in a URL is request splitting.
    EXPECT_EQ(n::check_webhook_url("https://hooks.example.com/x\r\nHost: evil"),
              n::UrlVerdict::Malformed);
    EXPECT_EQ(n::check_webhook_url("https://hooks.exámple.com/x"), n::UrlVerdict::Malformed);
    EXPECT_EQ(n::check_webhook_url(""), n::UrlVerdict::Malformed);
    EXPECT_EQ(n::check_webhook_url("https://"), n::UrlVerdict::NotHttps);
    EXPECT_EQ(n::check_webhook_url("https:///path"), n::UrlVerdict::Malformed);
}

TEST(WebhookUrl, AnOverLongUrlIsRefusedRatherThanParsed) {
    std::string url = "https://hooks.example.com/";
    url.append(4096, 'a');
    EXPECT_EQ(n::check_webhook_url(url), n::UrlVerdict::Malformed);
}
