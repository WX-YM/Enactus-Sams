#include "anvil/input/json.h"

#include <charconv>
#include <cstring>
#include <memory_resource>
#include <vector>

#include "anvil/i18n/utf8.h"

namespace anvil::input {
namespace {

constexpr char kQuote = '"';
constexpr char kBackslash = '\\';

[[nodiscard]] constexpr bool is_whitespace(char c) noexcept {
    // The four the grammar allows. Not std::isspace: that is locale-dependent
    // and accepts vertical tab and form feed, which JSON does not.
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

}  // namespace

// --- JsonValue accessors ---------------------------------------------------

std::optional<bool> JsonValue::as_bool() const noexcept {
    if (type_ != JsonType::Bool) { return std::nullopt; }
    return boolean_;
}

std::optional<std::string_view> JsonValue::as_string() const noexcept {
    if (type_ != JsonType::String) { return std::nullopt; }
    return text_;
}

std::optional<std::string_view> JsonValue::number_text() const noexcept {
    if (type_ != JsonType::Number) { return std::nullopt; }
    return text_;
}

std::optional<std::int64_t> JsonValue::as_int64() const noexcept {
    if (type_ != JsonType::Number) { return std::nullopt; }
    std::int64_t out = 0;
    const char* const first = text_.data();
    const char* const last = first + text_.size();
    const std::from_chars_result result = std::from_chars(first, last, out);
    // Partial consumption means the token was a real number but not an integer
    // one ("1.5", "1e3"). That is a type error, not a value to round.
    if (result.ec != std::errc{} || result.ptr != last) { return std::nullopt; }
    return out;
}

std::optional<double> JsonValue::as_double() const noexcept {
    if (type_ != JsonType::Number) { return std::nullopt; }
    double out = 0.0;
    const char* const first = text_.data();
    const char* const last = first + text_.size();
    const std::from_chars_result result = std::from_chars(first, last, out);
    // result_out_of_range is how `1e309` arrives. Reported, never saturated to
    // infinity (docs/06-input-validation.md §2).
    if (result.ec != std::errc{} || result.ptr != last) { return std::nullopt; }
    return out;
}

std::span<const JsonValue> JsonValue::elements() const noexcept {
    if (type_ != JsonType::Array) { return {}; }
    return std::span<const JsonValue>{elements_, count_};
}

std::span<const JsonMember> JsonValue::members() const noexcept {
    if (type_ != JsonType::Object) { return {}; }
    return std::span<const JsonMember>{members_, count_};
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (type_ != JsonType::Object) { return nullptr; }
    for (const JsonMember& member : members()) {
        if (member.key == key) { return &member.value; }
    }
    return nullptr;
}

// --- parser ----------------------------------------------------------------

class JsonParser final {
public:
    JsonParser(std::string_view body, BodyArena& arena, const JsonLimits& limits)
        : body_{body},
          limits_{limits},
          arena_{arena},
          value_stack_{arena.resource()},
          member_stack_{arena.resource()} {}

    [[nodiscard]] JsonDocument run() {
        JsonDocument document;

        const std::span<JsonValue> root = arena_.allocate<JsonValue>(1);
        root[0] = JsonValue{};
        if (!parse_value(0, root[0])) {
            document.error_ = error_;
            return document;
        }
        skip_whitespace();
        if (position_ != body_.size()) {
            document.error_ = JsonError::TrailingData;
            return document;
        }

        document.root_ = root.data();
        document.error_ = JsonError::Ok;
        return document;
    }

private:
    [[nodiscard]] bool at_end() const noexcept { return position_ >= body_.size(); }
    [[nodiscard]] char peek() const noexcept { return body_[position_]; }

    void skip_whitespace() noexcept {
        while (!at_end() && is_whitespace(peek())) { ++position_; }
    }

    // Always returns false, so a failing branch reads `return fail(...)`.
    // First error wins: a depth violation inside a malformed array reports the
    // depth, which is the actionable one.
    [[nodiscard]] bool fail(JsonError error) noexcept {
        if (error_ == JsonError::Ok) { error_ = error; }
        return false;
    }

