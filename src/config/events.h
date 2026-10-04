#pragma once

// Product analytics events (anvil docs/17-analytics.md §8). The code is STORED
// on every event row and every rollup: append only.
//
// One event: a page view of the public website. No dimensions, so a row says
// "someone viewed the site" and nothing about who — the visitor id it is
// sessionised under is a keyed daily digest of a coarsened address, never the
// address itself (§11).

#include <array>

#include "anvil/analytics/event_spec.h"
#include "anvil/analytics/repository.h"

namespace enactus {

namespace an = anvil::analytics;

inline constexpr an::EventCode kPageView = 1;

inline constexpr std::array<an::DimensionSpec, 0> kNoDimensions{};

inline constexpr std::array<an::EventSpec, 1> kEvents{{
    {"page_view", kNoDimensions, kPageView, an::EventClass::Behaviour, false},
}};

static_assert(an::event_table_is_well_formed(kEvents));

inline constexpr an::AnalyticsCollections kAnalyticsCollections{
    "analytics_events", "analytics_sessions", "analytics_rollups"};

}  // namespace enactus
