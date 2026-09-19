#pragma once

#include <array>
#include <cstdint>
#include "anvil/core/perm_catalogue.h"

namespace enactus {

enum class Perm : std::uint8_t {
    SuperAdmin = 0,
    HrManager = 1,
    ContentEditor = 2,
};

inline constexpr std::array<anvil::PermName, 3> kPermNameTable{{
    {"SuperAdmin",    static_cast<std::uint8_t>(Perm::SuperAdmin)},
    {"HrManager",     static_cast<std::uint8_t>(Perm::HrManager)},
    {"ContentEditor", static_cast<std::uint8_t>(Perm::ContentEditor)},
}};

inline constexpr anvil::PermCatalogue kPerms{kPermNameTable};
static_assert(kPerms.well_formed(), "duplicate bit, duplicate name, empty name, or a bit outside PermSet");

inline constexpr anvil::PermSet kSuperAdmin = anvil::perm_mask(Perm::SuperAdmin);
inline constexpr anvil::PermSet kHrManager = anvil::perm_mask(Perm::HrManager);
inline constexpr anvil::PermSet kContentEditor = anvil::perm_mask(Perm::ContentEditor);

}  // namespace enactus
