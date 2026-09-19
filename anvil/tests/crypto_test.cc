// The crypto primitives.

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>
#include <vector>

#include "anvil/crypto/aead.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/crypto/secret.h"

namespace anvil::crypto {
namespace {

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string hex(std::span<const std::uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0FU]);
    }
    return out;
}

}  // namespace

// --- 1: CSPRNG ------------------------------------------------------------

TEST(Random, ProducesDistinctValues) {
    // Not a randomness test — a smoke test that the CSPRNG is wired up. A
    // generator returning a constant is the failure this catches.
    std::set<std::string> seen;
    for (int i = 0; i < 5000; ++i) {
        seen.insert(hex(random_array<16>()));
    }
    EXPECT_EQ(seen.size(), 5000U);
}

TEST(Random, TokenIs256BitsOfEntropy) {
    const std::string token = random_token();
    EXPECT_EQ(token.size(), 43U) << "32 bytes as unpadded base64url";
    EXPECT_EQ(token.find('='), std::string::npos) << "never padded";
    EXPECT_EQ(token.find('+'), std::string::npos) << "url-safe alphabet only";
    EXPECT_EQ(token.find('/'), std::string::npos);
}

TEST(Random, EmptySpanIsANoOp) {
    std::array<std::uint8_t, 0> empty{};
    EXPECT_NO_THROW(random_bytes(empty));
}

// --- 2: digests against known vectors ------------------------------------

