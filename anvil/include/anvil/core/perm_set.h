#pragma once

// A 128-bit permission set: 16 bytes, stack-allocated, fully constexpr.
//
// Why not std::bitset<128>, which docs/04-access-control.md originally named:
//
//   1. std::bitset's operations are not constexpr until C++23, so the
//      compile-time route masks that make authorization free would not compile
//      under the C++20 standard anvil targets.
//   2. std::bitset exposes NO portable access to its underlying words.
//      to_ullong() throws above 64 bits, and there is no to_bytes(). Encoding
//      one as a 16-byte BSON BinData would mean testing 128 bits one at a time
//      or round-tripping through to_string() — on a field read on every
//      authorization decision.
//   3. Its byte order is unspecified, so a value written by one build could be
//      read differently by another. Permission bits are persisted; that is not
//      acceptable.
//
// This type fixes the wire format explicitly: little-endian words, bit i lives
// in word i/64 at position i%64, serialised low word first. Bit indices are
// stored in tokens and in the users collection and must never be renumbered.

#include <cstddef>
#include <cstdint>
#include <array>

namespace anvil {

class PermSet final {
public:
    static constexpr std::size_t kBits = 128;
    static constexpr std::size_t kWords = 2;
    static constexpr std::size_t kBytes = 16;

    constexpr PermSet() noexcept : words_{0, 0} {}
    constexpr explicit PermSet(std::uint64_t low, std::uint64_t high) noexcept
        : words_{low, high} {}

    // --- single-bit access ------------------------------------------------
    // Out-of-range indices are a programming error, not a runtime condition:
    // every caller passes a Perm enumerator. They are ignored rather than
    // throwing so the whole type stays noexcept and usable in constant
    // expressions.
    constexpr PermSet& set(std::size_t bit) noexcept {
        if (bit < kBits) { words_[bit / 64] |= (std::uint64_t{1} << (bit % 64)); }
        return *this;
    }

    constexpr PermSet& reset(std::size_t bit) noexcept {
        if (bit < kBits) { words_[bit / 64] &= ~(std::uint64_t{1} << (bit % 64)); }
        return *this;
    }

    [[nodiscard]] constexpr bool test(std::size_t bit) const noexcept {
        return bit < kBits && ((words_[bit / 64] >> (bit % 64)) & 1U) != 0U;
    }

    // --- set algebra ------------------------------------------------------
    [[nodiscard]] constexpr PermSet operator|(const PermSet& other) const noexcept {
        return PermSet{words_[0] | other.words_[0], words_[1] | other.words_[1]};
    }
    [[nodiscard]] constexpr PermSet operator&(const PermSet& other) const noexcept {
        return PermSet{words_[0] & other.words_[0], words_[1] & other.words_[1]};
    }
    [[nodiscard]] constexpr PermSet operator~() const noexcept {
        return PermSet{~words_[0], ~words_[1]};
    }
    constexpr PermSet& operator|=(const PermSet& other) noexcept {
        words_[0] |= other.words_[0];
        words_[1] |= other.words_[1];
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const PermSet& other) const noexcept = default;

    // The authorization check. Two ANDs and two compares, no branch per
    // permission, no allocation — this is what makes the request pipeline's
    // stage 6 free (docs/00-architecture.md §4).
    [[nodiscard]] constexpr bool contains_all(const PermSet& required) const noexcept {
        return (words_[0] & required.words_[0]) == required.words_[0] &&
               (words_[1] & required.words_[1]) == required.words_[1];
    }

    [[nodiscard]] constexpr bool any() const noexcept {
        return (words_[0] | words_[1]) != 0U;
    }
    [[nodiscard]] constexpr bool none() const noexcept { return !any(); }

    [[nodiscard]] constexpr std::size_t count() const noexcept {
        // Not std::popcount: <bit> would be a header dependency for something
        // only used in tests and admin listings, never on the request path.
        std::size_t total = 0;
        for (const std::uint64_t word : words_) {
            for (std::uint64_t w = word; w != 0; w &= (w - 1)) { ++total; }
        }
        return total;
    }

    // --- wire format ------------------------------------------------------
    // Little-endian, low word first. Explicit and byte-exact so a value written
    // today reads identically on any platform and any future build.
    [[nodiscard]] constexpr std::array<std::uint8_t, kBytes> to_bytes() const noexcept {
        std::array<std::uint8_t, kBytes> out{};
        for (std::size_t w = 0; w < kWords; ++w) {
            for (std::size_t b = 0; b < 8; ++b) {
                out[(w * 8) + b] = static_cast<std::uint8_t>((words_[w] >> (b * 8)) & 0xFFU);
            }
        }
        return out;
    }

    [[nodiscard]] static constexpr PermSet from_bytes(
        const std::array<std::uint8_t, kBytes>& bytes) noexcept {
        PermSet out;
        for (std::size_t w = 0; w < kWords; ++w) {
            std::uint64_t word = 0;
            for (std::size_t b = 0; b < 8; ++b) {
                word |= static_cast<std::uint64_t>(bytes[(w * 8) + b]) << (b * 8);
            }
            out.words_[w] = word;
        }
        return out;
    }

    [[nodiscard]] constexpr std::uint64_t word(std::size_t index) const noexcept {
        return words_[index];
    }

private:
    std::array<std::uint64_t, kWords> words_;
};

static_assert(sizeof(PermSet) == 16, "PermSet must be exactly 16 bytes");
static_assert(alignof(PermSet) == 8);

}  // namespace anvil
