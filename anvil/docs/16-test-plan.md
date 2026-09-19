# 16 — Test plan

The security properties in this library are only facts to the extent the tests assert them.
Everything below states **what is asserted** and **why it is worth asserting** — a test whose
purpose is not written down is a test somebody deletes when it becomes inconvenient.

## Structure

Seven binaries, and the split is not arbitrary. Each one exists because something in it
cannot share a process with something in another.

| Binary | Links | Label | Why separate |
|---|---|---|---|
| `anvil_foundation_tests` | `anvil::foundation` | `unit` | Pure CPU, no driver, no framework. Fast enough to run on every save — which is the point of the target split |
| `anvil_platform_tests` | `anvil::app` | `unit` | Needs the drivers linked. It was planned as two — one per layer above `foundation` — and stayed one, because nothing in it needs a process the other half must not share |
| `anvil_listener_tests` | `anvil::app` | `unit` | Boots a **real Drogon listener**. `drogon::app()` is a process singleton whose `run()` does not return until `quit()`, so a second listener cannot share this binary — and a RUNNING event loop is also the only thing that can hold a repeating timer, which is why the flush-timer cases live here rather than beside the sinks they are about. It links `anvil::app` for those two sinks, the same link `anvil_platform_tests` makes |
| `anvil_alloc_tests` | `anvil::foundation` | `unit` | Replaces global `operator new` to count allocations. Cannot coexist with anything that allocates for its own reasons |
| `anvil_concurrency_tests` | `anvil::platform` | `concurrency` | Selected by `ctest --preset tsan` |
| `anvil_db_tests` | `anvil::app` | `concurrency;database` | **Excluded from the tsan preset**: vcpkg builds libmongoc uninstrumented, so the driver's client-pool free-list synchronises with atomics TSan cannot see and every recycled client reports as a race. Suppressing driver symbols wholesale would blind the gate to our own bugs |
| `anvil_load_tests` | `anvil::app` | `load;database` | **Excluded from every default preset.** It saturates thread pools on purpose and sizes its own, which is antisocial to run beside a suite that assumes it can post a task. `ctest --preset load` is how it runs |

Plus four CTest entries that are shell scripts, labelled `lint`, because they enforce rules no
unit test can express. Three are about what the source is ALLOWED to contain:
`check-source-bans.sh`, `check-db-discipline.sh` and `check-vocabulary.sh`. The fourth,
`release-artefacts.sh --check`, is about what the two published files are allowed to BE — it
emits each twice and diffs, and it refuses any descriptor that is not the reference
application's. It runs here rather than only at a tag because determinism is what makes the
descriptor's hash answer "is this client built from this server" at all, and a tag is the worst
moment to find out that it does not.

And one more, labelled `database` and skipping where there is none: `check-reference-server.sh`
starts the binary below, reads its base URL off the first line of stdout, signs in with the
password it drew at boot, and asserts that the table it serves is the one `tests/testapp/`
declares. The server exits 3 when a dependency is unreachable and 1 when it refuses for any
other reason, and CTest reads the first as a skip — one non-zero exit for both would turn "you
pointed me at somebody's data" into a green run on a machine with no database.

Plus two programs that are not tests and are built by the suite anyway, for the reason
`tests/testapp/` exists at all — a seam that cannot be satisfied from outside anvil has to fail
here, where it fails cheaply. `testapp_emit_descriptor` links `anvil::foundation` and prints the
reference application's client descriptor. `anvil_emit_envelopes` is phase 12's and prints every
error body the library can produce, from the writer that produces them rather than from a string
of its own. Both are deterministic because both are published as release artefacts that a
client's staleness check diffs against, and a file that changes on every run is a check nobody
keeps.

**An eighth binary is phase 12's, and it cannot be a fixture inside another one.**
`anvil_reference_server` serves the reference application on a port, and `drogon::app()`'s
`run()` does not return until `quit()`, which is the same constraint that gave
`anvil_listener_tests` its own process. It exists because four suites written on a client's side
— a live contract run and three browser runs, the two-tab credential run among them — currently
fail rather than skip for want of anything to point at. The argument that they are worth running
is the one browser run that could execute without a server: it found a Trusted Types sink that
all 1,064 of that side's own unit tests had passed over. Its rules are in
[`15-tasks.md`](15-tasks.md) phase 12, and the one that matters here is that it shares every
table with `tests/testapp/` — a harness that sees a different route table from the suite is
testing a third application.

## Fixtures

`tests/db_fixture.h` is the only shared fixture header.

- `test_uri()` — `ANVIL_TEST_MONGODB_URI`, defaulting to `mongodb://127.0.0.1:27017`.
- `pool_ready()` — **`inline` at namespace scope, not `static` in an anonymous namespace.**
  This matters: `MongoPool::init` throws on a second call, so a per-translation-unit copy
  means the first TU takes the pool and every other one reports "no database reachable" and
  skips. That failure is silent and the suite stays green while running almost nothing.
- `transactions_available()` — probes with an actual **write** inside the transaction. A
  standalone `mongod` accepts the session and rejects the first operation carrying a
  transaction number, so probing with a read proves nothing.
- `ANVIL_REQUIRE_TRANSACTIONS()` — a macro, which the style rules otherwise forbid, because
  `GTEST_SKIP` expands to `return` and cannot return on a caller's behalf. As a helper method
  it marked tests skipped and then ran the body anyway.

No mocking framework. Dependencies are injected as references or spans, so a test supplies a
real object or a small hand-written stand-in.

---

## Phase 0 — dependency smoke

`dependency_smoke_test.cc`. Runs before a single feature is written against any of it.

| Assertion | Why |
|---|---|
| `RAND_bytes` fills distinct buffers and returns 1 | A CSPRNG that silently fails is the worst possible failure mode — every token becomes predictable and nothing reports it |
| `CRYPTO_memcmp` distinguishes equal from unequal | Every secret comparison in the library routes through it |
| `OPENSSL_cleanse` zeroes a buffer the optimiser could otherwise elide | A plain `memset` before a buffer dies is legally removed |
| Argon2id at production parameters, with a non-Latin passphrase, verifies | Proves the algorithm, the parameters *and* UTF-8 handling in one step |
| simdutf rejects overlong NUL, a lone surrogate, a truncated sequence, and > U+10FFFF | These are the four shapes a lenient decoder accepts and a strict one must not |
| ICU normalises and detects a confusable | NFC and `uspoof` are both load-bearing |
| xxHash produces a stable digest for a known input | It keys ETags and dedupe |
| bsoncxx round-trips a document with BinData subtype 4 | The UUID storage format |

