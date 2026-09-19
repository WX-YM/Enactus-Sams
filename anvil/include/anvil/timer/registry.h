#pragma once

// The application's job table, and the lookups over it.
//
// anvil ships the queue, the lease, the reclaim and the backoff; which jobs exist
// and what runs them is the application's (docs/01-seams.md §7).
//
// The table is in the application's configuration header rather than passed as a
// span for one reason: kMaxLeaseSeconds below is DERIVED from it and is used to
// size the reclaim pass, so the table has to be visible where anvil is compiled.
//
// JobKind is an INDEX into that table, and it is stored in Redis. Slot 0 is a
// reserved None, and a retired kind keeps its slot with a null handler so its id
// can never be reused by something else.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <anvil_app_jobs.h>

#include "anvil/timer/job_spec.h"

namespace anvil::timer {

inline constexpr std::size_t kJobKindCount = config::kJobSpecs.size();

static_assert(kJobKindCount >= 1, "slot 0 is the reserved None and must exist");
static_assert(kJobKindCount <= 65535, "a job kind is carried as a uint16 in the envelope");

// The longest visibility timeout any kind declares.
//
// It is what the reclaim pass uses as its idle threshold, for ALL kinds. A
// per-entry threshold is what each kind's own lease would need, and XAUTOCLAIM
// does not offer one; the shortest would reclaim a nightly sweep that is merely
// slow and run it a second time. Reclaim latency after a crash is therefore
// bounded by the longest-running job class, which is the right trade: the crash
// path is rare and the duplicate-execution path is not free.
inline constexpr std::uint32_t kMaxLeaseSeconds = [] {
    std::uint32_t longest = 0;
    for (const JobSpec& spec : config::kJobSpecs) {
        if (spec.lease_seconds > longest) { longest = spec.lease_seconds; }
    }
    return longest;
}();

// nullptr for None, for a retired id, and for a kind this build does not know —
// all three of which dispatch must treat as a PERMANENT failure rather than as
// something to retry until the attempt budget runs out.
[[nodiscard]] constexpr const JobSpec* spec_of(std::uint16_t kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index == 0 || index >= kJobKindCount) { return nullptr; }
    if (config::kJobSpecs[index].handler == nullptr) { return nullptr; }
    return &config::kJobSpecs[index];
}

// Exponential backoff: 1 s, 4 s, 15 s, 60 s, 5 min, then dead-letter. Indexed by
// the attempt that just failed, minus one. Jitter is applied at the call site.
inline constexpr std::array<std::uint32_t, 5> kRetryBackoffSeconds{1, 4, 15, 60, 300};

[[nodiscard]] constexpr std::uint32_t retry_delay_seconds(std::uint8_t failed_attempt) noexcept {
    const std::size_t index = failed_attempt == 0 ? 0 : failed_attempt - 1U;
    return index >= kRetryBackoffSeconds.size() ? kRetryBackoffSeconds.back()
                                                : kRetryBackoffSeconds[index];
}

// Every declared kind carries a key, an attempt budget and a lease. A future
// addition that forgets one is a compile error rather than a job that silently
// never runs, or one that is reclaimed the instant it is claimed.
//
// The handler pointers are deliberately NOT asserted against null: they name
// functions defined in another translation unit, and the address of a function
// this one has only seen declared is not a constant expression. A null handler is
// caught by spec_of at dispatch, which is where a retired kind has to be handled
// anyway.
[[nodiscard]] constexpr bool job_table_is_well_formed() noexcept {
    if (!config::kJobSpecs[0].key.empty() && config::kJobSpecs[0].key != "none") {
        return false;
    }
    if (config::kJobSpecs[0].max_attempts != 0) { return false; }

    for (std::size_t i = 1; i < kJobKindCount; ++i) {
        const JobSpec& spec = config::kJobSpecs[i];
        if (spec.key.empty()) { return false; }
        if (spec.max_attempts == 0) { return false; }
        if (spec.lease_seconds == 0) { return false; }
        // A duplicate key makes two kinds indistinguishable in the dead-letter
        // record, which is the one place a human looks when a job stops working.
        for (std::size_t j = 1; j < i; ++j) {
            if (config::kJobSpecs[j].key == spec.key) { return false; }
        }
    }
    return true;
}

static_assert(job_table_is_well_formed(),
              "slot 0 must be the reserved None, and every other kind needs a unique "
              "key with a non-zero attempt and lease budget");

// A recurrence naming a kind that does not exist fires nothing, forever, in
// silence. That is the worst shape a scheduling bug can take, so it is a build
// error.
static_assert([] {
    for (const RecurringSpec& spec : config::kRecurringJobs) {
        if (spec.kind == 0 || spec.kind >= kJobKindCount) { return false; }
        if (spec.period.count() <= 0) { return false; }
        if (spec.offset_in_period >= spec.period) { return false; }
        if (spec.max_jitter >= spec.period) { return false; }
    }
    return true;
}(), "a recurring declaration names an unknown job kind, or its offset or jitter "
     "does not fit inside its period");

static_assert(std::is_trivially_copyable_v<JobRunContext>);

}  // namespace anvil::timer
