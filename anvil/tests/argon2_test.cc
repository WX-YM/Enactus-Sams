// The reusable Argon2 primitive (anvil/crypto/argon2.h).

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "anvil/crypto/argon2.h"
#include "anvil/crypto/errors.h"

namespace anvil::crypto {
namespace {

[[nodiscard]] std::vector<std::uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

[[nodiscard]] std::string hex_of(std::span<const std::uint8_t> data) {
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

// --- 1: RFC 9106 §5.3, the Argon2id known-answer test ----------------------
//
// The one vector that exercises `secret` and `associated_data`, which is the
// reason argon2_hash_raw takes the full argon2_context shape rather than the
// narrower *_hash_raw entry points: neither auth::PasswordHasher nor the
// prehash client stage uses either, but a known-answer test that cannot
// express them is not testing the wiring those two fields depend on.
TEST(Argon2Raw, MatchesRfc9106Argon2idVector) {
    const std::vector<std::uint8_t> password(32, 0x01);
    const std::vector<std::uint8_t> salt(16, 0x02);
    const std::vector<std::uint8_t> secret(8, 0x03);
    const std::vector<std::uint8_t> ad(12, 0x04);

    const Argon2Params params{.memory_kib = 32, .iterations = 3, .parallelism = 4};
    std::array<std::uint8_t, 32> tag{};
    argon2_hash_raw(Argon2Type::Argon2id, kArgon2Version13, params, password, salt, secret, ad,
                    tag);

    EXPECT_EQ(hex_of(tag), "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
}

// --- 2: the same known-answer test for the other two types ----------------
//
// RFC 9106 §5.1 and §5.2 use §5.3's inputs, so the type switch is pinned to an
// exact answer per arm. Asserting only that two outputs DIFFER would pass for a
// switch that called argon2d_ctx twice with the arguments in a different order.
TEST(Argon2Raw, MatchesRfc9106Argon2dAndArgon2iVectors) {
    const std::vector<std::uint8_t> password(32, 0x01);
    const std::vector<std::uint8_t> salt(16, 0x02);
    const std::vector<std::uint8_t> secret(8, 0x03);
    const std::vector<std::uint8_t> ad(12, 0x04);
    const Argon2Params params{.memory_kib = 32, .iterations = 3, .parallelism = 4};

    std::array<std::uint8_t, 32> d_tag{};
    std::array<std::uint8_t, 32> i_tag{};
    argon2_hash_raw(Argon2Type::Argon2d, kArgon2Version13, params, password, salt, secret, ad,
                    d_tag);
    argon2_hash_raw(Argon2Type::Argon2i, kArgon2Version13, params, password, salt, secret, ad,
                    i_tag);

    EXPECT_EQ(hex_of(d_tag), "512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb");
    EXPECT_EQ(hex_of(i_tag), "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8");
}

TEST(Argon2Raw, RejectsParametersBelowLibargon2sOwnFloor) {
    const std::vector<std::uint8_t> password(8, 0x01);
    const std::vector<std::uint8_t> salt(16, 0x02);
    // t_cost = 0 is below ARGON2_MIN_TIME.
    const Argon2Params params{.memory_kib = 8192, .iterations = 0, .parallelism = 1};
    std::array<std::uint8_t, 32> out{};
    EXPECT_THROW(argon2_hash_raw(Argon2Type::Argon2id, kArgon2Version13, params, password, salt,
                                 {}, {}, out),
                CryptoError);
}

// --- 3: the PHC-encoded form -------------------------------------------------

TEST(Argon2Encoded, RoundTripsThroughHashAndVerify) {
    const std::vector<std::uint8_t> password = {'c', 'o', 'r', 'r', 'e', 'c', 't'};
    const std::array<std::uint8_t, 16> salt{};
    const Argon2Params params{.memory_kib = 8192, .iterations = 2, .parallelism = 1};

    const std::string encoded = argon2id_hash_encoded(params, password, salt, 32);
    EXPECT_EQ(encoded.find("$argon2id$"), 0U);

    EXPECT_EQ(argon2id_verify_encoded(encoded, password), VerifyOutcome::Match);
    const std::vector<std::uint8_t> wrong = {'w', 'r', 'o', 'n', 'g'};
    EXPECT_EQ(argon2id_verify_encoded(encoded, wrong), VerifyOutcome::Mismatch);
}

TEST(Argon2Encoded, EmptyAndGarbageAreMalformed) {
    EXPECT_EQ(argon2id_verify_encoded("", {}), VerifyOutcome::Malformed);
    const std::vector<std::uint8_t> password = {'x'};
    EXPECT_EQ(argon2id_verify_encoded("not-a-hash", password), VerifyOutcome::Malformed);
    EXPECT_EQ(argon2id_verify_encoded("$argon2id$garbage", password), VerifyOutcome::Malformed);
}

TEST(Argon2Encoded, ParsesParameters) {
    const std::vector<std::uint8_t> password = {'p'};
    const std::array<std::uint8_t, 16> salt{};
    const Argon2Params params{.memory_kib = 16384, .iterations = 3, .parallelism = 2};
    const std::string encoded = argon2id_hash_encoded(params, password, salt, 32);

    const std::optional<Argon2Params> parsed = argon2id_parse_params(encoded);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, params);

    EXPECT_FALSE(argon2id_parse_params("$2a$10$abcdef").has_value()) << "bcrypt is not accepted";
    EXPECT_FALSE(argon2id_parse_params("").has_value());
}

// --- 4: the contract's own golden vector, salt "anvil-prehash-v1" ----------
//
// docs/*/prehash-contract.md §4, m=64 MiB t=3 p=1 row: this is the vector the
// prehash module's enrol-from-plaintext path is checked against too, but it
// belongs here first because it is a fact about this primitive, computed with
// the reference libargon2 CLI.
TEST(Argon2Raw, MatchesThePrehashContractsGoldenVector) {
    const std::vector<std::uint8_t> password =
        hex_to_bytes("70c3a4737377c3b6726420d983d984d985d8a920f09f9491");
    const std::string salt = "anvil-prehash-v1";
    const std::vector<std::uint8_t> salt_bytes(salt.begin(), salt.end());
    ASSERT_EQ(salt_bytes.size(), 16U);

    const Argon2Params params{.memory_kib = 65536, .iterations = 3, .parallelism = 1};
    std::array<std::uint8_t, 32> k{};
    argon2_hash_raw(Argon2Type::Argon2id, kArgon2Version13, params, password, salt_bytes, {}, {},
                    k);

    EXPECT_EQ(hex_of(k), "68b44374338bc1f32e154d12fed4f7897e5d046d730815aaab695ef09c1f583a");
}

}  // namespace anvil::crypto
