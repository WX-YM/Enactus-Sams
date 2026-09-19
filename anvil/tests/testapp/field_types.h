#pragma once

// The reference application's field-type table.
//
// This is what every application built on anvil writes, and it is compiled by
// every build of the test suite — so the worked example in docs/01-seams.md §5 is
// a file that must keep compiling rather than a snippet that can rot.
//
// Ten rows: the nine anvil ships validators for, and one the application writes
// itself. The tenth is the point of the whole seam. `IDENTITY` is a PII type, its
// rules are a jurisdiction's rather than a library's, and its validator is a free
// function of anvil's own `ValidateFn` shape declared right here — which is the
// extension point, exercised from OUTSIDE anvil, where it is the only place it
// can fail cheaply.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/forms/field_type.h"
#include "anvil/forms/pii.h"
#include "anvil/forms/validators.h"

namespace testapp {

namespace f = anvil::forms;
using F = anvil::forms::FieldTypeFlag;

// --- the application's own validator ----------------------------------------

// A generic identity document: 6 to 20 characters of `[A-Z0-9]` after
// normalisation, and not all the same character.
//
// Deliberately NOT a national-id checksum. There is no universal structure to
// check beyond this, and inventing one rejects real documents — which is a far
// more damaging failure than accepting an invalid number, because the person
// holding the real document has no way to proceed. A deployment that knows its
// jurisdiction replaces this function and nothing else.
//
// The hyphen a passport number may be written with is already gone by this point:
// normalise_identity strips separators, deliberately, so that "AB-123456" and
// "AB123456" produce ONE blind index.
[[nodiscard]] inline anvil::Status identity(const f::FieldSpec& field, const f::RawAnswer& raw,
                                            f::Answer& out, std::string& identity_out) {
    (void)field;
    (void)out;
    if (raw.shape != anvil::input::JsonType::String) {
        return anvil::fail(anvil::ErrorCode::ValidationFailed, f::kTypeField);
    }
    const std::string normalised = f::normalise_identity(raw.text);
    if (normalised.size() < 6 || normalised.size() > 20) {
        return anvil::fail(anvil::ErrorCode::ValidationFailed, f::kIdentityField);
    }
    bool varied = false;
    for (const char c : normalised) {
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        if (!upper && !digit) {
            return anvil::fail(anvil::ErrorCode::ValidationFailed, f::kIdentityField);
        }
        varied = varied || c != normalised.front();
    }
    // "AAAAAA" and "000000" are placeholders, not documents.
    if (!varied) { return anvil::fail(anvil::ErrorCode::ValidationFailed, f::kIdentityField); }

    // The value leaves through `identity_out` and NEVER through `out`. There is
    // no branch here that could store it as an ordinary answer, which is the one
    // rule a flag cannot enforce and the reason this table is reviewed as a unit.
    identity_out = normalised;
    return anvil::ok();
}

// --- the table --------------------------------------------------------------
//
// `code` is STORED in every field of every definition. NEVER renumber, and never
// reuse a retired value: a submission records the definition version it was
// validated against, so old rows are read back through the field types their
// definition still names.

inline constexpr std::array<f::FieldTypeSpec, 10> kFieldTypes{{
    {"TEXT_SHORT",     &f::validators::text,             200, 0, F::CodePointCapped},
    {"TEXT_LONG",      &f::validators::text,            4000, 1,
     F::CodePointCapped | F::MultiLine},
    {"NAME",           &f::validators::text,             120, 2, F::CodePointCapped},
    {"NUMBER",         &f::validators::number,             0, 3, F::Ranged},
    {"EMAIL",          &f::validators::email,              0, 4, F::None},
    {"DATE",           &f::validators::date,               0, 5, F::None},
    {"SELECT_SINGLE",  &f::validators::select_single,      0, 6, F::Options},
    {"CHECKBOX_MULTI", &f::validators::checkbox_multi,     0, 7,
     F::Options | F::MultiSelect},
    {"IMAGE_UUID",     &f::validators::attachment_uuid,    0, 8, F::Attachment},
    // The application's own. One PII type, because a form may declare at most one
    // PII FIELD and two PII types would only ever look like a way around that.
    {"IDENTITY",       &identity,                          0, 9, F::Pii},
}};

static_assert(f::table_is_well_formed(kFieldTypes),
              "empty name, a duplicate code or name, a negative code, or a flag combination "
              "that contradicts itself");
// A null validator cannot be part of the assertion above — see the note on
// validators_are_present. tests/forms_test.cc asserts it over this table, and the
// application calls it at boot.
static_assert(f::is_dense_from_zero(kFieldTypes),
              "the lookup is a direct index; a sparse table turns it into a scan per answer");
static_assert(kFieldTypes.size() == 10,
              "adding a field type is a deliberate act: the code is stored on disk and can "
              "never be renumbered or reused");

// Named so the tests and the seeded definitions below refer to a type by meaning
// rather than by a number that must be counted out of the table above.
enum class FieldType : anvil::forms::FieldTypeCode {
    TextShort = 0,
    TextLong = 1,
    Name = 2,
    Number = 3,
    Email = 4,
    Date = 5,
    SelectSingle = 6,
    CheckboxMulti = 7,
    ImageUuid = 8,
    Identity = 9,
};

// The enum and the table are two spellings of one numbering, so they are checked
// against each other rather than kept in step by hand.
static_assert(f::field_type_spec(kFieldTypes,
                                 static_cast<anvil::forms::FieldTypeCode>(FieldType::Identity))
                      ->wire_name == "IDENTITY",
              "the enumerator and the table disagree about which code IDENTITY is");
static_assert(anvil::forms::has_flag(
                  f::field_type_spec(kFieldTypes, static_cast<anvil::forms::FieldTypeCode>(
                                                      FieldType::Identity))
                      ->flags,
                  F::Pii),
              "the one type whose value must never reach `ans` must say so");

}  // namespace testapp
