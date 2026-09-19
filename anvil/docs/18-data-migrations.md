# 18 — Data migrations

[`09-mongodb.md`](09-mongodb.md) §7 covers index migrations: a catalogue of `IndexSpec`, a
retired list, and a version marker written with `$max`. This document covers the other half —
moving the documents themselves — and what the index catalogue grew to need alongside it.

## 1. Why a migration is a separate invocation

The same sentence [`09-mongodb.md`](09-mongodb.md) §7 already makes, for a stronger reason. An
index build holds its collection
for the duration and every `db_pool` thread queues behind it; a data migration does that *and*
writes every document in the collection. Run at boot in a rolling deploy, it runs once per
instance, and the second instance starts its pass while the first is still going.

So `migrate` is a process that exits, run once, from one place. It is not a boot step, not a
job on the queue, and not a route. [`00-architecture.md`](00-architecture.md) §7 carries it as
a deployment invariant, and §4 below explains why the lock inside it is a safety net rather
than the thing that makes that invariant true.

## 2. Two ledgers, two questions

| | Records | Written by | Answers |
|---|---|---|---|
| `anvil_schema_meta` | one integer per database | `apply_migrations` | *do this database's indexes exist?* |
| `anvil_migration_ledger` | one document per step | `run_data_migrations` | *has this step finished, and where did it get to?* |

They stay separate and the integer is **not** shared, because they answer questions with
different shapes. A schema version is a high-water mark: `$max`, monotone, and two instances
writing the same value race harmlessly. A step is not ordered against other steps by a number
at all — it is done or it is not, and if it is not, it is somewhere. Folding a step into the
version counter would mean a partially-applied step is representable only as "the version has
not advanced yet", which is exactly the state the ledger exists to describe.

`applied_schema_version()` therefore answers after a data run exactly what it answered before.
That is asserted, because it is the regression easiest to ship and hardest to notice.

Both collections are **anvil's own**, named by anvil and absent from `config::kCollections`.
An application does not declare the mechanism's storage and cannot collide with it. Access is
`_id` equality only, so neither needs an index and neither appears in the query catalogue.

## 3. The step catalogue

The steps are the application's ([`01-seams.md`](01-seams.md) §13). anvil owns the cursor, the
batching, the ledger and the lock; a step owns only the transform.

```cpp
using StepFn = StepOutcome (*)(StepContext&,
                               std::span<const bsoncxx::document::view>) noexcept;
```

`db/migration_step.h` includes `<mongocxx/client-fwd.hpp>` rather than the client — the same
trick `timer/job_spec.h` uses, for the same reason: the table names a driver type, and
`anvil::foundation` must never see one. It does include the bsoncxx **value** headers, and it
has to: `StepFn`'s own signature names `bsoncxx::document::view` and `std::span` requires a
complete element type. "Client-fwd and nothing more" was written before the signature was, and
the part that was load-bearing — no connection type in a header the low layer can see — holds.

The table arrives as a `std::span` in `MigrationDeps`, not through a third config header.
`<anvil_app_jobs.h>` exists because `timer::kMaxLeaseSeconds` is *derived from the table inside
anvil's own compilation*, which is the "a size must be a size" exception; migrations derive no
such constant, so the ordinary mechanism applies. If one is ever needed,
`<anvil_app_migrations.h>` is the shape and the jobs seam is the template — written down here
so it is a decision rather than a rediscovery.

`StepContext` hands the step a `mongocxx::client&` for **reads** and an accumulator for
**writes**. It never hands it a collection handle to write through. §7 is why.

The accumulator takes the **fields** a document should carry — `set(id, fields)` and
`unset(id, fields)` — and the runner is what wraps them in `$set` or `$unset`. So §5's rule is
not a rule a step has to follow: there is no overload that takes an update document, and `$inc`
is not something a step is able to say.

## 4. The lock, and what it does not buy

The claim is one `find_one_and_update` on the ledger: match the step's `_id` with no live
lease, set an owner and an expiry, return the document. One atomic operation, never
check-then-act (ENGINEERING_RULES.md §6). A runner that does not win reports *held* and exits non-zero
rather than waiting — a second operator watching a migration "hang" is how two of them end up
force-killing the one that was working.

"No live lease" is spelled as an expiry in the past rather than as a null owner, and that is
not cosmetic: the filter is then one equality and one range, which an **upsert** can derive a
new document from. MongoDB builds an upserted document out of a query's equality conditions, so
a filter written as `$or` over three ways of being free derives nothing — and it reads as three
rules where there is one. A row that exists and is held fails the filter, so the upsert collides
on `_id`, and that duplicate key **is** the contention signal, arriving from one atomic
operation rather than from a read followed by a decision.

