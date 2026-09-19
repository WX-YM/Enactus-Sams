#pragma once

// Schema binding: JSON in, a plain C++ struct out.
//
// This is the structural defence against NoSQL injection, and it is structural
// rather than defensive. Repositories build BSON from struct fields with typed
// appends; nothing downstream ever holds a JSON node. There is therefore no
// code path along which `{"email": {"$gt": ""}}` can reach a query builder — it
// fails at bind time as "not a string", before any value is examined and before
// any BSON exists.
//
// Two rules the binder enforces that hand-written extraction always eventually
// forgets:
//
//   1. An UNKNOWN field is an error, never silently dropped. Silently ignoring
//      an unrecognised key is how a client discovers that `is_admin` is
//      accepted somewhere else in the system, and how a renamed field fails
//      silently instead of loudly.
//   2. finish() must be called, and it is [[nodiscard]], so forgetting rule 1
//      is a compiler warning — and warnings are errors in this build.

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/input/fields.h"
#include "anvil/input/json.h"

namespace anvil::input {

class ObjectBinder final {
public:
    // A non-object root fails every subsequent bind, so a caller that ignores
    // the type here still cannot read a value out of an array or a string.
    explicit ObjectBinder(const JsonValue& value) noexcept
        : object_{value.is_object() ? &value : nullptr} {}

    [[nodiscard]] bool is_object() const noexcept { return object_ != nullptr; }

    // --- required fields --------------------------------------------------
    [[nodiscard]] Reason text(std::string_view key, const TextRules& rules,
                              std::string_view& out) noexcept;
    [[nodiscard]] Reason boolean(std::string_view key, bool& out) noexcept;
    [[nodiscard]] Reason integer(std::string_view key, std::int64_t min, std::int64_t max,
                                 std::int64_t& out) noexcept;
    [[nodiscard]] Reason uuid(std::string_view key, Uuid& out) noexcept;
    [[nodiscard]] Reason timestamp(std::string_view key, std::int64_t& out_ms) noexcept;

    // --- optional fields --------------------------------------------------
    // Absent leaves `out` untouched and returns Ok. A field that is PRESENT but
    // of the wrong type still fails: "optional" is about presence, never about
    // type discipline.
    [[nodiscard]] Reason optional_text(std::string_view key, const TextRules& rules,
                                       std::optional<std::string_view>& out) noexcept;
    [[nodiscard]] Reason optional_boolean(std::string_view key,
                                          std::optional<bool>& out) noexcept;
    [[nodiscard]] Reason optional_integer(std::string_view key, std::int64_t min,
                                          std::int64_t max,
                                          std::optional<std::int64_t>& out) noexcept;

    // Three-state, and the third state is why this is not optional_timestamp.
    //
    // A partial edit has to distinguish "do not touch this field" from "clear
    // this field", and one optional cannot carry both: absent and null would
    // collapse into the same answer. So the outer optional is presence and the
    // inner is the value —
    //
    //     key absent          out stays nullopt        leave the field alone
    //     key present, null   out = {nullopt}          clear the field
    //     key present, value  out = {ms}               set the field
    //
    // Every other optional_ binder here treats an explicit null as absence,
    // which is right for a create and wrong for an edit. This is the shape an
    // edit needs.
    [[nodiscard]] Reason optional_nullable_timestamp(
        std::string_view key, std::optional<std::optional<std::int64_t>>& out) noexcept;

    // The same three states for a number and for a string. A limit has to be
    // clearable: "no limit" and "not part of this edit" are different
    // intentions, and a binder that cannot tell them apart can set a cap but
    // never lift one.
    [[nodiscard]] Reason optional_nullable_integer(
        std::string_view key, std::int64_t min, std::int64_t max,
        std::optional<std::optional<std::int64_t>>& out) noexcept;
    [[nodiscard]] Reason optional_nullable_text(
        std::string_view key, const TextRules& rules,
        std::optional<std::optional<std::string_view>>& out) noexcept;

    // --- structure --------------------------------------------------------
    // Marks the key as known and returns the child, so a nested object is bound
    // by its own ObjectBinder — which enforces the unknown-field rule at every
    // level rather than only at the top.
    [[nodiscard]] const JsonValue* object(std::string_view key) noexcept;
    // REQUIRED: an absent key reports `Required`, so a caller that only tests
    // `is_ok(reason)` refuses the body. That is right where the array must be
    // there — `week`, `topics`, `fields` — and wrong everywhere else.
    [[nodiscard]] const JsonValue* array(std::string_view key, std::size_t max_elements,
                                         Reason& reason) noexcept;
    // OPTIONAL: an absent key reports `Ok` and returns nullptr, which is what
    // every other `optional_*` above already does. The asymmetry between those
    // and `array` is not a detail — it is the direct cause of four separately
    // shipped defects, each one a route refusing a perfectly ordinary body with
    // `{"<key>":"INVALID"}` because the key was simply not there. Reach for this
    // whenever the code after the call reads `if (value != nullptr)`, because
    // that branch says the absence is legal.
    [[nodiscard]] const JsonValue* optional_array(std::string_view key,
                                                  std::size_t max_elements,
                                                  Reason& reason) noexcept;

    template <typename E, std::size_t N>
    [[nodiscard]] Reason enumeration(std::string_view key,
                                     const std::array<EnumEntry<E>, N>& table, E& out) noexcept {
        std::string_view raw;
        const Reason reason = text(key, kIdentifierRules, raw);
        if (!is_ok(reason)) { return reason; }
        return parse_enum(raw, table, out);
    }

    // Every key the caller never asked for. Returns a FieldError whose `field`
    // is EMPTY: naming the offending key would echo client-controlled bytes
    // into a response and a log line. The client knows what it
    // sent; the server says only that something was not accepted.
    [[nodiscard]] std::optional<FieldError> finish() const noexcept;

private:
    [[nodiscard]] const JsonValue* claim(std::string_view key) noexcept;

    const JsonValue* object_;
    // One bit per member, indexed by position. max_keys is 64 (json.h), so the
    // whole consumed-set is one register.
    std::bitset<64>  claimed_{};
};

}  // namespace anvil::input