---

## Phase 1 — foundation

### `core_types` / seams

| Assertion | Why |
|---|---|
| `sizeof(UserContext) == 64`, trivially copyable | It is read on every protected request and crosses thread-pool boundaries by value |
| `sizeof(PermName) == 24`, `sizeof(LocaleSpec) == 40`, `sizeof(Locale) == 1` | Padding that creeps in is a silent memory regression across every table |
| `PermCatalogue::well_formed()` rejects duplicate bit, duplicate name, empty name, out-of-range bit | Each is a real mistake that would otherwise ship |
| `all()` excludes reserved gaps | A gap is not a permission; granting one puts a control on a screen that authorises nothing |
| `for_each_name` visits in **bit order**, not table order | Two responses for one holder must be byte-identical so a client can diff them |
| `perm_mask` is usable in a `constexpr` context with an app enum | The whole route table depends on this |
| `Locale::from_index` rejects `>= kLocaleCount`; `from_tag` rejects an unknown tag | The only constructors are validating ones, which is what lets `get()` index without a check |
| `Localized::complete()` is false for present-but-empty | A blank string renders as a blank heading. Missing and empty are the same defect |

### `crypto`

Round-trip AEAD; a tampered ciphertext, a tampered AAD and a wrong key each **fail**;
base64url round-trips and rejects padding; `SecretBuffer` is move-only and zeroes on
destruction; the blind index is stable across equivalent spellings of one input and differs
under a different key.

### `input`

| Assertion | Why |
|---|---|
| An unescaped string is a **view into the body**, not a copy | The no-allocation claim is the whole design; assert it, do not believe it |
| Duplicate keys are an error | Parser-differential attacks: two parsers disagreeing about which value wins |
| Limits are enforced **during** the parse | A depth-64 document must not be fully built and then rejected |
| `as_string()` on a number returns `nullopt` | This is what makes `{"email": {"$gt": ""}}` fail as "not a string" — the NoSQL-injection defence |
| A number is kept as source text | Eager `double` silently rounds a 64-bit id and loses the `1e309` overflow |
| An unknown field is a binder **error** | Silently dropping it is how a forgotten field becomes an auth bypass |
| `finish()` is `[[nodiscard]]` | Forgetting the check must not compile |
| The three `optional_nullable_*` arms distinguish absent / null / value | A partial edit cannot express "clear this field" otherwise |
| No error carries the submitted value or a client-chosen key | Reflected XSS, log injection, and an encoding hazard with non-Latin input |
| `validation_fuzz`: a realistic body stays **inside the arena's inline buffer** | Measured by counting global `operator new`. This is why that binary exists |

### `auth`

Token round-trips; a flipped byte anywhere fails; **the tag is verified before any field is
read**, including expiry; an expired token fails; the ±60 s skew tolerance holds; an unknown
`kid` fails *identically* to a bad signature — a forged token must not confirm which key it
was aimed at; an out-of-range locale byte is `Malformed`; decode allocates nothing.

Argon2id: verify succeeds for the right password and fails for a near-miss; `needs_rehash`
flips when parameters change; `consume_dummy_time` takes comparable wall time to a real
verify, so a missing account and a wrong password are not distinguishable by timing.

`EpochCache`: a hit inside the TTL, a miss after it, fixed capacity under a flood of distinct
ids — the input is an attacker-chosen user id, so an unbounded map is memory exhaustion.

### `html_sanitize` / `filesystem` / `i18n`

Sanitiser output is well-formed by construction; every hostile element is dropped, not
escaped; nesting depth is bounded. Path building rejects traversal by **path component**, not
string prefix (`/srv/storage-evil` is not inside `/srv/storage`); the `openat` walk applies
`O_NOFOLLOW` per component; sniffing is a closed allow-list and SVG is rejected explicitly so
the rejection is auditable. Digit folding, bidi stripping and NFC normalisation each
round-trip.

---

## Phase 2 — platform

| Suite | Assertions |
|---|---|
| `codec` | **Decoding never coerces** — a string where an int is expected is an error, not a `0`. `Uuid` is BinData/4 and 16 bytes, never a 36-char string. Enums are range-checked against a stated maximum, because rows are read back long after the enum grew. A decode failure names the field but carries no value |
| `collation` | A query's collation and its index's collation come from the same `LocaleSpec`, so they cannot disagree — the failure mode otherwise is a silent `COLLSCAN` |
| `versioned_concurrency` | **8 concurrent writers, exactly one winner.** The other seven get `VersionMismatch`, never a silent lost update. A stale version and a deleted document are indistinguishable, deliberately — distinguishing them is an existence oracle |
| `thread_pools` | `try_post` returns false when full and the caller can shed; an exception in a task body does not reach `std::terminate`; shutdown drains |
| `access_control` | A denied request and a nonexistent route return **byte-identical** responses and complete **without I/O**. A foreign `kid` and a bad signature deny identically. Every route in the table resolves by its own pattern and method; pattern+method pairs are unique; a method-specific entry does not leak across methods; an undeclared route is **not** silently permitted; every `Stealth` route requires a permission; no entry is value-initialised by an over-counted table size |
| `route_declaration` | `declared()` throws at boot for a handler whose pattern is not in the table — the failure is a startup crash, not a route that quietly 404s in production |
| `images` | A decompression bomb is rejected **from the header**, before pixels are decoded. EXIF orientation is applied to pixels and then the tag dropped. The ICC profile is converted to sRGB and then dropped, not merely dropped. Variants never upscale. An un-encodable format is skipped; a format that fails mid-encode aborts the whole set, so a half-populated variant list is never recorded |
| `timer` | A claimed job is invisible to other consumers for its lease; an expired lease is reclaimed; a repeatedly failing job dead-letters rather than looping; a recurring declaration fires **once per bucket across N workers** |

---

## Phase 3 — identity, audit, media

Auth: a disabled, locked and pending account each fail authentication with the **same response
and the same timing** as a wrong password — any difference is an account-enumeration oracle. A
permission change bumps `perm_epoch` and takes effect without waiting out a token lifetime.
Refresh rotation invalidates the previous token. A capability token is consumed exactly once
under concurrency.

Audit: the buffer flushes on size and on interval; a full queue drops and **reports the drop
count**, because a blank forensic record during a flood is the failure this design exists to
prevent.

