// AES-256-SIV (anvil/crypto/siv.h).
//
// The known-answer vectors are Project Wycheproof's, not produced here:
// testvectors_v1/aes_siv_cmac_test.json (C2SP/wycheproof), the keySize 512
// group, cited by tcId. They were cross-checked against an independent AES-SIV
// (pyca/cryptography's AESSIV) before being copied in. Wycheproof feeds every
// case exactly one associated-data component, empty or not, which is the
// framing siv.h commits to — so the empty-aad vectors are what pin down that an
// empty component is authenticated rather than dropped.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/crypto/errors.h"
#include "anvil/crypto/random.h"
#include "anvil/crypto/siv.h"

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

[[nodiscard]] std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

struct Vector final {
    int tc_id;
    std::string_view key;
    std::string_view aad;
    std::string_view msg;
    std::string_view ct;
};

constexpr std::array kWycheproof{
    // tcId 296: empty message, empty aad.
    Vector{296,
           "bc7635c1fd566aa8357fd103714bfaee1c9e5b3c578b3980401a981030254a54"
           "b1756a8c96e600b7252fd0aab12f39d115d256b3f3e7c2c41a7fece72ba7c3c4",
           "", "", "44b1c6fe8a8c07dee5377b161f283c31"},
    // tcId 297: empty message, 12-byte aad.
    Vector{297,
           "aff6388fdd2908e0c3b610e3dcd410c8146a268d6befd5c45ffdd23508b5b311"
           "cc3a9d8f838f456436b289018682151dd57d8d65d1a823c06eca8ab8ee01da01",
           "d0bb2949a411e22d32964526", "", "e288d802a0e56ed7544a2e5775459389"},
    // tcId 302: message size divisible by the block size.
    Vector{302,
           "c25cafc6018b98dfbb79a40ec89c575a4f88c4116489bba27707479800c01302"
           "35334a45dbe8d8dae3da8dcb45bbe5dce031b0f68ded544fda7eca30d6749442",
           "deeb0ccf3aef47a296ed1ca8f4ae5907", "beec61030fa3d670337196beade6aeaa",
           "5865208eab9163db85cab9f96d846234a2626aae22f5c17c9aad4b501f4416e4"},
    // tcId 322: plaintext longer than a block and not a multiple of it.
    Vector{322,
           "c0f4bede68c6ed5ad14918e2ddeced692dfd5419c04204d5b96f4ca47078b070"
           "28c6fb87b1b490d875f070bbe4d790f65e5df19947f02c9d3a4e493b542d0291",
           "b411e4d2facca67ea4a9f2a1",
           "2f358d4534559ac99dd71798b7925705d6f013f6b848ffe01cc86cef09d88f",
           "d27ae26dfe02e3eedf544e1b452cb0f0c9303a4e4819318315ae08839bcce558"
           "e4741817ec08dae406bbfc09f59aa9"},
    // tcId 326: an all-zero synthetic IV, the CTR-counter edge case.
    Vector{326,
           "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
           "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
           "000102030405060708090a0b0c0d0e0f", "bfa1733d07afa03cb3f2eeb81bbde037",
           "000000000000000000000000000000004ce637d6ca9cdf1c2feb0d140865cddb"},
};

[[nodiscard]] std::vector<std::uint8_t> random_key() {
    std::vector<std::uint8_t> key(kSivKeyBytes);
    random_bytes(key);
    return key;
}

}  // namespace

TEST(Siv, SealMatchesWycheproofVectors) {
    for (const Vector& v : kWycheproof) {
        EXPECT_EQ(siv_seal(from_hex(v.key), from_hex(v.msg), from_hex(v.aad)), from_hex(v.ct))
            << "tcId " << v.tc_id;
    }
}

