#pragma once
#include <array>
#include "anvil/db/indexes.h"

inline constexpr std::array<anvil::db::IndexSpec, 1> kIndexes{{
    {"users", "email", anvil::db::IndexType::Unique},
}};
inline constexpr std::array<anvil::db::RetiredIndexSpec, 0> kRetiredIndexes{{}};