Media: `X-Accel-Redirect` paths are percent-encoded unconditionally, even though every
component is server-generated hex — "this value can never contain a newline" is exactly the
assumption a refactor breaks, and a CR in a response header is response splitting. Content
type comes from the **stored** enum, never from the request. `X-Content-Type-Options: nosniff`
is present. A role that resolves to no written variant falls back to the largest available,
never to a 404 — a legitimate id must not 404 because a variant was skipped. A private
namespace is not served publicly.

`media_concurrency`: concurrent uploads of identical bytes converge without corrupting
either; publish is atomic by rename; a killed upload leaves only a sweepable temp file.

---

## Phase 4 — sections

`registry_is_sorted`, `fields_fit_buffers`, `choices_are_well_formed` and
`defaults_match_registry` are `static_assert`s over `tests/testapp/sections.h`, so part of this
suite is a build. The two runtime suites cover what a table cannot assert about itself.

`sections` (unit). An unknown key is an **error** with an empty field name, never a silent drop
— silence hides probing and is how a client discovers what some other endpoint accepts. Type
before value: a `Bool` handed `"true"` and a localised field handed a bare string are both
rejected on shape. A localised field needs **every** declared locale and refuses an extra one.
Blanking every locale clears an optional field and the clear lands in the patch as an explicit
empty value — dropping the key would leave the merge holding the old one; blanking *some* is a
half-translated field and is refused. `required` is checked against the **merged** document, so
a patch touching one optional field is legal and clearing a required one is not. Hostile rich
text is *rejected*, not cleaned. `canonicalise` produces **byte-identical** output for the same
content received in a different key order, which is what makes the ETag a function of the
content rather than of the request that wrote it. `content_etag` moves when a **non-default**
locale changes — a digest over one rendering would not. A slot with no aspect constraint
serialises as `null`, never `{"num":0,"den":0}`, which is always truthy and prints "shaped 0:0"
beside a requirement that does not exist.

`content_db` (database). Published and draft are two documents and a read of one cannot see the
other. `insert_if_absent` returns *false* rather than an error on a duplicate and does not
overwrite — a boot path that overwrote would turn every deploy into a content wipe, and every
instance runs it at every boot. A stale version loses. A field the registry no longer declares
is **gone from storage** after the next write, asserted by reading the raw BSON — which is what
the whole-subdocument write buys. Reference counts move inside the section write's own
transaction, and rewriting the same id in the same slot does not double-count it. An image
below its slot's minimum is refused; 1920×1081 passes a 16:9 slot and 1920×1440 does not; a
slot declaring `0/0` accepts any shape. An id from another namespace is **indistinguishable**
from one that does not exist. The first draft is created from the published content, only from
version 0, and is not cached. A write **replaces** tier 1 rather than leaving it cold — the
ordering assertion that caught the defect where it stored and then immediately nulled it.
Invalidation drops every locale of one key and leaves other sections alone; a key from another
process gets the same allow-list treatment a request would. With no Redis, reads, writes and
the local drop all still work and the degradation is logged.

**Two families here assert a number rather than a behaviour, and both were added because the
number was wrong while everything around it was right.**

`BootstrapReport::image_slots_unbound` is asserted against a database seeded BEFORE a resolver
existed and booted again after one arrives — the reported case, reproduced exactly — where
`images_missing` reads 0 and `images_registered` reads 2 while both slots are still empty. Both
numbers are asserted in the same case, so the two are pinned as *different answers* rather than
as one renamed. Beside it: a section whose document never arrived counts all of its slots; a
slot bound while its neighbour in the same section is not counts one, over a registry declared
in the test because no section in `tests/testapp/` has two slots and the difference between
counting sections and counting slots is invisible until one does; and a slot holding the
36-character string form of an id — written through the raw collection, because no encoder here
produces it — counts as unbound, because a slot no renderer resolves is the same gap on the page
as an empty one.

The invalidation hook is asserted by `peek()`ing **from inside the callback**, which is the only
form that distinguishes a hook called after the drop from one called before it; a call-counting
case passes either way. A key the registry does not declare never reaches it. A hook that throws
costs that callback and nothing else — the drop already happened, and the next key still works.
And `TheHookFiresOnTheSubscribersPathAndNotOnlyWhereTheWriteHappened` runs two `SectionService`
instances sharing one cache prefix, so the second hears about the first's write only through
Redis: it publishes on a loop until the subscription lands, because `SUBSCRIBE` is asynchronous
and a message published before it does is a message nobody receives.

---

## Phase 5 — forms

`forms` (unit) and `forms_db` (database).

`table_is_well_formed` and `is_dense_from_zero` are `static_assert`s over the reference
application's own table, so that half of the seam is a build. `validators_are_present` is
**not**, and cannot be — see docs/13 §10 — so it is asserted here instead.

Beyond that: an answer whose JSON shape disagrees with its declared type is rejected before
the value is examined; an unknown field id is rejected and a required one must be present;
`Fid::parse` rejects anything that is not `f` followed by 1–3 digits, so `$set` and `a.b` are
**unrepresentable** in the answer key space rather than merely filtered — asserted as a
`static_assert`, because `parse` is `constexpr`; at most one PII field per form; `has_pii` is
derived and never accepted from a client; a PII value **never appears in `ans`** — asserted by
reading the stored document back and looking, not by trusting the code path that wrote it; the
blind index matches across equivalent spellings and differs under a different index key; CSV
export prefixes a cell beginning `=`, `+`, `-`, `@`, tab or CR, **including a staff-authored
header label**; an edit incompatible with existing submissions is refused with `Conflict`
rather than `ValidationFailed`, because the caller can only offer "duplicate this form" if it
can tell the two apart.

The database half asserts what only a server can show: a partial unique index rejects the
second submission of a one-per-user form and leaves two anonymous ones alone; a multikey
unique index rejects a second binding of one object; a versioned edit matches nothing once
the version has moved, and does not rewrite the submission counter; five submissions become
ONE `$inc` of five; a drop destroys the definition, its rows and its references together, and
invalidates the cache before reaping; one form's submissions are never visible from another;
a type is stored as an int and not as its name.

And one benchmark, recorded rather than assumed: table dispatch against the `switch` it
replaces, in docs/13 §9. Only its CORRECTNESS half is a test — a wall-clock threshold on a
shared runner is a flaky test, and jitter is not a fix for anything.

---

## Phase 6 — notifications

