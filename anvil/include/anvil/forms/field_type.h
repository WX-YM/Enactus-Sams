#pragma once

// The field-type seam: anvil ships the registry and the universal validators,
// your application assembles the table and owns the numbering.
//
// It owns the numbering because it owns the DISK. `code` is stored in every
// field of every definition, and every submission records the definition version
// it was validated against, so old rows are read back through the field types
// their definition still names. Never renumber, never reuse a retired code — the
// rule permission bits and locale indices already live under, failing the same
// way: silently, by reinterpreting rows that are already written.
//
// A table of eleven field types is one application's vocabulary. `FieldTypeSpec`
// is machinery. That split is the whole of ENGINEERING_RULES.md §1, and it is why this
// arrives as a `std::span` handed to the service rather than through the
// configuration header: a field-type table is only ever LOOKED UP and dimensions
// nothing anvil compiles (docs/01-seams.md §5).
//
// --- the one rule a flag cannot enforce -------------------------------------
//
// A validator NEVER writes a PII value into `out`. It writes the normalised
// identity to `identity` and leaves `out.kind` untouched, so there is no branch
// anywhere that could store one as an ordinary answer. That is not expressible in
// the type system, which is why this table is reviewed as a unit and why the
// submission service asserts the shape rather than trusting it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/forms/answer.h"

namespace anvil::forms {

// One answer against one declared field.
//
// `identity` is an OUT parameter rather than another arm of `Answer` on purpose:
// the two destinations are physically different — `ans` for an answer, the sealed
// envelope for an identity — so making them two parameters means a validator
// cannot accidentally produce the wrong one by filling the wrong member.
using ValidateFn = Status (*)(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                              std::string& identity);

// Ordered largest-alignment-first so the table packs with no interior padding.
struct FieldTypeSpec final {
    // What the wire calls this type, in both directions. Unlike `code` it is not
    // persisted, so the cost of a name is paid once per request rather than once
    // per stored document — and a client that reads a name cannot silently
    // disagree with a table it does not have.
    std::string_view wire_name;            // 16
    ValidateFn       validate;             //  8

    // CODE POINTS, never bytes, and meaningful only for a CodePointCapped type.
    // It is what a field of this type gets when its author named no cap, resolved
    // into FieldSpec::max_code_points once per definition by bind_field_type.
    std::uint32_t    default_code_points;  //  4

    // STORED. Append only.
    FieldTypeCode    code;                 //  4
    FieldTypeFlag    flags;                //  1
};

static_assert(sizeof(FieldTypeSpec) == 40, "FieldTypeSpec must not grow padding");

// ONE identity field per form, and it is a LIBRARY rule rather than a flag.
//
// It is a property of the storage format — one `pii` envelope, one `pii_index`,
// and one AEAD AAD binding one envelope to one field id — not of any field type.
// A per-type flag would let an application declare two PII types and get a
// silently single-valued write, where the second identity is validated, accepted
// and then dropped.
inline constexpr std::size_t kMaxPiiFieldsPerForm = 1;

// --- table conformance ------------------------------------------------------
//
// Each condition below is a mistake that would otherwise ship. static_assert both
// checks over your own table.

[[nodiscard]] constexpr bool table_is_well_formed(std::span<const FieldTypeSpec> table) noexcept {
    if (table.empty()) { return false; }
    for (std::size_t i = 0; i < table.size(); ++i) {
        const FieldTypeSpec& spec = table[i];
        if (spec.wire_name.empty()) { return false; }
        // A null validator is checked by validators_are_present() below, NOT
        // here: see the note there.
        //
        // Negative codes would break the direct-index lookup, and a code is
        // stored as int32 rather than as a signed sentinel.
        if (spec.code < 0) { return false; }

        // A PII value never becomes an answer, so it can carry neither an option
        // list nor a media id — both are things `ans` would have to hold.
        if (has_flag(spec.flags, FieldTypeFlag::Pii) &&
            (has_flag(spec.flags, FieldTypeFlag::Options) ||
             has_flag(spec.flags, FieldTypeFlag::Attachment))) {
            return false;
        }
        // Selecting several of something requires there to be something to
        // select from.
        if (has_flag(spec.flags, FieldTypeFlag::MultiSelect) &&
            !has_flag(spec.flags, FieldTypeFlag::Options)) {
            return false;
        }
        // A line break is only meaningful in text, and text is what a code-point
        // cap bounds.
        if (has_flag(spec.flags, FieldTypeFlag::MultiLine) &&
            !has_flag(spec.flags, FieldTypeFlag::CodePointCapped)) {
            return false;
        }
        // A number is not text, so it has no code-point cap. The two together
        // would also make answer_kind_of ambiguous, which is the shape the reader
        // and the writer agree on — see below.
        if (has_flag(spec.flags, FieldTypeFlag::Ranged) &&
            has_flag(spec.flags, FieldTypeFlag::CodePointCapped)) {
            return false;
        }
        // A capped type needs a usable default, because a field whose author
        // named no cap resolves to it; an uncapped type carrying one is dead data
        // that reads as though it bounded something.
        if (has_flag(spec.flags, FieldTypeFlag::CodePointCapped)) {
            if (spec.default_code_points == 0 ||
                spec.default_code_points > kMaxFieldCodePoints) {
                return false;
            }
        } else if (spec.default_code_points != 0) {
            return false;
        }

        for (std::size_t j = 0; j < i; ++j) {
            // A duplicate code makes the second entry unreachable; a duplicate
            // name makes the wire ambiguous in the other direction.
            if (table[j].code == spec.code) { return false; }
            if (table[j].wire_name == spec.wire_name) { return false; }
        }
    }
    return true;
}

// Every row has a validator.
//
// This belongs in table_is_well_formed and cannot be there: under
// `-fsanitize=undefined` this compiler refuses to fold a function-pointer null
// comparison into a constant expression, so a static_assert over the check above
// stops compiling the moment the null test is inside it — and the sanitiser build
// is the gate. A well_formed() that does not compile is worse than one that
// checks less, which is the same trade query_catalogue_is_well_formed already
// makes for the same reason.
//
// So it is a runtime check with three places behind it: call it at boot, the seam
// test asserts it over the reference table, and the submission path refuses a
// null validator by NAME rather than dereferencing it. A null validator is a type
// that accepts everything, and there is no "no validation needed" case — the
// shape check alone is validation.
[[nodiscard]] inline bool validators_are_present(std::span<const FieldTypeSpec> table) noexcept {
    for (const FieldTypeSpec& spec : table) {
        if (spec.validate == nullptr) { return false; }
    }
    return true;
}

// Codes are exactly 0..N-1 in table order, so `field_type_spec` is one bounds
// check and one index rather than a scan. Assert it and the lookup on the
// submission hot path costs nothing; leave it and a form with 100 fields pays a
// linear scan per answer.
[[nodiscard]] constexpr bool is_dense_from_zero(std::span<const FieldTypeSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].code != static_cast<FieldTypeCode>(i)) { return false; }
    }
    return true;
}

