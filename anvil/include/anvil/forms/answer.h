#pragma once

// One form field, one submitted answer, and the limits both live under.
//
// Deliberately free of any database type, so the field-type table and the
// validators an application writes against it compile without the driver. That is
// what lets a locale module ship a validator: anvil/locale_egy is pure CPU and
// links no driver at all, and a header that dragged bsoncxx in here would put the
// seam out of its reach.
//
// FormDefinition and the submission row are in anvil/forms/definition.h, which is
// the half that does need the driver.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/locale.h"
#include "anvil/core/types.h"
#include "anvil/forms/fid.h"
#include "anvil/input/json.h"

namespace anvil::forms {

// --- limits, all enforced at creation ---------------------------------------

inline constexpr std::size_t kMaxFormFields = 100;
inline constexpr std::size_t kMaxFieldOptions = 50;
inline constexpr std::size_t kMaxLabelCodePoints = 200;

// The ceiling a field's own cap is clamped against. A form author cannot raise a
// text field above it, because the submission body cap below is what actually
// bounds the write and a field claiming more would be a lie: the author would get
// a field that fails at submission time rather than one that holds more.
inline constexpr std::size_t kMaxFieldCodePoints = 4000;

// Enforced at the PARSER, so an oversized body is abandoned during the parse
// rather than after it has been materialised.
inline constexpr std::size_t kMaxSubmissionBytes = 64 * 1024;
inline constexpr std::size_t kMaxSubmissionMedia = 8;

// An option value is stored, compared and exported. `[A-Za-z0-9_-]` keeps it out
// of every escaping question at once: it is safe in a CSV cell, in a JSON string,
// in a URL and in a BSON value, so no downstream stage needs to know where it
// came from.
inline constexpr std::size_t kMaxOptionValueLength = 64;

// A field type as it is STORED, in `fields[].type`.
//
// An int rather than the display string "NAME": a type string repeated across
// every field of every form is wasted bytes on disk and a string compare per
// field at validation time, where an int is a direct index. On the WIRE it is the
// name in both directions, which is the opposite trade for the opposite reason —
// a client reading `9` has to carry a copy of the table and goes wrong silently
// when the two disagree, and that cost is paid once per request rather than once
// per stored document.
using FieldTypeCode = std::int32_t;

// What a field type MEANS, as a bitmask. Declared here rather than beside the
// table because a FieldSpec carries a resolved copy: the validators receive a
// FieldSpec and nothing else, so the facts they need about their own type have to
// travel on it.
//
// Never stored. Flags are a property of the build's table, and a stored copy
// would be a second answer to "what does type 6 mean" that a deploy could make
// disagree with the first.
enum class FieldTypeFlag : std::uint8_t {
    None            = 0,
    // Never written to `ans`; the value goes to the sealed envelope instead.
    Pii             = 1U << 0,
    // >= 2 options, and answers are checked against the SERVER's list.
    Options         = 1U << 1,
    // Resolves through the application's attachment hooks.
    Attachment      = 1U << 2,
    // min_value / max_value are meaningful.
    Ranged          = 1U << 3,
    // default_code_points / max_code_points are meaningful.
    CodePointCapped = 1U << 4,
    MultiLine       = 1U << 5,
    MultiSelect     = 1U << 6,
};

[[nodiscard]] constexpr FieldTypeFlag operator|(FieldTypeFlag a, FieldTypeFlag b) noexcept {
    return static_cast<FieldTypeFlag>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr FieldTypeFlag operator&(FieldTypeFlag a, FieldTypeFlag b) noexcept {
    return static_cast<FieldTypeFlag>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr bool has_flag(FieldTypeFlag held, FieldTypeFlag wanted) noexcept {
    return (held & wanted) == wanted;
}

// A stored localised string, one per declared locale. Owned rather than viewed:
// a decoded definition outlives the BSON buffer it came from and is shared across
// threads through a shared_ptr.
using LocalizedText = std::array<std::string, kLocaleCount>;

struct FormOption final {
    std::string   value;   // the STORED answer; `[A-Za-z0-9_-]`, <= 64 bytes
    LocalizedText label;
};

// One declared field of one form.
//
// Ordered largest-alignment-first so there is no interior padding. Validation
// walks a span of these with no allocation, which is what "the cached definition
// is a parsed struct" buys.
struct FieldSpec final {
    LocalizedText           label;
    std::vector<FormOption> options;
    // Meaningful only for a Ranged type, and both are required with min <= max.
    std::int64_t            min_value;
    std::int64_t            max_value;
    // RESOLVED, never zero for a CodePointCapped type: bind_field_type fills it
    // from the table's `default_code_points` when the author named none. The
    // validator therefore reads one field rather than switching on a type to
    // pick a fallback, and the fallback is resolved once per definition instead
    // of once per answer.
    std::uint32_t           max_code_points;
    // MultiSelect only. Zero means "as many as there are options".
    std::uint32_t           max_selections;
    Fid                     fid;
    FieldTypeCode           type;
    // RESOLVED from the field-type table and never stored. It is a copy of one
    // row's flags, written in exactly one place — bind_field_type — so a
    // FieldSpec that came from the database and one that came from a request
    // body carry the same answer to "is this type multi-line".
    FieldTypeFlag           flags;
    bool                    optional;
};

// The BSON shape an answer takes. A tag rather than a variant of everything, so
// decoding asserts one exact BSON type per answer and never coerces.
enum class AnswerKind : std::uint8_t { Text, Number, Choice, Choices, Media };

// The JSON shape an answer of this kind arrives in and is read back as, named
// for a client generated from the descriptor (anvil/descriptor/descriptor.h).
// The kind itself is derived from the type's flags rather than stored, so this
// is the one place the derivation gets a word a client can hold.
[[nodiscard]] constexpr std::string_view answer_kind_name(AnswerKind kind) noexcept {
    switch (kind) {
        case AnswerKind::Text:    return "text";
        case AnswerKind::Number:  return "number";
        case AnswerKind::Choice:  return "choice";
        case AnswerKind::Choices: return "choices";
        case AnswerKind::Media:   return "media";
    }
    return "text";
}

struct Answer final {
    std::string              text;     // Text and Choice
    std::vector<std::string> choices;  // Choices
    std::int64_t             number;   // Number
    Uuid                     media;    // Media
    Fid                      fid;
    AnswerKind               kind;
};

// One answer as it arrived, still untyped.
//
// The JSON node is NOT carried forward: it borrows from the request body and the
// arena, and a view that crosses onto db_pool is the dangling read CLAUDE.md §2.2
// names as the most likely crash in code built on this library. Text is copied
// here, once, at the boundary.
struct RawAnswer final {
    std::string              text;
    std::vector<std::string> choices;
    std::int64_t             number;
    Fid                      fid;
    // Which arm the client actually sent, so the type check compares a PARSED
    // shape against a declared one rather than coercing one into the other.
    input::JsonType          shape;
};

}  // namespace anvil::forms