    [[nodiscard]] bool parse_value(std::size_t depth, JsonValue& out) {
        // >= not >: `depth` is the nesting level being ENTERED, so eight nested
        // containers occupy levels 0..7 and a ninth is over the limit.
        if (depth >= limits_.max_depth) { return fail(JsonError::DepthExceeded); }
        skip_whitespace();
        if (at_end()) { return fail(JsonError::Malformed); }

        switch (peek()) {
            case '{': return parse_object(depth, out);
            case '[': return parse_array(depth, out);
            case kQuote: {
                std::string_view text;
                if (!parse_string(text)) { return false; }
                out.type_ = JsonType::String;
                out.text_ = text;
                return true;
            }
            case 't': return parse_literal("true", JsonType::Bool, true, out);
            case 'f': return parse_literal("false", JsonType::Bool, false, out);
            case 'n': return parse_literal("null", JsonType::Null, false, out);
            default:  return parse_number(out);
        }
    }

    [[nodiscard]] bool parse_literal(std::string_view word, JsonType type, bool boolean,
                                     JsonValue& out) {
        if (body_.substr(position_, word.size()) != word) { return fail(JsonError::Malformed); }
        position_ += word.size();
        out.type_ = type;
        out.boolean_ = boolean;
        return true;
    }

    [[nodiscard]] bool parse_number(JsonValue& out) {
        const std::size_t start = position_;
        if (!at_end() && peek() == '-') { ++position_; }

        // Leading zeros are not valid JSON, and accepting them is another
        // parser-differential ("012" as 12 here and as octal 10 elsewhere).
        if (at_end() || !is_digit(peek())) { return fail(JsonError::Malformed); }
        if (peek() == '0') {
            ++position_;
        } else {
            while (!at_end() && is_digit(peek())) { ++position_; }
        }

        if (!at_end() && peek() == '.') {
            ++position_;
            if (at_end() || !is_digit(peek())) { return fail(JsonError::Malformed); }
            while (!at_end() && is_digit(peek())) { ++position_; }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++position_;
            if (!at_end() && (peek() == '+' || peek() == '-')) { ++position_; }
            if (at_end() || !is_digit(peek())) { return fail(JsonError::Malformed); }
            while (!at_end() && is_digit(peek())) { ++position_; }
        }

        out.type_ = JsonType::Number;
        out.text_ = body_.substr(start, position_ - start);
        return true;
    }

    // Returns the string contents. A string with no escape is a VIEW into the
    // body; only an escaped one is copied into the arena.
    [[nodiscard]] bool parse_string(std::string_view& out) {
        ++position_;   // the opening quote
        const std::size_t start = position_;
        bool escaped = false;

        while (true) {
            if (at_end()) { return fail(JsonError::Malformed); }
            const char c = peek();
            if (c == kQuote) { break; }
            if (c == kBackslash) {
                escaped = true;
                position_ += 2;   // the escape and its argument; validated below
                continue;
            }
            // Unescaped control characters are forbidden by the grammar. This
            // also rejects a raw NUL, which would truncate the value in every
            // C API downstream (docs/03-i18n-utf8.md §2).
            if (static_cast<unsigned char>(c) < 0x20) { return fail(JsonError::Malformed); }
            ++position_;
        }

        const std::size_t length = position_ - start;
        if (length > limits_.max_string_bytes) { return fail(JsonError::StringTooLong); }
        const std::string_view raw = body_.substr(start, length);
        ++position_;   // the closing quote

        if (!escaped) {
            out = raw;
            return true;
        }
        return unescape(raw, out);
    }

