# 17 — Analytics

## 1. Two faces, one subsystem

Analytics answers two questions that look unrelated and are not:

| | Question | Read by | Lives for |
|---|---|---|---|
| **Metrics** | Is the system healthy right now? | a scrape, every few seconds | as long as the process |
| **Events** | Did the thing we built get used? | a query, days later | a retention window |

They are one subsystem because they share the three rules that are hard to get right — a
bounded memory cost decided at compile time, a shedding policy that says what may be lost, and
the rule in §2 — and a rule stated in two documents is a rule that drifts. They are separate
*code* in separate link targets, for the reason [`00-architecture.md`](00-architecture.md) §2
gives: a counter is incremented inside every layer, so the cell array cannot live above the
layer that increments it.

Neither face holds an application's data. The metric names, the event names and the label value
spaces are all the application's, arriving through the seams in
[`01-seams.md`](01-seams.md) §11 and §12. anvil ships one table it populates, and §3 defends it.

## 2. The rule this exists to satisfy

[`00-architecture.md`](00-architecture.md) §9 has stated the requirement since Phase 0, and the
whole of §4 below is written against it:

> **A log line per occurrence is not a metric.** Anything that can happen once per request must
> be counted and reported periodically, never logged per event: under the load that makes it
> fire, a per-event line is itself the outage.

That is a statement about *cost*, and it has a corollary the implementation cannot compromise
on: **an increment must cost no allocation, no lock, and no contended cache line.** A counter
that is cheap at ten requests per second and expensive at ten thousand is a counter that
switches itself off exactly when it is being read.

The same sentence bounds the second face from the other end. An event is a row, and a row per
request is `N × request_rate` inserts against the cluster the request path is already using.
§9 and §13 carry the bounds that keep analytics from taking down the product it measures.

## 3. The metric table, and the one table anvil populates

The application declares its metrics. anvil declares its own, in
`analytics/internal_metrics.h`, and that needs defending against CLAUDE.md §1 — *a `constexpr`
table anvil ships is machinery; a `constexpr` table anvil populates is a bug*:

```cpp
inline constexpr std::array<MetricSpec, 12> kInternalMetrics{{ ... }};
```

The test is whether a name would have to change if the application changed.

```
anvil_audit_rows_dropped_total        anvil_pool_queue_depth
anvil_transactions_aborted_total      anvil_mongo_pool_wait_microseconds
anvil_authz_cache_hits_total          anvil_stealth_denials_total
anvil_ttl_collection_rows             anvil_orphan_files_swept_total
anvil_idempotency_claims_total        anvil_rate_limit_decisions_total
anvil_notifications_shed_total        anvil_notifications_outbox_rows
```

None of them would: every one names a mechanism that lives in this repository.
`checkout_completed_total` would, and that is the line.

The table is APPEND-ONLY in practice, for a reason §7 makes exact: its order is the scrape
order, and two scrapes of an unchanged registry are byte-identical so that an operator can
diff them. A row inserted in the middle rewrites a file somebody compares.

One of the twelve has a label whose values an APPLICATION chooses:
`anvil_ttl_collection_rows` is labelled by collection, and the value space is the
application's own collection table filtered to the collections whose rows have a lifetime.
That keeps the rule intact — the space is still `constexpr`, so the series count is still a
compile-time number — while letting anvil count rows in collections it does not name. It is
also the one series in the table an application can push past `kMaxCellsPerMetric`, which it
does as a build error with the ceiling named in it. A build that excludes an optional
module still declares its series and reports zero, which is the right answer — a metric that
disappears from a scrape is indistinguishable from a collector that broke. The `anvil_` prefix is enforced in **both** directions — the application's
table may not use it, and anvil's table may not omit it — so the two can never collide and a
reader of a scrape always knows which side a series came from.

Four of the twelve exist because their absence once hid, or would hide, a real failure, and
[`00-architecture.md`](00-architecture.md) §9 says why: **audit rows dropped**, **MongoDB
transactions aborted**, **rate-limit decisions answered locally** and **notifications shed**
are all zero in a healthy deployment and none of them is visible from outside. A dropped audit
row produces no error response, a correctly retried transaction produces none either, a limiter
that has fallen back to a per-process bucket answers exactly the same status as one that has
not — while every limit in the deployment is now N times looser, once per instance — and a
notification the storm breaker dropped produces nothing anywhere at all. That last one is the
reason the storm breaker shipped with its counter rather than acquiring one afterwards
([`11-notifications.md`](11-notifications.md) §12).

**`anvil_notifications_shed_total` is not labelled by topic, and that is the same refusal the
rate limiter's bucket gets.** A topic table arrives as a `std::span` handed to a service rather
than from the config header anvil reads at compile time, so its value space is not `constexpr`
and could not be closed even if the name were anvil's to use. The label it carries instead is
the OUTCOME — `dropped` or `deferred` — which is one ratio inside one family rather than two
counters that can disagree about the denominator, and it answers the question an operator
actually has: how much of what we shed was actually lost. Per-topic attribution lives on the
notification row, bounded by a retention window rather than by resident memory (§6).

## 4. Cells: sharded, padded, relaxed

A cell is one `std::atomic<std::uint64_t>` — a counter or a gauge is one cell, a histogram
series is several (§5) — and the registry holds them all in
**one heap allocation made at construction** — sized from the two tables, never grown, never
rehashed. There is no map, because a lookup by name on an increment path is a hash of a string
that was already known at compile time.

```
series index  =  metric_base[metric] + Σ label_index[i] × stride[i]
cell index    =  series_base[series] + slot        // slot is 0 for a counter or gauge,
                                                   // the bucket / sum / count for a histogram
cell address  =  cell_base[cell] + shard × kCacheLine       // shard is always 0 for a gauge
```

