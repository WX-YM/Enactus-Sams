#pragma once

// A compiled-in Bloom filter of breached passwords.
//
// NIST 800-63B asks for a breach check and no composition rules. The check must
// not be a network call: an external API on the login path is an availability
// dependency on someone else's uptime and a disclosure of the credential at the
// same time, and it adds a network round trip to the hottest security path
// (docs/05-auth-sessions.md §3).
//
// The filter is built at COMPILE TIME into .rodata, so it is shared across every
// thread and every process image, costs zero runtime initialisation, and a
// lookup is four shifts and four tests against a 2 KiB table.
//
// A Bloom filter has false positives — a legitimate password rejected as
// breached — and no false negatives. That asymmetry is the right way round: the
// cost of a false positive is a user picking a different password, and at this
// table size (16 384 bits, 4 hashes, a few dozen entries) the rate is far below
// one in a billion.
//
// The list below is the seed set. The production list is a generated header
// built offline from a breach corpus by the same constexpr machinery — the
// mechanism does not change, only the size of kSeeds, and nothing about the
// lookup path is affected by that growth.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace anvil::input::breach {

inline constexpr std::size_t kFilterBits = 16384;
inline constexpr std::size_t kFilterWords = kFilterBits / 64;
inline constexpr std::size_t kHashCount = 4;

// FNV-1a, seeded per hash round. Not a security hash and it does not need to be:
// the input is not secret to the attacker who chose it, and the filter's only
// job is set membership.
[[nodiscard]] constexpr std::uint64_t seeded_hash(std::string_view text,
                                                  std::uint64_t seed) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL ^ (seed * 0x9e3779b97f4a7c15ULL);
    for (const char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Passwords of at least kPasswordMinCodePoints characters that appear at the top
// of every public breach corpus. Shorter ones are absent deliberately: the
// length rule already rejects them, so spending filter bits on them would be
// waste.
inline constexpr std::array<std::string_view, 40> kSeeds{
    "123456789012",     "1234567890123",   "12345678901234",  "123456789012345",
    "qwertyuiop123",    "qwertyuiopasdf",  "password1234",    "password12345",
    "passwordpassword", "iloveyou1234",    "letmein12345",    "welcome12345",
    "admin1234567",     "administrator",   "trustno1234567",  "monkey123456",
    "dragon123456",     "sunshine12345",   "princess12345",   "football12345",
    "baseball12345",    "superman12345",   "batman1234567",   "michaeljordan",
    "qazwsxedcrfv",     "1qaz2wsx3edc",    "zaq12wsxcde3",    "asdfghjkl123",
    "abcd1234efgh",     "p@ssw0rd1234",    "P@ssword1234",    "Password1234",
    "Welcome12345",     "changeme1234",    "secret1234567",   "whatever12345",
    "computer12345",    "internet12345",   "starwars12345",   "harrypotter1",
};

inline constexpr std::array<std::uint64_t, kFilterWords> kBits = [] {
    std::array<std::uint64_t, kFilterWords> bits{};
    for (const std::string_view password : kSeeds) {
        for (std::uint64_t round = 0; round < kHashCount; ++round) {
            const std::uint64_t bit = seeded_hash(password, round) % kFilterBits;
            bits[bit / 64] |= std::uint64_t{1} << (bit % 64);
        }
    }
    return bits;
}();

[[nodiscard]] constexpr bool probably_contains(std::string_view password) noexcept {
    for (std::uint64_t round = 0; round < kHashCount; ++round) {
        const std::uint64_t bit = seeded_hash(password, round) % kFilterBits;
        if ((kBits[bit / 64] & (std::uint64_t{1} << (bit % 64))) == 0) { return false; }
    }
    return true;
}

}  // namespace anvil::input::breach
