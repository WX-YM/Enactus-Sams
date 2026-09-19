#pragma once
#include <array>
#include "anvil/timer/job_spec.h"

namespace anvil::config {

inline constexpr std::array<anvil::timer::JobSpec, 1> kJobSpecs{{
    {"none", nullptr, 0, 0},
}};

inline constexpr std::array<anvil::timer::RecurringSpec, 0> kRecurringJobs{};

} // namespace anvil::config