**A counter is sharded; a gauge is not**, and conflating the two is the easiest way to make this
whole subsystem report nonsense. A counter accumulates, so N shards summed on read is the
counter — that is what sharding is *for*. A gauge is a **sampled current value**: queue depth,
resident rows, pool wait. Summing sixteen shards of a gauge reports up to sixteen times the
depth that exists, and it does so silently, in the one direction an operator would act on.

So a gauge is one cell, written with `store(relaxed)` and read with `load(relaxed)`. It is
set-only: anything you would want to increment and decrement is two counters whose difference is
the answer, which also survives a process restart honestly where a decremented gauge does not.
Every internal metric in §3 that is a gauge is sampled at collection, which is what makes
set-only the natural shape rather than a restriction.

Three properties of the sharded path, and each is load-bearing:

- **The index is computed, not looked up.** The metric's base offset is a compile-time constant
  and the label indices are `enum` values. An increment is an add, a multiply and a
  `fetch_add(std::memory_order_relaxed)`.
- **Shards are padded to 64 bytes** so two threads incrementing the same series never write the
  same cache line. False sharing on a hot counter costs more than the thing being counted —
  which is how a metric becomes the reason the number it reports is bad.
- **The shard is the thread's, not the caller's choice.** A thread-local index assigned at first
  touch, `% kShards`. Relaxed ordering is correct because nothing depends on the counter's value
  ordering against anything else; a snapshot reads a value between the first and last
  observation, which is what a monotonically-increasing counter means.

`kShards` follows hardware concurrency, rounded up to a power of two, capped. It is a
memory-versus-contention trade and the cap is the memory half: see §6.

## 5. Histograms: fixed buckets, no quantiles

A histogram is `n + 3` cells — one per bucket boundary, one overflow, one `_sum` and one
`_count`. The boundaries are in the `constexpr` table, at most twelve, strictly increasing.

**An observation is three adds, not twelve.** It increments the one bucket the value falls in,
plus `_sum` and `_count` — and the cumulative form OpenMetrics wants is computed at snapshot,
by a running total over twelve cells that are already being read. Incrementing every bucket at
or above the value is the obvious implementation and it is the wrong one: it costs twelve
atomic writes across twelve cache lines on the hot path, to save an addition on a path that
runs once per scrape. §2's rule is a rule about the increment, and a histogram is where it is
easiest to break.

The bucket search is a linear scan of at most twelve `std::int64_t` in one or two cache lines,
which beats a binary search at this size and has no branch misprediction worth naming.

anvil computes **no quantiles**. A p99 computed per process is a p99 of that process, and
averaging them across N instances produces a number that is not a percentile of anything. The
scrape emits buckets; the thing that aggregates across instances is the thing that can compute
a quantile, and it is not this process.

Bucket boundaries are integers in the metric's declared unit — `std::int64_t` microseconds,
bytes or rows — never `double`. A floating-point boundary makes two processes disagree about
which bucket a value landed in, and a bucket count that differs by one between instances is
indistinguishable from a real signal.

## 6. Cardinality is a compile-time memory bound

An unbounded label space is `series × shards × 64` bytes of resident memory chosen by whoever
can reach the increment. This is the security risk in the metrics half, and the answer is three
layers deep because the first one is the only one that actually holds.

**First: the value space is `constexpr`.** A `LabelSpec` carries the *entire* set of values the
label may take, so `well_formed()` can multiply them:

```cpp
// Π |values| over the metric's labels, summed over the table.
static_assert(metric_table_is_well_formed(kMetrics));
```

It refuses a metric above `kMaxCellsPerMetric` (64) and a table above `kMaxCells` (1024).
**The ceiling counts cells, not metrics**, because a histogram series is `buckets + 3` of them —
a metric with twelve buckets and four label values is sixty cells, and a ceiling that counted
it as four would be a ceiling that does not bound anything.

**The registry's memory cost is therefore a compile-time constant:**

```
bytes = Σ over cells  (gauge ? 1 : kShards) × 64
      ≤ 1024 × 16 × 64  =  1 MiB
```

stated in the header, and a cardinality explosion is a build failure rather than an OOM at 3am.
1024 cells is roughly three times what a substantial application needs — twenty counters, ten
gauges and five histograms come to a few hundred — and the right response to exceeding it is
almost always to reduce a label's value space rather than to raise the ceiling. Raising it is a
one-line change with the memory cost written beside it, which is the point of having the formula
there.

**Second: the API cannot be handed a request byte.** `observe()` takes label values as *indices*
into the declared value space, produced by an `enum`. There is no `std::string_view` overload,
not as a convenience and not as an escape hatch. A function that cannot accept a request byte
cannot be made to accept one by a refactor that was not thinking about metrics.

**Third: `std::to_string` is banned under `src/analytics/`**, by
`tools/check-source-bans.sh`. It is the shape of every cardinality bug that has ever shipped — a
label value built from a number is one change away from a label value built from a request — and
a ban is greppable where the reasoning is not.

The operational half belongs here too. **The scrape endpoint is permissioned and
CIDR-restricted**, via the application's route table and `METRICS_SCRAPE_CIDRS`. Queue depths,
cache hit rates, denial counts and transaction aborts are a live map of where the system is
weak and when it is weakest; they are exactly what an attacker would collect first.

## 7. Export, and the route the application declares

A pull scrape needs no periodic task at all: each counter's shards are summed on read and each
gauge is read directly, into one buffer the caller reserves. Summing on read rather than
maintaining a running total is what keeps the write path to a single uncontended add.

