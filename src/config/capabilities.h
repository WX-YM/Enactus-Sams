#pragma once
#include <array>
#include "anvil/identity/capabilities.h"

inline constexpr std::array<anvil::identity::ScopeSpec, 1> kCapabilityScopes{{
    {"ContentDelete", 300},
}};
