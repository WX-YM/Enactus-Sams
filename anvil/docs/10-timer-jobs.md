# 10 — Background jobs

A Redis Streams queue with consumer groups, visibility leases and dead-lettering.

Two claims govern everything below:

> **Every queue here is at-least-once.** A crashed worker's claim is *reclaimed*,
> not lost, so a job that was half-done runs again. **Every handler must therefore
> be idempotent** — this is not a recommendation, it is the contract.

> **In-process timers are not durable state.** A timer does not survive `SIGTERM`,
> and it fires N times with N instances.

## 1. Why Redis Streams

A stream with a consumer group gives three things a list does not: a pending-entries
list, so a claimed-but-unacknowledged entry is visible rather than vanished;
`XAUTOCLAIM`, so a dead worker's claim is recoverable; and an acknowledgement
step, so "delivered" and "completed" are different facts.

Redis must run with `appendonly yes, appendfsync everysec`. The default
configuration snapshots periodically and can lose the last few minutes of writes
on a crash — which for a job queue means scheduled work vanishing silently.

### Redis is not the system of record

Even with `appendonly`. **Anything that MUST happen records its intent in MongoDB,
and the Redis entry is only the delivery mechanism** — so a queue that lost its
last second of writes loses a delivery attempt rather than the obligation to make
one.

That is what makes a reconciliation pass possible: something durable to compare
the queue against. `notifications::PublishService::sweep_outbox` is the worked
example — the canonical row is committed first and marked `dispatched_at` last, so
a row with the intent and no completion is exactly a job that was owed and never
enqueued. See `docs/11-notifications.md` §3.

A job with no such record behind it must therefore be one it is acceptable to
lose: a sweep, a purge, a digest. That is the same line §"What belongs here" draws
from the other side.

## 2. The job table

An application declares its jobs; anvil dispatches them. See
[`01-seams.md`](01-seams.md) §7.

**The kind is an index and it is stored in Redis**, so the table is append-only.
Slot 0 is a reserved `None`: a zero kind in an envelope is a *malformed envelope*,
not job number zero, and a zeroed or truncated envelope decodes to exactly that.

A retired kind keeps its slot with a **null handler** rather than being removed.
An envelope written before the retirement still names it, and dispatch must answer
"this is gone" rather than running whatever moved into the slot.

`spec_of` returns nullptr for the reserved slot, for a retired kind, and for a
kind this build does not know. All three are **permanent** failures: they
dead-letter immediately rather than retrying until the attempt budget runs out,
because four more attempts will not make an unknown kind known.

The handler is a plain function pointer — no capture, nothing address-space
dependent — which is what keeps the table `constexpr` and in `.rodata` while the
bodies live in the application.

## 3. Leases

A claimed entry is invisible to other consumers for its **lease**: the visibility
timeout. It must exceed the job's realistic worst-case runtime, or a slow job is
reclaimed while it is still running and executes twice — survivable, because
handlers are idempotent, but wasteful.

The reclaim pass uses **one** threshold for all kinds: the longest lease any kind
declares, computed from the table. Using each kind's own lease would need a
per-entry threshold, which `XAUTOCLAIM` does not offer; using the shortest would
reclaim a nightly sweep that is merely slow.

So reclaim latency after a crash is bounded by the longest-running job class. That
is the right trade: the crash path is rare and the duplicate-execution path is not
free.

## 4. Retries and dead-lettering

Exponential backoff — 1 s, 4 s, 15 s, 60 s, 5 min — indexed by the attempt that
just failed. Past the last rung, dead-letter.

Jitter is applied at the call site, up to a quarter of the delay, so N workers
retrying the same class of failure at the same instant spread out instead of
synchronising into a second storm.

A handler returns one of three outcomes, and the distinction matters:

| Outcome | Meaning |
|---|---|
| `Done` | Acknowledge and forget. The work either happened or was already done |
| `Retry` | Transient. Back off, retry until the budget runs out, then dead-letter |
| `Failed` | **Permanent.** Dead-letter now |