```cpp
inline constexpr std::string_view kOpenMetricsContentType =
    "application/openmetrics-text; version=1.0.0; charset=utf-8";

void append_openmetrics(std::string& out, const Snapshot& snapshot);
```

The writer appends into one caller-owned `std::string` after a single `reserve()`, exactly as
[`http/json_writer.h`](../include/anvil/http/json_writer.h) and `http/csv_writer.h` do, and for
the same reason: a serialiser that allocates per field allocates once per series, and there are
a thousand of them.

**anvil registers no route**, here as everywhere. It ships `ScrapePolicy` — the permission bit,
the CIDR set and the content type — and the application mounts it, declares it in its route
table like any other endpoint, and gets the fail-closed treatment from
[`04-access-control.md`](04-access-control.md) for free.

Two snapshots of an unchanged registry are **byte-identical**, including series order. That is
not tidiness: it is what lets an operator diff two scrapes and see only what moved.

## 8. The event table

```cpp
using EventCode = std::int32_t;                 // STORED. Append only, forever.
enum class EventClass : std::uint8_t { Behaviour, Conversion };
```

`EventClass` plays exactly the role `AuditClass` plays in
[`01-seams.md`](01-seams.md) §9, and for exactly the same reason: **it is the load-bearing
column, not a label.** It decides what may be lost, anvil cannot infer it, and getting it
backwards means a flood of page views evicts the one signup it was hiding.

`EventCode` is stored. It joins the list of integers this library can never renumber — the
locale index, the permission bit, the namespace index, the field-type code, the audit action
value and the notification template id — and so does the index of a value within a dimension's
allow-list, because that index is what a stored row carries instead of a string.

A dimension is a small, closed set declared beside the event: `{"plan", {"free","pro","team"}}`.
The same reasoning as §6, for the same reason — an event carrying a free-text dimension is a
collection whose index cardinality is chosen by a visitor.

## 9. Ingest: classify, coalesce, bound

`analytics/buffer.h` is `audit/buffer.h`'s policy a second time, and it is deliberately a
**second type rather than a shared one**. The two sinks have the same shape and different
stakes: an analytics flood must never be able to evict an audit row, and a shared buffer is one
refactor away from letting it.

1. **Classify.** A `Conversion` is never dropped while a `Behaviour` row is buffered. The buffer
   evicts oldest-behaviour-first, and refuses a conversion only when it holds nothing else.
   That is counted as its own thing rather than folded into a drop total, because it is the
   state worth alerting on. **It is not assumed to mean the database is gone** — the audit
   sink's comment made exactly that assumption, and a load run falsified it. The paragraph
   after this list is about what follows from that.
2. **Coalesce.** Consecutive behaviour rows sharing `(session, code, dimensions)` fold into one
   row carrying a repeat count. A refresh storm is one row, and a count *is* the rate.
3. **Bound.** Coalescing removes most of the pressure the bound would otherwise be sized for.

Then one `insert_many` per flush interval on `analytics_pool`, never one insert per event.
Batching is the difference between analytics costing a round trip per request and costing a
round trip per second.

**A refused batch is re-admitted, not discarded**, and this was the correction the audit sink's
shape needed rather than a copy of it. `AuditService::post` drained the buffer, tallied the
classes it was carrying, and — if `try_post` refused — counted the loss and dropped the batch.
Rows that the classify-and-shed policy had just protected were then lost wholesale, because the
policy governed admission to the buffer and stopped at its boundary. The comment beside that
code reasoned that a refusal means the database has been unreachable for minutes; a load run
measured 8,485 refusals against a live, busy `mongod`, which is the case that reasoning
excludes. **That sink has since been corrected to this shape**, `AuditBuffer::readmit` beside
`EventBuffer::readmit`, and the load gate now drives both.

So a refused flush here returns its `Conversion` rows to the buffer and charges the
`Behaviour` rows as the drop. The buffer is already bounded, so the re-admission cannot grow
without limit; if the conversions do not fit either, they are dropped and counted against their
own class, which is the honest end of the policy rather than a hole in the middle of it.

**And a refusal ARMS A LATCH, because re-admission on its own is a storm.** The rows come back,
`offer()` posts a flush the moment the buffer crosses `kBatchRows`, and the buffer is now above
it — so with a queue that stays full, every subsequent event drained the whole buffer, copied it
into a task, was refused, and put it back, with a `LOG_WARN` each time. Three allocations and a
log line per event, at event rate, in the state where the process is already behind: the second
of those is exactly what [`00-architecture.md`](00-architecture.md) §9 forbids by name.

While the latch is armed the flush is SKIPPED for as long as the pool reports itself saturated,
and the division of labour is the one `BoundedThreadPool::saturated()` documents for itself:
`try_post` is the authority and one refusal is what tells us the queue is full; `saturated()` is
advisory and is what stops us asking again until the answer can be different. Paying that one
refusal rather than consulting `saturated()` first is also what keeps the refusal path — where
the re-admission lives — reachable by a test at all.

Nothing is lost by waiting. The rows sit in a buffer that is already bounded and already sheds
by class, so the loss happens at the bound, where the policy is, rather than at every refused
flush. The 1 Hz timer is deliberately NOT gated on the latch: it is what retries when no new
event arrives to retry for, and at one attempt per second its cost is nothing beside a
per-request path. Both sinks carry the identical shape, and both expose `deferred_flushes()` so
the load gate can assert that neither number moved.

