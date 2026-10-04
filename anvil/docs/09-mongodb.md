# 09 — MongoDB

## 1. The pool

`MongoPool` is the single owner of `mongocxx::instance` and `mongocxx::pool`.

Correctness here is enforced by **declaration order**: `instance_` is declared
before `pool_`, so the driver instance is constructed first and destroyed last,
and `-Werror=reorder` keeps it that way. The instance must outlive every client
drawn from the pool; getting this wrong is a crash at shutdown that reproduces
about one run in ten.

A `mongocxx::client` drawn from the pool is **not thread-safe** and must never
cross a thread boundary. `acquire()` returns an RAII entry.

## 2. The codec

`db/codec.h` converts between BSON and anvil's value types. One rule governs all
of it:

> **Decoding never coerces.**

A string where an integer is expected is an **error**, not a zero. This is what
makes `{"role": {"$gt": ""}}` fail as *not an integer* rather than being forwarded
into a filter as an operator document — which is an authorisation bypass.

| Type | Storage |
|---|---|
| `Uuid` | BinData subtype 4, 16 bytes. Never a 36-character string: that is 2.25× the storage and turns every comparison into a string compare |
| `PermSet` | BinData subtype 0, 16 bytes, little-endian words |
| Timestamps | BSON date, milliseconds, UTC |
| Enums | int32, **range-checked against a stated maximum** |
| Localised text | A subdocument keyed by locale tag, in the application's declared order |

Enum range-checking matters more than it looks: these rows are read back long
after the enum grew, so `read_enum` takes the highest defined enumerator and
refuses anything past it. An out-of-range value is corruption or a newer writer,
and either way it must not become a valid-looking enum.

A decode failure is `Internal`, not `ValidationFailed` — stored data that does not
decode is our bug, not the client's — and it carries the field **name** only,
never the value.

Localised text is written in the application's declared locale order, which makes
two instances that received the same content produce **byte-identical** documents.
That is what lets a hash of one be a usable ETag.

## 3. Collation

One collation string, handed to both the query and the index that must serve it.

MongoDB will not use an index built with a different collation than the query
carries. The failure is a `COLLSCAN` — **slowness, not an error** — so it is found
under load rather than in review, and by then it is on every query against that
collection.

`db/collation.h` is the one place a collation document comes from. Two
independently written ones that happen to agree today are two that can stop
agreeing.

## 4. Versioned writes

`db/versioned.h` is the only sanctioned way to modify a versioned document, and
`tools/check-db-discipline.sh` fails the build on a repository write that bypasses
it.

`find_one` followed by an unconditional `update_one` is a **lost update**. Two
people editing the same row from two tabs is not a rare event — it is the normal
case for a small team working the same content — and the loser's write vanishes
with no error anywhere.

The mechanism: the expected version is part of the **filter**, and the update
`$inc`s it.

```cpp
filter:  { <identity>, v: <expected> }
update:  { $set: <fields>, $inc: { v: 1 }, $currentDate: { updated_at: true } }
```

The server matches at most one document, so exactly one of N concurrent writers
wins and the rest see zero matched. **There is no window between the check and the
act, because there is no separate check.**

Details that are decisions rather than implementation:

- `VersionMismatch` deliberately does **not** distinguish "your version is stale"
  from "it is gone". Telling a caller which is an existence oracle on an admin
  route, and the remedy for both is the same: re-read and retry.
- `identity` must never contain the version. That is the helper's job, and a
  caller-supplied version filter would let an unversioned write pass the lint.
- Two overloads — with and without a session — rather than a defaulted
  `session = nullptr`. A defaulted null reads as though the session were optional
  to the *correctness* of the write, which for every caller that has one it is not.
- `$currentDate`, never a client clock: the stored instant then comes from one
  authority however many instances are running.
- The projection returns **only** the new version. Returning the whole document to
  read one integer costs network, decode CPU and heap on every write.
- Versions are created as int64 and `$inc` preserves the stored width, so a
  version created as int32 would stay int32 and never match an int64 filter.