// The BSON shape an answer of this type takes, derived from the flags in exactly
// ONE place so that the writer and the reader cannot disagree about it.
//
// This is the contract a CUSTOM validator has to meet, and it is the one hazard
// the extension point carries: a validator that fills `out.number` for a type
// with no `Ranged` flag produces a row the decoder will refuse, months later,
// with an integrity fault nobody can trace back. So the submission service checks
// the kind the validator produced against this, and a mismatch is a REFUSED WRITE
// at the moment the table is wrong rather than an unreadable row afterwards.
//
// A Pii type produces no answer at all, so it has no kind — its validator writes
// to `identity` and leaves `out` untouched, and the service asserts that instead.
[[nodiscard]] constexpr AnswerKind answer_kind_of(FieldTypeFlag flags) noexcept {
    if (has_flag(flags, FieldTypeFlag::Attachment)) { return AnswerKind::Media; }
    if (has_flag(flags, FieldTypeFlag::MultiSelect)) { return AnswerKind::Choices; }
    if (has_flag(flags, FieldTypeFlag::Options)) { return AnswerKind::Choice; }
    if (has_flag(flags, FieldTypeFlag::Ranged)) { return AnswerKind::Number; }
    return AnswerKind::Text;
}

// --- lookup -----------------------------------------------------------------

// nullptr for a code this build does not declare — which is a real state rather
// than a defect: a definition written by a newer process during a rolling deploy
// names a type an older one has never heard of. Refusing to decode it is the
// correct direction to fail, and it is why a retired code must never be reused.
//
// A direct index when the table is dense, which is_dense_from_zero makes a build
// assertion; a scan otherwise, so a table that chose not to be dense still works.
[[nodiscard]] constexpr const FieldTypeSpec* field_type_spec(
    std::span<const FieldTypeSpec> table, FieldTypeCode code) noexcept {
    const auto index = static_cast<std::size_t>(code);
    if (code >= 0 && index < table.size() && table[index].code == code) { return &table[index]; }
    for (const FieldTypeSpec& spec : table) {
        if (spec.code == code) { return &spec; }
    }
    return nullptr;
}

// The wire name back to a spec, for binding a request body. Linear over a table
// of tens of entries with mostly distinct lengths, which beats the hash a map
// would compute and stays constexpr.
[[nodiscard]] constexpr const FieldTypeSpec* field_type_by_name(
    std::span<const FieldTypeSpec> table, std::string_view name) noexcept {
    for (const FieldTypeSpec& spec : table) {
        if (spec.wire_name == name) { return &spec; }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::string_view field_type_name(std::span<const FieldTypeSpec> table,
                                                         FieldTypeCode code) noexcept {
    const FieldTypeSpec* spec = field_type_spec(table, code);
    return spec == nullptr ? std::string_view{} : spec->wire_name;
}

// --- resolving a field against the table ------------------------------------

// Fills the two members of `field` that are derived from its type rather than
// stored: the flag copy and the code-point cap.
//
// THE one place either is written. The create path and the decode path both go
// through it, so a field bound from a request body and the same field read back
// from the database cannot answer "is this multi-line" differently — which they
// would the first time somebody added a branch to only one of the two.
//
// A code the table does not declare is a failure rather than a default, and so is
// a cap above the global ceiling: resolving either one to something plausible
// would launder a definition this build cannot read into one that looks readable.
// `field` is left untouched in both cases.
//
// Zero is the ONE value that resolves, because zero is how "the author named no
// cap" is stored.
[[nodiscard]] constexpr Status bind_field_type(std::span<const FieldTypeSpec> table,
                                               FieldSpec& field, ErrorCode on_unknown,
                                               std::string_view field_name) noexcept {
    const FieldTypeSpec* spec = field_type_spec(table, field.type);
    if (spec == nullptr) { return fail(on_unknown, field_name); }
    if (field.max_code_points > kMaxFieldCodePoints) { return fail(on_unknown, field_name); }
    field.flags = spec->flags;
    if (has_flag(spec->flags, FieldTypeFlag::CodePointCapped)) {
        if (field.max_code_points == 0) { field.max_code_points = spec->default_code_points; }
    } else {
        // A cap on a type that is not text bounds nothing, and leaving it set
        // would make an export or an editor render a limit that is not enforced.
        field.max_code_points = 0;
    }
    return ok();
}

}  // namespace anvil::forms
