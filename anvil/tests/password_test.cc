// Password hashing.

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "anvil/auth/password.h"
#include "anvil/i18n/utf8.h"

namespace anvil::auth {
namespace {

// Reduced cost so the suite stays fast. Production parameters are exercised
// once, in ProductionParametersAreUsable below.
constexpr Argon2Params kTestParams{
    .memory_kib = 8192,
    .iterations = 2,
    .parallelism = 1,
};

const PasswordHasher& test_hasher() {
    // Constructed once: the constructor computes the dummy hash, which costs a
    // full Argon2 run.
    static const PasswordHasher hasher{kTestParams};
    return hasher;
}

}  // namespace

// --- 1: Argon2id, never bcrypt -------------------------------------------

TEST(Password, ProducesArgon2idEncodedHashes) {
    const std::string encoded = test_hasher().hash("correct horse battery staple");

    EXPECT_EQ(encoded.find("$argon2id$"), 0U);
    EXPECT_EQ(encoded.find("$2a$"), std::string::npos) << "bcrypt must appear nowhere";
    EXPECT_EQ(encoded.find("$2b$"), std::string::npos);
}

TEST(Password, VerifiesWhatItHashes) {
    const std::string encoded = test_hasher().hash("correct horse battery staple");

    EXPECT_EQ(test_hasher().verify(encoded, "correct horse battery staple"),
              VerifyOutcome::Match);
    EXPECT_EQ(test_hasher().verify(encoded, "correct horse battery stapl"),
              VerifyOutcome::Mismatch);
    EXPECT_EQ(test_hasher().verify(encoded, ""), VerifyOutcome::Mismatch);
}

TEST(Password, SaltIsPerHashSoIdenticalPasswordsDiffer) {
    const std::string a = test_hasher().hash("same password");
    const std::string b = test_hasher().hash("same password");

    EXPECT_NE(a, b) << "a shared salt would make the store rainbow-table-able";
    EXPECT_EQ(test_hasher().verify(a, "same password"), VerifyOutcome::Match);
    EXPECT_EQ(test_hasher().verify(b, "same password"), VerifyOutcome::Match);
}

// --- 2: the reason bcrypt is banned --------------------------------

TEST(Password, LongArabicPassphraseIsNotTruncated) {
    // 40 Arabic characters is ~80 bytes in UTF-8. bcrypt truncates at 72 BYTES,
    // so it would silently ignore everything past character 36 — the user's
    // deliberately long password would be weaker than they believe, with no
    // error. This test is the regression guard for ever "optimising" back.
    std::string passphrase;
    for (int i = 0; i < 40; ++i) { passphrase += "ق"; }

    ASSERT_EQ(i18n::count_code_points(passphrase), 40U);
    ASSERT_EQ(passphrase.size(), 80U) << "must exceed bcrypt's 72-byte limit";

    const std::string encoded = test_hasher().hash(passphrase);
    EXPECT_EQ(test_hasher().verify(encoded, passphrase), VerifyOutcome::Match);

    // Truncation at 72 bytes would make these two indistinguishable.
    std::string differs_past_byte_72 = passphrase;
    differs_past_byte_72.replace(74, 2, "ب");
    EXPECT_EQ(test_hasher().verify(encoded, differs_past_byte_72), VerifyOutcome::Mismatch)
        << "bytes past 72 must affect the hash";
}

TEST(Password, ArabicPasswordOfMaximumPolicyLengthWorks) {
    // 128 code points is the policy cap; in Arabic that is 256 bytes.
    std::string passphrase;
    for (int i = 0; i < 128; ++i) { passphrase += "م"; }
    ASSERT_EQ(passphrase.size(), 256U);

    const std::string encoded = test_hasher().hash(passphrase);
    EXPECT_EQ(test_hasher().verify(encoded, passphrase), VerifyOutcome::Match);
}

// --- 3: normalisation must match on both paths ---------------------------

TEST(Password, NfcNormalisationIsAppliedIdentically) {
    // The same Arabic passphrase typed on two keyboards produces different byte
    // sequences. Without identical normalisation at signup and login the user
    // simply cannot log in — and it looks like a forgotten password.
    // Written as explicit bytes, not as source literals: an editor normalises
    // pasted text, which would silently make both forms identical and leave the
    // test asserting nothing.
    //
    //   U+0623 ARABIC LETTER ALEF WITH HAMZA ABOVE  = D8 A3        (composed)
    //   U+0627 ALEF + U+0654 ARABIC HAMZA ABOVE     = D8 A7 D9 94  (decomposed)
    const std::string composed = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\xD8\xA3";
    const std::string decomposed = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\xD8\xA7\xD9\x94";
    ASSERT_NE(composed, decomposed) << "test inputs must actually differ in bytes";

    const std::string encoded = test_hasher().hash(composed);
    EXPECT_EQ(test_hasher().verify(encoded, decomposed), VerifyOutcome::Match);

    const std::string encoded_other_way = test_hasher().hash(decomposed);
    EXPECT_EQ(test_hasher().verify(encoded_other_way, composed), VerifyOutcome::Match);
}

TEST(Password, IsNeverTrimmedOrCaseFolded) {
    // A password is bytes the user chose. Leading whitespace is part of it, and
    // case is certainly part of it.
    const std::string padded = "  Secret Passphrase  ";
    const std::string encoded = test_hasher().hash(padded);

    EXPECT_EQ(test_hasher().verify(encoded, padded), VerifyOutcome::Match);
    EXPECT_EQ(test_hasher().verify(encoded, "Secret Passphrase"), VerifyOutcome::Mismatch);
    EXPECT_EQ(test_hasher().verify(encoded, "  secret passphrase  "), VerifyOutcome::Mismatch);
}

// --- oversized input ------------------------------------------------------

TEST(Password, RejectsInputAboveTheHardByteCap) {
    // Defence in depth behind the 128-code-point policy limit: an unbounded
    // password is a memory amplification vector regardless of what ran before.
    const std::string huge(kMaxPasswordBytes + 1, 'x');
    EXPECT_THROW((void)test_hasher().hash(huge), std::invalid_argument);

    // Verify must not throw on a hostile candidate — it is a failed login.
    const std::string encoded = test_hasher().hash("short");
    EXPECT_EQ(test_hasher().verify(encoded, huge), VerifyOutcome::Mismatch);
}

// --- malformed stored hashes ---------------------------------------------

TEST(Password, MalformedStoredHashIsDistinguishedFromAWrongPassword) {
    // Distinguished for the SERVER: Malformed means a corrupt row worth
    // alerting on. Callers map both to the same client-visible failure.
    EXPECT_EQ(test_hasher().verify("not-a-hash", "password"), VerifyOutcome::Malformed);
    EXPECT_EQ(test_hasher().verify("", "password"), VerifyOutcome::Malformed);
    EXPECT_EQ(test_hasher().verify("$argon2id$garbage", "password"), VerifyOutcome::Malformed);
}

// --- 6: timing equalisation ----------------------------------------

TEST(Password, DummyVerifyCostsComparableTimeToARealOne) {
    // Skipping the hash for an unknown account makes a missing user return in
    // microseconds and an existing one in ~100 ms, which is a trivially
    // exploitable enumeration oracle.
    const std::string encoded = test_hasher().hash("a real password");

    const auto time_real = [&] {
        const auto start = std::chrono::steady_clock::now();
        (void)test_hasher().verify(encoded, "a real password");
        return std::chrono::steady_clock::now() - start;
    };
    const auto time_dummy = [&] {
        const auto start = std::chrono::steady_clock::now();
        test_hasher().consume_dummy_time();
        return std::chrono::steady_clock::now() - start;
    };

    // Warm caches before measuring.
    (void)time_real();
    (void)time_dummy();

    const auto real = time_real().count();
    const auto dummy = time_dummy().count();

    ASSERT_GT(real, 0);
    ASSERT_GT(dummy, 0);
    // Loose bounds: this asserts the work is genuinely performed, not a precise
    // timing property, so it does not flake on a loaded machine.
    EXPECT_GT(dummy * 4, real) << "dummy verify must not be trivially fast";
    EXPECT_LT(dummy, real * 4) << "dummy verify must not be far slower either";
}

// --- 8: rehash policy -----------------------------------------------------

TEST(Password, DetectsHashesBelowCurrentPolicy) {
    const PasswordHasher weak{Argon2Params{8192, 2, 1}};
    const PasswordHasher strong{Argon2Params{16384, 3, 1}};

    const std::string weak_hash = weak.hash("password");

    EXPECT_TRUE(strong.needs_rehash(weak_hash)) << "below policy on memory and iterations";
    EXPECT_FALSE(weak.needs_rehash(weak_hash)) << "at policy";

    const std::string strong_hash = strong.hash("password");
    EXPECT_FALSE(weak.needs_rehash(strong_hash)) << "above policy is fine";

    // A hash still verifies across a policy change — rehashing is a background
    // upgrade, never a lockout.
    EXPECT_EQ(strong.verify(weak_hash, "password"), VerifyOutcome::Match);
}

TEST(Password, UnparseableHashCountsAsNeedingRehash) {
    EXPECT_TRUE(test_hasher().needs_rehash("not-a-hash"));
    EXPECT_TRUE(test_hasher().needs_rehash(""));
}

TEST(Password, ParsesEncodedParameters) {
    const std::string encoded = test_hasher().hash("password");
    const std::optional<Argon2Params> parsed = parse_encoded_params(encoded);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->memory_kib, kTestParams.memory_kib);
    EXPECT_EQ(parsed->iterations, kTestParams.iterations);
    EXPECT_EQ(parsed->parallelism, kTestParams.parallelism);