**The flush task captures no `this`.** A task `analytics_pool` has already accepted runs after
`post()` returns, and nothing keeps the sink alive until then — so the task carries copies of
the two repositories, the retention and the batch, and touches no member at all. A repository
is a string and a view, so the copy is cheaper than the lifetime rule it replaces. This was not
reasoned out in advance: the load gate in §17 reported it as a stack-use-after-return the first
time it drove a sink hard enough to leave a flush in flight.

The same capture was in `AuditService::post`, where it was safe only because an `AuditService`
is a `main()`-level object that outlives `Pools::shutdown()` — an ordering rule in one
application's `main()` standing in for a property of the class. Both are fixed, and so is the
third owner of a `this` neither of them had noticed: the periodic flush **timer**, which both
sinks installed and neither took out, so a destroyed sink left a timer firing on freed memory
every second for the life of the loop. `stop()` invalidates it now, and both headers state the
contract that makes that reliable — call `stop()` from the loop thread, or before the loop
runs.

## 10. `analytics_pool`, and why `audit_pool` was not reused

`audit_pool` exists for one stated reason: so a saturated request path cannot starve the record
of what saturated it. Putting a second writer on it re-creates precisely the problem it was
created to solve — and the second writer is the one that floods, because an analytics event
fires on requests that are not interesting enough to audit.

So there is a fifth pool. It is small and fixed; its queue holds *batches*, not rows; and when
it is full the sink sheds and counts, like every other bounded queue in this library
([`00-architecture.md`](00-architecture.md) §3).

This costs `PoolSizes` two members, which is an API and aggregate-initialisation break for every
consumer — called out per CLAUDE.md §9.3 rather than slipped in.

The renderer's boot-time load failures and the migration runner's progress are both counted
here rather than logged, for the reason in §2.

## 11. Sessionisation without an address

A visitor id is a keyed, day-rotating HMAC:

```
visitor = HMAC-SHA256(ANALYTICS_VISITOR_PEPPER, packed_address ‖ day)[0:16]
```

Five properties, in the order they matter:

- **The address never reaches a row.** Not truncated, not hashed without a key — an IPv4 address
  is a 32-bit input space, and an unkeyed digest of one is reversible by anybody holding a
  database dump in the time it takes to enumerate it. The pepper is what makes the derivation
  one-way in practice, and it lives in a `SecretBuffer` like every other secret
  ([`14-config.md`](14-config.md)).
- **The input is the address and the day, and nothing else.** A user-agent string looks like
  free entropy and is not: it makes a browser update a new visitor, which inflates the count
  the measurement exists to produce, and it widens the input beyond what a keyed digest of a
  32-bit space needs.
- **It rotates daily**, so the same visitor on two days is two ids. That bounds what the
  collection can be used to reconstruct to a day, which is also the window the product question
  needs.
- **IPv6 is coarsened to /64 before it is hashed; IPv4 is not.** `PackedAddress` is sixteen
  bytes with v4 mapped into it, so the naive implementation hashes all of them — and privacy
  addressing rotates the v6 interface identifier, which mints a new visitor for the same person
  several times a day and inflates exactly the number this exists to produce. /64 is the
  smallest block a site is assigned, so it is the coarsest cut that does not merge households.
  Going further, or coarsening v4 at all, merges everyone behind one NAT into a single visitor
  and deflates it instead.
- **A session is a `(visitor, day)` upsert**, and the unique key *is* the sessionisation —
  there is no read-then-write, so N instances converge without coordination
  (CLAUDE.md §6). The pair is the document's **`_id`**, not a unique secondary index: a
  compound `_id` is already unique, so the collection carries no secondary index for its write
  path at all, and the upsert is a primary-key write. Whether the call CREATED the row comes
  from the server's own `upsertedId` rather than from a read that raced another instance
  asking the same question.

`PackedAddress` comes from [`http/client_address.h`](../include/anvil/http/client_address.h),
which already resolves the true client behind the trusted proxy set. Analytics does not re-do
that, and must not: two implementations of "who is the client" is one of them being wrong.

## 12. Consent, retention, erasure

- **An event declaring `requires_consent` is not recorded without consent.** Not recorded and
  then filtered, not recorded pseudonymously — refused at `offer()`, before the buffer. The test
  for this reads the collection back rather than trusting the path that refused it.
- **Retention is a TTL index**, and per [`09-mongodb.md`](09-mongodb.md) §6 a TTL index is a
  garbage collector and not an access control: the monitor lags by up to a minute, so **every
  query against `analytics_events` and `analytics_sessions` also filters on `expires_at`
  explicitly.** `tools/check-db-discipline.sh` fails the build over a query that forgets.
- **Erasure is one `delete_many` on `subject`**, riding a *partial* index that anonymous rows
  never enter — so the index costs nothing for the rows that are the overwhelming majority, and
  the erasure path stays a point query rather than a scan.
- Rollups carry no subject and are not erased. That is the trade being made explicitly: a count
  of signups per day is not personal data, and rebuilding history after every erasure request is
  a cost with no beneficiary.

## 13. Sampling that keeps a funnel whole

Above a high-water mark the sink samples — and it samples **deterministically per session**:

```
keep = (xxh3(session) % ANALYTICS_SAMPLE_DENOMINATOR) == 0
```

A session is therefore kept whole or dropped whole. Per-event sampling keeps a random half of
every session, and a half-observed funnel is worse than an unobserved one: it reports a drop-off
that is an artefact of the sampler, and there is no way to tell it from a real one afterwards.

The sampled-out count is itself a counter, so the true rate is recoverable by multiplication.
Sampling that loses the denominator loses the measurement.

## 14. Rollups, and why an idempotent rollup cannot `$inc`

