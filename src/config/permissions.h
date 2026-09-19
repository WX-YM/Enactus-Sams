#pragma once
#include <cstdint>
#include <array>
#include "anvil/accesscontrol/permissions.h"

enum class Perm : std::uint8_t {
    SuperAdmin = 0,
    HR = 1,
    Manager = 2,
    ContentWrite = 3,
};

inline constexpr std::array<anvil::PermName, 4> kPermNameTable{{
    {"SuperAdmin",  static_cast<std::uint8_t>(Perm::SuperAdmin)},
    {"HR",          static_cast<std::uint8_t>(Perm::HR)},
    {"Manager",     static_cast<std::uint8_t>(Perm::Manager)},
    {"ContentWrite",static_cast<std::uint8_t>(Perm::ContentWrite)},
}};
inline constexpr anvil::PermSet kPermsSuperAdmin = anvil::PermSet{}.with(Perm::SuperAdmin);
inline constexpr anvil::PermSet kPermsHR = anvil::PermSet{}.with(Perm::HR);
inline constexpr anvil::PermSet kPermsManager = anvil::PermSet{}.with(Perm::Manager);

static_assert(anvil::PermNameTable{kPermNameTable}.well_formed());