The inbox row is written **before** any transport is attempted — the inbox is the system of
record and every transport is best-effort on top, so a failed webhook must not lose the
notification. Fan-out respects the topic's `PermSet`: a recipient without the bit does not
receive. Coalescing collapses a burst within the window into one row. An SSE connection whose
ring fills is **dropped rather than buffered unboundedly**. The stream ceiling turns a flood
into a clean 429 rather than an `accept()` failure. A webhook signature verifies against the
sealed secret and a tampered body fails. Every handler is idempotent under redelivery —
asserted by delivering twice and comparing state.

Three things about how these are tested are worth stating, because each is the difference
between a suite that proves something and one that agrees with itself.

**The crash is simulated by stopping where the crash would stop.** `publish_db` writes a row
through the transactional overload — which deliberately does not dispatch — and then runs the
sweeper the way a deployment would. It advances the SWEEPER'S clock rather than back-dating
the row: the grace is measured against the row's UUIDv7 `_id`, which is minted from the wall
clock, so a back-dated `now` would move `created_at` and leave the id where it was, and the
test would be asserting against a row shaped like nothing the code can produce.

**Web Push is decrypted with the subscription's private key**, which is the browser's side of
the exchange. Encrypting and then re-deriving the key our own way would pass with the two
public keys in the wrong order, with every HKDF label's trailing NUL missing, and with the
record delimiter dropped — three mistakes that each produce a body no browser can read while
looking entirely correct. HKDF is checked against RFC 5869's published vectors for the same
reason. The VAPID signature is generated three hundred times, not once: a DER integer whose
leading byte falls below `0x80` is one byte short, and the conversion bug it catches appears
roughly one time in 256.

**The SMTP conversation runs against a scripted server.** That is not a compromise — it is the
only way to assert what a broken or hostile one does to this client. A multiline reply with no
end, a line with no newline, a close in the middle of DATA and a reply arriving one byte at a
time are all unreachable against a working mail server, and every one of them is a hang or a
memory exhaustion if it is not handled.

**The storm breaker is asserted as a property of the topic TABLE, not as a case about a topic.**
`shed_verdict` is pure, so the security property — a topic the reader may not silence is one the
breaker may not drop — is a loop over `kTopics` at nine pressures from 0.0 to 4.0, and a
separate case asserts the table *has* such a topic, without which that loop would pass by
testing nothing. The watermark boundaries are tested AT their values and not merely around them,
because `>` quietly becoming `>=` is the change a test of 0.0 and 0.9 does not see. A NaN
pressure proceeds.

Both gates are then asserted against the cluster, because what each leaves behind is a state in
the database. Gate 1 writes **nothing**, proved through the dedupe index rather than by a count:
the same idempotency key publishes clean once the pressure is gone, which it could not do had a
row been written and then refused. Gate 2 leaves `dispatched_at` unset — byte-for-byte what a
process killed between commit and enqueue leaves — and the sweeper then finishes exactly that
row and delivers **once** across both attempts, with a second sweep finding nothing. A probe
that throws does not cost a notification, and with no probe at all nothing is shed whatever the
pressure.

**Two structural assertions worth naming.** `SseStream.TheWakeCallbackRunsWithoutTheStreamLockHeld`
calls `queued()` — which takes the same non-recursive mutex `push()` holds — from inside the
wake callback. It asserts by not deadlocking, which is the only way to assert the absence of a
lock. And `sse_concurrency` mints every UUID before its threads start: vcpkg builds OpenSSL
without TSan instrumentation, so `RAND_bytes` reports its lock-free algorithm cache as a race
that is not one, and suppressing that would hide real races in the same stacks.

---

## Phase 8 — analytics

**The Structure table gains one binary and no more.** Seven of the eight hold unchanged, and
each new suite lands in the binary whose isolation property it needs: `metrics_test` (unit,
foundation), `metrics_wiring_test`, `event_seam_test`, `event_buffer_test` and `sessions_test`
(unit, app), `metrics_alloc_test` (`anvil_alloc_tests`), `metrics_concurrency_test`
(concurrency), and `analytics_db_test`, `rollup_db_test` and `transaction_retry_db_test`
(database).

The eighth is **`anvil_load_tests`**, labelled `load` and excluded from every default preset.
It is not a suite that was moved; it is a gate that did not exist, and it is separate for the
same reason every other binary here is: it cannot share a process with the rest. It saturates
pools deliberately, and it sizes its own — `tests/app_fixture.h`'s are tiny so that shedding is
observable, which is the opposite of what "no batch is refused while the database is answering"
needs to mean anything. It holds five cases, and that claim is asserted on **both** sinks: the
8,485 refusals it exists for were audit batches. See [`17-analytics.md`](17-analytics.md) §17.

**A gauge is asserted not to be summed.** Sixteen threads each `store` a gauge, and the snapshot
reports a value one of them wrote — not their sum, and not sixteen times the largest. It is the
cheapest possible test and it guards the one confusion that would make every capacity number in
a dashboard wrong in the direction an operator acts on.

`metric_table_is_well_formed` and `event_table_is_well_formed` are `static_assert`s over the
reference application's tables, so half of each seam is a build rather than a test — including
the series count, which is the assertion that matters most: **a table whose label space exceeds
the ceiling fails to compile**, which is the only place a cardinality bug is cheap.

**An increment costs zero allocations**, counted in `anvil_alloc_tests` rather than believed.
The no-allocation claim is the whole design, and the JSON arena is already asserted the same
way. `metrics_concurrency` adds the properties a sharded counter can quietly lose: N threads
incrementing one series sum to exactly N×M with no lost updates, a snapshot taken concurrently
with writes returns a value between the first and last observation rather than a torn one, and
a histogram's cumulative buckets are monotone by construction.

The OpenMetrics writer escapes `\\`, `\n` and `"` in `help` and in every label value, and emits
`# EOF`. **Two snapshots of an unchanged registry are byte-identical**, series order included —
that is what lets a scraper diff them, so it is asserted rather than assumed.

Ingest restates the audit buffer's assertions against the second sink: a `Conversion` is never
dropped while a `Behaviour` row is buffered, and a full buffer refuses a conversion only when it
holds nothing else. Three assertions are new.

**A saturated `analytics_pool` does not starve an audit flush**, which is the reason the pool is
separate at all. Asserted by filling one until it sheds and posting to the other — by
ACCEPTANCE and not by timing, because a shared pool refuses the audit task outright and a
stopwatch would be exactly the flaky assertion this document rules out everywhere else.

