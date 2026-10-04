// The job handlers kJobSpecs names (src/config/anvil_app_jobs.h).

#include <chrono>

#include <trantor/utils/Logger.h>

#include "anvil/analytics/rollup.h"
#include "anvil/http/errors.h"
#include "anvil/timer/job_spec.h"
#include "app/services.h"

namespace anvil::config {

// Recomputes the last two days of hourly buckets and the last three days of
// daily ones. A rollup $sets each bucket from the raw rows, so re-rolling a
// window already rolled is harmless, and the overlap covers a run that was
// missed while the server was down.
timer::JobOutcome analytics_rollup_handler(const timer::JobRunContext& ctx) noexcept {
    try {
        const db::TimeMs now = db::now_ms();
        const auto hourly = enactus::services().hourly_rollup.run(*ctx.client, now - std::chrono::hours{47}, now);
        if (!hourly) {
            LOG_WARN << "analytics hourly rollup failed: " << anvil::http::wire_name(hourly.error().code);
            return timer::JobOutcome::Retry;
        }
        const auto daily = enactus::services().daily_rollup.run(*ctx.client, now - std::chrono::days{3}, now);
        if (!daily) {
            LOG_WARN << "analytics daily rollup failed: " << anvil::http::wire_name(daily.error().code);
            return timer::JobOutcome::Retry;
        }
        return timer::JobOutcome::Done;
    } catch (...) {
        LOG_ERROR << "analytics rollup threw";
        return timer::JobOutcome::Retry;
    }
}

}  // namespace anvil::config
