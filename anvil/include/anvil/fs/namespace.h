#pragma once

// Which API owns a stored object, as a validated one-byte index.
//
// The application declares the list; anvil gives it a type. The point is the
// type: a function taking a namespace STRING from a caller can be handed
// "../section", and a function taking an `Ns` cannot. The segmentation is
// enforced by the type system at zero runtime cost, and there is no traversal to
// defend against because no caller-supplied component reaches a path
// (docs/07-filesystem.md §2).
//
// THE ORDER IS PERSISTED. The index is stored as int32 in the media row, so the
// application's table is append-only: never reorder, never reuse a retired slot.
// This is the same rule permission bits and locales live under.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <string_view>
#include <type_traits>

#include <anvil_app_config.h>

#include "anvil/fs/namespace_spec.h"

namespace anvil::fs {

inline constexpr std::size_t kNsCount = config::kNamespaces.size();

static_assert(kNsCount >= 1, "at least one storage namespace must be declared");
static_assert(kNsCount <= 255, "a namespace is stored and carried as one byte");

// The longest declared directory name. RelPath sizes its buffer from this, so a
// longer namespace name widens the buffer rather than silently overflowing it —
// which is why it is COMPUTED here and not a hand-maintained constant that a new
// entry can outgrow.
inline constexpr std::size_t kMaxNsDirLength = [] {
    std::size_t longest = 0;
    for (const NamespaceSpec& spec : config::kNamespaces) {
        if (spec.dir.size() > longest) { longest = spec.dir.size(); }
    }
    return longest;
}();

static_assert(kMaxNsDirLength > 0, "a namespace directory name may not be empty");

class Ns final {
public:
    // No default constructor. There is no sensible "default namespace" — every
    // stored object belongs to exactly one API — and a default would be the value
    // a forgotten assignment silently lands on.
    Ns() = delete;

    [[nodiscard]] static constexpr std::optional<Ns> from_index(std::size_t index) noexcept {
        if (index >= kNsCount) { return std::nullopt; }
        return Ns{static_cast<std::uint8_t>(index)};
    }

    // The URL segment back to a namespace. Used ONLY where a route pattern is
    // matched against the table — never to build a path, and never with request
    // bytes that have not matched an entry exactly. A miss is a miss, never a
    // fallback.
    [[nodiscard]] static constexpr std::optional<Ns> from_dir(std::string_view dir) noexcept {
        if (dir.empty()) { return std::nullopt; }
        for (std::size_t i = 0; i < kNsCount; ++i) {
            if (config::kNamespaces[i].dir == dir) {
                return Ns{static_cast<std::uint8_t>(i)};
            }
        }
        return std::nullopt;
    }

    // The stored int32 back to a namespace, range-checked. A value outside the
    // table is corruption or a newer writer, and either way it must not become a
    // valid namespace.
    [[nodiscard]] static constexpr std::optional<Ns> from_stored(std::int32_t value) noexcept {
        if (value < 0) { return std::nullopt; }
        return from_index(static_cast<std::size_t>(value));
    }

    // For an application enumerator, in a constant expression.
    //
    //     inline constexpr Ns kSection = Ns::of(MediaNs::Section);
    //
    // An out-of-range enumerator makes this non-constant, so it is a COMPILE
    // error at the declaration rather than a nullopt somebody dereferences.
    template <typename Enumerator>
    [[nodiscard]] static constexpr Ns of(Enumerator value) noexcept {
        static_assert(std::is_enum_v<Enumerator>,
                      "Ns::of takes the application's namespace enumerator");
        return *from_index(static_cast<std::size_t>(value));
    }

    [[nodiscard]] constexpr std::uint8_t index() const noexcept { return index_; }
    [[nodiscard]] constexpr std::int32_t stored() const noexcept { return index_; }
    [[nodiscard]] constexpr std::string_view dir() const noexcept {
        return config::kNamespaces[index_].dir;
    }

    // Which sniffed types this namespace takes. Read from the SAME table the
    // descriptor emits `accepts` from, so the list a client is offered and the
    // list the upload path enforces are one table read twice rather than two
    // lists that agree until somebody edits one.
    [[nodiscard]] constexpr MimeMask accepts() const noexcept {
        return config::kNamespaces[index_].accepts;
    }

    [[nodiscard]] constexpr bool operator==(const Ns&) const noexcept = default;

private:
    explicit constexpr Ns(std::uint8_t index) noexcept : index_{index} {}

    std::uint8_t index_;
};

static_assert(sizeof(Ns) == 1, "a namespace must cost one byte wherever it is stored");
static_assert(std::is_trivially_copyable_v<Ns>);

// Every declared namespace, for the boot-time loop that opens one descriptor per
// directory and the sweeps that walk all of them.
// Built by pack expansion rather than by filling a default-constructed array:
// Ns has no default constructor on purpose, so there is nothing to fill it with.
inline constexpr std::array<Ns, kNsCount> kAllNamespaces =
    []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<Ns, kNsCount>{*Ns::from_index(I)...};
    }(std::make_index_sequence<kNsCount>{});

// --- the width ladder and the role mapping ---------------------------------
//
// Both are the application's: which rungs the pipeline writes, and which rung
// each role resolves to, are facts about its design rather than about anvil. The
// two are one coupled decision, which is why they are declared together and
// checked against each other below.

inline constexpr auto& kVariantWidths = config::kVariantWidths;

[[nodiscard]] constexpr std::uint16_t role_width(Ns ns, MediaRole role) noexcept {
    return config::kRoleWidths[ns.index()][static_cast<std::size_t>(role)];
}

// A function rather than a constant so that a per-namespace default stops being
// one value without a call-site change.
[[nodiscard]] constexpr MediaRole default_role(Ns ns) noexcept {
    (void)ns;
    return kDefaultRole;
}

// Every role width must BE a rung of the ladder. A role pointing at 900 would
// silently resolve to the next rung down for every object in the system, and the
// table would look entirely correct while doing it.
static_assert([] {
    for (const auto& row : config::kRoleWidths) {
        for (const std::uint16_t width : row) {
            bool found = false;
            for (const std::uint16_t rung : config::kVariantWidths) {
                found = found || rung == width;
            }
            if (!found) { return false; }
        }
    }
    return true;
}(), "a role must name a width the image pipeline actually writes");

static_assert(config::kRoleWidths.size() == kNsCount,
              "every declared namespace needs a row in the role-width table");

}  // namespace anvil::fs