A rollup is a recurring job ([`10-timer-jobs.md`](10-timer-jobs.md)) that reads one closed time
window of raw events and writes one document per `(code, bucket, dimensions)`.

Its `_id` **is** the identity tuple — `(code, granularity, bucket, dimensions)` as a compound
`_id`, the `sections` precedent, where two documents per key under a compound `_id` mean the
collection carries no secondary index for the write path at all. A compound `_id` rather than a
digest of one, because it is already unique, it needs no collision argument, and it is legible
in a shell. Field ORDER inside it is load-bearing — the server compares subdocuments
field-by-field — so it is built in exactly one place. That is what makes the write a
primary-key upsert, and the upsert is what makes the job re-runnable:

> **A rollup that `$inc`s is not re-runnable.** Every queue in this system is at-least-once
> (CLAUDE.md §6), so a rollup that adds to what it finds double-counts the first time a worker
> is reclaimed after a lease expiry — and the resulting number is wrong in a way nothing
> reports and nobody can reconstruct.

So a rollup **recomputes its window and `$set`s the result**. Running it twice produces one
document with identical values, which is the only assertion that proves the property.

Windows are closed before they are rolled: a bucket is computed only once `now` is past its end
plus a grace margin, because a row inserted late into an already-rolled bucket is a row the
rollup will not see. The grace margin is the flush interval plus the clock skew the deployment
tolerates, and it is stated rather than guessed.

## 15. Querying: rollups only, bounded, indexed

Reads hit **rollups**, never raw events. A dashboard query over raw rows is a scan of the
highest-volume collection in the system, issued by whoever can open the dashboard.

```cpp
Result<std::vector<Bucket>> counts_over_time(EventCode, TimeRange, Granularity);
```

Bounded by a `limit` like every other read (CLAUDE.md §7), paginated by the indexed bucket key,
never `skip(n)`. The raw collection has exactly two readers: the rollup job, and the erasure
path.

Storage layout is in the app's table, and the reference app puts the two high-churn collections
in the **second** database — which is what that database was declared for. A collection turning
over its whole contents every few days, kept beside hot application data, spends WiredTiger
cache on rows that are about to expire.

## 16. What was rejected

**A Prometheus client library.** It brings a registry keyed by string, a label map allocated per
observation, and a cardinality model that is a runtime concern. The three properties §4 and §6
are built on — computed index, compile-time memory bound, no `string_view` overload — are all
things such a library exists to *not* require.

**Per-request timing as a log line.** Rejected by §2, which is a rule and not a preference.

**Quantiles in-process.** See §5: a p99 per process averaged across instances is not a
percentile.

**An events collection queried directly by a dashboard.** See §15.

**Reusing `audit_pool`.** See §10.

**An `analytics` route shipped by anvil.** anvil registers no routes, here as everywhere; the
scrape endpoint is the application's, declared in its route table like everything else.

**OpenTelemetry.** A tracing model, a context propagation format and a wire protocol, none of
which the two questions in §1 need, and all of which would have to be carried by every
application that links anvil. An application that wants traces can have them; it does not need
them in this library to do it.

Three things were refused there and only one is expensive. The model and the protocol stay
refused; the CONTEXT shipped, because it crosses boundaries an application cannot reach into
from outside — `guarded()` and the job envelope are both inside this library. §18 is what was
built.

**A second index over a rollup's dimension slots.** The slots are an array, so an index over
them is multikey — and a multikey index cannot provide a sort on a key that follows it, which
is exactly what the bucket ordering needs. The planner correctly refuses such an index, so
shipping one would cost a write per rollup document to serve a plan nothing chooses. The
dimension-narrowed query applies its dimensions as a residual over a range that is already
bounded and limited. `query_catalogue_db_test` is what caught this: it asserts that every
declared index is ridden by a declared query, and this one was not.

## 17. The load gate

One CTest entry, labelled `load`, excluded from every default preset because it saturates pools
on purpose. **It is not a benchmark suite and it measures no latencies.** It exists because two
real defects in this library were invisible to 1,088 passing tests, and neither of them is a
timing property — so neither is the flaky kind [`16-test-plan.md`](16-test-plan.md) rules out.

| Asserted | Why load is the only way to falsify it |
|---|---|
| No flush is refused while the database is answering — on the analytics sink | The claim the audit sink's comment made, and the one a load run measured 8,485 counter-examples to |
| No flush is refused while the database is answering — **on the audit sink** | The 8,485 were audit batches. A gate that drove only the analytics sink was proving the claim against the half that shipped with the correction already in it |
| A full queue sheds rather than grows | An unbounded queue converts a flood into an OOM kill, and a queue that grows looks identical to one that does not until it is full |
| The registry stays inside the ceiling its own header states | Under a sanitiser the process's RSS measures the sanitiser, so what is asserted is the arena the registry allocated — which is what the ceiling is about |
| The counters that are zero in a healthy deployment are zero | Both are invisible from outside: a dropped audit row produces no error response, and a correctly retried transaction produces none either |

It sizes **its own pools**, and the first run is why. `tests/app_fixture.h`'s are deliberately
tiny — a queue of one, so shedding is observable — which is right for the suites asserting that
a full queue sheds and wrong here: sixteen thousand events in under a second outrun four
batches of headroom whatever the sink does, so the assertion would have measured the fixture.

It then found the `this` capture described in §9, which is the whole argument for having it.
The audit case was added afterwards, when fixing that sink made it obvious that the gate had
never driven the sink the measurement came from.

## 18. Trace context