## 5. Transactions

`guarded()` catches every driver exception and converts it to a typed `Failure`.
`guarded_in_transaction()` **rethrows** the transient one.

That distinction is load-bearing. `with_transaction` retries its callback when the
server reports a `TransientTransactionError`, and a write conflict on a contended
document is the ordinary case of that. A method that took a session and used
`guarded()` therefore turned a routine write conflict into a 500 and discarded the
transaction instead of retrying it.

Taking a `client_session&` **is** the signal that a method runs in a transaction,
so the rule is mechanical and the lint enforces it.

### 5.1 `repo::in_transaction`, and why the backoff is not in `guarded_in_transaction`

Every transaction in this library is started through `repo::in_transaction`, which wraps
`with_transaction` and adds two things at the **retry boundary**. Neither belongs in
`guarded_in_transaction`, and the reason is structural rather than stylistic:
`guarded_in_transaction` runs *inside* the callback. It sees one attempt, translates its
exceptions, and cannot observe a retry at all, because the retry is the driver's. Both of
these are about the boundary, so they live in a wrapper around the call.

**The abort counter.** `anvil_transactions_aborted_total`, labelled by the outcome of the
transaction that contained the aborted attempts. A retried transaction produces no error
response and no log line, so the whole thing is invisible from outside — which is why
[`00-architecture.md`](00-architecture.md) §9 names the counter and says to alert when it
stops tracking request volume. A load run against an application built on anvil measured
**393,116 aborted attempts**, and nothing in a passing suite could have shown it. The label
is there because 393,116 aborts that all eventually committed and 393,116 that did not are
different incidents.

**A bounded, jittered backoff.** `with_transaction` retries a labelled transient error
*immediately*, and keeps doing so for up to 120 seconds. anvil added no delay of its own, so
N workers contending for one document became a tight loop against the one document they were
all waiting for — every retry arriving at the moment the others did, which is the shape that
makes a conflict storm self-sustaining rather than self-clearing. The delay before attempt
`n` is a uniform draw from `[0, min(64 ms, 2^(n-1) × 2 ms))`. **Full jitter and not a fixed
delay**, because a fixed delay re-synchronises the workers it was meant to spread: they all
wait the same time and collide again. The draw comes from `crypto/random_below` for the
reason stated there — `% bound` biases towards the low values, and jitter that clusters is
jitter that does not do its job.

It **sleeps, on the calling thread.** That is only correct because every caller is already on
`db_pool`: the driver is synchronous, so a transaction is a blocking call by construction,
and a Trantor loop thread may never reach here (CLAUDE.md §4).

`translate()` maps driver errors to domain codes: a duplicate key becomes
`Conflict` — a unique index violation is a business outcome, not a fault — and a
connection failure becomes `ServiceUnavailable`, distinct from `Internal` so a
circuit breaker and a client can tell a fault from an outage. No branch leaks
driver text.

## 6. TTL indexes are not access control

> The TTL monitor runs roughly every **60 seconds**.

So an expired session, capability token or draft stays **readable** for up to a
minute past its expiry, and would still authenticate.

Every query against a collection whose rows have a lifetime must therefore carry
an explicit expiry predicate as well. `append_not_expired` is what adds it, the
application's collection table is what names the field, and
`check-db-discipline.sh` is what fails the build when a query forgets.

The distinction between a **lifetime** and a **retention policy** is the one to get
right. An audit log with a 400-day TTL is keeping history; filtering its reads on
`at > now` would return nothing at all. A collection declares an expiry field only
when the TTL expresses a lifetime.

## 7. Migrations

Indexes are **never** created on a request path: an index build holds its
collection for the duration and every `db_pool` thread queues behind it. So
migration is a separate invocation rather than a boot step — a rolling deploy that
ran it per instance would stall every request the new instance accepted.

Every entry is idempotent. `createIndexes` with an existing name and an identical
specification is a no-op, so N instances converge without coordination. A name
that exists with a **different** specification is an error, deliberately: silently
living with a stale index is how a query starts scanning in production.