**A refused flush is re-admitted rather than discarded**, which was the correction the audit
sink's shape needed and is now asserted on both: the conversions (changes) come back, the
behaviour (traffic) rows are charged as the drop, and returning two hundred rows to a buffer of
four still leaves four. Sampling is deterministic per
session: the same session id samples the same way a thousand times, so a funnel is whole or
absent.

Two privacy properties are asserted by *looking*, not by trusting the path that claims them. An
event requiring consent, offered without consent, **never reaches a row** — asserted by reading
the collection back. And the packed client address appears nowhere in a stored document, asserted
by scanning the raw BSON rather than the struct that was meant to omit it.

`rollup_db` asserts the property the job's correctness rests on: running the same rollup twice
produces **one** document with identical values. That is the `$set`-not-`$inc` design, and it is
the only assertion that proves the job is re-runnable — which it must be, because every queue
here is at-least-once. A rollup interrupted mid-window and resumed from its `_id` boundary
produces the same totals as one that ran straight through.

Erasure removes every row carrying the subject and leaves every anonymous row — and the scan
**rides the partial index**, asserted through `explain`. On a developer's four hundred rows an
index scan and a collection scan are indistinguishable by any other means.

`transaction_retry_db_test` covers the other half of the phase's instrumentation. A forced
conflict — one transaction holding a document open until a second has tried to write it —
asserts that the abort counter moves on a real abort, and eight contending writers assert that
every transaction still commits exactly once and that nothing is charged to the failed outcome
when none of them failed. The backoff's bound is asserted as an UPPER bound and never as an
equality, because an equality would be an assertion about how fast the machine ran.

---

## Phase 9 — data migrations

`migration_seam` and `migrate_cli` (unit, `anvil_platform_tests`); `data_migration_db`,
`index_expansion_db` and `migration_lock_db` (database, and therefore
concurrency-labelled). `step_table_is_well_formed` and `collection_options_are_well_formed`
are `static_assert`s over `tests/testapp/migrations.h`, so the seam's shape is a build. **No
new binary** — every one of these fits a suite that already exists.

**`migration_lock_db` is the suite that matters**, because everything the design rests on is a
property under concurrency. Eight runners claim one step and **exactly one wins**; the other
seven report *held* and exit 2 rather than waiting, which is asserted because a second operator
watching a migration hang is how two of them end up force-killing the one that was working. A
runner whose lease has lapsed is reclaimed by the next, and the attempt count on the row is what
tells an operator the step was picked up twice — the only trace a lapsed lease leaves. A runner
whose owner has changed **stops**, asserted by reading back what it wrote: its renewal, its
finish and its failure all carry its own id in the filter, so all three match nothing and the
row still holds the totals from before it was fenced.

`--unlock` is asserted in both directions, because it is the one operation whose purpose is to
override the safety net: it clears a lease it does not hold, and it reports *not found* for a
step nobody has run rather than creating a row for it.

The crash is simulated by stopping where the crash would stop — a step that applies its first
batch and refuses the second — rather than by back-dating a ledger row into a state no crash
produces. One thing is then constructed deliberately, and it is the window the whole design pays
for: a process that dies between applying a batch and recording it leaves the writes on disk and
the cursor where it was, so the resumed run re-reads documents it has already transformed.
Rewinding the recorded cursor is exactly that state, and the assertion is that the collection
ends up **byte-identical to the run that was never interrupted** — which proves idempotence
rather than describing it.

`data_migration_db`: a step that reaches the end records its terminal state, and a second
invocation does **nothing at all** — no batches, no writes, and no second attempt on the ledger,
because a finished step is never claimed. A failed step blocks every step after it, because N+1
was written assuming N ran; the blocked step is reported as blocked rather than merely absent,
so `--status` says which ones never started. A step whose schema precondition is unmet, and an
`IdRange` step over a collection whose `_id` is not a uuid, are both **refused before anything
is claimed** — exit 3, with no lease taken and nothing half-applied, because the alternative is
a run that walks a prefix and reports that it finished. `--dry-run` walks the same cursor, calls
the same step, reports the same counts and **writes nothing** — asserted by comparing the
collection's contents before and after rather than by reading the runner's own report, and
extended to the ledger, since a dry run that took the lock could block the real one. And
`schema_version` is untouched by a data run, so `applied_schema_version()` still answers what it
answered before: the regression easiest to ship and hardest to notice.

The compound-`_id` cursor has a case of its own, and it is why the reference table carries two
steps. `sections` keys on `{k, s}`, so every `$gt` there compares embedded documents rather than
sixteen bytes of binary, and a cursor proved against one shape is a cursor that breaks on the
other.

`migrate_cli` is the flags, which need no cluster. The case that matters is the one asserting an
unrecognised argument is **refused and named**: a misspelled `--dry-run` that was silently
dropped is a live run somebody believed was a rehearsal. A prefix of a real flag is asserted to
be refused too, and so is a flag missing its value, which would otherwise read past the end of
`argv`. The exit codes are asserted to BE `RunOutcome`'s values rather than a copy of them.

`index_expansion_db`: an index declared with a collation **is** used by a query carrying the
same collation and is **not** used by one carrying none, both proved through `explain`. The
failure this catches is a `COLLSCAN`, which is slowness rather than an error and is otherwise
found under load. A hidden index is invisible to the planner and still maintained — asserted by
un-hiding it and watching the planner pick it up, which is a `collMod` rather than a rebuild.
`verify_retired` reports an index the catalogue retired and the cluster still carries, and
`report_undeclared` reports one nobody declared — both against a cluster deliberately put into
that state, and both asserted **empty first**, because a check that reports nothing passes on any
cluster in the world. Re-applying a collection's options is a no-op; applying different ones to
an existing collection is an error rather than a silent divergence, in both directions. And the
declared validator is asserted to be in force by a write the server must refuse — a net that was
never actually attached catches nothing, and nothing else in the suite would notice.

---

## Phase 10 — HTML output

`html_writer` (unit, `anvil_foundation_tests`); `content_headers` (unit,
`anvil_platform_tests`, because it touches a `drogon::HttpResponse`); the end-to-end cases in
`anvil_listener_tests`. **No new lint entry** — the phase scoped down to writers that sit beside
`json_writer` and `csv_writer`, and they are tested where those are.

It does add the binary the table above has always listed and nothing had ever created, and that
is not a scope change: `drogon::app()` is a process singleton whose `run()` does not return until
`quit()`, so the one case that has to exist here cannot be a fixture inside another binary.