It is in **MongoDB and not Redis**, and that is deliberate. The ledger is in MongoDB; a lock
held in a store [`10-timer-jobs.md`](10-timer-jobs.md) §1 explicitly says is not the system of
record can disagree with the thing it is locking. A migration must also run when Redis is
down: it is the tool you reach for during an incident, and a dependency on the cache is a
dependency that fails at the worst moment.

Now the part that matters:

> **The lock is not what makes a step correct. Idempotence is.** The lock stops the wasted work
> and the write conflicts; it is never relied on for correctness — the same sentence
> [`10-timer-jobs.md`](10-timer-jobs.md) §3 writes about the promoter's lease, and it is true
> here for the same reason.

A lease can expire against a process that is alive but stalled — a paused VM, a partition, a
host that lost its disk for ninety seconds. Four mitigations, and the last is an admission:

1. Steps are re-runnable from any point, so an overlapping batch is re-applied rather than
   corrupted (§5).
2. **A step may not `$inc`.** That is the one write shape a concurrent overlap corrupts
   silently, and `tools/check-db-discipline.sh` fails the build over it rather than leaving it
   to review.
3. The lease is renewed at the **top of each batch**, in the same operation that records the
   previous one, and that operation carries the owner in its filter. So a runner whose owner has
   changed **fences itself and stops before it writes** rather than after — it writes nothing
   past that point, which is asserted by looking at what it wrote. There is no separate
   heartbeat thread: renewal and detection are the same write, which also means the lease must
   exceed the worst-case time one batch takes. `kDefaultLeaseSeconds` is 60; a step doing
   expensive per-document work raises both it and the batch size together.
4. A runner stalled *inside* a driver call cannot stop mid-call, so an overlapping `$set` batch
   can be applied twice. That is why `$inc` is banned rather than discouraged: with `$set` the
   double application is a no-op, and with `$inc` it is a wrong number nobody can reconstruct.

## 5. Idempotence is the correctness argument

Every step must produce the same result applied once, twice, or resumed from the middle. It is
the same contract the job handlers carry (ENGINEERING_RULES.md §6), and it is written as loudly here
because the failure is quieter: a job that runs twice usually shows up, and a migration that
runs twice over a batch boundary usually does not.

In practice that means a step writes what a document *should be*, never a delta from what it
found. `{$set: {status: derive(doc)}}` is re-runnable; `{$inc: {attempts: 1}}` is not, and
neither is anything whose new value is a function of the stored old one.

A step that genuinely cannot be expressed that way is a step that needs its own marker field on
each document, so re-reading tells it what it already did. That costs a field and it is the
honest price.

## 6. Resumable batching by `_id`

The runner walks `{_id: {$gt: last}}` in `_id` order, `batch_size` at a time, and records
`last` in the ledger — at the start of the *next* batch, and again when the step finishes.
Recording at the top rather than at the bottom is what makes §4's fencing work: the write that
records progress is also the write that renews the lease and detects a changed owner, so a
fenced runner learns it before it reads or writes anything else. Never `skip(n)` — it is O(n)
server-side, so the last batch of a large collection costs the most at the point the run is
most likely to be interrupted (ENGINEERING_RULES.md §7).

Resumption re-applies the batch that was in flight. That is not a rough edge to be tightened
later; it is the design, and it is what §5 pays for.

Two cursor shapes exist because two `_id` shapes exist, and the walk is the same for both —
`$gt` in `_id` order, since BSON ordering is total either way. What differs is what the runner
will accept and what the ledger can say about it:

- `IdRange` declares that `_id` is a 16-byte UUID, and the runner **refuses** a collection
  holding anything else rather than walking a prefix and reporting that it finished. When the id
  is a v7 it also records the millisecond that id encodes, which is the one progress figure an
  operator can read from `--status` without knowing the collection.
- `WholeCollection` accepts any `_id`; `sections`' compound key is the worked example. There is
  no time to derive, so the ledger records the cursor and nothing else.

The reference table exercises both, because a cursor proved against one shape is a cursor that
breaks on the other. A time-window *bound* on the query — `uuid::v7_boundary` as a second filter
— is deliberately not shipped: no CLI flag would select one, and an unused parameter on the hot
path of a migration is a parameter nobody has tested.

**A step that filters on anything other than `_id` needs an index and a `QuerySpec` entry, like
any other query.** A migration is not a request path, which is why the plain `_id` walk needs
neither — but a step selecting on `status` with nothing to ride collection-scans production at
the moment the cluster is least able to absorb it.