TEST(Siv, OpenMatchesWycheproofVectors) {
    for (const Vector& v : kWycheproof) {
        const auto opened = siv_open(from_hex(v.key), from_hex(v.ct), from_hex(v.aad));
        ASSERT_TRUE(opened.has_value()) << "tcId " << v.tc_id;
        EXPECT_EQ(*opened, from_hex(v.msg)) << "tcId " << v.tc_id;
    }
}

TEST(Siv, IsDeterministic) {
    // The whole reason for SIV here: the same object in the same expiry bucket
    // must be the same URL, or the browser cache never hits.
    const auto key = random_key();
    const auto first = siv_seal(key, bytes_of("ns|object|bucket"), bytes_of("grant"));
    const auto second = siv_seal(key, bytes_of("ns|object|bucket"), bytes_of("grant"));
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.size(), kSivTagBytes + std::string_view{"ns|object|bucket"}.size());
}

TEST(Siv, DifferentAadChangesTheOutputAndRefusesToOpen) {
    const auto key = random_key();
    const auto a = siv_seal(key, bytes_of("payload"), bytes_of("context-a"));
    const auto b = siv_seal(key, bytes_of("payload"), bytes_of("context-b"));
    EXPECT_NE(a, b);
    EXPECT_FALSE(siv_open(key, a, bytes_of("context-b")).has_value());
}

TEST(Siv, EmptyAadIsAComponentNotAnAbsence) {
    // RFC 5297 distinguishes one empty component from none. A wrapper that
    // skipped the empty update would still round-trip with itself and still
    // fail the tcId 296 vector; this states the property directly.
    const auto key = random_key();
    const auto sealed = siv_seal(key, bytes_of("payload"), {});
    const std::array<std::uint8_t, 1> one_byte{0};
    EXPECT_FALSE(siv_open(key, sealed, one_byte).has_value());
    EXPECT_TRUE(siv_open(key, sealed, {}).has_value());
}

TEST(Siv, EverySingleBitFlipRefusesToOpen) {
    const auto key = random_key();
    const auto sealed = siv_seal(key, bytes_of("ns|object|bucket"), bytes_of("grant"));
    for (std::size_t byte = 0; byte < sealed.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto tampered = sealed;
            tampered[byte] = static_cast<std::uint8_t>(tampered[byte] ^ (1U << bit));
            EXPECT_FALSE(siv_open(key, tampered, bytes_of("grant")).has_value())
                << "byte " << byte << " bit " << bit;
        }
    }
}

TEST(Siv, TruncatedInputRefusesWithoutThrowing) {
    // A mangled URL must not 500 the media origin.
    const auto key = random_key();
    const auto sealed = siv_seal(key, bytes_of("payload"), {});
    for (std::size_t len = 0; len < sealed.size(); ++len) {
        const std::span<const std::uint8_t> prefix{sealed.data(), len};
        EXPECT_NO_THROW({ EXPECT_FALSE(siv_open(key, prefix, {}).has_value()) << len; });
    }
}

TEST(Siv, EmptyPlaintextRoundTrips) {
    const auto key = random_key();
    const auto sealed = siv_seal(key, {}, bytes_of("aad"));
    EXPECT_EQ(sealed.size(), kSivTagBytes);
    const auto opened = siv_open(key, sealed, bytes_of("aad"));
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->empty());
}

TEST(Siv, WrongKeyRefusesToOpen) {
    const auto key = random_key();
    const auto other = random_key();
    const auto sealed = siv_seal(key, bytes_of("payload"), bytes_of("aad"));
    EXPECT_FALSE(siv_open(other, sealed, bytes_of("aad")).has_value());
}

TEST(Siv, WrongKeySizeIsADeploymentDefectAndThrows) {
    const std::vector<std::uint8_t> short_key(32, 0);
    EXPECT_THROW((void)siv_seal(short_key, bytes_of("x"), {}), CryptoError);
    EXPECT_THROW((void)siv_open(short_key, std::vector<std::uint8_t>(kSivTagBytes, 0), {}),
                 CryptoError);
}

}  // namespace anvil::crypto