    [[nodiscard]] bool unescape(std::string_view raw, std::string_view& out) {
        // The decoded form is never longer than the source: every escape is at
        // least two bytes and produces at most four.
        const std::span<char> buffer = arena_.allocate_chars(raw.size());
        std::size_t written = 0;

        for (std::size_t i = 0; i < raw.size();) {
            const char c = raw[i];
            if (c != kBackslash) {
                buffer[written++] = c;
                ++i;
                continue;
            }
            if (i + 1 >= raw.size()) { return fail(JsonError::Malformed); }
            const char escape = raw[i + 1];
            i += 2;
            switch (escape) {
                case '"':  buffer[written++] = '"'; break;
                case '\\': buffer[written++] = '\\'; break;
                case '/':  buffer[written++] = '/'; break;
                case 'b':  buffer[written++] = '\b'; break;
                case 'f':  buffer[written++] = '\f'; break;
                case 'n':  buffer[written++] = '\n'; break;
                case 'r':  buffer[written++] = '\r'; break;
                case 't':  buffer[written++] = '\t'; break;
                case 'u': {
                    std::uint32_t code_point = 0;
                    if (!read_escaped_code_point(raw, i, code_point)) { return false; }
                    written += encode_utf8(code_point, buffer.subspan(written));
                    break;
                }
                default:
                    return fail(JsonError::Malformed);
            }
        }

        out = std::string_view{buffer.data(), written};
        return true;
    }

    // \uXXXX, including the surrogate pair that encodes anything above the BMP.
    // A lone surrogate is rejected rather than replaced: a \ud800 that becomes
    // U+FFFD is corruption laundered into valid-looking text.
    [[nodiscard]] bool read_escaped_code_point(std::string_view raw, std::size_t& i,
                                               std::uint32_t& out) {
        std::uint32_t first = 0;
        if (!read_hex_quad(raw, i, first)) { return false; }

        if (first >= 0xD800 && first <= 0xDBFF) {
            if (i + 1 >= raw.size() || raw[i] != kBackslash || raw[i + 1] != 'u') {
                return fail(JsonError::Malformed);
            }
            i += 2;
            std::uint32_t second = 0;
            if (!read_hex_quad(raw, i, second)) { return false; }
            if (second < 0xDC00 || second > 0xDFFF) { return fail(JsonError::Malformed); }
            out = 0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U);
            return true;
        }
        if (first >= 0xDC00 && first <= 0xDFFF) {
            return fail(JsonError::Malformed);   // a trailing surrogate on its own
        }
        // \u0000 would embed a NUL, which terminates the value for every C API
        // it later reaches.
        if (first == 0) { return fail(JsonError::Malformed); }
        out = first;
        return true;
    }

    [[nodiscard]] bool read_hex_quad(std::string_view raw, std::size_t& i, std::uint32_t& out) {
        if (i + 4 > raw.size()) { return fail(JsonError::Malformed); }
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const int digit = hex_value(raw[i + k]);
            if (digit < 0) { return fail(JsonError::Malformed); }
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        i += 4;
        out = value;
        return true;
    }