TEST(Digest, Sha256MatchesRfcVectors) {
    EXPECT_EQ(hex(sha256("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex(sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Digest, HmacSha256MatchesRfc4231Vector) {
    // RFC 4231 test case 1.
    const std::array<std::uint8_t, 20> key{0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                           0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                           0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
    EXPECT_EQ(hex(hmac_sha256(key, "Hi There")),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(Digest, PepperChangesTheResult) {
    const std::array<std::uint8_t, 32> pepper_a{1};
    const std::array<std::uint8_t, 32> pepper_b{2};

    const Digest256 with_a = sha256_with_pepper("token", pepper_a);
    const Digest256 with_b = sha256_with_pepper("token", pepper_b);

    EXPECT_FALSE(secure_equal(with_a, with_b))
        << "a leaked database without the pepper must not permit token lookup";
}

TEST(Digest, BlindIndexIsKeyedAndDeterministic) {
    // A plain SHA-256 of a structured 14-digit National ID is exhaustible in
    // seconds; the HMAC key is what makes the index safe to store.
    const std::array<std::uint8_t, 32> key{7};
    const Digest256 a = blind_index("29801012345678", key);
    const Digest256 b = blind_index("29801012345678", key);
    const Digest256 c = blind_index("29801012345679", key);

    EXPECT_TRUE(secure_equal(a, b)) << "duplicate detection needs determinism";
    EXPECT_FALSE(secure_equal(a, c));

    EXPECT_FALSE(secure_equal(a, sha256("29801012345678")))
        << "must not be a plain hash";
}

// --- 3: constant-time comparison -----------------------------------------

TEST(ConstantTime, EqualAndUnequalBuffers) {
    const Digest256 a = sha256("secret");
    Digest256 b = a;
    EXPECT_TRUE(secure_equal(a, b));

    b[31] ^= 0x01U;
    EXPECT_FALSE(secure_equal(a, b));

    Digest256 c = a;
    c[0] ^= 0x01U;   // differs in the FIRST byte — must still be compared fully
    EXPECT_FALSE(secure_equal(a, c));
}

TEST(ConstantTime, LengthMismatchIsFalseNotUndefined) {
    const std::array<std::uint8_t, 4> short_buf{1, 2, 3, 4};
    const std::array<std::uint8_t, 8> long_buf{1, 2, 3, 4, 0, 0, 0, 0};
    EXPECT_FALSE(secure_equal(std::span<const std::uint8_t>{short_buf},
                              std::span<const std::uint8_t>{long_buf}));
}

TEST(ConstantTime, EmptySpansAreEqual) {
    EXPECT_TRUE(secure_equal(std::span<const std::uint8_t>{},
                             std::span<const std::uint8_t>{}));
}

// --- 4-5: secret buffers --------------------------------------------------

TEST(SecretBuffer, IsMoveOnly) {
    static_assert(!std::is_copy_constructible_v<Key256>);
    static_assert(!std::is_copy_assignable_v<Key256>);
    static_assert(std::is_move_constructible_v<Key256>);
    static_assert(std::is_move_assignable_v<Key256>);
}

TEST(SecretBuffer, MoveWipesTheSource) {
    // The moved-from buffer must not keep a readable copy of the plaintext.
    Key256 original;
    random_bytes(original.mutable_span());
    const Key256 moved = std::move(original);

    // Reading a moved-from object is the point of this test: the guarantee is
    // that it holds zeroes rather than a readable copy of the key.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    bool source_is_zero = true;
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (original.data()[i] != 0U) { source_is_zero = false; }
    }
    EXPECT_TRUE(source_is_zero);

    bool destination_has_data = false;
    for (std::size_t i = 0; i < moved.size(); ++i) {
        if (moved.data()[i] != 0U) { destination_has_data = true; }
    }
    EXPECT_TRUE(destination_has_data);
}

TEST(SecretBuffer, StartsZeroed) {
    const Key256 key;
    for (std::size_t i = 0; i < key.size(); ++i) {
        EXPECT_EQ(key.data()[i], 0U);
    }
}

// --- 6-7: base64url -------------------------------------------------------

TEST(Base64Url, RoundTripsEveryLengthUpToThirtyTwo) {
    for (std::size_t len = 0; len <= 32; ++len) {
        std::vector<std::uint8_t> input(len);
        if (len > 0) { random_bytes(input); }

        const std::string encoded = base64url_encode(input);
        const auto decoded = base64url_decode(encoded);

        ASSERT_TRUE(decoded.has_value()) << "len=" << len;
        EXPECT_EQ(*decoded, input) << "len=" << len;
    }
}

TEST(Base64Url, RejectsPaddingWhitespaceAndStandardAlphabet) {
    EXPECT_FALSE(base64url_decode("QQ==").has_value()) << "padding is rejected";
    EXPECT_FALSE(base64url_decode("QQ ").has_value()) << "whitespace is rejected";
    EXPECT_FALSE(base64url_decode("a+b/").has_value()) << "standard alphabet is rejected";
    EXPECT_FALSE(base64url_decode("A").has_value()) << "a lone character encodes nothing";
}

TEST(Base64Url, RejectsNonCanonicalTrailingBits) {
    // "QQ" decodes to one byte; the trailing 4 bits must be zero. Accepting
    // non-zero bits means several strings decode to the same value, and a hash
    // lookup keyed on the string form could be bypassed by re-spelling it.
    EXPECT_TRUE(base64url_decode("QQ").has_value());
    EXPECT_FALSE(base64url_decode("QR").has_value());
}

TEST(Base64Url, DecodeIntoRejectsOverflowRatherThanWriting) {
    const std::string encoded = base64url_encode(random_array<32>());
    std::array<std::uint8_t, 8> too_small{};
    EXPECT_FALSE(base64url_decode_into(encoded, too_small).has_value());

    std::array<std::uint8_t, 32> exact{};
    const auto written = base64url_decode_into(encoded, exact);
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 32U);
}

TEST(Base64Url, DecodedSizeIsExact) {
    static_assert(base64url_decoded_size(0) == 0U);
    static_assert(base64url_decoded_size(2) == 1U);
    static_assert(base64url_decoded_size(3) == 2U);
    static_assert(base64url_decoded_size(4) == 3U);
    static_assert(base64url_decoded_size(43) == 32U);
    static_assert(!base64url_decoded_size(1).has_value());
    static_assert(!base64url_decoded_size(5).has_value());
}

// --- 8: AES-256-GCM -------------------------------------------------------

TEST(Aead, SealAndOpenRoundTrip) {
    const Key256 key = random_secret<32>();
    const std::string aad = "form:abc|field:f1";
    constexpr std::string_view national_id = "29801012345678";

    const auto envelope = seal(key.span(), national_id, bytes_of(aad));
    const auto opened = open(key.span(), envelope, bytes_of(aad));

    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(*opened, national_id);
}

TEST(Aead, TamperedCiphertextFailsToOpen) {
    const Key256 key = random_secret<32>();
    auto envelope = seal(key.span(), "29801012345678", {});

    envelope.back() ^= 0x01U;
    EXPECT_FALSE(open(key.span(), envelope, {}).has_value());
}

TEST(Aead, TamperedTagFailsToOpen) {
    const Key256 key = random_secret<32>();
    auto envelope = seal(key.span(), "29801012345678", {});

    envelope[1 + kAeadIvBytes] ^= 0x01U;
    EXPECT_FALSE(open(key.span(), envelope, {}).has_value());
}

TEST(Aead, WrongKeyFailsToOpen) {
    const Key256 key = random_secret<32>();
    const Key256 other = random_secret<32>();
    const auto envelope = seal(key.span(), "29801012345678", {});
    EXPECT_FALSE(open(other.span(), envelope, {}).has_value());
}

TEST(Aead, WrongAadFailsToOpen) {
    // The envelope is bound to its context, so a ciphertext moved to another
    // form or another field fails rather than decrypting into the wrong record.
    const Key256 key = random_secret<32>();
    const auto envelope = seal(key.span(), "29801012345678", bytes_of("form:A|field:f1"));
    EXPECT_FALSE(open(key.span(), envelope, bytes_of("form:B|field:f1")).has_value());
}

TEST(Aead, IvIsFreshPerSeal) {
    // A repeated IV under one key destroys GCM's confidentiality and permits
    // tag forgery, which is why seal() generates it rather than accepting one.
    const Key256 key = random_secret<32>();
    const auto first = seal(key.span(), "same plaintext", {});
    const auto second = seal(key.span(), "same plaintext", {});

    const std::span<const std::uint8_t> iv_a{first.data() + 1, kAeadIvBytes};
    const std::span<const std::uint8_t> iv_b{second.data() + 1, kAeadIvBytes};
    EXPECT_FALSE(secure_equal(iv_a, iv_b));
    EXPECT_NE(first, second) << "identical plaintexts must not produce identical envelopes";
}

TEST(Aead, TruncatedEnvelopeReturnsNulloptRatherThanThrowing) {
    // A corrupt row must not 500 a listing page.
    const Key256 key = random_secret<32>();
    const std::vector<std::uint8_t> truncated(kAeadOverhead - 1, 0);
    EXPECT_NO_THROW({ EXPECT_FALSE(open(key.span(), truncated, {}).has_value()); });
    EXPECT_FALSE(open(key.span(), {}, {}).has_value());
}

TEST(Aead, UnknownVersionByteIsRejected) {
    const Key256 key = random_secret<32>();
    auto envelope = seal(key.span(), "x", {});
    envelope[0] = 0xFEU;
    EXPECT_FALSE(open(key.span(), envelope, {}).has_value());
}

TEST(Aead, EmptyPlaintextRoundTrips) {
    const Key256 key = random_secret<32>();
    const auto envelope = seal(key.span(), "", {});
    const auto opened = open(key.span(), envelope, {});
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->empty());
}

}  // namespace anvil::crypto
