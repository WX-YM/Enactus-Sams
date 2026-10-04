#pragma once

// The field types the Form Maker offers (anvil docs/13-dynamic-forms.md,
// docs/01-seams.md §5).
//
// The CODE is stored on every field of every form definition: dense from zero,
// append only, never renumbered or reused. The wire name is what the admin
// panel and the public form page send and receive.

#include <array>

#include "anvil/forms/field_type.h"
#include "anvil/forms/validators.h"

namespace enactus {

namespace f = anvil::forms;
using FF = anvil::forms::FieldTypeFlag;

inline constexpr std::array<f::FieldTypeSpec, 5> kFieldTypes{{
    {"TEXT_SHORT",    &f::validators::text,          200, 0, FF::CodePointCapped},
    {"TEXT_LONG",     &f::validators::text,         4000, 1, FF::CodePointCapped | FF::MultiLine},
    {"EMAIL",         &f::validators::email,           0, 2, FF::None},
    // Free text with a short cap rather than a phone validator: the forms are
    // filled by students from several countries, and refusing a real number is
    // worse than storing an odd one.
    {"PHONE",         &f::validators::text,           32, 3, FF::CodePointCapped},
    {"SELECT_SINGLE", &f::validators::select_single,   0, 4, FF::Options},
}};

static_assert(f::table_is_well_formed(kFieldTypes),
              "empty name, a duplicate code or name, a negative code, or a flag combination "
              "that contradicts itself");
static_assert(f::is_dense_from_zero(kFieldTypes),
              "the lookup is a direct index; a sparse table turns it into a scan per answer");

inline constexpr std::string_view kFormDefinitionsCollection = "form_definitions";
inline constexpr std::string_view kFormResponsesCollection = "form_responses";

}  // namespace enactus