    EXPECT_FALSE(parse_encoded_params("$2a$10$abcdef").has_value()) << "bcrypt is not accepted";
    EXPECT_FALSE(parse_encoded_params("").has_value());
}

// --- parameter policy floor ----------------------------------------------

TEST(Password, RefusesParametersBelowPolicy) {
    // Misconfiguration must fail at construction, not silently produce weak
    // hashes that nobody notices until a breach.
    EXPECT_THROW((PasswordHasher{Argon2Params{1024, 3, 1}}), std::invalid_argument);
    EXPECT_THROW((PasswordHasher{Argon2Params{65536, 1, 1}}), std::invalid_argument);
    EXPECT_THROW((PasswordHasher{Argon2Params{65536, 3, 0}}), std::invalid_argument);
}

TEST(Password, ProductionParametersAreUsable) {
    // The only test that pays the real 64 MiB / 3-iteration cost, so that the
    // configured production parameters are actually exercised somewhere.
    const PasswordHasher hasher{kDefaultArgon2Params};
    const std::string encoded = hasher.hash("production parameters");

    EXPECT_EQ(hasher.verify(encoded, "production parameters"), VerifyOutcome::Match);
    EXPECT_FALSE(hasher.needs_rehash(encoded));

    const std::optional<Argon2Params> parsed = parse_encoded_params(encoded);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, kDefaultArgon2Params);
}

}  // namespace anvil::auth