## 7. Dry run: the runner decides

`--dry-run` walks the same cursor, calls the same step function, and reports the same counts.
The difference is entirely on anvil's side of the seam: `StepContext` accumulates the writes
the step asks for, and the **runner** decides whether to execute them.

A `bool dry_run` threaded down into a write function is one `if` away from writing, and the
`if` is in a branch that by definition nobody exercises in the configuration where it matters.
A step that cannot write in dry-run mode cannot be made to write by a mistake in a step.

The test asserts the collection is byte-identical before and after.

## 8. Progress, failure, abort — and no down migration

A step reports a count per batch — never a log line per document
([`00-architecture.md`](00-architecture.md) §9). It goes into the ledger row and the run's
report rather than into the metric registry, and that is a correction to what this section
first said: `migrate` is a process that exits, so nothing is ever there to scrape a counter it
incremented. The ledger carries the cursor, the batches applied, the documents seen, the attempt
count and the last error; the report carries what **this** invocation did, which is not the same
number and must not be confused with it — an already-finished step reports zero work rather than
the totals of the run that did it.

**A failed step blocks every step after it.** Steps are ordered, and step N+1 was written by
somebody who assumed N had run; continuing past a failure produces a state no step's
precondition describes. The runner stops, reports which step failed, and exits non-zero.

**There is no down migration.** Not omitted — refused. A down migration is code that has run
exactly zero times in production and is expected to work under the one condition where
everything else has already gone wrong, against data the forward step has already changed. The
recovery path for a bad migration is a forward step that corrects it, written and tested like
any other, and a restore from backup for the cases where that is not enough. Shipping a
rollback that has never been executed is a promise the system cannot keep.

## 9. The CLI

anvil ships no `main` and no routes — but it ships the *body* of one:

```cpp
int migrate_main(int argc, const char* const* argv, const MigrationDeps& deps);
```

It is three functions, not one: `parse_migrate_args` reads the command line, `migrate_run` does
the work against a client somebody else opened, and `migrate_main` is the wrapper that opens the
pool. The split is what lets the flags be tested without a cluster and the CLI body be tested
inside a binary whose `MongoPool` is already initialised — `init` throws on its second call.

Five applications spelling `--dry-run` five ways is an operator running the wrong one against
production, at the moment they are least able to check. Flags: `--dry-run`, `--only <name>`,
`--indexes-only`, `--status`, `--unlock <name>`. `--unlock` is separate, loud, and prints the
current owner and expiry before it does anything, because it is the one flag whose whole
purpose is to override the safety net in §4.

Anything the parser does not recognise is **refused**, not ignored — a misspelled `--dry-run`
that was silently dropped is a live run somebody believed was a rehearsal — and flags are
matched on length as well as content, so `--dry` is not accepted as `--dry-run`.

Exit codes are stable and are what a deploy script branches on: `0` applied or nothing to do,
`1` a step failed, `2` held by another runner, `3` the run could not be attempted as described.
The last one covers both a catalogue that does not match the cluster and an invocation that was
not understood; a fourth code would change a contract deploy scripts already branch on, and both
mean the same thing to one. They are `RunOutcome`'s values, `static_assert`ed rather than
transcribed: a mapping written twice is a mapping that can disagree, and the disagreement would
be a deploy script treating a failure as success.

## 10. Index expansion

Three things the catalogue in [`09-mongodb.md`](09-mongodb.md) §7 could not express:

- **A collation.** §3 of that document requires a query and its index to carry the *same*
  collation string or the index is not used, and `LocaleSpec` exists so the two cannot disagree
  — but `IndexSpec` had nowhere to put it. The failure is a `COLLSCAN`, which is slowness
  rather than an error, and slowness is found under load.
- **Hidden.** A hidden index is maintained but invisible to the planner. It is how you find out
  whether an index is load-bearing *before* dropping it: hide, watch, and un-hide with one
  `collMod` if something degrades. A dropped index is a rebuild under load.
- **`background`, which is now nothing.** Since MongoDB 4.2 every index build is the hybrid
  build and the option is ignored. Stated here because a reader who learned it before 4.2 will
  otherwise assume a foreground build they need to schedule around.

Both new fields are **appended last** to `IndexSpec`, so every existing `indexes.h` still
aggregate-initialises and the change is not a break for any consumer. Both also carry a default
member initialiser — without one the appending compiles while emitting
`-Wmissing-field-initializers` twice per entry in every translation unit that includes a
catalogue, which is a warning nobody can act on and everybody learns to scroll past.

