// Every third-party primitive anvil's security properties rest on, exercised
// before a single feature is written against any of it.
//
// This is not a test of the libraries — if ICU's normaliser is wrong, that is not
// a bug this file can usefully find. It is a test of the BUILD: that the versions
// vcpkg resolved, the flags the hardening module applies, and the triplet's
// linkage together produce working primitives. Each assertion below has a failure
// mode that is silent in production:
//
//   - a CSPRNG that fails and is not checked makes every token predictable;
//   - a memcmp that short-circuits leaks a secret one byte at a time;
//   - a cleanse the optimiser elided leaves key material in freed memory;
//   - a lenient UTF-8 decoder accepts an overlong that a downstream check rejects,
//     which is where parser-differential bugs live.
//
// The driver primitives are NOT here. anvil::foundation does not link mongocxx,
// and asserting that boundary is worth more than the convenience of one file —
// see tests/platform_smoke_test.cc.
//
// None of them produces an error. All of them produce a working server.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <argon2.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <simdutf.h>
#include <unicode/normalizer2.h>
#include <unicode/unistr.h>
#include <unicode/utypes.h>
#include <xxhash.h>

#include <gtest/gtest.h>

#include "anvil/core/version.h"

namespace {

// --- the library itself ----------------------------------------------------

TEST(DependencySmoke, VersionIsLinkedNotInlined) {
    const anvil::Version linked = anvil::version();
    EXPECT_EQ(linked.major, 0U);
    EXPECT_FALSE(anvil::version_string().empty());
}

// --- OpenSSL ---------------------------------------------------------------

TEST(DependencySmoke, CsprngSucceedsAndDoesNotRepeat) {
    std::array<std::uint8_t, 32> first{};
    std::array<std::uint8_t, 32> second{};

    // The return value is the whole point. RAND_bytes CAN fail — a container with
    // no entropy source is the usual way — and a caller that ignores it gets a
    // buffer of zeros that looks exactly like a key.
    ASSERT_EQ(RAND_bytes(first.data(), static_cast<int>(first.size())), 1);
    ASSERT_EQ(RAND_bytes(second.data(), static_cast<int>(second.size())), 1);

    EXPECT_NE(first, second);

    const std::array<std::uint8_t, 32> zeros{};
    EXPECT_NE(first, zeros);
}

TEST(DependencySmoke, ConstantTimeCompareDistinguishesEqualFromUnequal) {
    const std::array<std::uint8_t, 32> a{};
    std::array<std::uint8_t, 32> b{};

    EXPECT_EQ(CRYPTO_memcmp(a.data(), b.data(), a.size()), 0);

    // The last byte, deliberately: a comparison that short-circuits on the first
    // difference still returns the right answer here, so this asserts the result
    // rather than the timing. The timing property is why CRYPTO_memcmp is used at
    // all, and it is not something a unit test on a shared runner can measure.
    b[b.size() - 1] = 1;
    EXPECT_NE(CRYPTO_memcmp(a.data(), b.data(), a.size()), 0);
}

TEST(DependencySmoke, CleanseZeroesABufferTheOptimiserCouldElide) {
    std::array<std::uint8_t, 32> secret{};
    ASSERT_EQ(RAND_bytes(secret.data(), static_cast<int>(secret.size())), 1);

    OPENSSL_cleanse(secret.data(), secret.size());

    const std::array<std::uint8_t, 32> zeros{};
    EXPECT_EQ(secret, zeros);
}

// --- Argon2 ----------------------------------------------------------------

TEST(DependencySmoke, Argon2idAtProductionParametersWithNonLatinInput) {
    // The parameters a deployment actually uses. Running this at toy settings
    // would prove the symbol links and nothing about whether the real cost is
    // survivable on this machine.
    constexpr std::uint32_t kMemoryKib = 65536;
    constexpr std::uint32_t kIterations = 3;
    constexpr std::uint32_t kParallelism = 1;

    // Non-Latin on purpose. bcrypt is banned partly because it truncates at 72
    // bytes, which for a multi-byte passphrase is silent truncation mid-password;
    // this asserts the replacement handles the case that motivated the ban.
    const std::string passphrase = "قلعة-الحديد-٢٠٢٦-a-long-passphrase";
    std::array<std::uint8_t, 16> salt{};
    ASSERT_EQ(RAND_bytes(salt.data(), static_cast<int>(salt.size())), 1);

    std::array<char, 128> encoded{};
    const int hashed = argon2id_hash_encoded(
        kIterations, kMemoryKib, kParallelism, passphrase.data(), passphrase.size(),
        salt.data(), salt.size(), 32, encoded.data(), encoded.size());
    ASSERT_EQ(hashed, ARGON2_OK) << argon2_error_message(hashed);

    EXPECT_EQ(argon2id_verify(encoded.data(), passphrase.data(), passphrase.size()),
              ARGON2_OK);

    // A near miss must fail. One character, at the end, so a comparison that
    // stopped early would still have to reach it.
    const std::string wrong = passphrase + "x";
    EXPECT_NE(argon2id_verify(encoded.data(), wrong.data(), wrong.size()), ARGON2_OK);
}

// --- simdutf ---------------------------------------------------------------

TEST(DependencySmoke, Utf8ValidationAcceptsWellFormedInput) {
    const std::string_view arabic = "مرحبا";
    EXPECT_TRUE(simdutf::validate_utf8(arabic.data(), arabic.size()));
    EXPECT_EQ(simdutf::count_utf8(arabic.data(), arabic.size()), 5U);
}

TEST(DependencySmoke, Utf8ValidationRejectsEveryLenientShape) {
    // The four shapes a permissive decoder accepts and a strict one must not.
    // Each is a parser-differential primitive: if this layer accepts what a later
    // one rejects — or the reverse — the two disagree about what the input says.
    const std::array<std::string, 4> hostile{{
        std::string("\xC0\x80", 2),          // overlong NUL
        std::string("\xED\xA0\x80", 3),      // lone surrogate U+D800
        std::string("\xE2\x82", 2),          // truncated three-byte sequence
        std::string("\xF5\x80\x80\x80", 4),  // beyond U+10FFFF
    }};

    for (const std::string& bytes : hostile) {
        EXPECT_FALSE(simdutf::validate_utf8(bytes.data(), bytes.size()))
            << "accepted a malformed sequence of " << bytes.size() << " bytes";
    }
}

// --- ICU -------------------------------------------------------------------

TEST(DependencySmoke, IcuNormalisesToNfc) {
    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* nfc = icu::Normalizer2::getNFCInstance(status);
    ASSERT_TRUE(U_SUCCESS(status)) << u_errorName(status);
    ASSERT_NE(nfc, nullptr);

    // "e" + COMBINING ACUTE normalises to the single code point U+00E9. Two
    // spellings of one string must produce one digest, or a blind index silently
    // stops matching.
    const icu::UnicodeString decomposed = icu::UnicodeString::fromUTF8("é");
    const icu::UnicodeString composed = nfc->normalize(decomposed, status);
    ASSERT_TRUE(U_SUCCESS(status)) << u_errorName(status);

    EXPECT_EQ(composed.length(), 1);
    EXPECT_EQ(composed.char32At(0), 0x00E9);
}

// --- xxHash ----------------------------------------------------------------

TEST(DependencySmoke, XxHashIsStableAndDistinguishes) {
    const std::string_view payload = "anvil";
    const XXH64_hash_t once = XXH3_64bits(payload.data(), payload.size());
    const XXH64_hash_t again = XXH3_64bits(payload.data(), payload.size());

    // Stability is the requirement: this keys ETags, so a value that varied per
    // process would make every cached response miss on every instance.
    EXPECT_EQ(once, again);

    const std::string_view other = "anvi1";
    EXPECT_NE(once, XXH3_64bits(other.data(), other.size()));
}

}  // namespace
