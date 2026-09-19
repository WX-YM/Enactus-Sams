#pragma once
#include <array>
#include "anvil/forms/field_types.h"

inline constexpr std::array<anvil::forms::FieldTypeSpec, 3> kFieldTypes{{
    {"text_short", anvil::forms::FieldKind::Text, 255},
    {"text_long", anvil::forms::FieldKind::Text, 4096},
    {"select_single", anvil::forms::FieldKind::Choice, 0},
}};