The escaping half is the security half and is asserted per context rather than in aggregate. A
value containing `<`, `&`, `"`, `'` and a NUL survives `append_html_text` as text and never as
markup — the NUL is in that input because the parameter is a `string_view` and a writer taking a
`const char*` would emit the prefix and stop. The quote characters are asserted to come through
element text **unescaped**, which is the difference between the two sets rather than an omission,
so a later "tighten it up" has to argue rather than assume. `&lt;` is asserted to become
`&amp;lt;`, which is the double-escaping window an escaper that handled `<` before `&` would
have left open.

`append_html_attr` emits the whole attribute, so what is asserted is that a value carrying a
quote lands inside the quotes the writer emitted itself, and that a NAME outside `[A-Za-z0-9-]`
emits nothing at all — the one shape that would otherwise be an injection is an attribute name
built from request data, and escaping a name does not help because `onload` is well-formed.

`append_url_attr` **emits nothing** for a URL `input::is_safe_link_target` rejects — asserted
against that function's own answer rather than a fresh list, since the point of delegating is
that there is one list and a test carrying a second copy is the drift it was meant to prevent.
The protocol-relative `//evil.test/x` is named explicitly in the test anyway, because it is the
case a reimplementation forgets and the test is also documentation. And an ACCEPTED URL is
asserted to be escaped: `/x" onmouseover=alert(1)` is a site-relative path the link rule
correctly accepts, so a writer that read "delegates the decision" as "delegates the escape"
would emit an event handler.

`append_tel_attr` is the scheme the list does NOT carry, arriving as a type instead, and the
first case in its family asserts the decision not taken: `append_url_attr` still refuses
`tel:+201012345678`. Then the layout is refused **one byte at a time** — no leading `+`, the
wrong calling code, a landline rather than a mobile, an operator prefix nobody was assigned, a
letter among the subscriber digits, and a byte from the attribute escape set, which has to fail
the check rather than survive it quoted. The two members are cross-checked, because
`operator_digit` and `e164[4]` describing one fact is two things that can disagree and the
disagreement is the signal that neither came from the scanner. A default-constructed `PhoneEgy`
emits nothing: the type is an aggregate behind an out-parameter API, so that value compiles and
has to, and a caller who ignored the `Reason` is holding thirteen NUL bytes. And six spellings
of one number — including the Arabic-Indic one — reach one identical `href`, which is what
storing E.164 and nothing else is for.

**These cases are compiled by `ANVIL_WITH_EGY`, which the `asan` and `dist` presets now set.**
The option is OFF by default because it decides whether a *consumer* ships a locale's
validators; before it was set here, every case written for the Egyptian validators — these and
the fuzz cases that predate them — was compiled by no preset at all, so nothing but a developer
passing `-D` by hand ever ran one.

**The raw-insertion rule is asserted at compile time, not by a runtime test**, and there are two
of them because one alone proves nothing. That `append_sanitized(out, some_string)` is
ill-formed, and that `input::SanitizedHtml{arbitrary, HtmlVerdict::Ok}` is ill-formed — the
second is the one that matters, because without a closed constructor the first is a speed bump
rather than a guarantee. A rule enforced by the type system is tested by proving the type system
enforces it, so both are `static_assert`s inside named cases: `!is_invocable_v` for the three
string spellings, and `!is_aggregate_v`, `!is_constructible_v` and `!is_default_constructible_v`
for the type.

Re-sanitisation is asserted by what reaches the output: a stored value that fails the second pass
**is not in the bytes**, and the withheld-content comment is, so the gap is visible rather than
silent. The case is driven with a value that would pass a write-time check and fail a render-time
one — a body stored under the field's cap and rendered under the tighter one a page puts on what
it holds — because a test where both passes agree proves nothing about why there are two. A
second case drives the other reason the two exist: markup that never went through the write path
at all, which is what a stored-XSS attempt against the database looks like from the renderer.

Headers: the CSP carries **no `script-src` directive at all**, asserted by its absence rather
than by a substring match on the rest — an accidentally-added `script-src 'none'` would also pass
a loose check while changing what the policy means if it later gains a source. `no-store`,
`noindex`, `no-referrer` and `nosniff` are each asserted on the response, not on a constant. And
a `CONTENT_ORIGIN` that could end the directive it sits in — one carrying a `;`, a space or a
comma — is asserted to produce `img-src 'none'` and no `script-src`, because a policy an operator
typo can rewrite is not a policy.

**The end-to-end case is the one that matters, and it exists because of a production failure that
no unit test could see.** The preview route's render path once required a `UserContext` derived
from a `__Host-` cookie, which is host-only and therefore never sent to the content origin the
design requires — so every preview answered a stealth 404 and the feature had never worked in the
documented deployment. Every test at the time drove the repository or the service, and both were
correct. So: a real listener on an ephemeral port, driven over a socket.

A request to the content origin carrying the path-scoped capability cookie and **no session
cookie** asserts a rendered page comes back. A request carrying a session cookie and nothing else
asserts the stealth 404 — that is the production defect in its original shape, and it is the case
that fails if the render path ever learns to want a `UserContext` again. A request with no cookie
at all asserts bytes identical to an unmatched route, compared across the status, the body, the
content type and the whole header map rather than the part that was convenient. A capability past
its expiry asserts the same 404 **from an explicit expiry predicate rather than TTL reaping** —
its row is still in the store when the case passes, and the same token against a clock before its
expiry is asserted to verify, so it is the predicate that refused it and not the token being
unknown. A capability for one subject asserts nothing for another. And the link that hands the
token out asserts a `303` whose `Location` does not carry it, and a cookie that is path-scoped to
the one subject, `Secure`, `HttpOnly`, `SameSite=Lax` and **not** `__Host-`.

One process cannot hold two registrable hosts and does not need to: the only thing the server
observes about the split is which cookies arrive, and both directions are sent here exactly as a
browser would send them. The credential store is a hand-written stand-in rather than
`identity::CapabilityService` — that service is in `anvil::app`, this binary links
`anvil::platform` by design, and its own behaviour already has a suite against a live cluster.
What had never been exercised anywhere is the path between the socket and it.

`config::require_origin_split` refusing a `CONTENT_ORIGIN` that shares `SITE_ORIGIN`'s host is
asserted at the config layer, because a deployment with the two equal passes every other test in
the suite while having lost the isolation the split exists for. Two origins differing only by
PORT are asserted to be refused as well: cookies are not port-scoped, so a byte inequality would
have passed a deployment whose split is in the configuration and not in the browser.


---

## Phase 11 — the client's tables

