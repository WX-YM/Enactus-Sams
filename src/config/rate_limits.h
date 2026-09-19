#pragma once
#include <array>
#include "anvil/http/rate_limits.h"

inline constexpr std::array<anvil::http::RateLimitSpec, 1> kRateLimits{{
    {"api_default", 60, 100}, // 100 requests per 60 seconds
}};
