#pragma once

// The vocabulary an application needs to declare its background jobs, with no
// dependency on the application's own configuration header.
//
// Same shape and same reason as anvil/core/locale_spec.h,
// anvil/fs/namespace_spec.h and anvil/db/collection_spec.h.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// FORWARD declaration, not the definition. JobRunContext holds a POINTER, which
// needs no complete type — and the application's configuration header includes
// this one, so pulling in mongocxx/client.hpp here would drag the whole driver
// into every translation unit in anvil::foundation. That target links no driver
// by design, so the first thing to notice would be a link error in a text
// validator.
#include <mongocxx/client-fwd.hpp>

#include "anvil/core/types.h"

namespace anvil::timer {

enum class JobOutcome : std::uint8_t {
    // Acknowledge and forget. The work either happened or was already done.
    Done = 0,
    // Transient: retry with backoff until max_attempts, then dead-letter.
    Retry = 1,
    // Permanent: dead-letter now. A malformed argument blob or a resource that is
    // gone for good gains nothing from four more attempts, and retrying it is a
    // denial of service against our own database.
    Failed = 2,
};

// Which pool a kind's work belongs to. A sweep that walks the filesystem and a
// fan-out that writes a few hundred documents have different costs and must not
// share a queue: one export on db_pool holds a mongocxx connection for the length
// of the whole job (docs/00-architecture.md §3).
enum class JobPool : std::uint8_t { Db = 0, Cpu = 1 };

// Everything a handler is given. Ordered largest-alignment-first so there is no
// interior padding, and deliberately small: it is constructed per execution.
//
// `args` borrows the claimed envelope's bytes and is valid only for the duration
// of the call. A handler that needs them afterwards copies them (ENGINEERING_RULES.md §2.2).
struct JobRunContext final {
    std::span<const std::uint8_t> args;    // 16
    Uuid                          id;      // 16 — for log correlation
    // The trace that ENQUEUED this job, all-zero when there was none.
    //
    // A link, not a parent, and the execution runs under a fresh root of its own
    // — `anvil::http::current_trace()` inside a handler is that root. The reason
    // is in queue.h beside the envelope: a job is at-least-once and may run days
    // later, so a span covering the publish and Thursday's retry is not a trace.
    std::array<std::uint8_t, 16>  linked_trace;  // 16
    mongocxx::client*             client;  //  8 — never null; owned by the pool task
    std::uint8_t                  attempt; //  1 — 1 on the first delivery
};

// 64: three 16-byte members, a pointer, a byte, and seven bytes of tail padding
// to the pointer's alignment. Asserted so that a fourth member is a deliberate
// act — this is constructed per execution and passed by reference to every
// handler an application writes.
static_assert(sizeof(JobRunContext) == 64);

// A plain pointer: no capture, nothing address-space dependent, and usable in the
// constexpr table below.
using JobHandler = JobOutcome (*)(const JobRunContext& ctx) noexcept;

// One job kind. Ordered largest-alignment-first.
struct JobSpec final {
    // For log lines, metrics and dead-letter records. STORED in Redis, so the key
    // is append-only: never rename one, never reuse a retired one.
    std::string_view key;
    JobHandler       handler;
    // Visibility timeout: how long a claimed-but-unacknowledged entry may sit in
    // the pending list before another worker may reclaim it. It must exceed the
    // job's realistic worst-case runtime, or a slow job is reclaimed while still
    // running and executes twice — survivable, because every handler is
    // idempotent, but wasteful.
    std::uint32_t    lease_seconds;
    std::uint8_t     max_attempts;
    JobPool          pool;
    // Whether reaching the dead-letter stream is a page. A failed nightly purge is
    // a data-integrity gap that must reach a human; a failed push delivery to one
    // dead endpoint is not.
    bool             alert_on_dead_letter;
};

// A recurrence, expressed as a period and an offset into it rather than as a cron
// expression.
//
// Cron carries a timezone, and a timezone means the recurrence MOVES when the
// local clock does — twice a year a daily sweep either runs twice or not at all.
// A period with no timezone in it cannot do that.
struct RecurringSpec final {
    std::chrono::seconds period;
    // Where in the period the job is due: 3 h into a 24 h period is 03:00 UTC.
    // Must be less than `period`.
    std::chrono::seconds offset_in_period;
    // Added to the due instant. Every instance firing at exactly 00:00:00 is a
    // self-inflicted thundering herd against the database.
    std::chrono::seconds max_jitter;
    // Index into the application's job table.
    std::uint16_t        kind;
};

inline constexpr std::chrono::seconds kDay{24 * 3600};

}  // namespace anvil::timer