`hidden` is reconciled with `collMod` against the live index rather than sent through
`createIndexes`. That is the point of the option: `createIndexes` refuses a name that exists with
a different specification, which is the right answer for every other field and the wrong one for
the field whose whole purpose is to be changed on an index that already exists.

## 11. Collection options, and the one-way doors

`CollectionOptionsSpec` declares what `createCollection` needs and `collMod` can change. An
existing collection whose options **differ** is an error, not a silent divergence — the stance
`createIndexes` already takes on a differing specification, for the same reason: living with a
stale one is how a production cluster stops matching the catalogue that describes it. The
differences are collected and reported together rather than thrown one at a time, because an
operator reconciling a cluster against a catalogue wants all of them in one pass.

It is applied in **two phases**, and the order is the deployment order. `Create` runs before the
index catalogue, because `createIndexes` creates a missing collection implicitly and one created
that way is not clustered, not capped and not a timeseries — the one-way doors would be decided
by whichever call arrived first. `Validate` runs after the data steps, for the reason §12 gives.
A collection this run *creates* carries its validator immediately: there is nothing in it that
could fail one.

There is no separate `ValidatorSpec`. A validator is one `ValidatorFn` on the collection's own
entry — the shape `PartialFilterFn` and `FilterFn` already use, and one type fewer to learn.

`collection_options_are_well_formed` reads `config::kCollections`, so a **capped** collection
whose rows have a declared lifetime is a build failure: the rows would be readable forever with
no path that could remove them. That contradiction lives between two tables and is caught where
both are visible.

Three options are effectively one-way doors and the doc names them as such:

| | Buys | Costs |
|---|---|---|
| **Clustered on `_id`** | The secondary `_id` index disappears; an `_id`-range scan is the collection order | Set at creation only. Changing it is a copy of the whole collection |
| **Capped** | A fixed size with automatic eviction | Forbids deletes and forbids a TTL index — so an erasure path is impossible |
| **Timeseries** | Strong compression on a measurement stream | A delete must match on the meta field. Making the erasure key the meta field would work and would also make it high-cardinality, which is the one thing the bucketing depends on not being |

The analytics events collection is **clustered and not capped or timeseries**, and
[`17-analytics.md`](17-analytics.md) §12 is why: it needs both a TTL and a `delete_many` by
subject, and the last two rows cost one or the other. Both look correct until they are found to be
one-way.

## 12. Validators are a net, never the validation

`$jsonSchema` on a collection catches the write nobody expected — a field written by a script,
a shell, a migration written in a hurry. It does **not** validate request data: that happens at
the edge, in `input/`, with typed errors and a field name the caller can act on
([`06-input-validation.md`](06-input-validation.md)).

`validationAction` is `error`, never `warn`. A warn-level validator is a log line per bad write
under exactly the load that produces bad writes, which §9 of the architecture document already
rules out, and it lets the bad document land anyway.

Adding a validator to a collection that already holds documents is a migration in its own
right, and in that order: the data step that makes every existing document conform, then the
validator. The reverse rejects the writes that would have fixed it.

## 13. Verifying a retired index, finding an undeclared one

`drop_if_present` tolerates `IndexNotFound` — it has to, because it runs against clusters at
different states. The consequence is that a retired index which was never actually dropped
anywhere is indistinguishable from one that was, and for a **unique** index that means a
superseded constraint still refusing writes against a rule nobody meant to be in force.

So two checks that read the live cluster and report rather than act:

- `verify_retired()` — a retired index that is still present. Reported, not dropped, because the
  runner has already tried once and a second attempt in the same process would report the same
  nothing.
- `report_undeclared()` — an index on a declared collection that the catalogue does not name.
  Someone created it by hand during an incident, and it is either load-bearing and undocumented
  or dead weight being maintained on every write. Both are worth knowing.

Neither fails the run. They are the `--status` output, because an operator deciding what to do
about a hand-made index needs to be reading it at a moment of their choosing.

## 14. What was rejected

**A version number per step, ordered like a schema version.** See §2: it cannot represent a
partially-applied step, which is the state that actually needs representing.

**Down migrations.** See §8.

**Running migrations at boot.** See §1. The lock would make it *safe*; it would still make
every rolling deploy wait for a full-collection pass.

**A Redis lock.** See §4. The ledger is in MongoDB and a migration must survive Redis being
down.

**Steps on the job queue.** A job is at-least-once, retried, and dispatched to whichever worker
claims it — every property that makes the queue good is one a migration does not want. And a
migration that runs from N workers is the thing §1 exists to prevent.

**Letting a step write directly through a collection handle.** See §7: dry-run stops being a
property of the runner and becomes a promise each step makes individually.
