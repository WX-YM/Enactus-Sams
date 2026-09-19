#pragma once

// An application's permission vocabulary, as one constexpr object.
//
// anvil owns the MECHANISM — a 128-bit set checked with one AND — and knows
// nothing about which permissions exist. An application declares its own enum and
// a table naming each bit, and hands both over as a PermCatalogue
// (docs/01-seams.md §1).
//
// A std::span rather than an std::array<N>: the catalogue is 16 bytes whatever
// the table's size, the table is not copied into it, and the type carries no N
// that every signature would then have to spell.
//
// Why a constexpr object and not a traits specialisation or an extern:
//
//   - PermTraits<App> would thread a tag type through every signature that reads
//     a name, for nothing a plain object does not already give.
//   - `extern const std::span<const PermName>` is the trap. An extern declaration
//     is NOT usable in a constant expression, so every static_assert below and
//     every compile-time name lookup would silently stop being one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "anvil/core/perm_set.h"

namespace anvil {

// One declared permission bit and the name it answers to.
//
// The name is NEVER translated. An investigator comparing a staff screen against
// a server log needs the same word on both, and a localised permission name makes
// those two things impossible to line up.
struct PermName final {
    std::string_view name;  // 16
    std::uint8_t     bit;   //  1
};

// Members are ordered largest-alignment-first, so the array packs with no
// interior padding.
static_assert(sizeof(PermName) == sizeof(std::string_view) + sizeof(std::size_t),
              "PermName must not grow padding");

class PermCatalogue final {
public:
    constexpr explicit PermCatalogue(std::span<const PermName> names) noexcept
        : names_{names} {}

    [[nodiscard]] constexpr std::size_t size() const noexcept { return names_.size(); }
    [[nodiscard]] constexpr std::span<const PermName> names() const noexcept { return names_; }

    // Empty for a bit no permission declares.
    //
    // The reserved gaps between an application's blocks are not permissions the
    // server understands, and inventing a plausible name for one would put a
    // control on a staff screen that authorises nothing. Returning nothing is the
    // honest answer.
    //
    // Linear: a permission table is tens of entries, and a linear scan over one
    // cache line beats the hash a map would compute.
    [[nodiscard]] constexpr std::string_view name_for_bit(std::size_t bit) const noexcept {
        for (const PermName& entry : names_) {
            if (entry.bit == bit) { return entry.name; }
        }
        return {};
    }

    // nullopt for a name nobody declares. A request naming one is a request
    // against a different server, and guessing which bit was meant is how a typo
    // becomes a grant.
    [[nodiscard]] constexpr std::optional<std::uint8_t> bit_for_name(
        std::string_view name) const noexcept {
        if (name.empty()) { return std::nullopt; }
        for (const PermName& entry : names_) {
            if (entry.name == name) { return entry.bit; }
        }
        return std::nullopt;
    }

    // Visits the name of every DECLARED bit in `held`, in BIT order.
    //
    // Bit order rather than table order so two responses for the same holder are
    // byte-identical and a client can diff them. Table order would make the
    // response depend on how the application happened to write its array.
    template <typename Visitor>
    constexpr void for_each_name(const PermSet& held, Visitor&& visit) const {
        for (std::size_t bit = 0; bit < PermSet::kBits; ++bit) {
            if (!held.test(bit)) { continue; }
            const std::string_view name = name_for_bit(bit);
            if (!name.empty()) { visit(name); }
        }
    }

    // Every declared bit, built FROM THE NAMES rather than from ~PermSet{}.
    //
    // The difference matters: ~PermSet{} sets the reserved gaps too, and a
    // "grant everything" built that way hands out bits the server has no meaning
    // for — which is indistinguishable from a grant until somebody adds a
    // permission at one of those indices.
    [[nodiscard]] constexpr PermSet all() const noexcept {
        PermSet mask{};
        for (const PermName& entry : names_) { mask.set(entry.bit); }
        return mask;
    }

    // static_assert on this in the application's header. Every condition below is
    // a real mistake that would otherwise reach production as a permission that
    // silently does nothing, or as two names for one authority.
    [[nodiscard]] constexpr bool well_formed() const noexcept {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (names_[i].name.empty()) { return false; }
            if (names_[i].bit >= PermSet::kBits) { return false; }
            for (std::size_t j = 0; j < i; ++j) {
                if (names_[j].bit == names_[i].bit) { return false; }
                if (names_[j].name == names_[i].name) { return false; }
            }
        }
        return true;
    }

private:
    std::span<const PermName> names_;
};

// Route permission requirements are built with this at compile time, so they live
// in .rodata and cost nothing at runtime:
//
//     inline constexpr PermSet kContentWrite = perm_mask(Perm::ContentWrite);
//
// Deliberately generic over the enumerator type: it static_casts whatever it is
// handed, so it never has to name an application's enum and an application's
// route table stays constexpr.
template <typename... Bits>
[[nodiscard]] constexpr PermSet perm_mask(Bits... bits) noexcept {
    PermSet mask{};
    (mask.set(static_cast<std::size_t>(bits)), ...);
    return mask;
}

[[nodiscard]] constexpr bool has_all(const PermSet& held, const PermSet& required) noexcept {
    return held.contains_all(required);
}

}  // namespace anvil
