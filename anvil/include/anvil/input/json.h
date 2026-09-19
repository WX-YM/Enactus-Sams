#pragma once

// A JSON parser with hard limits, written for this system rather than adopted.
//
// The requirements are unusual enough that a general-purpose parser would have
// to be wrapped in checks that duplicate half of it (docs/09-lib-inputvalidation
// §2):
//
//   * every limit — bytes, depth, keys, array length, string length — is
//     enforced DURING the parse, not after, so a hostile document is abandoned
//     at the point it goes out of bounds rather than after it is materialised;
//   * duplicate keys are an error. Accepting them is a parser-differential
//     vector: this parser sees the last value, a proxy or logger sees the first;
//   * scratch comes from one arena, so a parse is a bump pointer and a bulk
//     release rather than hundreds of small allocations;
//   * strings that contain no escape are VIEWS into the body, not copies.
//
// Numbers are kept as their source text and converted on demand. Converting
// eagerly to double would silently round a 64-bit id and would lose the
// overflow that `1e309` must be rejected for; keeping the token makes the
// caller state which type it expects, which is the "type before value" rule.
//
// The parser never repairs. There is no lenient mode, no trailing-comma
// tolerance, and no comment support: each of those is a place where two parsers
// disagree about what a document means.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "anvil/input/arena.h"

namespace anvil::input {

enum class JsonType : std::uint8_t { Null, Bool, Number, String, Array, Object };

enum class JsonError : std::uint8_t {
    Ok = 0,
    TooLarge,        // body exceeded the byte cap — checked before anything else
    InvalidUtf8,     // rejected, never repaired
    Malformed,       // anything the grammar does not accept
    DepthExceeded,
    TooManyKeys,
    ArrayTooLong,
    StringTooLong,
    DuplicateKey,
    TrailingData,    // a second value after the first is not one document
};

// The error as a constant, for a SERVER-SIDE log line only.
//
// Never for a response body: which limit a body broke is information about the
// parser, and a caller that learns "DepthExceeded" learns the depth cap. The
// wire answer for every one of these stays the same VALIDATION_FAILED with no
// field (ENGINEERING_RULES.md §5). But a staff write that is refused for a reason nobody
// records is a support ticket with nothing behind it — a rejected body left no
// trace at all, and "publish does nothing" could not be told apart from
// "publish sent something the parser would not read".
[[nodiscard]] constexpr std::string_view json_error_name(JsonError error) noexcept {
    switch (error) {
        case JsonError::Ok:            return "Ok";
        case JsonError::TooLarge:      return "TooLarge";
        case JsonError::InvalidUtf8:   return "InvalidUtf8";
        case JsonError::Malformed:     return "Malformed";
        case JsonError::DepthExceeded: return "DepthExceeded";
        case JsonError::TooManyKeys:   return "TooManyKeys";
        case JsonError::ArrayTooLong:  return "ArrayTooLong";
        case JsonError::StringTooLong: return "StringTooLong";
        case JsonError::DuplicateKey:  return "DuplicateKey";
        case JsonError::TrailingData:  return "TrailingData";
    }
    return "Unknown";
}

struct JsonLimits final {
    // Defaults are the numbers in docs/06-input-validation.md §2. They are members rather than
    // constants so a route with a genuinely larger shape can raise one limit
    // explicitly instead of the whole system living at the maximum.
    std::size_t max_bytes = 262144;
    std::size_t max_string_bytes = 65536;
    std::size_t max_depth = 8;
    std::size_t max_keys = 64;
    std::size_t max_elements = 256;
};

class JsonMember;

// A tag plus the widest payload. Values live in the arena and are never copied
// out of it; the type is trivially copyable so moving one between arena slots
// is a memcpy with no ownership to track.
class JsonValue final {
public:
    constexpr JsonValue() noexcept : elements_{nullptr} {}

    [[nodiscard]] JsonType type() const noexcept { return type_; }

    [[nodiscard]] bool is_null() const noexcept { return type_ == JsonType::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == JsonType::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == JsonType::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == JsonType::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == JsonType::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == JsonType::Object; }

    // Each accessor returns nullopt for the wrong type. There is no coercing
    // accessor and no as_string() on a number: `{"email": {"$gt": ""}}` must
    // fail as "not a string" before any value is looked at.
    [[nodiscard]] std::optional<bool> as_bool() const noexcept;
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept;

    // The number's source text, so the caller decides the target type. Both
    // conversions report overflow instead of saturating or wrapping.
    [[nodiscard]] std::optional<std::string_view> number_text() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> as_int64() const noexcept;
    [[nodiscard]] std::optional<double> as_double() const noexcept;

    [[nodiscard]] std::span<const JsonValue> elements() const noexcept;
    [[nodiscard]] std::span<const JsonMember> members() const noexcept;

    // Object lookup. Linear over at most max_keys entries, which beats a hash
    // map at this size and allocates nothing.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

private:
    friend class JsonParser;

    // 32 bytes. The tag says which arm of the union is live, so an array and an
    // object share one pointer instead of carrying a dead span each — at a few
    // hundred nodes per body that halves the arena the parse needs, which is the
    // difference between fitting the inline buffer and spilling to the heap.
    JsonType         type_{JsonType::Null};
    bool             boolean_{false};
    std::uint32_t    count_{0};
    std::string_view text_{};                  // string contents or number token
    union {
        const JsonValue*  elements_;
        const JsonMember* members_;
    };
};

// Members hold their value inline rather than a pointer to it: one arena
// allocation for the whole object instead of one per field, and lookup returns
// a pointer into that block.
class JsonMember final {
public:
    std::string_view key;
    JsonValue        value;
};

// A parsed document. The root borrows from both `body` and `arena`, so both
// must outlive it — the same lifetime rule as any string_view into a request
// body, and the reason a parsed document must never cross a thread-pool
// boundary without the buffer that backs it (ENGINEERING_RULES.md §2.2).
class JsonDocument final {
public:
    [[nodiscard]] JsonError error() const noexcept { return error_; }
    [[nodiscard]] bool ok() const noexcept { return error_ == JsonError::Ok; }
    [[nodiscard]] const JsonValue& root() const noexcept { return *root_; }

private:
    friend class JsonParser;
    friend JsonDocument parse_json(std::string_view, BodyArena&, const JsonLimits&);

    const JsonValue* root_{nullptr};
    JsonError        error_{JsonError::Malformed};
};

// Order of checks is deliberate: the size cap is enforced before UTF-8
// validation, which is enforced before parsing. A 100 KB body of "aaaa…" costs
// one comparison, not a megabyte of validation (docs/06-input-validation.md §2).
[[nodiscard]] JsonDocument parse_json(std::string_view body, BodyArena& arena,
                                      const JsonLimits& limits = {});

}  // namespace anvil::input