    [[nodiscard]] static std::size_t encode_utf8(std::uint32_t code_point,
                                                 std::span<char> out) noexcept {
        if (code_point < 0x80) {
            out[0] = static_cast<char>(code_point);
            return 1;
        }
        if (code_point < 0x800) {
            out[0] = static_cast<char>(0xC0U | (code_point >> 6U));
            out[1] = static_cast<char>(0x80U | (code_point & 0x3FU));
            return 2;
        }
        if (code_point < 0x10000) {
            out[0] = static_cast<char>(0xE0U | (code_point >> 12U));
            out[1] = static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
            out[2] = static_cast<char>(0x80U | (code_point & 0x3FU));
            return 3;
        }
        out[0] = static_cast<char>(0xF0U | (code_point >> 18U));
        out[1] = static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU));
        out[2] = static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
        out[3] = static_cast<char>(0x80U | (code_point & 0x3FU));
        return 4;
    }

    // Children accumulate on ONE stack shared by the whole parse, not in a
    // vector per container. A vector per container is what a first version does,
    // and against a monotonic arena — which never reuses what it hands out —
    // every growth step and every reserve() is permanently lost. A 6 KB body of
    // small objects exhausted a 32 KiB arena that way; the shared stack holds
    // the deepest concurrent frontier instead, which is orders of magnitude
    // smaller.
    [[nodiscard]] bool parse_array(std::size_t depth, JsonValue& out) {
        ++position_;   // '['
        const std::size_t mark = value_stack_.size();

        skip_whitespace();
        if (at_end()) { return fail(JsonError::Malformed); }
        if (peek() == ']') {
            ++position_;
            store_elements(mark, out);
            return true;
        }

        while (true) {
            if (value_stack_.size() - mark >= limits_.max_elements) {
                return fail(JsonError::ArrayTooLong);
            }
            // Parsed into a local first: a nested container pushes onto the same
            // stack, which may reallocate it, so a reference into it would
            // dangle mid-parse.
            JsonValue element;
            if (!parse_value(depth + 1, element)) { return false; }
            value_stack_.push_back(element);

            skip_whitespace();
            if (at_end()) { return fail(JsonError::Malformed); }
            const char c = peek();
            ++position_;
            if (c == ']') { break; }
            if (c != ',') { return fail(JsonError::Malformed); }
        }

        store_elements(mark, out);
        return true;
    }

    void store_elements(std::size_t mark, JsonValue& out) {
        const std::size_t count = value_stack_.size() - mark;
        const std::span<JsonValue> stored = arena_.allocate<JsonValue>(count);
        for (std::size_t i = 0; i < count; ++i) { stored[i] = value_stack_[mark + i]; }
        value_stack_.resize(mark);

        out.type_ = JsonType::Array;
        out.count_ = static_cast<std::uint32_t>(count);
        out.elements_ = stored.data();
    }

    [[nodiscard]] bool parse_object(std::size_t depth, JsonValue& out) {
        ++position_;   // '{'
        const std::size_t mark = member_stack_.size();

        skip_whitespace();
        if (at_end()) { return fail(JsonError::Malformed); }
        if (peek() == '}') {
            ++position_;
            store_members(mark, out);
            return true;
        }

        while (true) {
            if (member_stack_.size() - mark >= limits_.max_keys) {
                return fail(JsonError::TooManyKeys);
            }

            skip_whitespace();
            if (at_end() || peek() != kQuote) { return fail(JsonError::Malformed); }
            std::string_view key;
            if (!parse_string(key)) { return false; }

            // Linear, over at most max_keys entries of THIS object. A duplicate
            // key is an error rather than last-one-wins: two readers of the same
            // body must never disagree about what it says.
            for (std::size_t i = mark; i < member_stack_.size(); ++i) {
                if (member_stack_[i].key == key) { return fail(JsonError::DuplicateKey); }
            }

            skip_whitespace();
            if (at_end() || peek() != ':') { return fail(JsonError::Malformed); }
            ++position_;

            JsonValue value;
            if (!parse_value(depth + 1, value)) { return false; }
            member_stack_.push_back(JsonMember{key, value});

            skip_whitespace();
            if (at_end()) { return fail(JsonError::Malformed); }
            const char c = peek();
            ++position_;
            if (c == '}') { break; }
            if (c != ',') { return fail(JsonError::Malformed); }
        }

        store_members(mark, out);
        return true;
    }

    void store_members(std::size_t mark, JsonValue& out) {
        const std::size_t count = member_stack_.size() - mark;
        const std::span<JsonMember> stored = arena_.allocate<JsonMember>(count);
        for (std::size_t i = 0; i < count; ++i) { stored[i] = member_stack_[mark + i]; }
        member_stack_.resize(mark);

        out.type_ = JsonType::Object;
        out.count_ = static_cast<std::uint32_t>(count);
        out.members_ = stored.data();
    }

    const std::string_view body_;
    const JsonLimits       limits_;
    BodyArena&             arena_;
    // Scratch shared by every container in the document, reused as containers
    // close. Backed by the arena, so it costs no heap allocation for a body the
    // arena's inline buffer covers.
    std::pmr::vector<JsonValue>  value_stack_;
    std::pmr::vector<JsonMember> member_stack_;
    std::size_t            position_{0};
    JsonError              error_{JsonError::Ok};
};

JsonDocument parse_json(std::string_view body, BodyArena& arena, const JsonLimits& limits) {
    JsonDocument document;

    // Cheapest check first. A 100 KB body of "aaaa…" costs one comparison
    // rather than a full UTF-8 pass over it (docs/06-input-validation.md §2).
    if (body.size() > limits.max_bytes) {
        document.error_ = JsonError::TooLarge;
        return document;
    }
    if (i18n::validate(body) != i18n::Utf8Error::Ok) {
        document.error_ = JsonError::InvalidUtf8;
        return document;
    }

    JsonParser parser{body, arena, limits};
    return parser.run();
}

}  // namespace anvil::input
