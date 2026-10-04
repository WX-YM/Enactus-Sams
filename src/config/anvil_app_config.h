#pragma once

// The one header anvil includes by name (anvil docs/02-getting-started.md §2).
// Nothing in here may name a driver type.

#include <array>
#include <cstddef>
#include <cstdint>

#include "anvil/core/locale_spec.h"
#include "anvil/db/collection_spec.h"
#include "anvil/fs/namespace_spec.h"

namespace anvil::config {

// PERSISTED: the index is in every access token and every account row.
// Append only.
inline constexpr std::array<LocaleSpec, 1> kLocales{{
    {"en", "en", false},
}};

inline constexpr std::size_t kDefaultLocale = 0;

static_assert(!kLocales.empty(), "at least one locale must be declared");
static_assert(kDefaultLocale < kLocales.size(), "the default locale must exist");

// PERSISTED: the namespace index is stored on every media row, and the
// directory name is the `{ns}` segment of every media URL. Append only.
//
// `site` holds every image the public website shows: section images, gallery
// photos, the Tafrah screenshots. JPEG and PNG only — the decoders an upload
// from a staff member's phone or laptop actually needs, and the two oldest of
// the four image decoders anvil can run.
inline constexpr std::array<fs::NamespaceSpec, 1> kNamespaces{{
    {"site", fs::mime_bit(fs::Mime::Jpeg) | fs::mime_bit(fs::Mime::Png)},
}};

inline constexpr std::array<std::uint16_t, 4> kVariantWidths{{320, 640, 1024, 1600}};

// thumb, card, hero, full.
inline constexpr std::array<std::array<std::uint16_t, fs::kRoleCount>, 1> kRoleWidths{{
    {{320, 640, 1024, 1600}},
}};

static_assert(kRoleWidths.size() == kNamespaces.size(),
              "every declared namespace needs a row in the role-width table");

inline constexpr std::array<db::DatabaseSpec, 1> kDatabases{{
    {"application"},
}};

// The names avoid every collection the pre-anvil backend wrote (`users`,
// `teams`, `applications`, `content`, `logs`, `analytics`, `form_schema`,
// `form_submissions`). Those stay untouched as the source of the one-shot
// legacy migration (`enactus_migrate --legacy`), and nothing here ever reads
// them on a request path.
inline constexpr std::array<db::CollectionSpec, 14> kCollections{{
    {"accounts",            "",           0},
    {"user_sessions",       "expires_at", 0},
    {"email_verifications", "expires_at", 0},
    {"staff_guard",         "",           0},
    {"staff_profiles",      "",           0},
    {"media",               "",           0},
    {"audit_log",           "",           0},
    {"sections",            "",           0},
    {"entries",             "",           0},
    {"form_definitions",    "",           0},
    {"form_responses",      "",           0},
    {"analytics_events",    "expires_at", 0},
    {"analytics_sessions",  "expires_at", 0},
    {"analytics_rollups",   "",           0},
}};

}  // namespace anvil::config
