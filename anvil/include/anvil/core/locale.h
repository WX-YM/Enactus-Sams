#pragma once

// The application's locale set, resolved at compile time.
//
// This is the one seam that decides anvil's packaging. Localized<N> holds
// std::array<std::string_view, N> inline; it is a member of every sections
// FieldSpec and ImageSpec, it is what read_localized in db/codec.cc has to
// RETURN, and N is the stride of the tier-1 section cache. All of that needs N as
// a literal, and all of it lives inside anvil's own translation units. So anvil
// is a source dependency and the application's table arrives through this include
// (docs/01-seams.md §2, which also records the three alternatives and why each
// one costs either an allocation per localised value or a wasted cache slot per
// unconfigured locale).
//
// anvil/core/locale_spec.h is the other half of the cycle: the application's
// header includes THAT to spell its table, and this one includes the
// application's header to read it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include <anvil_app_config.h>

#include "anvil/core/locale_spec.h"

namespace anvil {

inline constexpr std::size_t kLocaleCount = config::kLocales.size();

static_assert(kLocaleCount >= 1, "at least one locale must be declared");
static_assert(kLocaleCount <= 255,
              "a locale is carried as ONE byte in the access token and in UserContext");
static_assert(config::kDefaultLocale < kLocaleCount, "the default locale must exist");

// One byte, and the ONLY constructors that produce one validate.
//
// That is what lets Localized::get index without a bounds check on the render
// path: an out-of-range Locale is unrepresentable rather than merely unexpected,
// so there is no branch to forget.
class Locale final {
public:
    constexpr Locale() noexcept
        : index_{static_cast<std::uint8_t>(config::kDefaultLocale)} {}

    [[nodiscard]] static constexpr std::optional<Locale> from_index(std::size_t index) noexcept {
        if (index >= kLocaleCount) { return std::nullopt; }
        return Locale{static_cast<std::uint8_t>(index)};
    }

    // The wire and the query string both name a locale by tag. Unknown is nullopt
    // rather than the default: silently answering an unrecognised `?lang=` in
    // English is how a missing translation goes unnoticed for a year.
    [[nodiscard]] static constexpr std::optional<Locale> from_tag(std::string_view tag) noexcept {
        if (tag.empty()) { return std::nullopt; }
        for (std::size_t i = 0; i < kLocaleCount; ++i) {
            if (config::kLocales[i].tag == tag) {
                return Locale{static_cast<std::uint8_t>(i)};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::uint8_t index() const noexcept { return index_; }
    [[nodiscard]] constexpr std::string_view tag() const noexcept {
        return config::kLocales[index_].tag;
    }
    [[nodiscard]] constexpr std::string_view collation() const noexcept {
        return config::kLocales[index_].collation;
    }
    [[nodiscard]] constexpr bool rtl() const noexcept { return config::kLocales[index_].rtl; }

    [[nodiscard]] constexpr bool operator==(const Locale&) const noexcept = default;

private:
    explicit constexpr Locale(std::uint8_t index) noexcept : index_{index} {}

    std::uint8_t index_;
};

static_assert(sizeof(Locale) == 1, "a locale must cost one byte wherever it is stored");
static_assert(std::is_trivially_copyable_v<Locale>,
              "it rides inside UserContext across thread-pool boundaries");

// For the preload, invalidation and cache-eviction loops that would otherwise
// spell a locale list a second time.
inline constexpr std::array<Locale, kLocaleCount> kAllLocales = [] {
    std::array<Locale, kLocaleCount> out{};
    for (std::size_t i = 0; i < kLocaleCount; ++i) {
        out[i] = *Locale::from_index(i);
    }
    return out;
}();

// Localised text, one value per declared locale.
//
// The values are string_views: for content read out of BSON they borrow from the
// document buffer, so a Localized must not outlive it. That is the same lifetime
// rule the JSON parser carries, and the same one that a thread-pool boundary
// breaks (ENGINEERING_RULES.md §2.2).
template <std::size_t N = kLocaleCount>
struct Localized final {
    std::array<std::string_view, N> values;

    // An indexed load. Not a branch, not a hash — and strictly cheaper than the
    // two-locale ternary this replaced.
    [[nodiscard]] constexpr std::string_view get(Locale locale) const noexcept {
        return values[locale.index()];
    }

    // Every locale present AND non-empty.
    //
    // Present-but-empty is the same defect as missing: it renders as a blank
    // heading in that locale and nothing reports it. A missing translation is a
    // validation error, never a silent fallback to another locale — a fallback
    // shows the wrong language to a reader who cannot tell it is wrong.
    [[nodiscard]] constexpr bool complete() const noexcept {
        for (const std::string_view value : values) {
            if (value.empty()) { return false; }
        }
        return true;
    }

    [[nodiscard]] constexpr bool operator==(const Localized&) const noexcept = default;
};

using LocalizedView = Localized<kLocaleCount>;

static_assert(sizeof(LocalizedView) == kLocaleCount * sizeof(std::string_view),
              "Localized must not grow padding");

}  // namespace anvil
