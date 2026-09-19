#include "anvil/input/schema.h"

namespace anvil::input {

const JsonValue* ObjectBinder::claim(std::string_view key) noexcept {
    if (object_ == nullptr) { return nullptr; }
    const std::span<const JsonMember> members = object_->members();
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (members[i].key == key) {
            claimed_.set(i);
            return &members[i].value;
        }
    }
    return nullptr;
}

// --- required --------------------------------------------------------------

Reason ObjectBinder::text(std::string_view key, const TextRules& rules,
                          std::string_view& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Required; }

    // Type before value: an object, array or number where a string belongs is
    // rejected here, without anything looking at what it contains.
    const std::optional<std::string_view> text = value->as_string();
    if (!text.has_value()) { return Reason::BadFormat; }

    const Reason reason = check_text(*text, rules);
    if (!is_ok(reason)) { return reason; }

    out = *text;
    return Reason::Ok;
}

Reason ObjectBinder::boolean(std::string_view key, bool& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Required; }

    const std::optional<bool> flag = value->as_bool();
    if (!flag.has_value()) { return Reason::BadFormat; }   // "true" is not true
    out = *flag;
    return Reason::Ok;
}

Reason ObjectBinder::integer(std::string_view key, std::int64_t min, std::int64_t max,
                             std::int64_t& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Required; }
    if (!value->is_number()) { return Reason::BadFormat; }

    // as_int64 rejects 1.5 and 1e3 rather than rounding them, and reports the
    // overflow that makes 1e309 a rejection instead of an infinity.
    const std::optional<std::int64_t> number = value->as_int64();
    if (!number.has_value()) { return Reason::BadFormat; }
    if (*number < min || *number > max) { return Reason::OutOfRange; }

    out = *number;
    return Reason::Ok;
}

Reason ObjectBinder::uuid(std::string_view key, Uuid& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Required; }

    const std::optional<std::string_view> text = value->as_string();
    if (!text.has_value()) { return Reason::BadFormat; }
    return parse_uuid(*text, out);
}

Reason ObjectBinder::timestamp(std::string_view key, std::int64_t& out_ms) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Required; }

    const std::optional<std::string_view> text = value->as_string();
    if (!text.has_value()) { return Reason::BadFormat; }
    return parse_timestamp(*text, out_ms);
}

// --- optional --------------------------------------------------------------

Reason ObjectBinder::optional_text(std::string_view key, const TextRules& rules,
                                   std::optional<std::string_view>& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Ok; }
    // An explicit null is "absent", which is what a client sends to mean "leave
    // this alone". A wrong TYPE is still a failure.
    if (value->is_null()) { return Reason::Ok; }

    const std::optional<std::string_view> text = value->as_string();
    if (!text.has_value()) { return Reason::BadFormat; }

    const Reason reason = check_text(*text, rules);
    if (!is_ok(reason)) { return reason; }

    out = *text;
    return Reason::Ok;
}

Reason ObjectBinder::optional_boolean(std::string_view key, std::optional<bool>& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr || value->is_null()) { return Reason::Ok; }

    const std::optional<bool> flag = value->as_bool();
    if (!flag.has_value()) { return Reason::BadFormat; }
    out = *flag;
    return Reason::Ok;
}

Reason ObjectBinder::optional_integer(std::string_view key, std::int64_t min, std::int64_t max,
                                      std::optional<std::int64_t>& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr || value->is_null()) { return Reason::Ok; }
    if (!value->is_number()) { return Reason::BadFormat; }

    const std::optional<std::int64_t> number = value->as_int64();
    if (!number.has_value()) { return Reason::BadFormat; }
    if (*number < min || *number > max) { return Reason::OutOfRange; }

    out = *number;
    return Reason::Ok;
}

Reason ObjectBinder::optional_nullable_timestamp(
    std::string_view key, std::optional<std::optional<std::int64_t>>& out) noexcept {
    const JsonValue* value = claim(key);
    // Absent. `out` is left alone rather than set, which is what the caller
    // reads as "do not touch this field".
    if (value == nullptr) { return Reason::Ok; }
    if (value->is_null()) {
        out = std::optional<std::int64_t>{};
        return Reason::Ok;
    }

    const std::optional<std::string_view> text = value->as_string();
    if (!text.has_value()) { return Reason::BadFormat; }
    std::int64_t at_ms = 0;
    const Reason parsed = parse_timestamp(*text, at_ms);
    if (!is_ok(parsed)) { return parsed; }
    out = std::optional<std::int64_t>{at_ms};
    return Reason::Ok;
}

Reason ObjectBinder::optional_nullable_integer(
    std::string_view key, std::int64_t min, std::int64_t max,
    std::optional<std::optional<std::int64_t>>& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Ok; }
    if (value->is_null()) {
        out = std::optional<std::int64_t>{};
        return Reason::Ok;
    }
    if (!value->is_number()) { return Reason::BadFormat; }

    const std::optional<std::int64_t> number = value->as_int64();
    if (!number.has_value()) { return Reason::BadFormat; }
    if (*number < min || *number > max) { return Reason::OutOfRange; }
    out = std::optional<std::int64_t>{*number};
    return Reason::Ok;
}

Reason ObjectBinder::optional_nullable_text(
    std::string_view key, const TextRules& rules,
    std::optional<std::optional<std::string_view>>& out) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) { return Reason::Ok; }
    if (value->is_null()) {
        out = std::optional<std::string_view>{};
        return Reason::Ok;
    }

    const std::optional<std::string_view> text = value->as_string();
    if (!text.has_value()) { return Reason::BadFormat; }
    const Reason reason = check_text(*text, rules);
    if (!is_ok(reason)) { return reason; }
    out = std::optional<std::string_view>{*text};
    return Reason::Ok;
}

// --- structure -------------------------------------------------------------

const JsonValue* ObjectBinder::object(std::string_view key) noexcept {
    const JsonValue* value = claim(key);
    return (value != nullptr && value->is_object()) ? value : nullptr;
}

const JsonValue* ObjectBinder::array(std::string_view key, std::size_t max_elements,
                                     Reason& reason) noexcept {
    const JsonValue* value = claim(key);
    if (value == nullptr) {
        reason = Reason::Required;
        return nullptr;
    }
    if (!value->is_array()) {
        reason = Reason::BadFormat;
        return nullptr;
    }
    // The parser's global array cap keeps a body cheap; this is the field's own
    // limit, which is usually far smaller (media ids per note: 32).
    if (value->elements().size() > max_elements) {
        reason = Reason::TooLong;
        return nullptr;
    }
    reason = Reason::Ok;
    return value;
}

const JsonValue* ObjectBinder::optional_array(std::string_view key, std::size_t max_elements,
                                              Reason& reason) noexcept {
    const JsonValue* value = array(key, max_elements, reason);
    // The ONLY difference from `array`, and the whole point of the overload:
    // absent is not a failure. A wrong type and an over-long array still are.
    if (value == nullptr && reason == Reason::Required) { reason = Reason::Ok; }
    return value;
}

std::optional<FieldError> ObjectBinder::finish() const noexcept {
    if (object_ == nullptr) { return FieldError{{}, Reason::BadFormat}; }

    const std::span<const JsonMember> members = object_->members();
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (!claimed_.test(i)) {
            // No key name in the error, by design. See the header.
            return FieldError{{}, Reason::NotAllowed};
        }
    }
    return std::nullopt;
}

}  // namespace anvil::input
