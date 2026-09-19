#pragma once

// The declared shape of a success body.
//
// This half is the VOCABULARY: what a field may be, and what makes a shape
// usable at all. The writer that turns a declaration into bytes is
// http/response_writer.h, and the two are separate headers for one reason —
// `descriptor/route_description.h` carries a span of these per route and is
// included by every table an application declares, while the writer is included
// only by the handlers that write one. A table should not pay for a template it
// never instantiates (ENGINEERING_RULES.md §8).
//
// Read response_writer.h for the argument this vocabulary exists to serve: that
// a described body can only be produced by walking its declaration in order, so
// the emitted schema is a description of the bytes rather than a claim about
// them.

#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/core/ct_text.h"

namespace anvil::http {

// The types a described field may have.
//
// Deliberately small, and deliberately not "the JSON types". Each entry is a
// distinction a CLIENT has to make — `Uuid` and `String` are both JSON strings
// and a client parses one into a typed id and the other into text; `TimeMs` is a
// JSON string too and a client turns it into a date. A vocabulary that collapsed
// them would describe the bytes correctly and the types uselessly.
enum class FieldKind : std::uint8_t {
    String,
    Int,
    Bool,
    // Hyphenated lowercase text on the wire; 16 bytes everywhere else. The
    // stored form never appears in JSON (http/json_writer.h).
    Uuid,
    // RFC 3339 in UTC with millisecond precision, which is the one spelling
    // json_writer.h will produce.
    Time,
    // An array of plain strings. The only nesting in the grammar, and it is
    // here because a list of names is the shape every table-driven response
    // already needed and the one a client cannot infer.
    Strings,
};

[[nodiscard]] constexpr std::string_view kind_name(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::String:  return "string";
        case FieldKind::Int:     return "int";
        case FieldKind::Bool:    return "bool";
        case FieldKind::Uuid:    return "uuid";
        case FieldKind::Time:    return "time";
        case FieldKind::Strings: return "strings";
    }
    // Unreachable for a valid enumerator, and "string" rather than an empty
    // name so a future kind with no case still produces a parseable schema.
    return "string";
}

struct ResponseField final {
    // The key as it appears in the body. Stable, for the reason a route id is:
    // renaming one breaks every client built against the old name.
    std::string_view name;   // 16
    FieldKind        kind;   //  1
    // Whether this field may be written as JSON `null`.
    //
    // A client's type for a nullable field is a different type, so the
    // declaration has to carry it — and the writer enforces it: `null_field` on
    // a field declared non-nullable does not compile, which is the direction
    // that matters. A client told a field is always present and handed a null
    // crashes on the field it was told it could trust.
    bool             nullable;  // 1
};

static_assert(sizeof(ResponseField) == sizeof(std::string_view) + sizeof(std::uint64_t),
              "one view and one word of tag; a third field here is a deliberate act");

// Whether a declared shape is usable at all: every name present, valid UTF-8,
// and no name declared twice.
//
// It belongs in a `static_assert` beside an application's table, the way
// `descriptions_match` and `well_formed` already do. A duplicate key is the one
// that has to be caught here: JSON does not forbid it, every parser resolves it
// differently, and a client generated from the schema would declare one field
// while the server sent two.
[[nodiscard]] constexpr bool response_shape_is_well_formed(
    std::span<const ResponseField> fields) noexcept {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (!ct::is_non_empty_utf8(fields[i].name)) { return false; }
        for (std::size_t j = i + 1; j < fields.size(); ++j) {
            if (fields[i].name == fields[j].name) { return false; }
        }
    }
    return true;
}

}  // namespace anvil::http