`descriptor` (unit, `anvil_foundation_tests` — generating a client needs no driver, no event
loop and no database, and the binary it is tested in is the proof); `rate_limit` and
`idempotency` (unit, `anvil_platform_tests`, both because they touch Redis and the second
because it touches nothing else); the served route table in `anvil_listener_tests`, beside the
preview cases and sharing their listener. **No new binary and no new lint entry.**

The descriptor cases are about what a generated client is generated FROM. A descriptor that is
wrong is not a wrong response; it is a client built with the wrong authority model, and nothing
in the request path will notice. Determinism is asserted first because the hash depends on it: two
runs producing two byte strings would make the staleness check report drift on every deploy, and
a check that cries wolf is ignored inside a week. The hash boundary is asserted in both
directions — an application version bump must not invalidate every client that is running
perfectly good code, and a table change must invalidate all of them. The public/holder split is
asserted on a route that is reachable with no credential and on one that stealth-404s, because
the second's path is the map that must never reach a bundle. And the projection is asserted to
agree with `satisfies()` over every route and a range of holders, rather than against an expected
output: a second implementation of "may this holder reach this route" is a second one to keep in
agreement, and the one that drifts is the one no attacker is reading.

**`bootstrap` is asserted against the POLICY, not against the table.** It is the one field in a
description that makes a claim about disclosure, and what it discloses is a path, so it is
exercised by holding one description fixed and swapping the policy under it: Authenticated with
no bit passes, Stealth and Guarded fail, Authenticated carrying bits fails, and Public fails for
being redundant rather than dangerous. The same five policies with the flag OFF all pass, which
is what makes those assertions about the flag rather than about the policies. A separate case
walks the reference table and asserts no route is in the bundle for both reasons at once — the
property that lets a reader of a route table always tell *which* reason put a path there.

Three cases pin the split at the layer above. The projection omits a bootstrap path by default
and includes it when asked, exactly as it does a public one. `reachable()` still answers yes for
it, asserted directly, because a reader who conflates authority with disclosure would "fix" its
absence by listing it. And the agreement case above now skips every path the bundle holds rather
than only the public ones — that line changed *because* the flag landed, which is the split
being real rather than decorative. The listener case that asserted
`"session.current":"GET /session"` in the session body now asserts its **absence**: the table
does not name the route that produced it, because the client already compiled that path in.

**The media grammar is asserted as a route and as a join.** One case pins that `media.object`
carries the settled pattern, is `GET`, is `public` — a public grammar being exactly what that
means — and is distinct from the list route at the shorter pattern; and that the media object
itself carries no path, because an address in two tables is two things to keep in agreement.

The join is the case worth having. A generated client types `{ns}` and `{role}` as enumerations
because the media object publishes value sets under those exact names, and that coupling is a
naming convention nobody would otherwise check. So the case WALKS the pattern, skips `{id}`
— the caller's own object id, which no table enumerates — and asserts every remaining segment
is a key the media object supplies values for, then asserts there were two of them so a pattern
that lost a segment cannot pass by having none. Renaming `{role}` to `{size}` fails it, which is
how it was checked.

**The namespace accept mask is asserted from both ends.** The upload path refuses an AVIF into
the `guest` namespace with its own reason — `upload.ns_type` rather than `upload.magic`, since a
legitimate image in the wrong place is a different event from an unrecognised file — and accepts
the identical bytes into `content`, without which the first case would pass on a sink that
refused everything. A third drives bytes nothing sniffs into the widest namespace there is,
because the mask may only NARROW the pipeline's list: `Mime::Unknown` has no bit at all, so even
an all-ones mask fails closed, and that is a `static_assert` beside the case.

The emitted half asserts both namespaces' lists and that SVG appears in neither — not because
nobody listed it, but because there is no `Mime` for it to be listed as.

One existing assertion changed and the change is the finding. `AttachesAWidthToARoleAndShipsNoLadder`
read `EXPECT_FALSE(contains(doc, "avif"))`, and the accept lists publish `image/avif`. The
blanket form became a precise one rather than being deleted: every occurrence of the token must
be inside a media type and none may stand alone, so a client can read "this namespace accepts
AVIF" and still cannot spell a variant's filename. The distinction holds because a variant
filename is not URL-addressable at all — the public grammar is a role, and the file is reached
through `X-Accel-Redirect`.

A third pins the decision not taken: the serving route counts into **no** rate bucket, while
upload and delete still count into `media`. That rule is twenty a minute and is sized for
uploads — one libvips decode on `cpu_pool` each — and a gallery page is thirty images in one
paint, so counting serves into it would answer `429` to an ordinary render and the symptom would
be images vanishing above the fold rather than anything that looks like a limit.

The content tables are asserted in the vocabulary a client speaks rather than the one the server
stores: flags as named booleans, a PII type's answer shape as `null` rather than `"text"`, an
unconstrained image slot's aspect as `null` rather than `{"num":0,"den":0}`, a gated topic as
`holder`. Each of those is a value a client renders WRONGLY rather than fails on. The media case
asserts both halves of the `srcset` reconciliation at once — that a width arrives attached to its
role, and that no ladder, format list or file extension arrives at all, since a client that has
either is a client that can build a path again. `CarriesNoStorageVocabulary` searches the emitted
bytes for "collection", "index" and "mongodb", and it runs against the full input including the
content tables: a client has no business knowing the storage layout, and a name in a bundle is a
name in an attacker's notes.

**The holder authority is asserted as an equality with the filter's own predicate.** The
superadmin flag is compared against `is_superadmin()` and against `satisfies()` over every
`UserType`, rather than against an expected string, because two tests of "is this a superadmin"
is two things to keep in agreement. The defect it closes has a case of its own: a superadmin
with an empty permission set is told `"superadmin":true` beside `"perms":[]`, which is the
answer a client counting bits could not have had. Names are asserted in **bit** order from
arguments written out of it, since the session response carries an ETag and two spellings of one
holder would make every revalidation a miss. And a mask carrying bits no permission declares —
the reserved gaps between an application's blocks — emits nothing for them, which is the other
half of why this is not `~PermSet{}`.

Both halves are then asserted **over the wire** in the listener suite, because what the reference
handler puts in the envelope is the thing a client actually reads.

The `Retry-After` cases are arithmetic, and they are the ones worth having: every one of them is a
header a client honours incorrectly if the rounding goes the other way, and the failure is
invisible — a client that comes back too early is answered 429 again and looks exactly like one
that was always going to be refused. Rounded up, never zero, clamped to the rule's window. The
limiter's own case asserts the window comes from Redis rather than from this process's clock, and
one case drives a bucket into the state the old script could not recover from: a key whose expiry
was lost, which under a `hits == 1` branch could never be restored because a counter above one
never returns to one. In production that state is reached by a failure nobody watches for.