**anvil owns context. The application owns export.** That line is the whole design, and §16 is
where the other half of it was refused: no tracing model, no wire protocol, no exporter. What
ships is [`http/trace_context.h`](../include/anvil/http/trace_context.h) — a parser, 25 bytes of
ambient state, and the two boundaries an application cannot thread an id across by itself.

### 18.1 `traceparent` only

```
traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
             vv trace-id, 16 bytes              span-id, 8 bytes  flags
```

Fixed at 55 bytes: four fields, lowercase hex, three hyphens at known offsets. No list, no
quoting, nothing to size — it parses with a length compare and a 256-byte table lookup per
nibble and allocates nothing, which is the same shape of parser as `auth::decode` and the
reason it is acceptable where a JSON parser was not ([04](04-access-control.md) §4).

`tracestate` is **dropped on the floor and not forwarded**. It is a comma-separated list of up
to 32 vendor entries with its own grammar and per-vendor key rules — the only variable-length
part of the standard — and nothing here would read a byte of it. Parsing it would buy a class
of parser bug on the request path in exchange for nothing.

Three parse rules carry weight rather than pedantry:

| Rule | Why |
|---|---|
| An all-zero trace-id or span-id reads as absent | A client sending zeros would otherwise mint one trace id shared by every request in the fleet, and the first person to debug that would be debugging the aggregator |
| An unknown version parses as version 00 over the first 55 bytes | Refusing unknown versions outright makes every deployment running today's build the reason a future version cannot be rolled out. A longer header is read only when byte 56 is the separator that proves the extra bytes are a new field |
| Lowercase hex only | One trace with two spellings costs an aggregator a join that silently misses half its rows |

The parsed value is **24 bytes of binary, never the string**. Once the id is
`std::array<std::uint8_t, 16>` there is no injection surface left downstream — no log
injection, no Redis key separator, no BSON field name — and no accessor anywhere hands out the
header's own bytes.

### 18.2 Who may set it

`traceparent` is a header, so on any request reaching the process directly it is whatever the
client typed. The proposal this section replaced spent a paragraph on `tracestate` being the
attacker-controlled part of the standard and then read `traceparent` without asking who sent
it. Both are headers.

Believing one unconditionally is not the harmless default it looks like. A chosen trace-id
collides with a real trace in the aggregator; a chosen `sampled` bit forces export on traffic
the deployment decided not to sample; a fresh id per request is unbounded cardinality in
whatever stores them. None of those is a hole in this process, which is why the question is
easy to skip — and every one of them is a hole in the thing the traces are for.

So it is the rule [`client_address.h`](../include/anvil/http/client_address.h) already applies
to `X-Forwarded-For`: consult the header only when the **peer** is a configured proxy. A
deployment with no trusted proxies is the edge, and the edge believes nothing.

```cpp
anvil::http::install_request_scope(anvil::http::TraceIngest::TrustedPeer);
```

`TraceIngest::Off` is the default, and a deployment that does not trace pays one comparison per
request: no header lookup, no proxy-list load, no parse. There is deliberately **no third
policy** for "believe any client" — a deployment whose callers are its own services has them
behind the proxy list by construction, so the only thing a third policy adds is a way to spell
the mistake.

### 18.3 Where it lives, and what it cost

In `RequestScope`, which grows from 88 bytes to 112 — in the allocation that already happens
per request, not in a second attribute. `core/user_context.h` measured that cost: a second
entry in Drogon's attribute map is a string hash, a map node and a control-block allocation on
every request, traced or not.

Folding it into `UserContext` was rejected. That struct is exactly 64 bytes and one cache line,
both asserted, and it is consulted on every protected request as a *security* value; growing it
to carry telemetry costs a second line on the hottest path in the system.

### 18.4 Propagation: one choke point

`guarded()` wraps the body of every task posted to every pool
([`core/thread_pools.h`](../include/anvil/core/thread_pools.h)), so it is the single place a
context has to be captured and restored — and because it is already mandatory, there is no way
to post a task that escapes it. It samples the ambient context **at post time**, on the posting
thread, which is the only place that still knows which request the work belongs to.

Storage is one thread-local `TraceContext`. `TraceScope` restores the previous value on the way
out, including on the catch path: a worker thread runs many tasks, and one that carries no
trace must not inherit the last one that did.

**The application writes one line**, at the top of its handler:

```cpp
const anvil::http::TraceScope scope{anvil::http::trace_of(req)};
```

That line is the application's because Drogon has no advice that *wraps* a handler — the
pre-handling advice returns before the handler runs, so a scope installed there would be gone
by the time there was anything to correlate. Everything anvil posts to a pool from inside it
carries the context.

### 18.5 Jobs: a link, never a parent

A job is at-least-once, may be reclaimed after a lease expiry, and may run days after it was
enqueued. A span covering the publish and Thursday's retry is not a trace. So the envelope
carries the enqueuing trace's id as a **link**, and each execution mints a new **root**;
`JobRunContext::linked_trace` is the one, `anvil::http::current_trace()` inside a handler is the
other. Getting this backwards is the most common mistake in traced job systems, which is why it
is written down rather than left to the next reader of `dispatch`.

The link is carried **only when the enqueuing trace is sampled**. An unsampled trace produces no
span, so a link to it names a trace with nothing in it — and carrying it only when sampled
means a link's presence says the enqueuer was sampled, which is what lets the execution decide
sampling without a flags byte travelling beside the id.

This is a change to a durable wire format, so the envelope has **two versions**
([`timer/queue.h`](../include/anvil/timer/queue.h)): version 2 carries the 16-byte link and is
written *only* when there is one, so an untraced deployment produces the same 32-byte envelope
it always did, byte for byte. That is not a size optimisation, it is the rollout — an older
worker rejects an unknown version, and a rejected envelope is ACKed and dropped, which is a lost
job rather than a retried one. Enabling ingest needs a parameter only a build with the version-2
decoder has, so a single-binary deployment cannot get the order wrong. **A fleet running two
builds at once must finish the upgrade before enabling ingest.**

