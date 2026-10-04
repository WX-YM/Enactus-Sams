// Ed25519 (anvil/crypto/ed25519.h).
//
// The known answers are RFC 8032 §7.1 TEST 1, TEST 2, TEST 3 and TEST SHA(abc),
// copied from the RFC text and cross-checked against pyca/cryptography before
// being copied in. Signing is deterministic, so reproducing the RFC's exact
// signature bytes proves the seed is loaded and the message framed as RFC 8032
// means, not merely that sign and verify agree with each other.

#include <gtest/gtest.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/crypto/ed25519.h"

namespace anvil::crypto {
namespace {

[[nodiscard]] std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

template <std::size_t N>
[[nodiscard]] std::array<std::uint8_t, N> array_from_hex(std::string_view hex) {
    const std::vector<std::uint8_t> bytes = from_hex(hex);
    std::array<std::uint8_t, N> out{};
    EXPECT_EQ(bytes.size(), N);
    std::copy_n(bytes.begin(), std::min(N, bytes.size()), out.begin());
    return out;
}

[[nodiscard]] Ed25519Seed seed_from_hex(std::string_view hex) {
    const auto bytes = array_from_hex<kEd25519SeedBytes>(hex);
    Ed25519Seed seed;
    std::copy(bytes.begin(), bytes.end(), seed.data());
    return seed;
}

struct Vector final {
    std::string_view name;
    std::string_view seed;
    std::string_view public_key;
    std::string_view message;
    std::string_view signature;
};

constexpr std::array kRfc8032{
    Vector{"TEST 1", "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
           "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
           "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
           "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    Vector{"TEST 2", "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
           "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
           "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
           "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    Vector{"TEST 3", "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
           "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
           "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
           "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    Vector{"TEST SHA(abc)", "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
           "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
           "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
           "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
           "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
           "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
};

// The y-coordinates of the small-order points, sign bit clear, recomputed
// outside this codebase (as [L]P for random points P) and matching libsodium's
// blocklist. Each is refused with the sign bit both clear and set.
constexpr std::array<std::string_view, 7> kSmallOrderY{
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0100000000000000000000000000000000000000000000000000000000000000",
    "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
    "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
};

}  // namespace

TEST(Ed25519, PublicKeyFromSeedMatchesRfc8032) {
    for (const Vector& v : kRfc8032) {
        EXPECT_EQ(ed25519_public_key_from_seed(seed_from_hex(v.seed)),
                  array_from_hex<kEd25519PublicKeyBytes>(v.public_key))
            << v.name;
    }
}

TEST(Ed25519, SignReproducesRfc8032Signatures) {
    for (const Vector& v : kRfc8032) {
        EXPECT_EQ(ed25519_sign(seed_from_hex(v.seed), from_hex(v.message)),
                  array_from_hex<kEd25519SignatureBytes>(v.signature))
            << v.name;
    }
}

TEST(Ed25519, VerifyAcceptsRfc8032Signatures) {
    for (const Vector& v : kRfc8032) {
        const auto public_key = array_from_hex<kEd25519PublicKeyBytes>(v.public_key);
        EXPECT_TRUE(ed25519_public_key_is_valid(public_key)) << v.name;
        EXPECT_TRUE(ed25519_verify(public_key, from_hex(v.message),
                                   array_from_hex<kEd25519SignatureBytes>(v.signature)))
            << v.name;
    }
}

TEST(Ed25519, AnySingleBitFlipFailsVerification) {
    const Vector& v = kRfc8032[2];
    const auto public_key = array_from_hex<kEd25519PublicKeyBytes>(v.public_key);
    const auto message = from_hex(v.message);
    const auto signature = array_from_hex<kEd25519SignatureBytes>(v.signature);

    for (std::size_t bit = 0; bit < message.size() * 8; ++bit) {
        auto tampered = message;
        tampered[bit / 8] = static_cast<std::uint8_t>(tampered[bit / 8] ^ (1U << (bit % 8)));
        EXPECT_FALSE(ed25519_verify(public_key, tampered, signature)) << "message bit " << bit;
    }
    for (std::size_t bit = 0; bit < signature.size() * 8; ++bit) {
        auto tampered = signature;
        tampered[bit / 8] = static_cast<std::uint8_t>(tampered[bit / 8] ^ (1U << (bit % 8)));
        EXPECT_FALSE(ed25519_verify(public_key, message, tampered)) << "signature bit " << bit;
    }
    for (std::size_t bit = 0; bit < public_key.size() * 8; ++bit) {
        auto tampered = public_key;
        tampered[bit / 8] = static_cast<std::uint8_t>(tampered[bit / 8] ^ (1U << (bit % 8)));
        EXPECT_FALSE(ed25519_verify(tampered, message, signature)) << "key bit " << bit;
    }
}

TEST(Ed25519, SmallOrderKeysAreRefusedUnderEitherSign) {
    for (const std::string_view hex : kSmallOrderY) {
        auto key = array_from_hex<kEd25519PublicKeyBytes>(hex);
        EXPECT_FALSE(ed25519_public_key_is_valid(key)) << hex;
        key[31] = static_cast<std::uint8_t>(key[31] | 0x80U);
        EXPECT_FALSE(ed25519_public_key_is_valid(key)) << hex << " with the sign bit";
    }
}

TEST(Ed25519, IdentityKeyIsRefusedThoughOpenSslAloneWouldAcceptIt) {
    // With A the identity, [S]B = R + [k]A holds for every message once R is
    // the encoding of [S]B. S = 0 makes R the identity's encoding too. This is
    // the forgery the strict key check exists to stop, and the second half
    // shows OpenSSL does not stop it on its own.
    const auto identity = array_from_hex<kEd25519PublicKeyBytes>(kSmallOrderY[1]);
    Ed25519Signature forged{};
    forged[0] = 0x01U;
    const std::array<std::uint8_t, 5> message{'h', 'e', 'l', 'l', 'o'};

    EXPECT_FALSE(ed25519_verify(identity, message, forged));

    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, identity.data(), identity.size()),
        &EVP_PKEY_free};
    ASSERT_TRUE(key);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(),
                                                                      &EVP_MD_CTX_free};
    ASSERT_TRUE(ctx);
    ASSERT_EQ(EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()), 1);
    EXPECT_EQ(EVP_DigestVerify(ctx.get(), forged.data(), forged.size(), message.data(),
                               message.size()),
              1)
        << "if OpenSSL starts refusing small-order keys itself, this half can go";
}