The idempotency cases are the five answers a claim can give, because each has exactly one correct
response and getting any of them wrong produces no error anybody sees — a `Fresh` that should have
been a `Replay` is a duplicate write, a `Replay` that should have been a `Mismatch` is one request
answered with another's response. A mismatch is asserted while the first attempt is still running
as well as after it finished, because reporting that as `InFlight` would tell the client to retry
into the same refusal until the marker expires. Another caller's identical key and another route's
identical key are both asserted fresh: `1` is the key every client picks first, and if the identity
or the route id were not in the key a caller would be handed somebody else's response. `release()`
is asserted NOT to delete a completed record — a `catch` on a path that already succeeded would
otherwise turn the store into a no-op for exactly the request that needed it — and the fence is
asserted by letting a superseded attempt try to record after a newer one already did. The counter
is asserted here rather than in `metrics_wiring_test`, for the reason that file already gives: a
site is covered where its own subsystem is.

Its sixth value, `unavailable`, has no case, and that is deliberate rather than an omission —
`RedisClient` is a process-wide singleton, so reaching that path means taking Redis away from
every other case in the binary. What the outage must do, refuse rather than answer `Fresh`, is a
`catch` with one exit; it is read rather than exercised, and it is named here so that a later
reader does not mistake the gap for an oversight.

### The table a holder is actually handed

`append_reachable_routes` had a suite and no caller. Every case asserted the projection against a
string the test itself had built, which proves the function and proves nothing about the response:
no handler had ever run it, no socket had ever carried it, and the envelope the header describes
and delegates to "the caller" — the status, the content type, the caching rules, the ETag — existed
only as a paragraph. A generated client built against a hand-written fixture asserts what we
BELIEVE the server sends, which is the failure mode the fixtures rule at the top of this document
names, so the reference application serves it here: a real listener, the real access filter, and
the `__Host-at` cookie a browser would send.

One case is the recording, and it asserts the bytes literally rather than by parsing them into
something forgiving, because those bytes are what a generator is built from: one envelope key, an
object of route **id** to `"<METHOD> <path>"`, an `Any` entry spelling `ANY` rather than defaulting
to `GET`. The rest are the properties that make the response safe to hold. A holder is handed
`content.get` and neither the id nor the PATH of anything above it — the path is the point, since a
bundle is a public file and a chunk is a public URL. Public paths are absent, because they are
already compiled in and resending them is bytes on a response every signed-in tab asks for. No
credential is a real `401` rather than the stealth `404`, since this is the route a client asks when
it is trying to tell "re-authenticate" from "route gone". The response is `private, no-cache` with
`Vary: Cookie`: a shared cache holding one copy would serve one holder's map to another, which is
the disclosure the projection exists to prevent, reintroduced one layer downstream of it.

**The case that could not be written anywhere else is the agreement one.** That the projection and
the filter return the same answer is already asserted as a function comparison, and a function
comparison cannot see a disagreement's actual shape: a holder, a socket, a route. So `/audit` is
registered behind the filter, and a holder without `AuditRead` is asserted to be given no
`audit.list` in the table AND the byte-identical not-found at the door, while a holder with it is
given the route and served by it. A route listed but denied is an affordance that fails; a route
denied there but served here is a stealth route with a public index.

The ETag cases assert what revalidation is for. An unchanged table answers `304` with no body and
the same tag, so a client that stored it keeps it. A grant does not: the epoch moves, the tag the
client is holding stops matching, and the new route arrives at the next revalidation rather than at
the next login — which is the whole reason the header keys the tag to `perm_epoch`. The tag carries
a second half the header does not mention, and one case pins it: `perm_epoch` alone leaves a deploy
gap, because a build that adds or re-authorises a route changes what a holder reaches while their
epoch stands still, so an epoch-only tag would answer `304` against yesterday's map until somebody
happened to edit that holder's permissions.

---

## What is deliberately not tested

- **Third-party library behaviour**, beyond the dependency smoke test. If ICU's normaliser is
  wrong, that is not a bug this suite can usefully find.
- **Timing as a measurement.** The stealth tests assert "the denial never defers to the epoch
  authority, under any cached verdict" structurally, not "these two took the same number of
  nanoseconds" — a wall-clock assertion on a shared CI runner is a flaky test, and jitter is
  not a fix for a timing oracle anyway. The structural assertion has to name the RETURN VALUE
  that costs a round trip, `Step::ResolveEpoch`: an earlier version counted calls to a resolver
  that `evaluate()` has no way to reach, so it could not fail, and it did not when the mask was
  checked after the epoch (docs/04-access-control.md §3).
- **The `database`-labelled suites under TSan.** See the table above.

**And one thing that was nearly not tested, wrongly.** A refused WebSocket upgrade used to be
followed by a four-byte close frame, and the residual was recorded as a FREQUENCY: 0/2/1 against
12/5/3 over forty handshakes, with a note that an assertion over the whole byte stream would be
the flaky kind this section rules out. It would have been — but the flakiness was in the reading,
not in the server. `raw_handshake` reads the socket once, and a single `recv` sees the frame only
when TCP coalesced it into the same segment as the response, so one behaviour measured two
different ways. Reading until the peer goes quiet (`raw_handshake_all`) turns it back into a fact:
the frame follows every refusal a filter makes and none the upgrade gate makes, exactly, every
time. **A measurement that comes out different on every run is a reason to distrust the
instrument before the property** — the rule above is about wall-clock timing, and it does not
license leaving a byte-level property unasserted.

**One case per process hides a class of defect, and this is what it hid.** Every
`anvil_listener_tests` case is its own ctest entry, so the pair asserting that a refused stealth
upgrade and an unmatched one are byte-identical never shared a listener with anything else. Run
the binary as ONE process — which is what a server is — and the pair failed on the mainline: the
refusal carried a `Connection: close` the unmatched answer did not, because Drogon sets that flag
on the per-IO-thread *copy* of the shared 404 and whether a given thread had one depended on what
it had served earlier. Isolation is what makes these cases parallelisable and is worth keeping;
what it costs is exactly the defects that need two requests to see. So the binary is also
registered once as a whole, `listener_in_one_process`, and the suite is run both ways for the
nine seconds it costs.