### 18.6 Nothing ever goes back out

anvil writes no trace header on any response, and the rule is **absolute rather than
conditional**. [`accesscontrol/stealth.h`](../include/anvil/accesscontrol/stealth.h) enumerates
`WWW-Authenticate`, `Set-Cookie` and `X-Request-Id` as tells for the same reason: any header
present on a denied route and absent on an unmatched one is an existence oracle. An echoed
`traceparent` is worse than those, because it is the client's own value coming back — a client
that sends the same header to two paths can separate them by which one echoed.

"Echo it except on stealth routes" is a rule that survives until the first handler that sets it
directly, which is why there is no such rule. Trace ids flow **inbound**, and outbound to
services the deployment controls. They do not flow back to the client.

### 18.7 What was left out

**MongoDB `$comment`.** It can carry a trace id into the profiler, and it costs bytes on the
wire for every query on a path [09](09-mongodb.md) spends its whole length minimising.

**A trace id on every log line.** The proposal put this in the redacting logger. There is no
redacting logger in this repository — it is a rule §5 of CLAUDE.md states and nothing yet
enforces — so there was nowhere to put the field that would not have been a format string at
every call site. It waits for the logger.

**An exporter, a span model, and outbound emission.** §16, unchanged. `format_traceparent` is
here so that an application's outbound call writes the same 55 bytes anvil parsed rather than a
second formatter that can disagree with the parser about a leading zero; anvil ships no caller
for it.

## 19. An open entity dimension

§8 states the reasoning a dimension has always followed: a closed, `constexpr` set of values,
because a free-text dimension is a collection whose index cardinality is chosen by a visitor.
That reasoning holds for "which surface" and "which referrer". It does not hold for "which
project" — an application like [`anvil/entries`](20-entries.md) lets a project be added by a
staff member rather than a deploy, and a `constexpr` list of project slugs makes a project
uncounted until the next release appends it, and a renamed slug relabel every row of its own
history. The table cannot enumerate the value space, because the value space is not fixed at
compile time. It never was going to be a `constexpr` table; the question is what replaces one.

### 19.1 A kind, not a different mechanism

`DimensionSpec` gains a `kind`: `Enum`, the closed set every dimension has always been, or
`Entity`, an application-minted UUID. Everything else about a dimension stays — declared beside
the event, checked by `event_table_is_well_formed`, refused at the door if malformed — because
the only thing that actually differs is the value SPACE, not the shape a dimension has in the
table:

```cpp
enum class DimensionKind : std::uint8_t { Enum = 0, Entity = 1 };

struct DimensionSpec final {
    std::string_view                  name;
    std::span<const std::string_view> values;  // ENUM ONLY — empty for Entity
    DimensionKind                     kind = DimensionKind::Enum;
};
```

An `Entity` dimension's `values` must be empty; an `Enum` dimension's must not be. The two
disagreeing — a kind naming one shape and a value list naming the other — is exactly the state
`well_formed()` exists to refuse rather than pick a side of silently, so it is checked alongside
every other malformed table (empty name, duplicate value, oversized set). One event may declare
**at most one** `Entity` dimension, for a storage reason §19.2 makes exact: unlike the enum
dimensions, which share one four-slot array regardless of how many an event declares, the entity
value has exactly one slot of its own, so a second `Entity` dimension on the same event would
have nowhere to be stored.

### 19.2 Storage: a slot of its own, not an index

An enum dimension's stored form is the INDEX — one byte, because the row IS the index into a
list the reader already has. An entity dimension has no list to index into, so its stored form
is the id itself: 16 bytes, BSON `BinData` subtype 4, never a string and never an index into
anything (CLAUDE.md §2.3's rule for every UUID in this codebase, applied here for the first time
to something that travels beside `dims` rather than inside it).

`Event` and `RollupRow` each gain one `Uuid entity` field, `kNilUuid` meaning "not supplied for
this occurrence" — the same absent-slot idea `kNoDimensionValue` gives an enum dimension's byte,
chosen because a real id is never nil in practice. It is a field of its own rather than a fifth
`dims` slot, because a `Uuid` does not fit in the one byte a `dims` slot has, and growing every
slot to sixteen bytes to fit the rare case would nearly triple `dims` for every event that never
uses one. The cost lands only on the feature that uses it: `Event` grows from 44 bytes to 60,
`EventRow` from 56 to 72 — under a third more, and still under a megabyte for eight thousand
buffered rows (`analytics/buffer.h`) — while the wire and storage form OMITS the field entirely
when it is nil, so an ordinary row, and every row on disk before this feature existed, costs
nothing extra at all. An enum dimension's slot is written unconditionally, even absent, so the
rollup's equality filter never has to distinguish "no dimensions" from "field missing" (§14); an
entity id does not share that reasoning, because there is no array position to keep uniform — the
field is simply present or it is not, and both states are exactly as before this feature existed.

### 19.3 Admission: bounded cardinality is the application's, again

§6 makes the case once for a metric label and once for an enum dimension: an unbounded value
space reachable from a request is a cardinality explosion, and the fix is to make the space
`constexpr` so a build enforces the bound. An entity dimension cannot take that fix — the whole
point is that the space is NOT fixed at compile time — so the bound has to be enforced a
different way, at the only other point that can see every id before it is written: `offer()`.