The version marker uses `$max`, not `$set`, so two instances applying the same
version race harmlessly and an older instance can never walk the recorded version
backwards. It is recorded in **each** database, so a secondary database dropped
whole leaves no stale marker claiming its indexes exist.

A **retired** index is named in a retired list rather than simply deleted from the
catalogue. Deleting the entry stops *creating* it and leaves it in place on every
cluster that already has one — and for a unique index that means a superseded
constraint still rejecting writes against a rule nobody meant to be in force. The
drops run **before** any creation, so a database mid-migration never briefly
carries both the old constraint and its replacement.

Index names are explicit and stable, never generated from the keys. A generated
name changes when a key is reordered, and the index is then created alongside the
old one rather than recognised as the same.

`background` is accepted and ignored: since MongoDB 4.2 every index build is the
hybrid build. It is worth saying because a reader who learned the option before
4.2 will otherwise assume a foreground build they have to schedule around.

**Hide before you drop.** A hidden index is maintained but invisible to the
planner, so hiding one and watching is how you find out whether it is load-bearing
*before* removing it — and un-hiding is one `collMod`, where recreating a dropped
index is a rebuild under load. `IndexSpec::hidden` is where that is declared, and
it is reconciled with `collMod` against the live state rather than through
`createIndexes`: a name that exists with a different specification is an error, and
this is the one option whose whole purpose is to be changed on an index that
already exists.

**A collated index carries its collation in the catalogue.** `IndexSpec::collation`
holds the locale string, the same one `LocaleSpec` gives the query, and
`db/collation.h` builds the one document both sides send. Without it the index and
the query could disagree and nothing would say so — §3 above is the failure, and it
is a `COLLSCAN` rather than an error.

## 7.1 Data migrations

Moving the documents themselves is a separate mechanism with a separate ledger, and
[`18-data-migrations.md`](18-data-migrations.md) is the whole of it. Three things
belong here, where the reader already is:

- **The schema version is not shared with it.** A version is a high-water mark
  written with `$max`; a data step is done, or not done, or somewhere in the middle,
  and a counter cannot represent the third. `applied_schema_version()` answers after
  a data run exactly what it answered before.
- **A step is claimed with one `find_one_and_update` against an expiring lease**, in
  MongoDB rather than Redis — the ledger is here, and a migration has to run when
  Redis is down.
- **Idempotence is the correctness argument, not the lock.** A step writes what a
  document should be, never a delta from what it found, and `$inc` in a step is a
  build failure for that reason. The seam makes it structural as well as
  enforced: `StepContext` takes the fields a document should carry and the runner
  is what wraps them, so there is no overload a step could spell a delta with.
- **The lease is renewed at the top of each batch, by the write that records the
  previous one.** Renewal and fencing are therefore the same operation, which is
  what lets a runner whose owner has changed stop *before* it writes rather than
  after — and it means the lease must exceed the worst case time one batch takes.

## 8. Query rules

- **Every query is covered by an index.** Adding a query without adding its index
  in the same commit is not allowed, and the explain check asserts no `COLLSCAN`.
- **Project only the fields you use.** Returning a 40 KB document to read one
  boolean costs network, decode CPU and heap.
- **Paginate by an indexed cursor key, never `skip(n)`** — `skip` is O(n)
  server-side, so the last page costs the most.
- **Bound every result set with a `limit`.**
- **Collection and database names are never derived from request data.** They come
  from the application's `constexpr` table, which is what makes the rule structural
  rather than a habit.

## 9. Repositories

`RepositoryBase` is deliberately thin: a database name, a collection name, and
`bind(client)`. It is **not polymorphic** — no repository is ever deleted through
it, so it carries no vtable and stays trivially destructible.

A repository takes a `mongocxx::client&` as a parameter rather than owning one, so
a service performing several writes inside one transaction uses one client and one
session. It takes and returns plain structs: it never sees a request, never throws
an HTTP error, never logs.

Repositories are the **only** place a BSON filter literal may appear.
