#pragma once

// The vocabulary an application needs to declare its audit actions.
//
// Same shape and same reason as anvil/timer/job_spec.h and
// anvil/identity/capability_spec.h: anvil ships the sink, the buffer, the
// shedding policy and the append-only collection; WHAT gets audited is a list of
// an application's own operations and belongs to it.
//
// The table arrives as a `std::span` handed to the service, because nothing
// anvil compiles is sized from it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace anvil::audit {

// What may be lost, and in what order, when the sink cannot hold everything.
//
// The distinction is the whole of the shedding policy, and getting it backwards
// is not a theoretical failure: a sink that drops the ARRIVING row fills with
// identical denials under flood and then discards the permission change written
// in the middle of them. A denial under flood is one of a million rows that say
// the same thing; a mutation is the only copy of what somebody did.
enum class AuditClass : std::uint8_t {
    // The only copy of what somebody did. Never compressible, never dropped
    // while anything else can go.
    Change = 0,
    // One of a million rows that say the same thing. Compressible by folding
    // consecutive identical events into a repeat count, and the first thing
    // discarded when the buffer cannot hold everything.
    Traffic = 1,
};

// One audit action. Ordered largest-alignment-first.
struct AuditActionSpec final {
    // The wire vocabulary, and the vocabulary of a filter over this collection.
    // NEVER translated, exactly as a permission name is not: an investigator
    // comparing a screen against a server log needs the same word on both, and a
    // localised action name makes the two impossible to line up — which is the
    // one thing such a screen exists to do.
    std::string_view name;   // 16

    // STORED as int32, and read back by dashboards long after the enum grew.
    // APPEND ONLY: never renumber, never reuse a retired value. A row written
    // four hundred days ago carries the numbering of the build that wrote it.
    std::int32_t     value;  //  4

    AuditClass       cls;    //  1
};

static_assert(sizeof(AuditActionSpec) == sizeof(std::string_view) + sizeof(std::int64_t),
              "AuditActionSpec must not grow padding");

// A stored action value, in a type of its own so a function taking one cannot be
// handed an error code by a caller whose arguments went in the wrong order.
//
// Deliberately not validated at construction — the table is a runtime span — and
// it does not need to be: the buffer classifies through the table, and a value
// the table does not know is treated as a Change, which is the safe direction.
class AuditAction final {
public:
    AuditAction() = delete;

    template <typename Enumerator>
    [[nodiscard]] static constexpr AuditAction of(Enumerator value) noexcept {
        static_assert(std::is_enum_v<Enumerator>,
                      "AuditAction::of takes the application's action enumerator");
        return AuditAction{static_cast<std::int32_t>(value)};
    }

    [[nodiscard]] static constexpr AuditAction from_stored(std::int32_t value) noexcept {
        return AuditAction{value};
    }

    [[nodiscard]] constexpr std::int32_t stored() const noexcept { return value_; }
    [[nodiscard]] constexpr bool operator==(const AuditAction&) const noexcept = default;

private:
    explicit constexpr AuditAction(std::int32_t value) noexcept : value_{value} {}

    std::int32_t value_;
};

static_assert(sizeof(AuditAction) == sizeof(std::int32_t));
static_assert(std::is_trivially_copyable_v<AuditAction>);

[[nodiscard]] constexpr const AuditActionSpec* audit_spec_of(
    std::span<const AuditActionSpec> table, AuditAction action) noexcept {
    for (const AuditActionSpec& spec : table) {
        if (spec.value == action.stored()) { return &spec; }
    }
    return nullptr;
}

// Empty for a value this build does not declare — which, unlike a permission
// bit, means a row written by a NEWER process. Historical rows outlive the code
// that wrote them: retention here is measured in hundreds of days, which is
// longer than any deploy cycle, so a reader must be able to say "I do not know
// this action" rather than invent a plausible name for it.
[[nodiscard]] constexpr std::string_view audit_action_name(
    std::span<const AuditActionSpec> table, AuditAction action) noexcept {
    // A direct scan rather than audit_spec_of() plus a null test. The two say
    // the same thing, but a pointer derived from a table and compared against
    // nullptr is not something every compiler will fold in a constant
    // expression — and an application asserting its own table at compile time is
    // exactly the caller that needs this to be constexpr.
    for (const AuditActionSpec& spec : table) {
        if (spec.value == action.stored()) { return spec.name; }
    }
    return {};
}

[[nodiscard]] constexpr std::optional<AuditAction> audit_action_from_name(
    std::span<const AuditActionSpec> table, std::string_view name) noexcept {
    if (name.empty()) { return std::nullopt; }
    for (const AuditActionSpec& spec : table) {
        if (spec.name == name) { return AuditAction::from_stored(spec.value); }
    }
    return std::nullopt;
}

// CHANGE for an action this build does not declare, and that default is the
// point: an unknown row is treated as the only copy of something, so a rolling
// deploy that introduces a new action cannot cause the older process to shed it
// as compressible traffic.
[[nodiscard]] constexpr AuditClass audit_class_of(std::span<const AuditActionSpec> table,
                                                  AuditAction action) noexcept {
    for (const AuditActionSpec& spec : table) {
        if (spec.value == action.stored()) { return spec.cls; }
    }
    return AuditClass::Change;
}

// A malformed table is a build error. A duplicate value makes the second entry
// unreachable by name; a duplicate name makes a filter ambiguous; a zero or
// negative value collides with the reserved "no action".
[[nodiscard]] constexpr bool audit_table_is_well_formed(
    std::span<const AuditActionSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].name.empty()) { return false; }
        if (table[i].value <= 0) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (table[j].value == table[i].value) { return false; }
            if (table[j].name == table[i].name) { return false; }
        }
    }
    return true;
}

}  // namespace anvil::audit