TEST(Ed25519, NonCanonicalYIsRefused) {
    // y = p + 2 .. 2^255 - 1 spell the same points as 2 .. 18. OpenSSL reduces
    // them silently; one key with two spellings defeats byte comparison.
    std::array<std::uint8_t, kEd25519PublicKeyBytes> key{};
    key.fill(0xffU);
    key[31] = 0x7fU;
    for (unsigned low = 0xedU; low <= 0xffU; ++low) {
        key[0] = static_cast<std::uint8_t>(low);
        EXPECT_FALSE(ed25519_public_key_is_valid(key)) << "low byte " << low;
    }
}

TEST(Ed25519, YWithNoCurvePointIsRefused) {
    // y = 2: (y^2 - 1)/(d y^2 + 1) is not a square mod p, so no x exists.
    // Verified outside this codebase with the RFC 8032 recover_x routine.
    std::array<std::uint8_t, kEd25519PublicKeyBytes> key{};
    key[0] = 0x02U;
    EXPECT_FALSE(ed25519_public_key_is_valid(key));
}

TEST(Ed25519, GeneratedKeysAreValidAndSignaturesVerify) {
    for (int i = 0; i < 32; ++i) {
        const Ed25519Keypair pair = ed25519_generate_keypair();
        EXPECT_TRUE(ed25519_public_key_is_valid(pair.public_key));
        const std::array<std::uint8_t, 3> message{1, 2, 3};
        EXPECT_TRUE(ed25519_verify(pair.public_key, message, ed25519_sign(pair.seed, message)));
    }
}

}  // namespace anvil::crypto
