#pragma once

// The job table (anvil docs/10-timer-jobs.md). Included only by
// anvil/timer/registry.h, because a handler is handed a database client.
//
// The index of a job in kJobSpecs is its KIND, stored in every queued envelope:
// append only. Index 0 is the reserved "none".

#include <array>
#include <chrono>
#include <cstdint>

#include "anvil/timer/job_spec.h"

namespace anvil::config {

// Rolls the closed hours of raw page-view events into hourly and daily counts
// (anvil docs/17-analytics.md §14). Idempotent: it recomputes a window and
// $sets the result, so a reclaimed lease running it twice is harmless.
[[nodiscard]] timer::JobOutcome analytics_rollup_handler(const timer::JobRunContext& ctx) noexcept;

inline constexpr std::uint16_t kAnalyticsRollupJob = 1;

inline constexpr std::array<timer::JobSpec, 2> kJobSpecs{{
    {"none",             nullptr,                   0,   0, timer::JobPool::Db, false},
    {"analytics.rollup", &analytics_rollup_handler, 300, 3, timer::JobPool::Db, true},
}};

// Every hour, ten minutes past, so the hour being rolled has closed and its
// last flush has landed.
inline constexpr std::array<timer::RecurringSpec, 1> kRecurringJobs{{
    {std::chrono::seconds{3600}, std::chrono::seconds{600}, std::chrono::seconds{120},
     kAnalyticsRollupJob},
}};

}  // namespace anvil::config
