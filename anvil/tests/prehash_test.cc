// Client-side password prehashing (anvil/auth/prehash.h).
//
// Every expected value below was computed OUTSIDE this codebase — the Argon2
// outputs by the reference libargon2 command-line tool, the HMACs and the
// records by Python's hmac and base64 — and the same numbers are asserted by
// hammer's own suite against its own Argon2. A vector this code produced and
// then checked against itself would be two copies of one belief.

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "anvil/auth/password.h"
#include "anvil/auth/prehash.h"
#include "anvil/crypto/argon2.h"
#include "anvil/i18n/normalize.h"

namespace anvil::auth {
namespace {

using crypto::Argon2Params;
using crypto::VerifyOutcome;

constexpr Argon2Params kClient{.memory_kib = 65536, .iterations = 3, .parallelism = 1};
// Cheap, so the tests that are not about the client stage's cost stay fast.
constexpr Argon2Params kCheapClient{.memory_kib = 8192, .iterations = 2, .parallelism = 1};

// UTF-8 of "pässwörd كلمة 🔑" — DECOMPOSED, as a paste from
// some keyboards produces it. The server must NFC it exactly as the client does.
constexpr std::string_view kTypedHex = "7061cc887373776fcc88726420d983d984d985d8a920f09f9491";
constexpr std::string_view kSaltAscii = "anvil-prehash-v1";

// k for the m=65536,t=3,p=1 row, as the client sends it.
constexpr std::string_view kCredential = "aLRDdDOLwfMuFU0S_tT3iX5dBG1zCBWqq2le8JwfWDo";

[[nodiscard]] std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoul(std::string{hex.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

[[nodiscard]] std::string hex_of(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0FU]);
    }
    return out;
}

// A key whose bytes are first, first+1, …, first+31.
[[nodiscard]] crypto::Key256 counting_key(std::uint8_t first) {
    crypto::Key256 key;
    for (std::size_t i = 0; i < key.size(); ++i) {
        key.mutable_span()[i] = static_cast<std::uint8_t>(first + i);
    }
    return key;
}

[[nodiscard]] PrehashSalt ascii_salt() {
    PrehashSalt salt{};
    std::copy(kSaltAscii.begin(), kSaltAscii.end(), salt.begin());
    return salt;
}

// What a CLIENT computes: NFC, UTF-8, Argon2id at the hasher's client-stage
// parameters. Written out here rather than borrowed from the code under test,
// so a defect in enroll_plaintext cannot hide by agreeing with itself.
[[nodiscard]] PrehashKey key_from_plaintext_for_test(const PrehashHasher& hasher,
                                                     std::string_view password,
                                                     const PrehashSalt& salt = ascii_salt()) {
    const std::optional<std::string> nfc = i18n::normalize(password, i18n::NormalizeMode::Nfc);
    EXPECT_TRUE(nfc.has_value());
    const std::string bytes = nfc.value_or(std::string{});
    PrehashKey k;
    crypto::argon2_hash_raw(
        crypto::Argon2Type::Argon2id, crypto::kArgon2Version13, hasher.policy().client,
        {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, salt, {}, {},
        k.mutable_span());
    return k;
}

[[nodiscard]] PrehashKey key_of(std::string_view credential) {
    std::optional<PrehashKey> k = decode_prehash_credential(credential);
    EXPECT_TRUE(k.has_value());
    return k.has_value() ? std::move(*k) : PrehashKey{};
}

[[nodiscard]] PrehashPolicy keyed_policy(Argon2Params client = kClient, std::string id = "k1") {
    return PrehashPolicy{
        .client = client,
        .server = PrehashKeyedDigestStage{.key_id = std::move(id), .key = counting_key(0x00)},
        .retired_peppers = {},
        .salt_key = counting_key(0x20),
    };
}

[[nodiscard]] PrehashPolicy argon2_policy(Argon2Params stage) {
    return PrehashPolicy{
        .client = kCheapClient,
        .server = PrehashArgon2Stage{.params = stage},
        .retired_peppers = {},
        .salt_key = counting_key(0x20),
    };
}

}  // namespace

// --- 1: the contract's golden vectors ------------------------------------------

TEST(PrehashGolden, EnrollFromPlaintextMatchesTheContractByteForByte) {
    const PrehashHasher hasher{keyed_policy()};
    const std::string record = hasher.enroll_plaintext(from_hex(kTypedHex), ascii_salt());
    EXPECT_EQ(record,
              "$anvil-prehash$v=1$argon2id$v=19$m=65536,t=3,p=1$YW52aWwtcHJlaGFzaC12MQ"
              "$hmac-sha256$k=k1$qRRjH7tBWhdaDulOzbN24Q6uQI7zP0dRj5mZl4o+H24");

    // And the credential a client computes from the same password verifies
    // against it — the whole contract, both halves, in one assertion.
    EXPECT_EQ(hasher.verify(record, key_of(kCredential)).outcome, VerifyOutcome::Match);
}

TEST(PrehashGolden, KeyedStageTagMatchesTheContract) {
    const PrehashHasher hasher{keyed_policy()};
    const std::string k_bytes =
        from_hex("57511e047b73ea51cd3542a631286e863e307ef117268122253bf47c4c6ce6e9");
    PrehashKey k;
    std::copy(k_bytes.begin(), k_bytes.end(), k.mutable_span().begin());

    const std::string record =
        hasher.enroll(k, PrehashSaltAnswer{ascii_salt(), {.memory_kib = 64, .iterations = 3,
                                                          .parallelism = 1}});
    EXPECT_EQ(record,
              "$anvil-prehash$v=1$argon2id$v=19$m=64,t=3,p=1$YW52aWwtcHJlaGFzaC12MQ"
              "$hmac-sha256$k=k1$zqSvbB0/Z68Yor2oLMEhExDXGnTiLcT+/wl4yhJarvU");
}

TEST(PrehashGolden, SaltDerivationMatchesTheContract) {
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    EXPECT_EQ(hex_of(hasher.derive_salt(0, "user@example.com")), "b69f49751b8edd7b30613739447faa71");
    EXPECT_EQ(hex_of(hasher.derive_salt(1, "someone")), "81444ba9e93411078fd27061953bd468");
    // The kind byte separates identifier spaces: one string, two kinds, two salts.
    EXPECT_NE(hasher.derive_salt(0, "someone"), hasher.derive_salt(1, "someone"));
}

// --- 2: the salt route ---------------------------------------------------------------

TEST(PrehashSaltRoute, AMissingAccountAnswersWithTheDerivedSaltAndPolicy) {
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    const PrehashSaltAnswer missing = hasher.answer_for(std::nullopt, 0, "user@example.com");
    EXPECT_EQ(missing.salt, hasher.derive_salt(0, "user@example.com"));
    EXPECT_EQ(missing.params, kCheapClient);
}

TEST(PrehashSaltRoute, AnEmailAnswersIdenticallyBeforeAndAfterRegistration) {
    // Registration stores the derived salt, so the salt route cannot be used to
    // watch an address become an account.
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    const PrehashSaltAnswer before = hasher.answer_for(std::nullopt, 0, "user@example.com");
    const std::string record =
        hasher.enroll_plaintext("a passphrase", hasher.derive_salt(0, "user@example.com"));
    const PrehashSaltAnswer after = hasher.answer_for(record, 0, "user@example.com");

    EXPECT_EQ(before.salt, after.salt);
    EXPECT_EQ(before.params, after.params);
}

TEST(PrehashSaltRoute, AnExistingAccountAnswersWithItsOwnSaltAndParameters) {
    const PrehashHasher old_policy{keyed_policy(kCheapClient)};
    const std::string record = old_policy.enroll_plaintext("a passphrase", ascii_salt());

    // Policy has since moved on; the stored record still says what the client
    // must hash with, because that is what it was enrolled under.
    const PrehashHasher hasher{keyed_policy(kClient)};
    const PrehashSaltAnswer answer = hasher.answer_for(record, 0, "user@example.com");
    EXPECT_EQ(answer.salt, ascii_salt());
    EXPECT_EQ(answer.params, kCheapClient);
}

TEST(PrehashSaltRoute, APlainModeRecordAnswersAsAMissingAccountWould) {
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    const PasswordHasher plain{kCheapClient};
    const PrehashSaltAnswer answer = hasher.answer_for(plain.hash("pw"), 0, "user@example.com");
    EXPECT_EQ(answer.salt, hasher.derive_salt(0, "user@example.com"));
}

TEST(PrehashSaltRoute, TheAnswerIsWrittenInTheContractsShape) {
    std::string body;
    append_prehash_salt_answer(body, PrehashSaltAnswer{ascii_salt(), kClient});
    EXPECT_EQ(body,
              R"({"algorithm":"argon2id","version":19,"salt":"YW52aWwtcHJlaGFzaC12MQ",)"
              R"("memory_kib":65536,"iterations":3,"parallelism":1,"hash_bytes":32})");
}

// --- 3: the credential on the wire -------------------------------------------------

TEST(PrehashCredential, OnlyExactlyFortyThreeBase64urlCharactersDecode) {
    EXPECT_TRUE(decode_prehash_credential(kCredential).has_value());

    EXPECT_FALSE(decode_prehash_credential("").has_value());
    EXPECT_FALSE(decode_prehash_credential(kCredential.substr(0, 42)).has_value());
    EXPECT_FALSE(decode_prehash_credential(std::string{kCredential} + "A").has_value());
    // Padded, and the standard alphabet's `/` and `+`: both are other encodings.
    EXPECT_FALSE(decode_prehash_credential(std::string{kCredential} + "=").has_value());
    std::string standard{kCredential};
    standard[20] = '/';
    EXPECT_FALSE(decode_prehash_credential(standard).has_value());
    // A plaintext password of the right length is not a credential.
    EXPECT_FALSE(decode_prehash_credential("correct horse battery staple and more text").has_value());
}

// --- 4: verification -----------------------------------------------------------------

TEST(PrehashVerify, AWrongCredentialIsAMismatch) {
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    const std::string record = hasher.enroll_plaintext("right", ascii_salt());
    PrehashKey wrong;
    wrong.mutable_span()[0] = 1;
    EXPECT_EQ(hasher.verify(record, wrong).outcome, VerifyOutcome::Mismatch);
}

TEST(PrehashVerify, UnparseableRecordsAreMalformed) {
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    const std::string good = hasher.enroll_plaintext("pw", ascii_salt());
    const PrehashKey any;

    const std::vector<std::string> bad = {
        "",
        good.substr(0, good.size() - 1),                                   // truncated tag
        good + "A",                                                         // trailing byte
        "$anvil-prehash$v=2" + good.substr(std::string_view{"$anvil-prehash$v=1"}.size()),
        [&] { std::string s = good; s.replace(s.find("m=8192"), 6, "m=08192"); return s; }(),
        [&] { std::string s = good; s.replace(s.find("k=k1"), 4, "k=K1"); return s; }(),
        [&] { std::string s = good; s.replace(s.find("hmac"), 4, "hmaC"); return s; }(),
        "$anvil-prehash$v=1$argon2id$v=19$m=99999999999,t=3,p=1$YW52aWwtcHJlaGFzaC12MQ"
        "$hmac-sha256$k=k1$qRRjH7tBWhdaDulOzbN24Q6uQI7zP0dRj5mZl4o+H24",
    };
    for (const std::string& record : bad) {
        EXPECT_EQ(hasher.verify(record, any).outcome, VerifyOutcome::Malformed) << record;
    }
}

TEST(PrehashVerify, AnUnwrappedPlainModeRecordIsRefused) {
    // Its hash IS the client's k for that salt. Accepting it would make the
    // stored value itself a working credential; wrap_legacy exists instead.
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    const PasswordHasher plain{kCheapClient};
    const std::string legacy = plain.hash("pw");
    EXPECT_EQ(hasher.verify(legacy, PrehashKey{}).outcome, VerifyOutcome::Malformed);
}

TEST(PrehashVerify, ARetiredPepperVerifiesAndAsksForARehash) {
    const PrehashHasher before{keyed_policy(kCheapClient, "old")};
    const std::string record = before.enroll_plaintext("pw", ascii_salt());

    PrehashPolicy rotated = keyed_policy(kCheapClient, "new");
    std::get<PrehashKeyedDigestStage>(rotated.server).key = counting_key(0x40);
    rotated.retired_peppers.push_back({.key_id = "old", .key = counting_key(0x00)});
    const PrehashHasher after{std::move(rotated)};

    EXPECT_TRUE(after.needs_rehash(record));

    // The credential is what a client computes; the server regains it only by
    // enrolling the same plaintext, so derive it the same way here.
    const PrehashHasher probe{keyed_policy(kCheapClient, "old")};
    PrehashVerification result = after.verify(record, key_from_plaintext_for_test(probe, "pw"));
    ASSERT_EQ(result.outcome, VerifyOutcome::Match);
    ASSERT_FALSE(result.upgraded_record.empty());
    EXPECT_NE(result.upgraded_record.find("$hmac-sha256$k=new$"), std::string::npos);
    EXPECT_FALSE(after.needs_rehash(result.upgraded_record));
    // The client half is untouched: same salt, same parameters.
    EXPECT_EQ(after.answer_for(result.upgraded_record, 0, "").salt, ascii_salt());
}

TEST(PrehashVerify, APepperThisProcessNoLongerHoldsIsMalformed) {
    const PrehashHasher before{keyed_policy(kCheapClient, "gone")};
    const std::string record = before.enroll_plaintext("pw", ascii_salt());
    const PrehashHasher after{keyed_policy(kCheapClient, "k1")};
    EXPECT_EQ(after.verify(record, PrehashKey{}).outcome, VerifyOutcome::Malformed);
}

TEST(PrehashVerify, TheArgon2StageRoundTripsAndUpgradesItsOwnParameters) {
    constexpr Argon2Params kWeak{.memory_kib = 64, .iterations = 1, .parallelism = 1};
    constexpr Argon2Params kStrong{.memory_kib = 128, .iterations = 2, .parallelism = 1};
    const PrehashHasher weak{argon2_policy(kWeak)};
    const std::string record = weak.enroll_plaintext("pw", ascii_salt());
    ASSERT_NE(record.find("$argon2id$v=19$m=64,t=1,p=1$"), std::string::npos);

    const PrehashKey k = key_from_plaintext_for_test(weak, "pw");
    EXPECT_EQ(weak.verify(record, k).outcome, VerifyOutcome::Match);
    EXPECT_FALSE(weak.needs_rehash(record));

    const PrehashHasher strong{argon2_policy(kStrong)};
    const PrehashVerification upgraded = strong.verify(record, k);
    ASSERT_EQ(upgraded.outcome, VerifyOutcome::Match);
    EXPECT_NE(upgraded.upgraded_record.find("$argon2id$v=19$m=128,t=2,p=1$"), std::string::npos);
    EXPECT_EQ(strong.verify(upgraded.upgraded_record, k).outcome, VerifyOutcome::Match);
}

TEST(PrehashVerify, MovingFromTheKeyedStageToArgon2KeepsEveryAccount) {
    // Why retired peppers belong to the policy and not to the keyed stage.
    const PrehashHasher keyed{keyed_policy(kCheapClient, "k1")};
    const std::string record = keyed.enroll_plaintext("pw", ascii_salt());

    PrehashPolicy moved = argon2_policy({.memory_kib = 64, .iterations = 1, .parallelism = 1});
    moved.retired_peppers.push_back({.key_id = "k1", .key = counting_key(0x00)});
    const PrehashHasher argon2{std::move(moved)};

    const PrehashVerification result = argon2.verify(record, key_from_plaintext_for_test(keyed, "pw"));
    EXPECT_EQ(result.outcome, VerifyOutcome::Match);
    EXPECT_NE(result.upgraded_record.find("$argon2id$v=19$m=64,t=1,p=1$"), std::string::npos);
}

TEST(PrehashVerify, TheDummyPathRunsWithoutAnAccount) {
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    hasher.consume_dummy_time();   // must neither throw nor terminate
    SUCCEED();
}

// --- 5: migration ---------------------------------------------------------------------

TEST(PrehashMigration, AWrappedPlainModeRecordAcceptsTheClientsCredential) {
    // The end-to-end proof that an existing deployment moves across offline:
    // PasswordHasher wrote this record long ago; nobody signs in during the
    // migration; afterwards, the credential a client derives from the SAME
    // password — against the salt the salt route now serves — verifies.
    const PasswordHasher plain{kCheapClient};
    const std::string legacy = plain.hash(from_hex(kTypedHex));

    const PrehashHasher hasher{keyed_policy(kClient)};
    const Result<std::string> wrapped = hasher.wrap_legacy(legacy);
    ASSERT_TRUE(wrapped.ok());

    const PrehashSaltAnswer served = hasher.answer_for(wrapped.value(), 0, "user@example.com");
    EXPECT_EQ(served.params, kCheapClient) << "the record's own parameters, not policy's";

    const PrehashHasher client{keyed_policy(served.params)};
    const PrehashKey k = key_from_plaintext_for_test(client, from_hex(kTypedHex), served.salt);
    EXPECT_EQ(hasher.verify(wrapped.value(), k).outcome, VerifyOutcome::Match);
}

TEST(PrehashMigration, WhatIsNotAPlainModeRecordIsRefusedRatherThanGuessed) {
    const PrehashHasher hasher{keyed_policy(kCheapClient)};
    EXPECT_FALSE(hasher.wrap_legacy("").ok());
    EXPECT_FALSE(hasher.wrap_legacy("$2a$10$abcdefghijklmnopqrstuv").ok());
    EXPECT_FALSE(hasher.wrap_legacy("$argon2i$v=19$m=8192,t=2,p=1$YW52aWwtcHJlaGFzaC12MQ$"
                                    "qRRjH7tBWhdaDulOzbN24Q6uQI7zP0dRj5mZl4o+H24").ok());
    EXPECT_FALSE(hasher.wrap_legacy("$argon2id$v=16$m=8192,t=2,p=1$YW52aWwtcHJlaGFzaC12MQ$"
                                    "qRRjH7tBWhdaDulOzbN24Q6uQI7zP0dRj5mZl4o+H24").ok());
    // A 64-byte tag: another length, therefore not the client's k.
    EXPECT_FALSE(hasher.wrap_legacy("$argon2id$v=19$m=8192,t=2,p=1$YW52aWwtcHJlaGFzaC12MQ$"
                                    "qRRjH7tBWhdaDulOzbN24Q6uQI7zP0dRj5mZl4o+H24qRRjH7tBWhdaDulOz"
                                    "bN24Q6uQI7zP0dRj5mZl4o+H24").ok());
    // Already wrapped is not wrapped twice.
    const std::string record = hasher.enroll_plaintext("pw", ascii_salt());
    EXPECT_FALSE(hasher.wrap_legacy(record).ok());
}

// --- 6: configuration ----------------------------------------------------------------

TEST(PrehashPolicyChecks, ConfigurationThatCannotBeRightIsRefusedAtConstruction) {
    EXPECT_THROW(PrehashHasher{keyed_policy({.memory_kib = 4096, .iterations = 3, .parallelism = 1})},
                 std::invalid_argument);
    EXPECT_THROW(PrehashHasher{keyed_policy({.memory_kib = 65536, .iterations = 1, .parallelism = 1})},
                 std::invalid_argument);
    EXPECT_THROW(PrehashHasher{keyed_policy(kCheapClient, "")}, std::invalid_argument);
    EXPECT_THROW(PrehashHasher{keyed_policy(kCheapClient, "Upper")}, std::invalid_argument);
    EXPECT_THROW(PrehashHasher{keyed_policy(kCheapClient, "has$dollar")}, std::invalid_argument);
    EXPECT_THROW(PrehashHasher{keyed_policy(kCheapClient, "seventeen-chars-x")}, std::invalid_argument);

    PrehashPolicy duplicate = keyed_policy(kCheapClient, "k1");
    duplicate.retired_peppers.push_back({.key_id = "k1", .key = counting_key(0x40)});
    EXPECT_THROW(PrehashHasher{std::move(duplicate)}, std::invalid_argument);

    EXPECT_THROW(PrehashHasher{argon2_policy({.memory_kib = 16, .iterations = 1, .parallelism = 4})},
                 std::invalid_argument);
}

}  // namespace anvil::auth
