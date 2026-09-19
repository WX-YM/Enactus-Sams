#pragma once
#include <array>
#include <cstdint>
#include "anvil/core/locale_spec.h"
#include "anvil/db/collection_spec.h"
#include "anvil/fs/namespace_spec.h"

namespace anvil::config {

inline constexpr std::array<anvil::LocaleSpec, 1> kLocales{{
    {"en", "en", false},
}};

inline constexpr std::size_t kDefaultLocale = 0;

inline constexpr std::array<anvil::fs::NamespaceSpec, 1> kNamespaces{{
    {"media"},
}};

inline constexpr std::array<std::uint16_t, 5> kVariantWidths{{320, 640, 1024, 1600, 2560}};

inline constexpr std::array<std::array<std::uint16_t, anvil::fs::kRoleCount>, 1> kRoleWidths{{
    {{320, 640, 1024, 1600}},
}};

inline constexpr std::array<anvil::db::DatabaseSpec, 1> kDatabases{{
    {"application"},
}};

inline constexpr std::array<anvil::db::CollectionSpec, 12> kCollections{{
    {"users",               "",           0},
    {"user_sessions",       "expires_at", 0},
    {"drafts",              "expires_at", 0},
    {"teams",               "",           0},
    {"forms",               "",           0},
    {"applications",        "",           0},
    {"calendar_events",     "",           0},
    {"audit_log",           "",           0},
    {"analytics_events",    "",           0},
    {"notifications",       "",           0},
    {"media",               "",           0},
    {"content_sections",    "",           0},
}};

} // namespace anvil::config