```cpp
using EntityAdmission =
    std::function<bool(std::string_view dimension, EventCode code, const Uuid& entity)>;
```

Supplied through `IngestConfig::entity_admission`, and consulted as a new gate — the SECOND of
five, immediately after consent and before a visitor is even derived, because an id nothing has
vetted should not go on to cost a session or a sampling decision. `dimension` is the declared
name, so one hook serves every `Entity` dimension in the table by dispatching on it; `code` is
the event the id was offered against, for a hook that admits different ids to different events
under the same dimension name.

**Unset refuses every id.** This is deliberate and it is CLAUDE.md §5's "deny by default" applied
to a seam rather than a route: a table can declare an `Entity` dimension without anyone wiring up
`EntityAdmission`, because nothing at compile time can catch a runtime hook being missing — a
`std::function` is not a fact `static_assert` can see. The safe failure for an unbounded id space
nothing is vetting is to admit none of it, matching the direction `requires_consent` and an
unknown `EventCode` already fail in (§8, §9): every gate in this sink fails towards recording
less, never more.

**The hook must not block**, for the reason every gate in `offer()` must not: it runs on a
Trantor event-loop thread (CLAUDE.md §4). A real implementation therefore answers from an
in-memory set the application keeps current, never from a database read — and anvil already
ships the signal such a set is built from. [`20-entries.md`](20-entries.md) §6 gives
`EntryServiceConfig::on_invalidated(kind)` precisely so an application can refill a cache of its
own on the write path and on every other instance, and "is this UUID a currently-published entry
of this kind" is exactly the shape of question `EntityAdmission` asks. The two seams are
designed to fit together without anvil naming the fit: entries owns publishing a project,
analytics owns counting one, and the admission hook is the only line connecting them.

**The hook must not throw.** `offer()` is itself `noexcept`, so an exception escaping the
application's callable is caught at the call site and treated as a refusal rather than reaching
`std::terminate` — the same direction an unset hook already fails, so a throwing implementation
degrades to a refusing one rather than taking the process down. That is a safety net, not a
license: a real implementation should not rely on it, because a caught-and-refused admission is
indistinguishable, from the caller's side, from one that plainly said no.

### 19.4 The rollup, and a series query with two shapes

The rollup's identity tuple gains the entity id: `(code, granularity, bucket, dimensions,
entity)`, grouped exactly as an enum dimension combination is — two ids in one bucket are two
rollup documents, and re-running the job over the same window still `$set`s the same documents
it always did (§14 is otherwise unchanged; the entity id is one more field of the same `_id`).

A series query now comes in two shapes, mirroring the existing narrowed/unnarrowed pair for enum
dimensions:

```cpp
// Narrowed to ONE entity, across every bucket and enum-dimension combination.
Result<std::vector<Bucket>> counts_over_time_for_entity(
    mongocxx::client&, EventCode, const Uuid& entity, const TimeRange&, Granularity,
    std::int32_t limit = 512) const;

// GROUPED by entity: every id that appeared, each with its own total.
Result<std::vector<EntityCount>> counts_by_entity(
    mongocxx::client&, EventCode, const TimeRange&, Granularity,
    std::int32_t limit = 512) const;
```

`counts_over_time_for_entity` is `counts_over_time_for`'s counterpart: an equality filter on
`_id.ent` rides the same index `_id.dims` already answers a narrowed query from. `counts_by_entity`
is new in kind rather than degree — it is the "top projects" question, which has no enum-dimension
equivalent because the enum value space is small enough that a caller already knows every value
to ask for one at a time. It reads a window unfiltered, exactly as `counts_over_time` does, and
folds the rows by entity instead of by bucket — discarding rows carrying no entity at all, because
"no entity" is not an id worth reporting in a result whose whole point is a list of ids — returning
the highest count first so two reads of an unchanged window agree byte for byte. `limit` bounds
the rollup documents READ, exactly as it does everywhere else in this file; the entities returned
can never exceed it, because each row read contributes to at most one entity's total.

`Uuid` stays the raw 16 bytes through every one of these, as everywhere else in anvil — a caller
building JSON converts with `anvil::uuid::to_string`, the one place `uuid.h` itself draws that
boundary.

### 19.5 What was rejected

**A `std::string_view` value for the entity dimension.** The same refusal §6 gives an enum
dimension's `observe()`: a function that cannot accept a request byte cannot be made to accept
one by a refactor that was not thinking about cardinality. `Offer::entity` is a `Uuid` the
application already parsed and validated — with `anvil::uuid::parse`, the strict canonical-form
validator every other UUID boundary in this codebase already uses — so a malformed value is
refused at the SAME boundary an out-of-list enum value already is: in the application's own
request handling, before anything reaches `offer()`, with the application's own `ValidationFailed`
and no driver text anywhere near it.

**A database read inside `offer()`.** Considered and rejected for the same reason every gate in
this sink is synchronous: `offer()` runs on a Trantor event-loop thread, and `EntityAdmission`
blocking on `db_pool` would stall every connection that loop owns for the length of a query. The
seam is deliberately an in-memory predicate, with `entries::EntryServiceConfig::on_invalidated`
named as the signal a real one is built from, rather than a callback shaped to invite a query.

**Renumbering `dims` to fit a UUID.** Widening every dimension slot to sixteen bytes so an entity
value could live inside `DimensionValues` was rejected on the same memory argument §2 makes for
the whole subsystem: it would nearly triple the row for every event that never declares an entity
dimension, to save one field on the rare event that does. A dedicated `Uuid entity` field costs
nothing on the rows that do not use it, because it is omitted from the wire form entirely when it
is nil (§19.2).
