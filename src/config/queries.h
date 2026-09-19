#pragma once
#include <array>
#include "anvil/db/queries.h"

inline constexpr std::array<anvil::db::QuerySpec, 1> kQueries{{
    {"users", "find_by_email", "email"},
}};
