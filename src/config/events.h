#pragma once
#include <array>
#include "anvil/analytics/events.h"
#include "anvil/analytics/metrics.h"

inline constexpr std::array<anvil::analytics::EventSpec, 2> kEvents{{
    {"page_visit", anvil::analytics::EventClass::Standard, false},
    {"form_filled", anvil::analytics::EventClass::Standard, false},
}};

inline constexpr std::array<anvil::analytics::MetricSpec, 1> kMetrics{{
    {"active_users", anvil::analytics::MetricType::Gauge, {}},
}};
