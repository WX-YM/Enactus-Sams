#pragma once

// The reference application's background jobs.
//
// Separate from anvil_app_config.h on purpose. That header is included by
// anvil::foundation for the locale table, and foundation links no database driver
// — while a job handler is handed a mongocxx::client, so this table cannot be
// declared anywhere foundation can see. The split is the same boundary the link
// targets already draw (docs/00-architecture.md §2).
//
// Only anvil/timer/registry.h includes this, and that header is platform-only.

#include <array>
#include <chrono>
#include <cstdint>

#include "anvil/timer/job_spec.h"

namespace anvil::config {

// Declared, not defined. Taking the address of a function this header has only
// seen declared keeps the table below constexpr and keeps the job bodies out of
// the low layer — the linker binds them. tests/testapp/jobs.cc defines these.
[[nodiscard]] timer::JobOutcome sweep_handler(const timer::JobRunContext& ctx) noexcept;
[[nodiscard]] timer::JobOutcome fanout_handler(const timer::JobRunContext& ctx) noexcept;
[[nodiscard]] timer::JobOutcome chat_push_handler(const timer::JobRunContext& ctx) noexcept;

// --- the job table ---------------------------------------------------------
//
// The INDEX is the job kind and it is stored in Redis, so this table is
// append-only. Slot 0 is a reserved None: a zero kind in an envelope is a
// malformed envelope, not job number zero.
//
// A retired kind keeps its slot with a null handler rather than being removed, so
// its id can never be handed to something else — an envelope written before the
// retirement still names it, and dispatch must answer "this is gone" rather than
// running whatever moved into the slot.
inline constexpr std::array<timer::JobSpec, 4> kJobSpecs{{
    {"none",        nullptr,          0,   0, timer::JobPool::Db,  false},
    {"test.sweep",  &sweep_handler,   900, 3, timer::JobPool::Cpu, true},
    {"test.fanout", &fanout_handler,   60, 5, timer::JobPool::Db,  false},
    // A chat push nudge (anvil/chat/push.h): one per conversation per window.
    // A dead endpoint is the endpoint's problem, not a page; a nudge that
    // failed three times is a minute late and then lost, which the chat list
    // still shows.
    {"chat.push",   &chat_push_handler, 60, 3, timer::JobPool::Db, false},
}};

// --- recurrences -----------------------------------------------------------
//
// A period and an offset into it, never a cron expression. Cron carries a
// timezone, and a timezone means the recurrence MOVES when the local clock does —
// twice a year a daily sweep either runs twice or not at all.
inline constexpr std::array<timer::RecurringSpec, 1> kRecurringJobs{{
    // 03:00 UTC, with up to 15 minutes of jitter so N workers do not synchronise
    // into one thundering herd against the database.
    {timer::kDay, std::chrono::seconds{3 * 3600}, std::chrono::seconds{900}, 1},
}};

}  // namespace anvil::config
