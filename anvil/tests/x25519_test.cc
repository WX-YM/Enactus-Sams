// X25519 key validation (anvil/crypto/x25519.h).
//
// The known answers are RFC 7748 §5.2 (the two one-iteration vectors) and
// §6.1 (Alice and Bob), copied from the RFC text. They prove x25519_derive is
// X25519, which is what lets the low-order test below use OpenSSL's own refusal
// as an independent check on the table in x25519.cc.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/x25519.h"

namespace anvil::crypto {
namespace {

[[nodiscard]] X25519PublicKey key_from_hex(std::string_view hex) {
    X25519PublicKey out{};
    EXPECT_EQ(hex.size(), out.size() * 2);
    for (std::size_t i = 0; i < out.size() && 2 * i + 1 < hex.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(2 * i, 2)}, nullptr, 16));
    }
    return out;
}

[[nodiscard]] X25519PrivateKey private_from_hex(std::string_view hex) {
    const X25519PublicKey bytes = key_from_hex(hex);
    X25519PrivateKey key;
    std::copy(bytes.begin(), bytes.end(), key.data());
    return key;
}

[[nodiscard]] bool shared_equals(const X25519SharedSecret& shared, std::string_view hex) {
    return secure_equal(std::span<const std::uint8_t>{shared.span()},
                        std::span<const std::uint8_t>{key_from_hex(hex)});
}

// libsodium's blocklist, after https://cr.yp.to/ecdh.html#validate, and
// re-derived outside this codebase as the Montgomery images of the Edwards
// torsion points. Bit 255 clear; each is also tried with it set.
constexpr std::array<std::string_view, 7> kLowOrderU{
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0100000000000000000000000000000000000000000000000000000000000000",
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
    "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
};

}  // namespace

TEST(X25519, Rfc7748OneIterationVectors) {
    const auto first = x25519_derive(
        private_from_hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
        key_from_hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"));
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(
        shared_equals(*first, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

    // This input u has bit 255 set: X25519 masks it, which is exactly why the
    // validator must compare its tables with the bit masked too.
    const auto second = x25519_derive(
        private_from_hex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
        key_from_hex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"));
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(
        shared_equals(*second, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"));
}

TEST(X25519, Rfc7748AliceAndBobAgree) {
    const auto alice_public =
        key_from_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const auto bob_public =
        key_from_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    constexpr std::string_view kShared =
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

    EXPECT_TRUE(x25519_public_key_is_valid(alice_public));
    EXPECT_TRUE(x25519_public_key_is_valid(bob_public));

    const auto at_alice = x25519_derive(
        private_from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"),
        bob_public);
    const auto at_bob = x25519_derive(
        private_from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"),
        alice_public);
    ASSERT_TRUE(at_alice.has_value());
    ASSERT_TRUE(at_bob.has_value());
    EXPECT_TRUE(shared_equals(*at_alice, kShared));
    EXPECT_TRUE(shared_equals(*at_bob, kShared));
}

TEST(X25519, LowOrderPointsAreRefusedWithAndWithoutTheHighBit) {
    const X25519Keypair pair = x25519_generate_keypair();
    for (const std::string_view hex : kLowOrderU) {
        auto key = key_from_hex(hex);
        for (int high = 0; high < 2; ++high) {
            if (high == 1) { key[31] = static_cast<std::uint8_t>(key[31] | 0x80U); }
            EXPECT_FALSE(x25519_public_key_is_valid(key)) << hex << " high bit " << high;
            // The table is not merely believed: OpenSSL's X25519 produces the
            // all-zero output for each row and refuses.
            EXPECT_FALSE(x25519_derive(pair.private_key, key).has_value())
                << hex << " high bit " << high;
        }
    }
}

TEST(X25519, NonCanonicalEncodingsAreRefused) {
    // A real key with bit 255 set is the same key to X25519 but different bytes.
    const X25519Keypair pair = x25519_generate_keypair();
    X25519PublicKey spelled_high = pair.public_key;
    spelled_high[31] = static_cast<std::uint8_t>(spelled_high[31] | 0x80U);
    EXPECT_FALSE(x25519_public_key_is_valid(spelled_high));

    // p + 2 .. 2^255 - 1 are 2 .. 18 spelled a second way.
    X25519PublicKey key{};
    key.fill(0xffU);
    key[31] = 0x7fU;
    for (unsigned low = 0xedU; low <= 0xffU; ++low) {
        key[0] = static_cast<std::uint8_t>(low);
        EXPECT_FALSE(x25519_public_key_is_valid(key)) << "low byte " << low;
    }
    // The largest canonical value, p - 1, is low order; p - 2 is the largest
    // value that is both canonical and accepted.
    key[0] = 0xebU;
    EXPECT_TRUE(x25519_public_key_is_valid(key));
}

TEST(X25519, GeneratedKeysAreAcceptedAndAgree) {
    for (int i = 0; i < 32; ++i) {
        const X25519Keypair a = x25519_generate_keypair();
        const X25519Keypair b = x25519_generate_keypair();
        EXPECT_TRUE(x25519_public_key_is_valid(a.public_key));
        const auto ab = x25519_derive(a.private_key, b.public_key);
        const auto ba = x25519_derive(b.private_key, a.public_key);
        ASSERT_TRUE(ab.has_value());
        ASSERT_TRUE(ba.has_value());
        EXPECT_TRUE(secure_equal(std::span<const std::uint8_t>{ab->span()},
                                 std::span<const std::uint8_t>{ba->span()}));
    }
}

}  // namespace anvil::crypto