`Failed` is for a malformed argument blob or a resource that is gone for good.
Retrying one gains nothing and is a denial of service against our own database.

Whether reaching the dead-letter stream is a **page** is per-kind. A failed
nightly purge is a data-integrity gap that must reach a human; a failed push
delivery to one dead endpoint is not.

## 5. Recurrences

Declared as a **period and an offset into it**, never as a cron expression.

Cron carries a timezone, and a timezone means the recurrence **moves when the
local clock does** — twice a year a daily sweep either runs twice or not at all.
Several jurisdictions have reinstated or abolished daylight saving in recent
years, so this is live rather than theoretical.

A cron expression is also a parser, and the only thing it would ever parse is a
compile-time constant in a header. That is all cost and no benefit, and a parser
on a scheduling path is a liability. Period plus offset is the same declaration
with no parser, and it is arithmetic the compiler can check.

Everything is UTC, and the bucket arithmetic has no timezone in it at all — which
is the only way to be sure. `recurrence_bucket` is integer arithmetic on the Unix
epoch with floor division, so an instant before the epoch (a badly set clock) maps
monotonically rather than folding two buckets into one.

A declaration fires **once per bucket across the whole deployment**, with jitter.
Every instance firing at exactly 00:00:00 is a self-inflicted thundering herd.

`registry.h` asserts the whole table at once: every offset and jitter inside its
period, and every declaration naming a kind that exists. A recurrence naming a
kind that does not exist fires nothing, forever, in silence — which is the worst
shape a scheduling bug can take.

## 6. Delayed jobs

Future-dated work goes into a sorted set, and a **promoter** moves due entries
into the stream.

The promoter holds a Redis **lease** rather than running on every instance, so
promotion happens once. The lease is renewed by a heartbeat rather than taken for
the expected duration up front: a promoter that dies mid-pass releases its lease
in seconds instead of blocking promotion for however long the pass was expected
to take.

The lease is never relied on for correctness. The promote script is atomic on its
own, so two promoters produce one stream entry per job anyway — the lease only
stops the duplicated work.

### Backpressure, never a trim

Ten thousand jobs coming due in the same second must not all materialise in the
stream at once. Two ways to bound that, and only one of them is acceptable:

- **`MAXLEN` on the stream** bounds memory by **dropping jobs**, silently. A queue
  that loses work to stay small is not a queue.
- **Refusing to promote** above a high-water mark leaves the due entries in the
  ZSET, which is where they are durable anyway. The work is *delayed*, not lost,
  and it drains over the next few ticks.

So the promoter stops promoting above `kStreamHighWater` and moves at most
`kMaxPromotedPerTick` entries per pass. The stream is allowed to grow while
consumers fall behind — dropping a job to bound memory would be strictly worse
than the backpressure it is meant to avoid.

## 7. Which pool

A sweep that walks the filesystem and a fan-out that writes a few hundred
documents have different costs and must not share a queue. Each kind names its
pool.

One export on `db_pool` holds a database connection for the length of the whole
job, which is why that is a per-kind decision rather than a global one. See
[`00-architecture.md`](00-architecture.md) §3.

## 8. Shutdown order

On `SIGTERM`, the order is a correctness requirement:

1. **Stop claiming**, so nothing new is posted to a pool.
2. **Stop the subscribers**, so nothing new is queued to a connection.
3. **Drain the pools** — where in-flight jobs finish and release their leases and
   database clients.
4. **Quit the framework.**

Draining before stopping the queue would let a claimer post work onto a pool that
is already shutting down. Destroying the queue before the pools drain is a
use-after-free in a task that still holds it.

Each claimer parks in `XREADGROUP BLOCK` holding one Redis connection for its
whole block period, so the connection pool must be sized for them on top of the
request path — otherwise a request-path Redis call queues behind a blocked
claimer.
