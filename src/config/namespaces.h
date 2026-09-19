#pragma once
#include <array>
#include <cstdint>
#include "anvil/fs/namespace_spec.h"

inline constexpr std::array<anvil::fs::NamespaceSpec, 3> kNamespaces{{
    {"content"}, {"media"}, {"guest"},
}};

inline constexpr std::array<std::uint16_t, 5> kVariantWidths{{320, 640, 1024, 1600, 2560}};

inline constexpr std::array<std::array<std::uint16_t, anvil::fs::kRoleCount>, 3> kRoleWidths{{
    {{320, 1024, 1600, 2560}},  // content
    {{320,  640, 1024, 1600}},  // media
    {{320,  640, 1024, 1600}},  // guest
}};
