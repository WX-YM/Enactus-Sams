#pragma once

// The vocabulary an application needs to declare its capability scopes.
//
// Same shape and same reason as anvil/timer/job_spec.h: anvil ships the
// mechanism — mint, store only a digest, consume atomically — and the LIST of
// scopes is the application's, because a scope names one of its operations.
//
// A scope table is only ever looked up, never used to size anything anvil
// compiles, so it arrives as a `std::span` handed to the service rather than
// through the configuration header (docs/01-seams.md).
//
// --- why there is no "general" scope ----------------------------------------
//
// A capability token exists because some operations genuinely need a credential
// redeemed by a LATER, SEPARATE request: an upload slot, a destructive-action
// confirmation, a preview of something not yet published. It does not exist to
// re-tokenise a decision the access filter already made two stack frames up.
//
// An UNSCOPED capability is a bearer token with a replay window as long as its
// lifetime and authority over everything. Every scope names exactly one
// operation, so a token issued for one cannot be presented against another —
// and that is enforced by the scope being part of the consume FILTER rather
// than a check performed on the document that came back.

#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace anvil::identity {

// One capability scope. Ordered largest-alignment-first so the table packs.
struct CapabilityScopeSpec final {
    // The audit vocabulary. NEVER translated, exactly as a permission name is
    // not: an investigator lining a screen up against a server log needs the
    // same word on both.
    std::string_view name;          // 16

    // STORED as int32 in the capability row. APPEND ONLY — never renumber, never
    // reuse a retired value. A renumbering silently reinterprets every token in
    // flight, which for a ten-minute lifetime means a window in which a token
    // issued for one operation authorises another.
    std::int32_t     value;         //  4

    // Whether redemption BURNS the token.
    //
    // Almost everything here is single-use: a confirmation presented twice is a
    // double-spend, and check-then-act on it is the bug the atomic consume
    // exists to remove. The exception is a scope that authorises READS of one
    // thing for as long as that thing lives — a preview credential presented on
    // an origin where no session cookie can arrive. A page is reloaded, so
    // burning it on first use would break the feature it exists for.
    bool             single_use;    //  1
};

static_assert(sizeof(CapabilityScopeSpec) == sizeof(std::string_view) + sizeof(std::int64_t),
              "CapabilityScopeSpec must not grow padding");

// A malformed table is a build error, not a runtime surprise. Each condition is
// a mistake that would otherwise ship: a duplicate value makes the second entry
// unreachable through from_stored, and an empty name makes an audit row
// unreadable.
[[nodiscard]] constexpr bool capability_table_is_well_formed(
    std::span<const CapabilityScopeSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].name.empty()) { return false; }
        // Zero is reserved for "no scope", so a token can never be stored with
        // one and a default-initialised value can never name a real scope.
        if (table[i].value <= 0) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (table[j].value == table[i].value) { return false; }
            if (table[j].name == table[i].name) { return false; }
        }
    }
    return true;
}

// A scope as it travels through anvil: the STORED value, in a type of its own so
// that a function taking a scope cannot be handed an error code or a row count
// by a caller that got its arguments in the wrong order.
//
// Constructed from the application's enumerator. It is deliberately NOT
// validated at construction — the table is a runtime span, so there is nothing
// to validate against here — and it does not need to be: every path that can act
// on one validates against the table it was handed, and an undeclared scope
// therefore mints nothing and consumes nothing.
class CapabilityScope final {
public:
    CapabilityScope() = delete;

    template <typename Enumerator>
    [[nodiscard]] static constexpr CapabilityScope of(Enumerator value) noexcept {
        static_assert(std::is_enum_v<Enumerator>,
                      "CapabilityScope::of takes the application's scope enumerator");
        return CapabilityScope{static_cast<std::int32_t>(value)};
    }

    // The stored int32 back to a scope. Unchecked by itself — see above — so it
    // is paired with spec_of at every site that acts on the result.
    [[nodiscard]] static constexpr CapabilityScope from_stored(std::int32_t value) noexcept {
        return CapabilityScope{value};
    }

    [[nodiscard]] constexpr std::int32_t stored() const noexcept { return value_; }
    [[nodiscard]] constexpr bool operator==(const CapabilityScope&) const noexcept = default;

private:
    explicit constexpr CapabilityScope(std::int32_t value) noexcept : value_{value} {}

    std::int32_t value_;
};

static_assert(sizeof(CapabilityScope) == sizeof(std::int32_t));
static_assert(std::is_trivially_copyable_v<CapabilityScope>);

// nullptr for a value this build does not declare — which, unlike a permission
// bit, can mean a token minted by a NEWER process during a rolling deploy. A
// caller must be able to say "I do not know this scope" and refuse, rather than
// invent a plausible meaning for it.
[[nodiscard]] constexpr const CapabilityScopeSpec* capability_spec_of(
    std::span<const CapabilityScopeSpec> table, CapabilityScope scope) noexcept {
    for (const CapabilityScopeSpec& spec : table) {
        if (spec.value == scope.stored()) { return &spec; }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::string_view capability_scope_name(
    std::span<const CapabilityScopeSpec> table, CapabilityScope scope) noexcept {
    const CapabilityScopeSpec* spec = capability_spec_of(table, scope);
    return spec == nullptr ? std::string_view{} : spec->name;
}

}  // namespace anvil::identity
