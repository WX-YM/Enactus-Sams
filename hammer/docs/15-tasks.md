# 15 — Tasks

The work, decomposed to units with a stated done condition. Kept current as each phase lands;
a task is checked only when its code, its tests and its doc are all green.

**hammer is a library with a partner, not a client of one.** It is written against anvil's
published contract — the envelope, the cookies, the error codes, the cursor rule, the origin
split — and it holds none of anvil's internals and none of any application's vocabulary. Where
a task needs something anvil does not yet emit, it is listed in §Cross-repo below rather than
worked around here, because a workaround in a library is a workaround in every application
built on it. Paths are relative to `~/Code/hammer/`.

**Phase gate:** nothing in phase N+1 starts while any phase-N test is red.

**Doc numbers are allocation order, not a table of contents.** `15` and `16` are meta docs,
numbered to match anvil's so a reader moving between the two repositories does not have to
re-learn where the plan lives; a content doc added later takes the next free number.

Legend: `[ ]` open · `[~]` in progress · `[x]` done

---

## Phase 0 — scaffold and plan of record

| | Task | Done when |
|---|---|---|
| [x] | `git init`, directory skeleton | `src/{core,wire,state,dom,react,codegen}`, `tests/testapp/`, `docs/`, `tools/` exist |
| [x] | `package.json` | Five browser entry points plus `codegen`; `"sideEffects": false`; `dependencies` empty; react a *optional* peer |
| [x] | `tsconfig.json` | `strict`, plus `exactOptionalPropertyTypes`, `noUncheckedIndexedAccess`, `verbatimModuleSyntax`, `isolatedModules`, `noPropertyAccessFromIndexSignature` |
| [x] | `.gitignore`, `.editorconfig` | Build output, `node_modules`, generated clients outside `tests/testapp/` |
| [x] | `CLAUDE.md` | The three axes, the attribution rule, §1 library rules, and the eleven sections |
| [x] | `README.md` | What hammer is, what it refuses to hold, how an application consumes it |
| [x] | `docs/00-architecture.md` | Layers, origins, pools, lifecycle, credential flow, error table, cache model, invariants |
| [x] | `docs/01-seams.md` | The descriptor, all twelve seams, the generation contract, and what it must never carry |
| [x] | `docs/02-getting-started.md` | A minimal application end to end |
| [x] | `docs/15-tasks.md`, `docs/16-test-plan.md` | This file and the test plan |
| [x] | `tools/check-source-bans.sh` | Fails on `innerHTML`, `eval`, `new Function`, `document.write`, `Math.random`, credential storage, `any`, `@ts-ignore`, `!` assertions; comments and string literals stripped; `// ban-exempt: <reason>` honoured |
| [x] | `tools/check-wire-discipline.sh` | Fails on `fetch` outside `src/wire/`, a URL built by concatenation, an offset/`skip`/`page=` parameter, a mutating call with no idempotency route flag, and a credential in a query string |
| [x] | `tools/check-vocabulary.sh` | Fails on an application's feature names in `src/`, and on a citation of a register this repository does not have |
| [x] | `tools/check-layering.sh` | Parses every import specifier and fails on an edge pointing up or sideways; `core` importing anything is a failure |
| [x] | `tools/check-dependencies.sh` | Fails on a non-empty `dependencies`, on a non-optional peer, and on a lockfile whose tree contains a runtime package |
| [x] | `npm install` and a green `npm run check` | 16 tests, `tsc --noEmit` clean under every strict flag, five scripts clean |
| [x] | CI | One workflow: `npm ci`, type-check, suite, the five scripts. No step is `continue-on-error` |

**Phase 0 gate: MET.** `npm run check` is green: `tsc --noEmit` under `strict` plus
`noUncheckedIndexedAccess`, `exactOptionalPropertyTypes` and `verbatimModuleSyntax`; 16 tests
in 2 files; five source checks clean. Every check was driven against a file written to violate
it before it was trusted, because a check that cannot fail reports clean for the wrong reason.

One of them failed honestly on its first real input: the layering check's globals half matched
the word `document` in the sentence explaining that `core` needs no document. It strips
comments now, the way `check-source-bans.sh` always has — a check that reports its own
documentation is a check somebody turns off.

---

## Phase 1 — the core (`hammer`)

No DOM, no `fetch`, no framework. `tools/check-layering.sh` proves it by refusing any import
out of `src/core/`.

| | Task | Done when |
|---|---|---|
| [x] | `core/result.ts` | `Result<T, E>`, `ok`, `fail`, exhaustive narrowing; `throw` reserved for a violated precondition |
| [x] | `core/errors.ts` | `ServerError` (code, `requestId`, `fields`), `TransportError`, `ClientError`, `StaleClientError`. **No message field anywhere.** The code and reason types are parameters, because the vocabulary is the descriptor's |
| [x] | `core/brand.ts` | The branding helper, and the rule it enforces: every brand's factory is module-private and does the work the brand asserts |
| [x] | `core/perm_set.ts` | `Uint8Array(16)`; `has`, `hasAll`, `hasAny`, `isEmpty`; a hand-written base64url codec **proven against vectors emitted by anvil's own encoder**, failing closed on a wrong length, a standard-base64 character, and a non-canonical trailing character |
| [x] | `core/uuid.ts` | 16 bytes; parse, format, compare without a string; `crypto.randomUUID` only |
| [x] | `core/text.ts` | `codePointLength`, NFC normalisation, truncation that never splits a code point, `Intl.Segmenter` for caret positions, and a `graphemeLength` that is documented as **not** a limit |
| [x] | `core/bidi.ts` | Isolation for interpolated user text, direction detection, and the isolate characters for the places an attribute cannot reach |
| [x] | `core/digits.ts` | Shaping for display; ASCII on the wire; folding of Arabic-Indic input before it is sent |
| [x] | `core/validate.ts` | Combinators over the descriptor's bounds: code points, ranges, enums, patterns **without a backtracking regex on a hot path** |
| [x] | `core/cursor.ts` | `Cursor<RouteId>` brand. An offset cannot be expressed in the public API — proven by a type-level test |
| [x] | `core/time.ts` | Durations in ms; a `ServerInstant` type that has no arithmetic with the device clock, so §6's rule is a type rule |
| [x] | Suite | `tests/core/**`; runs with no DOM and no network; `perm_set` and `text` have property tests |

**Phase 1 gate: MET.** 107 tests in 10 files, `tsc --noEmit` clean under every strict flag,
five source checks clean, and `check-layering.sh` reporting no edge out of `src/core/` — no
import, and none of the globals an import graph cannot see. The suite runs with
`environment: node` and asserts that it does: a layer that cannot reach a document is a layer
whose validators cannot accidentally render, and a one-line change to `vitest.config.ts`, in a
commit about something else, would otherwise take that property away silently.

Three things were found by tests rather than by review, which is the only reason they are
recorded here:

- **`withinCodePointBounds` failed open on an impossible bound.** An empty string never enters
  the counting loop, so a maximum of zero or less was satisfied by the one value that can
  never be within it. Found by the property test, not by the twelve cases written first.
- **The wire check fired on its own prose.** `offset` and `skip` are English words as well as
  identifiers, and the only places they appear in `src/` are the comments explaining why the
  library cannot express either. The check strips comments now, the way the layering check
  had to learn to in phase 0 — string literals are still matched, because half of what it
  looks for lives inside one.
- **`ServerInstant` cannot prove its own provenance**, and the comment claiming its
  constructors were entry-point-private was wrong. Response shapes are the application's
  (`docs/01-seams.md` §4), so the application is what turns a decoded field into an
  instant. What the type does guarantee is the half that gets forgotten: whatever the number's
  provenance, it cannot be subtracted from the device clock once it is inside.

The type-level assertions are the load-bearing half of two modules. `Cursor` cannot be written
from a string or a number, does not travel between routes, and `PageRequest` has no offset
field for a call site to fill in; `ServerInstant` cannot be subtracted from `Date.now()`,
cannot be assigned to a number, and hands out no accessor that would put the number back
within reach. Each `@ts-expect-error` fails the build the day the line it guards starts
compiling, which is the only way a rule about what a caller can *express* stays true.

---

## Phase 2 — the descriptor and the generator (`hammer/codegen`)

| | Task | Done when |
|---|---|---|
| [x] | `codegen/descriptor.ts` | The format types for `descriptor: 2`, and a validator making anvil's `well_formed()` checks: duplicate bit, duplicate name, empty name, bit out of range, and a route naming a permission, a capability scope or a rate-limit bucket that does not exist. Plus two the emission needs: a path whose parameters cannot be parsed, and two names that land on one identifier |
| [x] | `tests/testapp/hammer.descriptor.json` | **The real output of anvil's reference emitter**, not a hand-written example: committed, and the input to every generator test |
| [x] | `codegen/emit.ts` | Emits literal-union types, `as const` tables, the descriptor hash, and a `DO NOT EDIT` banner; output is side-effect-free and tree-shakeable |
| [x] | Visibility-split emission | A public route's method and path are emitted as values; a **holder route's path is not emitted at all** (`docs/01-seams.md` §4.1). Every route's shape is emitted as a `type`, which costs nothing because types are erased |
| [x] | Tree-shakeable emission shape | Individual `const` exports, never one object literal. A bundler can drop an export nothing references; it cannot drop one member of an imported object — which is what makes "the public bundle does not contain it" true rather than hoped for |
| [x] | Built-output assertions | A test builds the reference application and greps the **output**, not the source: no holder path, no unreferenced permission name, and no descriptor file in the published directory |
| [x] | `codegen/cli.ts` | `hammer codegen --descriptor <path> --out <dir>`; node only; a malformed descriptor exits non-zero naming the key |
| [x] | `tools/check-descriptor.sh` | Regenerates into a temp dir and diffs against the committed output; a difference fails with the command that fixes it |
| [x] | Generated client for the testapp | Committed under `tests/testapp/api/`, type-checked by the build |
| [x] | Copy, class-name and invalidation maps | `docs/01-seams.md` §13: three application-supplied tables, each checked against a generated type, each a **compile error when the server adds a member** |
| [x] | Emit the content tables | Done: all five, in two commits — the four whose every value may be emitted, then topics, where §4.1 decides. A holder topic's `const` carries `key: null` and its code; the key stays in the erased union, `publicTopics` aggregates the only tier with a key to aggregate, and the built-output assertions cover both bundles. A `visibility` that disagrees with `perms` fails generation in both directions |
| [x] | Refresh the committed descriptor | Done: the fixture is anvil's thirteen-table output, format 2, and the client is regenerated from it. `check-descriptor.sh` still cannot see a stale fixture — it regenerates the client from the committed descriptor — so the staleness it catches is the client's and never the descriptor's, which is the §Cross-repo row below |

**Phase 2 gate: MET**, against the descriptor as it stood. Two rows were added above
afterwards: anvil emitted the five content tables once this generator existed to read them, so
the phase acquired work it could not have had while its gate was being met. The gate is left as
it was rather than reopened — it records what was true when the phase closed, and a gate edited
to match later news is a gate that records nothing.

160 tests in 14 files, `tsc --noEmit` clean under every strict flag, and
six scripts clean — `check-descriptor.sh` regenerating the committed client and diffing it to
nothing. The reference consumer now has a generated client, the three application tables in
both declared locales, and two screens that exist to be *built*: the anonymous bundle and the
staff bundle are produced by esbuild inside the suite and read as bytes, because a source
assertion about what a bundle contains is the one a bundler configuration silently
invalidates.

Four things were found by the work rather than by review.

- **The reference descriptor fails a check this plan asked for.** "A capability scope no route
  requires, and a rate-limit bucket no route names, fail generation" would reject anvil's own
  output: six of its eight buckets are named by no route in the emitted table, and
  `StaffPermissionChange` is required by none of them. anvil limits by keys that are not route
  buckets — `login-acct` and `verify-addr` are account-keyed rather than route-keyed — and
  mints scopes for operations whose routes its reference emitter does not carry. They are
  warnings now, printed by the CLI; the **reverse** direction, a route naming a scope that is
  not there, stays a hard failure, and it is the one that would have stopped a build anyway.
- **Two rows of the error table in `docs/00-architecture.md` §6 were wrong**, and the
  descriptor is what says so: `CAPABILITY_REQUIRED` is a 428, not a 403, and
  `VALIDATION_FAILED` is a 400, not a 422. The same reading settled the spelling: the union
  carries anvil's wire names unchanged, because a PascalCase client union costs a fifteen-entry
  translation table in every bundle that decodes an envelope, a second copy of an append-only
  enum, and a word on a screen that does not match the word in the log. The zero member is
  dropped from both failure unions — an application would otherwise owe a sentence for a case
  no error surface can reach.
- **The identifier derivation lowercased what it should have kept.** `ContentRead` became
  `kPermContentread`, and it was found by reading the first generated file rather than by any
  test, because every test written until then asserted the bit and not the name. A part keeps
  its own capitals now and is lowered only when it is entirely upper — so `TOO_LONG` still
  reads as `TooLong` — and two names that land on one identifier are a generation failure
  rather than a `const` silently shadowing another.
- **`@types/node` opened a hole `check-layering.sh` could not see.** A `node:fs` specifier
  names no layer, so the import loop skipped it exactly as it skips any bare specifier, and
  nothing would have reported a core module reading from disk. It fails on a `node:` import
  outside `src/codegen/` now — which is also the honest statement of what that layer is: the
  only one allowed not to run in a browser.

A pass over the finished phase against anvil's source added three things and corrected the
record on a fourth.

The two HTTP statuses were confirmed against `include/anvil/http/errors.h` rather than against
the committed JSON alone, because a correction that inherits a stale fixture is not a
correction. The same header settles the naming: `wire_name()` is documented there as stable
client API, so translating it here would have been a breaking change invented on this side.

Every descriptor string that reaches the emitted file now passes a character class before it
gets there — a route pattern, a locale tag, a collation and a cursor field were the only four
that did not have to become an identifier, and they were reaching the output on the strength of
one `JSON.stringify` call. The call is still there and a hand-built descriptor proves it holds
on its own; the classes are so that the guarantee does not rest on a line a refactor can drop.

The identifier derivation has a suite of its own, with every expectation written out rather
than computed. The `kPermContentread` defect survived a green suite because the assertions in
place checked the bit a permission const holds and not the name it is bound to, and an
expectation derived the way the code derives it would have agreed with the bug.

**What "a staff route is hidden from an ordinary account" rests on, and how much of it is
built.** The chain is four links and two of them are done:

1. The generator never emits a holder route's path — **built**, and asserted three ways: on
   the emitted module, on the built bundle, and on a source map built from it. Driven by
   deliberately regressing the emitter, which fails all three.
2. The server sends each holder only the routes that holder reaches — **built, on anvil's
   side**, by `append_reachable_routes()` through the same `satisfies()` the filter calls.
3. The client resolves a holder route through that table, and a missing entry refetches the
   session, retries once, and reports **not-found** — **built**, `wire/resolve.ts`.
4. Affordances are gated on the holder's route table and a `403` means "refetch" — **built**,
   `wire/affordance.ts` and the retry policy. What remains is Phase 5's: the components that
   consult it.

Link 2 is also the one that makes the difference worth having: the filtering is done by the
only participant an attacker does not own, and it is per holder rather than per audience, so a
compromised account yields that account's map rather than the whole one. None of it is a
boundary. anvil's stealth 404 is the control, and it is worth exactly as much after this phase
as before it.

**The two rows added after the gate are closed, and four things came out of closing them.**

**The emission shape is decided by how a consumer RESOLVES a row, not by a blanket rule.** The
file said "individual `const` exports, never one object literal", and `publicRoutes` had already
been an exception with a reason. The content tables made the reason general: a row a call site
spells is a `const`, because a bundler can drop an export nothing references; a row that arrives
at run time — a field type by the code a definition carries, a media width by the namespace an
object was stored in — has no useful subset, so it is a table and the consumer is told it is
paying for all of it. Sections get both, because both kinds of consumer exist.

**A vocabulary anvil owns is derived from the table and never written down here.** The answer
kinds, the section field types, the event classes and the media roles are each a closed set
server-side, and each would have been one line to copy. A copy is the copy nobody updates, and
it fails in the direction that costs the most: a member anvil appends would be a generation
failure in every application on the day the server started sending it.

**`visibility` is checkable because anvil derives it.** A topic's is `perms.none() ? public :
holder`, so a descriptor where the two disagree is not anvil's output. It is refused in both
directions — `public` with a permission emits a key into a bundle the permission was meant to
gate, and `holder` with none withholds a key nothing protects, which reads as a gate that is not
there.

**A check fired on its own prose for the third time in this repository.** The source-map
assertion looks for a holder path in the generated module, the media section of that module
explains `/media/{ns}/{id}/{role}`, and `/me` is a holder path and a substring of it. The fix is
the one the emit suite already used and the one `check-layering.sh` and `check-wire-discipline.sh`
each arrived at: match the value the way it would be EMITTED — quoted — rather than the way it
reads in a sentence. The bundle assertions stay unquoted, because minification leaves no prose
for them to fire on.

And a fifth that is about the plan rather than the code: the descriptor refresh inserted two
routes in the middle of the table and failed nine assertions that were about something else
entirely, because each had its route's index written beside it. They derive the index from the
table now. A key is still asserted exactly — it is what a person opens the file at — but a
position is not a thing any of those tests was testing.

---

## Phase 3 — the wire (`hammer/wire`)

| | Task | Done when |
|---|---|---|
| [x] | `wire/route.ts` | Path parameters through `encodeURIComponent`, query through `URLSearchParams`; there is no public `fetch(url)` |
| [x] | `wire/session_view.ts` | The session payload decoded once: the permission set, the holder-scoped route table, the descriptor hash and the superadmin flag. Every path validated before it can become the path of an authenticated request; an `ANY` method costs its route and not the session; the table is a `Map`, because its keys come off the wire |
| [x] | `wire/resolve.ts` | Route resolution across the two tiers: the compiled table for a public route, the session's holder-scoped table otherwise. A missing entry refetches the session, retries once, then reports the **not-found** shape — never a permission-flavoured one (`docs/00-architecture.md` §4.2) |
| [x] | `wire/affordance.ts` | The permission pre-check. A route affordance is answered from the session's table, which is the server's own `satisfies()`; a non-route affordance from the bits, with the superadmin short-circuit. It governs what is rendered and is given no way to refuse a call |
| [x] | `wire/envelope.ts` | **The only** decode site. An unknown error code decodes to `Unknown`, never throws |
| [x] | `wire/client.ts` | `call<RouteId>` with params, body, capability and response inferred; `AbortSignal` on every call; one in-flight request per `(route, params)` |
| [x] | `wire/queue.ts` | Bounded in-flight per origin, FIFO, shed with `ShedError`; `queueWaitMs` recorded |
| [x] | `wire/retry.ts` | The policy from `docs/00-architecture.md` §6 as one pure function, branching on the HTTP **status** rather than the error code — a proxy's 502 has no code to branch on and is exactly the failure worth retrying; `Retry-After` honoured exactly; full jitter from `crypto.getRandomValues` |
| [x] | `wire/breaker.ts` | Per-origin circuit; opens after N consecutive transport failures; a single probe closes it |
| [x] | `wire/idempotency.ts` | A key minted for every retryable non-idempotent route, reused across every replay of that call, and dropped when the call finally settles |
| [x] | `wire/capability.ts` | `Capability<Scope>` produced only by the minting call; a route declaring a scope will not compile without one; never minted implicitly on `CapabilityRequired` |
| [x] | `wire/leader.ts` | `navigator.locks` election, `BroadcastChannel` fan-out, and a documented degraded path where locks are unavailable (one refresh per tab, and the race is reported rather than hidden) |
| [x] | `wire/credentials.ts` | `401` → leader refresh → replay once; second `401` clears identity and broadcasts logout; every tab drops caches and closes streams |
| [x] | `wire/sse.ts` | `Last-Event-ID` resumption, bounded ring, dedupe by id, reconnect with jittered backoff, **leader-owned** with follower fan-out |
| [x] | `wire/upload.ts` | Streams from the `File`; size and type refused before the first byte; progress from the stream; abortable; **no `readAsArrayBuffer` anywhere in the library** |
| [x] | `wire/origin.ts` | Construction refuses a cross-**site** API outright and records the preflight cost of a cross-origin one (`docs/00-architecture.md` §8.4) |
| [x] | `wire/rate_limit.ts` | Local token buckets from the descriptor; a `429` holds back the whole bucket |
| [x] | Suite | A fetch stand-in, not a mocking framework; every error code's behaviour asserted; the refresh race driven with two simulated tabs |

A row the plan did not have: `wire/schedule.ts`, the one place this library waits. Three
mechanisms need to not-act for a while — the backoff between attempts, the fallback covering a
leader that died mid-refresh, and the pause before a stream reconnects — and each would
otherwise have grown its own timer, its own abort handling and its own leak.

**Gate: MET.** 528 tests in 33 files, `tsc --noEmit` clean under every strict flag, six scripts
clean, and the two-tab test making exactly one `POST /auth/refresh` — asserted against the URLs
a fetch stand-in recorded, from two clients sharing one lock manager and one channel.

**The first five rows, recorded when they landed.** The first three complete the chain §4.1 of
`docs/01-seams.md` is about — a staff route being invisible to an account that does not reach
it: the session's scoped table decoded and validated, resolution across the two tiers with the
one refetch §4.2 allows, and the affordance gate. The two added since are the ends of the
request that resolution sits between — the address a route becomes, and the value a response
decodes to. 284 tests, six scripts clean.

Four things about it are worth writing down rather than rediscovering.

**The affordance for a route is answered from the table, not from the bits.** The bitset check
§4.1 describes is right for an affordance that is not a route and wrong for one that is: anvil's
`satisfies()` short-circuits on user type, and a superadmin's permission set is deliberately not
all-ones, so a client counting bits would hide every guarded control from the one account that
reaches all of them. The route table does not have that problem, because the server built it
with `satisfies()` itself. A second implementation of "may this holder reach this route" is a
second one to keep in agreement, and the one that drifts is the one nobody is reading.

**The decode is the trust boundary, and the check that matters is the one nobody writes.** Every
path in that table becomes the path of an authenticated request, so a malformed payload does not
merely fail — it chooses where the session's cookies are sent. `//evil.example/x` resolved
against an origin is not a path on that origin; it is a different host, and it is the one
malformed path that succeeds. It is refused, along with a scheme, a backslash, whitespace and a
query, by the same character class the generator applies to a route pattern.

**Encoding a path parameter is necessary and it is not sufficient**, and the two gaps are why
`wire/route.ts` is a module rather than a template literal. `encodeURIComponent` leaves `.` and
`..` alone because both are unreserved, so a parameter holding `..` is a dot segment by the time
a URL parser sees it — `/thing/../elsewhere` is a request for `/elsewhere`, resolved before a
byte leaves the device, against an origin carrying the session's cookies. That is the same class
of defect as the protocol-relative path above, arriving from the other direction: not from the
server's table but from whatever the screen put in the parameter. And it throws a `URIError` on
a lone surrogate, which is what a string truncated in the middle of an emoji is; the query side
needs the identical check for the opposite reason, because `URLSearchParams` does not throw
there — it substitutes U+FFFD and sends a value the caller never passed.

**The error shape was wrong in three places, and reading anvil's header is what said so.**
`requestId` was typed `string`; anvil's 404 body is one constexpr string shared byte for byte by
a stealth drop, an unmatched route and a genuinely missing object, Nginx serves those same bytes
with no request behind them, and anvil's own suite asserts the absence — so the null is the
common case and a surface built on the old type renders "undefined" on the most frequent failure
there is. A failure may also never have reached anvil at all, which is why the decoded error now
carries the HTTP status beside the code: without it a proxy's 502 and a 200 whose body did not
parse are one `Unknown` with two different right answers. And `fields` is a `Map` that is null
rather than absent — a Map because the keys come off the wire, null because an optional property
is two object shapes (§2.3).

The same reading is what put an `Unknown` member on `ValidationReason`, which `ErrorCode` had
had all along. `input::Reason` is append-only on anvil's side too, and the alternative to a
member is dropping that entry from the `fields` map — which is a form that refuses to submit
with nothing marked on it.

**The rest of the phase, and the seven things it found.** The `403` refetch that the note above
left open is built, and it landed where that note said it would: in the retry policy, which is
the one place that has to decide it, rather than in `wire/credentials.ts` and the policy
separately.

**The policy branches on the HTTP status, not on the error code**, and the plan's own row said
otherwise. Two reasons, and the second is the one that costs a user something. A code-keyed
table would be a second copy of the descriptor's vocabulary, which is what `wire/envelope.ts`
takes a vocabulary parameter to avoid — and a failure may never have reached anvil at all, so a
proxy's 502 and an empty gateway timeout decode to `Unknown` with no code to branch on. Those
are precisely the failures where retrying is right. Where a status is ambiguous the tie is
broken by what this client knows about the call it made rather than by a second look at the
body: a 403 refetches the session unless the call presented a capability, because anvil maps a
spent token to 403 as well.

**A 401 on the refresh route asked the refresh machinery to recover the refresh.** In one tab
that is a deadlock — the second call joins the in-flight refresh it is itself performing —
and against the server it is a loop. Found by a test, and fixed by the refresh request starting
as though it had already replayed: there is no second credential behind the one it is renewing.

**A follower must register for the news BEFORE it attempts the lock.** Taking the lock is
asynchronous, so a leader whose refresh is quick can finish and broadcast in the window between
a tab failing to take the lock and that tab beginning to listen; the follower then waited out
the entire leader window for a message that had already been delivered. In a browser the window
is narrow. In a test, where the refresh resolves immediately, it is the common case — which is
the argument for the stand-in being a stand-in rather than a mock.

**Deduplicating onto an in-flight request needs a signal of its own, and a refcount is not
enough on its own.** Joiners arrive over several microtasks, so the first one leaving before the
second arrives would abort a request the second then joined. The abandonment is reported
synchronously and the entry dropped, so a later caller starts a new request rather than joining
a dead one. And the dedupe covers reads only: two identical writes are two writes somebody
asked for, and merging them would be this client deciding one of them did not happen.

**A `Retry-After` hold has to correct the local bucket in both directions.** Zeroing the count is
obvious — the server has just said the local one was wrong. The other half is not: the refill has
to be dated so that exactly one event is available when the hold lifts, or a `Retry-After: 1`
against a fifteen-minute window is honoured by the server and then refused locally for another
quarter of an hour, which is this client overriding the only participant entitled to name that
number.

**`EventSource` was the obvious transport and is the wrong one.** It reconnects on a fixed delay
with no jitter, so every tab of every client dropped by one deploy reconnects in the same second
— the thundering herd the retry policy spends a paragraph avoiding, arriving through the one API
that looked like it would save work. It also cannot carry a header, so a resume is whatever the
browser chose to send. The frame parser is forty lines and it is fed the way a socket feeds it,
in chunks that do not respect frame boundaries.

**What the suite still does not have is the recorded envelopes** `docs/16-test-plan.md` lists as
a fixture. Every error code's behaviour is asserted, by the status each maps to in the table
anvil's own header owns, and every envelope those cases decode is written by hand — which
asserts what this repository believes the server sends. Closing it needs a capture from a
running anvil, which is the same shape of gap as the descriptor refresh below.

**hammer ships no public suffix list, so a cross-origin deployment names its own site.** Deciding
"same site" for two arbitrary hosts needs that list: thousands of rules, revised monthly, and a
stale copy is wrong in the direction that matters. The application declares the registrable
domain it believes the two share and both hosts are required to lie within it — a checked claim
rather than a trusted one. A cross-**site** API is refused outright, because `SameSite=Lax`
cookies are not sent there at all and the session is already gone.

---

## Phase 4 — state and resources (`hammer/state`)

| | Task | Done when |
|---|---|---|
| [x] | `state/store.ts` | A minimal observable: subscribe returns unsubscribe, no framework, no globals |
| [x] | `state/cache.ts` | Bounded LRU keyed by `(route, params, identity)`; entry ceiling per class; **nothing persisted** |
| [x] | `state/resource.ts` | Freshness from the server's `Cache-Control`; stale-while-revalidate only where granted; in-flight dedupe |
| [x] | `state/invalidate.ts` | A mutation's declared invalidations applied locally and broadcast to other tabs |
| [x] | `state/session.ts` | Identity, the permission set, the `staleClient` flag from the descriptor hash, and a refetch on `403` |
| [x] | `state/paginate.ts` | Cursor pager; `hasMore` from the server's cursor, never from a count |
| [x] | `state/versioned.ts` | Carries the version read into every write; `VersionMismatch` surfaces a reconciliation, never a retry |
| [x] | `state/optimistic.ts` | Apply, confirm against the returned document, roll back on failure; a rollback is counted |
| [x] | `state/forms.ts` | Form state over a definition: per-field validation in code points, error placement, dirty tracking, submission with its version |
| [x] | `state/sections.ts` | Published and draft as separate keys; a draft is never served from the published key |
| [x] | `state/inbox.ts` | The notification store over `wire/sse`; idempotent handlers; unread count from the server, not inferred |
| [x] | `state/media.ts` | `srcset` construction from the namespace widths; upload orchestration through `imagePool` |
| [x] | `state/analytics.ts` | Consent gate at the door — an event requiring consent is **not queued**; batching; `sendBeacon` on `pagehide` |
| [x] | `state/workers/` | `imagePool` (2, memory-capped, `ImageBitmap.close()` on consume) and `decodePool`; every task body catches; a full queue rejects |
| [x] | Suite | Cache eviction, identity change clearing, multi-tab invalidation, a rollback, and a consent gate that stays shut |

Three rows the plan did not have. `wire/cache_control.ts`, because "freshness comes from the
server" is not a rule anything could follow until something could read the header, and the
header belongs to the layer that holds the `Response`. `wire/beacon.ts`, because `sendBeacon`
may be named in `src/wire/` and nowhere else and the sink that needs it is in `src/state/`.
And `state/counts.ts`, because five modules report tallies and one union of names is better
than five.

**Gate: MET.** 747 tests in 49 files, `tsc --noEmit` clean under every strict flag, six scripts
clean, and the memory test showing `imagePool` at two bitmaps with the third request refused
rather than queued.

**Three checks in `tools/` changed, and two of them fired on this repository's own prose for
the fourth and fifth time.**

`check-vocabulary.sh` read a `throw new Error(...)` as copy. `throw` is reserved for programmer
error here (`CLAUDE.md` §3.1), so the audience for one of those strings is whoever is holding
the stack trace and never the person using the application; firing on it would push every such
message into a wordless constant the next reader cannot act on.

`check-layering.sh` matched the WORD `document`. It is this repository's own word for the thing
an optimistic write confirms against and a versioned write carries the version of, so
`readonly document: T` in a layer that has never seen a DOM was firing the check on the
vocabulary rather than on the defect. It matches the global as it is USED now — `document.` or
`document[` — which gives up an alias and keeps every accidental reach.

The same script also assumed a layer was one flat directory. `src/state/workers/` reaching
`../counts.js` is a hop inside its own layer, and counting the `../` read it as an escape from
one; it resolves the specifier against the importing file now.

`check-wire-discipline.sh` fired on the one comment in `state/analytics.ts` that says the
beacon lives in `src/wire/` and why. The call-site patterns strip comments now; the
string-shaped ones still do not, because half of what they look for lives inside a string
literal. Every one of the five was driven against a deliberate violation afterwards, because a
check that cannot fail reports clean for the wrong reason.

**Seven things the work found rather than review.**

**A method named `fetch` in the state layer is not a false positive.** `check-wire-discipline.sh`
flagged `private async fetch(entry)` in the resource store, and the right treatment was to
rename it rather than exempt it: a method named after the thing a layer may not do is exactly
where a real one would eventually hide. It is `perform` now, which is also what the session
store calls its equivalent.

**Freshness had to reach the caller without the header doing so.** A `Cache-Control` value handed
up a layer is a header that gets logged (`CLAUDE.md` §5), and a second return value on `call`
would put an attempt's answer on a result that belongs to the whole call — a request that was
retried, refreshed and replayed has one body and several responses. It goes through a callback
on the call options, invoked on the attempt that produced the body, and the layer that caches is
the layer that dedupes so the request that RAN is the one with the header.

**A resource released before its load lands correctly abandons the read**, which three tests
asserted the opposite of before being fixed. That is the behaviour — an entry with no watchers
aborts its request — and it is worth recording because the tests were written by somebody who
had just written the code.

**The upload preflight was checking the wrong media type.** `uploadImage` checked the original's
type against the route's accept list and then re-encoded it, so a photograph converted to WebP
for a route that accepts WebP was refused for being a JPEG — the conversion refused for the
thing the conversion was doing. It checks the type the upload will CARRY now. Found by a test.

**A section field's `type` is a control, not a field type.** `SectionFieldType` and
`FieldTypeName` are two closed sets in the descriptor, and nothing bridges them: whether a
rich-text control stores `TEXT_LONG` is a product decision. The map is the application's,
`satisfies Record<SectionFieldType, FieldTypeSpec>`, which makes the run-time refusal in
`definitionsFrom` unreachable from a typed call site. Found by writing the reference consumer.

**`Resource` had no way to await its first settled value.** A submit handler reading the version
it is about to carry cannot subscribe and re-render its way to one, so every such caller would
have written its own subscribe-and-unsubscribe dance — which is where a listener gets left
attached. `ready(signal)` resolves on the first non-loading state and resolves rather than
rejects on an abort, because the caller is abandoning a wait rather than discovering an error.

**The reference consumer's package specifiers had never been resolved at run time.** Nothing
imported a value from `tests/testapp/app/` — only types — so `hammer/wire` was a specifier the
compiler mapped and the runtime had never seen. `vitest.config.ts` mirrors `tsconfig.json`'s
`paths` now, and the integration test is what caught it: type-checking alone would have let the
specifier stay a fiction, which is the opposite of what the reference consumer is for.

**And one about the plan.** "Nothing is persisted" cannot be asserted by a suite whose
environment has no storage APIs in it, because the absence is indistinguishable from the
absence of the thing that would have used them. `tests/state/persistence.test.ts` installs them
as tripwires and drives the layer through everything that would be tempted — a cached read, an
eviction, an identity change, a two-tab invalidation, a form holding a draft, and an analytics
batch the network refused — and its last case drives the tripwire itself.

---

## Phase 5 — the rendered surface (`hammer/dom`)

| | Task | Done when |
|---|---|---|
| [x] | `dom/sanitized.ts` | `SanitizedHtml` with a module-private constructor; **one** insertion site; a Trusted Types policy installed where available, throwing by default; two type-level tests: the site is not callable with a string, and the brand is not constructible from bytes |
| [x] | `dom/form.ts` | The form renderer over the field-type table: labels, descriptions, error placement, `aria-describedby`, `aria-invalid`, required semantics, and submission that does not reload |
| [x] | `dom/section.ts` | The section renderer; rich text through the insertion site and nothing else |
| [x] | `dom/auth.ts` | The login flow, the session gate and the permission gate. The largest logic in the library and it renders almost nothing: it drives a credential it cannot read (`docs/00-architecture.md` §5.1) |
| [x] | `dom/bell.ts` | The notification bell: unread count reconciled against the stream **and** the server's count, dedupe by event id, popover focus and return, `aria-expanded`, a live region that announces without stealing focus, mark-read as one idempotent versioned call |
| [x] | `dom/inbox.ts` | The inbox list: live region for arrivals, read/unread semantics, keyboard traversal |
| [x] | `dom/upload.ts` | The upload control: drag-and-drop, a size and type refusal before the first byte, progress from the stream, abort, and a keyboard path that does not depend on drag |
| [x] | `dom/image.ts` | `srcset`, `sizes`, intrinsic `width`/`height` to prevent layout shift, `loading`, `decoding`; no JS decode path |
| [x] | `dom/pager.ts` | Cursor paging controls; no page numbers, because there are no offsets |
| [x] | `dom/error.ts` | The error surface: the application's copy by code, the `request_id` displayed verbatim, and **no distinction rendered for a 404** |
| [x] | `dom/consent.ts` | The consent gate: explicit, revocable, and no event queued before it answers |
| [x] | Accessibility suite | Every component: labelled, reachable, operable by keyboard, focus visible and ordered, no `aria-hidden` over a focusable node, no positive `tabindex` |
| [x] | `chart/` | Scales, ticks, marks, hit regions, series traversal by keyboard, and the **data-table fallback** — a chart that exists only as pixels is a chart part of the audience cannot read. Colours, formatting and labels are the application's; no charting dependency, because a linear scale and a path string are arithmetic |
| [x] | Direction suite | Every component rendered under `dir="rtl"`; user text isolated; no physical-property layout |
| [x] | CSP suite | The DOM suite runs under a policy with no `unsafe-inline` and no `unsafe-eval`, and a component that sets an inline style fails it |

Four rows the plan did not have, each for the same shape of reason the phase-4
additions had — something two layers both needed, in a place only one of them
could reach.

`core/upload_bounds.ts`, because `dom/upload.ts` refuses a file before the first
byte and may not import `hammer/wire`, where the bounds lived. The choice was a
second copy of the arithmetic or moving the one copy down; two copies is how a
control accepts what the transport then refuses. `AnalyticsSink.consent`, because
`consentIs()` is a getter and a gate has to redraw when the answer changes
somewhere it did not call — every consumer would otherwise keep a `Store<Consent>`
beside the sink and write through it, which is two places that can disagree about
whether somebody consented. `dom/mount.ts` and `chart/mount.ts`, because eleven
components need one handle type and one document accessor, and chart may not
import dom. And `Form.definition(key)`, because the renderer needs the type to
pick a control and the bound to draw a counter, and a second copy of the
definitions is one that can disagree with what the validation uses.

**Gate: MET.** 1026 tests in 69 files, `tsc --noEmit` clean under every strict
flag, six scripts clean, and `dist/dom/index.js` and `dist/chart/index.js`
emitting — which is what makes the `exports` entries real rather than declared.

**Two checks in `tools/` changed, and both were driven against a deliberate
violation before being believed.**

`check-layering.sh` now refuses a bare `document.` or `window.` in `src/dom/` and
`src/chart/`. Those layers NEED a document, and that is the reason they are
checked rather than the reason they are exempt: a component takes its document
from the element it was asked to mount in, so a test supplies its own instead of
racing every other test in the file, and the same component renders into a
document that is not the tab's — a preview, a print view, a frame — with no
branch for it. `mount.ownerDocument` does not trip it, because the pattern is
case-sensitive and that is a capital D.

`check-vocabulary.sh` now scans `src/chart` for copy as it already scanned
`src/dom`. An axis caption, a units suffix and the header row of a data table are
each a word a person reads, and the layer was unpoliced.

**Nine things the work found rather than review.**

**happy-dom's `DOMParser` is not inert, and a browser's is.** It attaches the
parsed document to a window, so a `<script>` in one executes as the parser
appends it — during `sanitize`, before the walk has had a chance to drop it. A
real parser builds a document with no browsing context and runs nothing. The
dangerous cases therefore assert the STRING the sanitiser produces, which is the
real contract and the same in every environment; that a real parser runs nothing
is a phase-7 live-run row, and a unit test of an environment that gets it wrong
was never going to be worth anything either way.

**The insertion site does not use `innerHTML`, and the built-output row is why.**
A `// ban-exempt:` line would have satisfied `check-source-bans.sh` and left
`docs/16-test-plan.md`'s claim — that the string appears nowhere in a bundle —
failing. Parsing into an inert document and importing the nodes makes it
literally true, leaves `src/` with zero markup exemptions, and has a second
effect: `DOMParser` is not a Trusted Types sink, so the only policy hammer
installs is one that REFUSES rather than one that would have to be trusted.

**`ClassNames<AuthPart> & ClassNames<FormPart>` type-checks and is a trap.** Both
unions carry `root` and `error`, so one spread silently wins and the login form's
failure region ends up wearing the same class as every field's error.
`renderLogin` takes two tables. Found by writing the test that looked for it.

**happy-dom cannot remove a `<form>` from its parent.** It resolves the parent
through the form-owner rather than through the tree and throws `removeChild` at
itself, so `node.remove()` leaves a form renderer's root behind in the suite and
nowhere else. `detach` goes through `parentNode`, which is correct in both. The
failed call also leaves the tree unrecoverable, which is why the case for it uses
two.

**The tick routine's float snap did not snap.** `Math.round(v / step) * step`
reintroduces exactly the error it was meant to remove, so the axis still carried
`0.30000000000000004`. Ticks are counted from the first rather than accumulated,
and rounded to the step's own decimal count. Its thresholds were wrong as well —
the bare 1, 2 and 5 bias every decision upward and produced three labels where a
caller asked for five, so they are the geometric midpoints now.

**`renderPermissionGate` was exported and mounted by nobody.** The accessibility
pass compares its registry against what the entry points EXPORT, and that is the
whole value of the comparison: without it the enumeration is a list somebody has
to remember to add to, which is what it exists not to be.

**A focusable row is named by its contents and an input is not.** The first
version of `accessibleName` treated both the same and reported the inbox's list
items and the chart's table rows as unlabelled. A screen reader landing on a
focusable row announces what is inside it; an `<input>`'s neighbours are not its
label, and that asymmetry is why the `for`, `aria-label` and `aria-labelledby`
routes exist at all.

**A section field with no value never had its direction set.** The container
always holds somebody's words in whatever language they wrote them, and a field
that is empty today is one an editor fills tomorrow, so `dir` goes on
unconditionally and before anything is put in it.

**A CSP tripwire on `Document.prototype` never fires.** happy-dom defines
`createElement` as an own property of the document, which shadows it. The case
that drives the tripwires themselves is what caught it — a check that cannot fail
reports clean for the wrong reason, and this one reported clean for exactly that
reason until its own test was written.

**And one about what could not be asserted.** "No component contains a
user-visible string" is checked over the source and not over the bundle. Finding
string literals in minified JavaScript needs a real parse: a regex literal can
contain a quote — `escapeAttribute` contains exactly one — and a scanner that
pairs quotes by hand mistakes the code between two strings for a string, which is
what it did, reporting the library's own module boundaries as copy. A
dependency-free repository does not get a JavaScript parser in its suite for one
assertion, and a scanner that is wrong in the direction of passing is worse than
no scanner. `check-vocabulary.sh` owns that claim; the bundle keeps the half that
needs no parse, where a sink is a fixed identifier that either appears in the
bytes or does not.

---


## Phase 6 — the adapter and the reference application

| | Task | Done when |
|---|---|---|
| [x] | `react/hooks.ts` | `useResource`, `useMutation`, `useSession`, `useInbox`, `useForm`; correct under StrictMode's double invoke; every subscription released on unmount; no state update after unmount |
| [x] | Adapter boundary test | The React entry point contains no logic of its own — it binds stores. Asserted by a size ceiling and by the core suite covering the behaviour |
| [x] | `tests/testapp/` | The reference consumer: the generated client, the three application tables, one screen per seam. Type-checked on every build |
| [x] | Contract suite | Recorded anvil responses: every error code decoded, every route built, the envelope's shape asserted against anvil's writer |
| [x] | Live suite | **Run.** Eleven cases against `anvil_reference_server`, started by `tools/run-live.sh`. Three of the six the row asked for are still unwritable — the reference application has no upload route, no HTTP stream route and no write route — and the versioned case asserts the half that is reachable: a write with no version read is refused rather than sent |

**Gate: MET.** All five rows are done and green: 1,170 tests in 76 files, `tsc --noEmit` clean
under every strict flag, eleven scripts clean, `dist/react/index.js` emitting, and eleven live
cases passing against `anvil_reference_server`.

`tools/run-live.sh` starts the server, reads its base URL and its two passwords off the
server's own stdout — both drawn at boot and printed once — hands it a database of its own and
drops it afterwards. `npm run test:live` still works with the variables set by hand, is
excluded from `npm run check`, and fails rather than skips with no `HAMMER_LIVE_ORIGIN`.

**The first execution found the defect that justifies the whole row.** `decodeSessionView` was
written against a session payload anvil does not send: `perms` as a base64url mask at the root,
where anvil writes an `authority` object carrying permission NAMES, and a `hash` no anvil writer
produces. Every one of the 1,145 unit tests in place at the time passed over it, because every
one of them built its payload with the same fixture the decode was written against — two copies
of one belief. Against a real anvil the client signed in and could then call nothing.

**Four things the phase found rather than review.**

**Three of the live row's six cannot be written at all.** The reference descriptor has eleven
routes: no upload route, no stream route, and no write route past `auth.login` and
`auth.refresh`. So the versioned-conflict case sends its body to `content.get` — which is what
`tests/testapp/app/state.ts` has had to do since phase 4 — and `wire/upload.ts` and
`wire/sse.ts` are driven by the unit suites and by nothing else.

**`request_id` is in no writer anywhere in anvil.** It has been in anvil's architecture
document since phase 0, every error surface this library ships displays it verbatim, and the
only two bodies anvil assembles carry the code and nothing else. The `fields` map is missing
the same way, and that one is sharper: a form that cannot place a server reason on a field
refuses to submit with nothing marked on it.

**The adapter's rule needed a size ceiling to be a rule.** "It binds stores and holds no logic"
is a sentence, and behaviour arrives in an adapter one helpful commit at a time. The bundle is
664 bytes gzipped against a declared kilobyte; the edge assertion is the stronger half — every
import of a layer below is `import type` save one constant, so there is nothing down there the
hooks could be calling.

**`kResourceLoading` had to be exported rather than copied.** `useSyncExternalStore` compares
snapshots with `Object.is`, so the loading state a resource has before its subscription exists
must be the same OBJECT the store publishes. A literal of the same shape, built per call, is an
infinite render loop — and it is the kind of defect that appears only under a framework, which
is exactly what the react suite is for.

---

## Phase 7 — budgets, hardening, and the runs that only end-to-end can make

| | Task | Done when |
|---|---|---|
| [x] | `tools/check-bundle-budget.sh` | Per-entry-point gzipped ceilings, declared in the repository and read by the build. Exceeding one fails |
| [x] | The ceilings | `hammer` ≤ 8 KB, `hammer/wire` ≤ 12 KB, `hammer/state` ≤ 12 KB, `hammer/dom` ≤ 18 KB, `hammer/chart` ≤ 9 KB, `hammer/react` ≤ 3 KB, gzipped. Each is measured at the first build and **raised only in a commit whose body says what bought it** |
| [x] | Source-map policy | A privileged bundle publishes no public source map — it re-leaks everything tree-shaking removed (`docs/01-seams.md` §4.2). Asserted against the built output |
| [x] | Protected-asset note in the deployment doc | Where the existence of a feature is itself sensitive, its entry point is served from a location behind the session. One Nginx block, and the only thing that stops `curl` |
| [x] | **The two-tab credential run** | **Run.** Two real tabs of one session: the access cookie is deleted from the browser's jar to make the expiry real, both tabs are driven into a call at the same instant, and exactly one refresh happens with `unelected` and `takeovers` both zero. The logout fan-out empties the tab that did not sign out, and `document.cookie` carries no `__Host-` before or after a rotation |
| [x] | The freeze/discard run | **Run.** The request is slowed to 3G first so the freeze lands while it is on the wire, and one call records one outcome. The WRITE half is unwritable — the reference application has no write route — and the case is named for what it asserts rather than for what the row asked |
| [x] | The slow-network run | **Run**, in the half that is reachable: throttled to 3G, twenty concurrent calls never have twenty in flight and all twenty are answered — shedding and bounding score the same on the first assertion and are opposite behaviours. `Retry-After` is skipped and says why: the reference application installs no rate limiter, so no route's limit can be reached deliberately (§Cross-repo) |
| [x] | Trusted Types run | A page enforcing Trusted Types renders every component and no policy violation is reported |
| [x] | Release | Semver, the published package name chosen, `exports` frozen, and a `CHANGELOG.md` whose entries name the migration for every entry-point change. **Since Phase 8, a release also needs `npm run check:production` green**, which it is not until Phase 8 Part B empties the tolerated tier |

**Gate: MET.** All nine rows are done and green, and the three that needed a server run against
`anvil_reference_server` through `tools/run-live.sh --browser`: twelve browser cases pass and
one is skipped, which is the `Retry-After` case and which says why — the reference application
installs no rate limiter, so no route's limit can be reached deliberately (§Cross-repo). The
server being present and unable to answer is a different thing from the server being absent,
and only the second is a failure.

**The two-tab credential run — the row above that says it is not optional — executes.** The
access cookie is deleted from the browser's own jar to make the expiry real, which is what the
first version of the case could not do: it drove both tabs into a call, neither refreshed, and
"at most one refresh" held vacuously. The assertion is `exactly one` now, with `unelected` and
`takeovers` both zero, both calls answered, and `document.cookie` carrying no `__Host-` before
or after the rotation.

The Release row is met as an INTERNAL library: hammer and anvil are two halves of one
framework, consumed by this organisation's applications, so the package stays `private` and has
no registry name. Semver starts at 0.1.0, `exports` is frozen at seven entry points, and
`CHANGELOG.md` carries the one migration this release has along with a plain statement that the
credential lifecycle is unverified against a real server.

**Five things the phase found rather than review.**

**`DOMParser` is a Trusted Types sink, and this library's source said it was not.** The
sentence had been in `src/dom/sanitized.ts`'s header since phase 5, load-bearing: it was the
stated reason hammer needed no permissive policy of its own. It is false.
`parseFromString(..., "text/html")` is a sink, so on any page enforcing
`require-trusted-types-for 'script'` every rich-text render threw — on exactly the deployment
careful enough to serve the policy this repository's own documentation prescribes. All 1064
tests passed while it was broken and every one of them would have kept passing, because no fake
document enforces a CSP. **This is the entire argument for the phase existing**, and it was
found on the browser run's first execution. The fix is the shape Trusted Types asks for: one
named policy at the one audited place a string becomes markup, which is the claim
`SanitizedHtml`'s private constructor already makes to the compiler, made to the platform
instead. Consumers must now serve `trusted-types hammer default`.

**Every ceiling was met at the first measurement, and that is the least interesting half of the
script.** core 3649/8192, wire 9696/12288, state 10122/12288, dom 6651/18432, chart 2188/9216,
react 664/3072 — four of six with more than half their budget unused, because the numbers came
from this document rather than from a build. What the script is actually worth is the coverage
assertion: every subpath in `exports` must be either budgeted or declared build-time only, in
both directions. A ceiling with headroom is a ceiling; an entry point with no ceiling at all is
bytes nobody is measuring.

**The build was publishing source maps, and nothing in the repository noticed.** `sourceMap`
and `declarationMap` were inherited from the development `tsconfig.json`, so `dist/` carried a
`.js.map` per module — for a library an application BUNDLES, which means a bundler folds them
into the application's map, and the application's bundle is the one the holder route table was
never emitted into. The declaration maps were duller and worse: `files` publishes `dist` and not
`src`, so every `.d.ts.map` pointed at a file that was not in the package.

**Three of the four browser runs are blocked on the row the live suite is blocked on, and
writing them is what proves it.** They are written, they fail loudly, and the only thing
missing is a server. The page also has to be SAME-ORIGIN with anvil — not a convenience, since
hammer refuses a cross-site API because `SameSite=Lax` cookies are not sent there — so the
harness fulfils its own bundle at one path on the live origin and touches nothing else. A run
that intercepted an API call would be a stub, and the whole point of the suite is that it is
not one.

**"`exports` frozen" needed a snapshot to be a rule.** It is a sentence, and a library's surface
moves one helpful refactor at a time. `tools/public-surface.txt` is 374 committed lines,
regenerated and diffed by `npm run lint`. Types are in it as well as values, for the reason that
makes them easy to drop: a type costs a consumer nothing at run time and is still a compile
error in every application that imported it.

---

## Phase 8 — the dependency policy, and the tree it has to shrink

**The runtime was always empty. The build was not.** `dependencies` has been `{}` since phase
0 and `tools/check-dependencies.sh` has enforced it from the first commit. The toolchain that
builds and tests hammer is ten `devDependencies` resolving to **131 packages and 110 MB**, and
on 2026-09-23 `npm audit` over that tree reported **6 advisories: 2 critical, 1 high, 3
moderate**:

| Package | Severity | Reached through | Fixed in |
|---|---|---|---|
| `happy-dom` ≤ 20.8.8 | critical: VM context escape to RCE, unsanitised export names compiled as code, cross-origin cookie use in its `fetch` | direct | 20.14.5 (major) |
| `vitest` ≤ 4.1.10 | critical | direct | 5.0.1 (major) |
| `vite` ≤ 6.4.2 | high | `vitest` | via `vitest` 5 |
| `@vitest/mocker`, `vite-node` | moderate | `vitest` | via `vitest` 5 |
| `esbuild` ≤ 0.24.2 | moderate: its dev server answers any origin | direct | 0.25.0 and later |

None of these reach a consumer, because no dev package is in `dist`. All of them run on the
machine that builds the SDK, which is the one holding the publish token. The rule this phase
writes down is [`CLAUDE.md`](../CLAUDE.md) §12. The plan is two halves:

- **Part A** makes the policy mechanical: tiers, a warning in development, an error in
  production.
- **Part B** retires the tolerated tier. Every Part B row unblocks a release, because until the
  tier is empty `npm run check:production` is red by design.

Nothing here changes a byte on the wire. The anvil contract (the envelope, the cookies, the
descriptor format, the cursor rule) is untouched. The proof is `tools/run-live.sh` and
`tools/run-live.sh --browser` passing after each part, as well as before.

### The tiers, as decided

| Package | Tier | Why |
|---|---|---|
| `typescript` | allowed | The compiler. Microsoft, zero dependencies, a decade of releases. Pinned to the installed 5.x; a move to the native 7.x compiler is its own row |
| `esbuild` (+ its one `@esbuild/<platform>` binary) | allowed | Nothing in the platform measures what a consumer's bundler emits, and the byte budgets, the tree-shaking assertions (`tests/codegen/built_output.test.ts`) and the markup-sink scan of a shipped bundle (`tests/dom/bundle.test.ts`) are all claims about exactly that. Zero JS dependencies. Upgraded past its advisory. Its install script is not needed when the platform binary is installed as an optional dependency, so `ignore-scripts=true` leaves it working |
| `react`, `react-dom`, `scheduler` | allowed | The peer `hammer/react` adapts. An adapter tested without the thing it adapts is tested against a belief |
| `@types/react`, `@types/react-dom`, `csstype`, `@types/node`, `undici-types` | allowed | Declarations only; nothing in them executes |
| `vitest` (+ ~100 transitive, incl. `vite`, `rollup`, `fsevents`) | **tolerated** | Replaced by `node:test` + `node:assert` and a small in-repo `expect` (Part B) |
| `happy-dom` | **tolerated** | Replaced by the real Chromium the browser suite already drives (Part B). A fake DOM is a belief about the DOM, and this one was wrong three times already (§Phase 5: an executing `DOMParser`, a `<form>` it could not remove, a shadowed `Document.prototype` trap) |
| `playwright-core` | **tolerated** | Zero dependencies and well-owned, but a large surface for the 16 calls the suite makes. Replaced by an in-repo DevTools-protocol client over Node's built-in `WebSocket` (Part B) |

### Part A — the gate

| | Task | Done when |
|---|---|---|
| [x] | `tools/dependency-policy.json` | Three keys: `allowed`, `tolerated`, `installScripts`. Every `allowed` root carries `why`, `owner`, `replaces` (the built-in it was weighed against) and `tree`, the exact list of package names it may bring into the lockfile. Every `tolerated` root carries `why`, `replacement` and `task` (a pointer to its Part B row). `installScripts` names each lockfile package with `hasInstallScript` and why it is harmless under `ignore-scripts` |
| [x] | `tools/check-dependencies.sh` reads the policy | The three existing checks stay, and stay errors in every mode. New checks: a `devDependencies` key in no tier is an **error**. A non-exact version spec is an **error**. A lockfile package not reachable from an allowed root's `tree` and not under a tolerated root is an **error**. A package under a tolerated root is a **warning** that names the root and its task row. An install script not listed in `installScripts` is a **warning**. `.npmrc` missing `ignore-scripts=true` is an **error**. A summary line always states whether the tree is releasable |
| [x] | `--production` | Every warning above becomes an error. Driven against the current tree it **fails**, naming exactly the three tolerated roots — 91 packages under `vitest`, 4 under `happy-dom`, 1 under `playwright-core` — and nothing else; that failure is the expected state until Part B lands |
| [x] | Every new check driven against a violation | A caret spec, an unlisted devDependency, an unlisted transitive package, a missing `.npmrc`, each written to fail before the check is trusted (the Phase 0 rule) |
| [x] | `tools/check-audit.sh` | Runs `npm audit --json` over the whole tree. Development: a finding is a warning, and an audit that cannot reach the registry is a warning that says so. `--production`: any finding at any severity is an error, and an audit that cannot run is an **error**. Not part of `npm run lint`, which must pass offline — driven against a real advisory (six, on 2026-09-23) and against a registry pointed at a closed port |
| [x] | `.npmrc` | `ignore-scripts=true`, `save-exact=true`, `engine-strict=true`, `fund=false`. Verified by `rm -rf node_modules && npm ci` followed by a green `npm run check`, because the one package with an install script it needs to survive is `esbuild` — it still runs, off the optional `@esbuild/linux-x64` package rather than its disabled postinstall |
| [x] | Exact pins | Every `devDependencies` spec is the installed version with no range. The lockfile's root entry is updated to match (`npm install --package-lock-only --ignore-scripts`); no other lockfile entry changed |
| [x] | `esbuild` 0.24.2 → 0.28.2 | Its own commit. The budget table from `tools/check-bundle-budget.sh` is in the body before and after, and no ceiling is raised to absorb the upgrade |
| [x] | `npm run check:production` | `npm run check && tools/check-dependencies.sh --production && tools/check-audit.sh --production`. The one command a release, the SDK's release and a tag build run — red today by design, on the tolerated tier and six advisories, until Part B lands |
| [x] | CI | The `check` job runs `npm ci` and `npm run check` on every push and surfaces each dependency warning as a `::warning::` annotation (from `tools/check-dependencies.sh` itself, under `GITHUB_ACTIONS`). A second job, `release-gate`, runs `npm run check:production` on `v*` tags and has no `continue-on-error`. Both run on Node 22, the oldest supported line |
| [x] | Node 22 | `engines.node` `>=22`, and README's table matches. Node 20 went end-of-life on 2026-04-30 and gets no security fixes. `CHANGELOG.md` records the change, since `engines` is read by a consumer's install |
| [x] | Docs | `README.md` names `npm run check:production`. `docs/16-test-plan.md` §Dependencies and `docs/03-deployment.md` §6 carry the gate — all three landed with the policy itself in "Plan the development dependency policy" |

**Part A gate: MET.** `npm run check` is green — 1170 tests in 76 files, `tsc --noEmit` clean,
eleven scripts clean, `tools/check-dependencies.sh` warning on exactly the 96 packages under
`vitest`, `happy-dom` and `playwright-core` — and `npm run check:production` fails for exactly
those 96 packages plus the six advisories `tools/check-audit.sh --production` reports on the
same tree, which is the state Part A exists to make mechanical rather than remembered. Nothing
here touches `src/`, so nothing here is a claim about the wire; the live and browser runs were
not re-executed this round and remain whatever `tools/run-live.sh` last reported.

### Part B — retire the tolerated tier

Each row deletes one `tolerated` entry and its whole subtree in the same commit (`CLAUDE.md`
§11.1). The order matters, because B2 and B3 need the runner B1 builds.

| | Task | Done when |
|---|---|---|
| [x] | **B1 — `vitest` → `node:test`** | `tests/support/expect.ts` implements exactly the matchers the suite uses (today `toBe`, `toEqual` with Jest's undefined-property semantics, `toContain`, `toMatchObject`, `toHaveLength`, `toMatch`, `toBeNull`, `toBeDefined`, `toBeUndefined`, `toBeTruthy`, `toBeInstanceOf`, the four orderings, `toContainEqual`, `toThrow`, `toHaveBeenCalled(With\|Times)`, `.not`, `.resolves`/`.rejects`), each with its own test. `describe`, `it`, `it.each` and the four hooks come from `node:test`. `vi.resetModules` is replaced by a fresh-instance import in `tests/dom/sanitized.test.ts`. The `hammer/*` aliases become a `node:module.registerHooks` resolve hook in `tests/support/register.mjs`, the same map `tsconfig.json` carries. Test count before and after is equal |
| [x] | **B2 — `happy-dom` → Chromium** | The DOM suite runs in the page the browser harness already serves, under the real CSP header, bundled by `esbuild`. `npm run check` now needs a Chromium and **fails rather than skips** without one (`HAMMER_BROWSER` or the system binary). The `happy-dom` workarounds in `src/` (`src/dom/mount.ts`) and in tests are removed, and each removal is checked against a real browser, not assumed |
| [x] | **B3 — `playwright-core` → a DevTools-protocol client** | `tests/browser/cdp.ts`: launch with `--remote-debugging-pipe` or a port, speak JSON over Node's `WebSocket`, and implement only the calls the suite makes (`Target.*`, `Page.navigate`/`reload`, `Runtime.evaluate`, `Network.getCookies`/`clearBrowserCookies`/`emulateNetworkConditions`, `Fetch.enable`/`fulfillRequest`, `Page.setWebLifecycleState` for the freeze run). The three browser rows run against the reference server and pass, run by `tools/run-live.sh --browser` |
| [x] | **The tolerated tier is empty** | `npm run check:production` is green on a clean `npm ci`. The lockfile holds only the allowed trees; the package count and `node_modules` size are in the body of the commit that gets there |

**B1: MET.** 1,170 tests passed under vitest 2.1.9 in 76 files; `node --test` over the same
suite plus `tests/support/expect.test.ts` reports 1,216 — the same 1,170, unchanged in
substance, plus 46 new cases proving the matchers that now stand in for `vitest`'s own. No case
was dropped. Two things the grep-first survey missed and the type-checker caught: `expect(actual,
message)`, a second parameter several property-based cases use as a per-iteration label, and
`it.runIf(condition)`, used by the two superadmin-only live rows and one rate-limited browser
row. Both are now part of `tests/support/expect.ts` and `test.ts`. `toBeInstanceOf`,
`toBeUndefined` and `toHaveBeenCalledWith` were in the suite already and are implemented, which
the original row's matcher list did not name. `tools/check-dependencies.sh` warns on exactly two
tolerated roots now, `happy-dom` and `playwright-core`; the lockfile went from 132 packages
(111 MB) to 41 (86 MB). `tools/run-live.sh` and `tools/run-live.sh --browser` both still pass
against a real `anvil_reference_server` on this runner's own machine, unchanged from before this
row.

**B2: MET.** Nineteen of the twenty files `grep -rl happy_dom_env tests` named actually
imported it; the twentieth, `tests/core/environment.test.ts`, only mentions the specifier in a
comment warning against exactly this happening by accident, and stayed a plain `node:test`
file. `tests/support/test_browser.ts` is the browser backend, exporting the identical surface
`test.ts` does; both now share `it.each`'s formatting from a new `tests/support/each.ts` rather
than keeping two copies. `tests/dom/in_browser.test.ts` launches one Chromium for the whole
run, bundles each of the nineteen files with `esbuild` (a resolve plugin maps `hammer/*` from
`package.json`'s `exports`, the same map `tests/browser/harness.ts` already derived, and
redirects the one specifier `"../support/test.js"` to `test_browser.ts` rather than `test.ts`),
serves each bundle from its own path in one HTTP server, and runs it in a fresh browser context
under the exact CSP `tests/browser/harness.ts` already serves for the Trusted Types run. Every
browser result is replayed as its own `node:test` `it()`, nested under `describe`s matching its
original path, so a failure's output names the file, the suite and the case exactly as it did
under `node --test` directly.

Four things a fake DOM had no opinion on, and a real browser did:

- **`DOMParser.parseFromString` is a Trusted Types sink**, confirmed a second time: two DOM
  tests (`tests/dom/mount.test.ts`, `tests/dom/sanitized.test.ts`) built a "foreign document" by
  parsing empty markup, which throws under this suite's own enforced CSP. Both now use
  `document.implementation.createHTMLDocument()`, which needs no parse.
- **A background page does not reliably move `document.activeElement`.** A page Chromium never
  activated let `.focus()` calls no-op silently; `tests/dom/bell.test.ts`'s focus-return case
  caught it, and the driver now calls `Page.bringToFront` once per context before evaluating.
- **`window.trustedTypes` is a getter with no setter.** happy-dom implements no Trusted Types at
  all, so `tests/dom/sanitized.test.ts`'s `freshSanitizer` helper could assign over it directly;
  a real browser throws `Cannot set property trustedTypes ... which has only a getter`. The
  helper now uses `Object.defineProperty`, saving and restoring the original descriptor.
- **A hidden element is blurred synchronously, to `document.body`.** `src/dom/bell.ts` read
  `document.activeElement` to decide whether focus was still inside the popover AFTER setting
  `popover.hidden = true` — which a real browser had already blurred by then. The check now
  runs before `hidden` is set. This is a genuine defect in shipped code that no suite had ever
  run in an environment where hiding an element actually moves focus, found the same way the
  Trusted Types `DOMParser` defect was in Phase 7.

One more thing was esbuild's own, not the browser's: `sanitized.test.ts`'s cache-busting
`import(\`...?fresh=${n}\`)` is exactly the shape esbuild's automatic "glob import" bundling
matches, which inlines every statically-discoverable match into one module instead of leaving
the expression as a genuine runtime `import()` — defeating the whole point of a fresh module
instance per call. Building the specifier in its own variable, one line above the `import()`
call, is outside the syntactic pattern esbuild matches and restores the real dynamic import;
the driver's HTTP server serves the resulting request (`/src/dom/sanitized.js?fresh=1`, and so
on) by transpiling the source on demand, the same `.js`→`.ts` extension mapping
`tests/support/register.mjs` already does for Node.

The one `happy-dom` workaround in `src/` — `src/dom/mount.ts`'s `detach()`, which went through
`parentNode`/`removeChild` because happy-dom 15 threw `removeChild` at itself when a `<form>`'s
short-spelling `.remove()` was called — is gone; `detach` is `node.remove()` now, checked
against Chromium first (`tests/dom/mount.test.ts`'s form case, rewritten to assert the ordinary
outcome rather than the fake DOM's bug). The CSP tripwire test that used to trap
`document.createElement` on the instance rather than `Document.prototype` because happy-dom
shadowed the prototype method — `tests/dom/csp.test.ts` — keeps that shape: it is correct in
both environments and the comment is now historical. That same file's own tripwire probe, and
two `tests/dom/sanitized.test.ts` cases that fed a mocked `trustedTypes` through a real parse,
now catch the real refusal a browser under enforcement produces rather than asserting the
mocked round trip happy-dom let through uncontested.

Test count: 276 in the nineteen files before this row (under happy-dom) and 276 after (in
Chromium), all under the same names except the two noted above
(`tests/dom/mount.test.ts`'s form case and two `tests/dom/sanitized.test.ts` policy cases,
renamed because what they assert changed). `npm run test` overall: 1,216 tests, unchanged.
Wall time for `npm run test`: 2.8s before this row (happy-dom) and 5.0s after (one Chromium,
nineteen bundles, nineteen contexts) — under 2× and well inside the "about 3×" bound, so no
further work went into it. `tools/run-live.sh` and `tools/run-live.sh --browser` are unchanged:
eleven live, twelve of thirteen browser, the same `Retry-After` skip. No Chromium process or
`--user-data-dir` temporary directory survives a passing or a failing run, checked with `pgrep`
after each.

**B3: MET.** `tests/browser/cdp.ts` speaks JSON over `--remote-debugging-pipe` (fds 3 and 4)
rather than a `ws://` port: the two are the same protocol, and the pipe is never a socket a
second process on the machine could attach to, which a debugging port always is. Flattened
sessions throughout — `Target.createTarget` then `Target.attachToTarget({flatten: true})` per
page — surfaced one quirk this Chromium build has that playwright's own client absorbs
invisibly: creating a target inside a freshly created `Target.createBrowserContext` fails
`"Failed to open new tab - no browser is open"` unless `newWindow: true` is set (verified
against Chromium 153.0.8010.52). `context.clearCookies({name})` needed a different primitive
than the obvious one: `Storage.setCookies` is additive, so writing back every cookie except the
one being removed does not remove it — checked before being trusted, the Phase 0 rule —
and `Network.deleteCookies`, a per-page rather than a per-context command, is what actually
deletes one. Two non-vacuity checks against the live reference server: skipping the deliberate
`clearCookies({name: "__Host-at"})` call turns "refreshes once across both tabs" from a pass
into `expected 0 to be 1`, and swapping the bundle and document bodies `context.route` serves
turns four cases across both live-tab suites into `TypeError: Cannot read properties of
undefined (reading 'login')` — the route's `Fetch.enable`/`fulfillRequest` path is genuinely
exercised, not a no-op the tests happen not to notice. `tools/run-live.sh --browser` gives the
same result as before this row: eleven live cases, twelve of thirteen browser cases, the
`Retry-After` skip unchanged. No `--remote-debugging-pipe` Chromium process or `user-data-dir`
temporary directory survives a run, passing or failing, checked with `pgrep -f user-data-dir`
after each. `npm uninstall playwright-core` took the lockfile from 41 packages (86 MB) to 40
(73 MB); `tools/check-dependencies.sh` now warns on exactly one tolerated root, `happy-dom`.

**The tolerated tier is empty: MET.** `npm uninstall happy-dom` removed the last tolerated
root; `npm` dropped `"dependencies": {}` from `package.json` on the way, the same regression
Phase 8 Part A's own notes warned about, and it is restored in the same commit. The lockfile
went from 40 packages (73 MB, B3's ending state) to 11 (47 MB) on a clean `npm ci`.
`tools/check-dependencies.sh` prints `dependencies: releasable — no error, no warning` with the
`tolerated` object empty, and `npm run check:production` — `npm run check`,
`tools/check-dependencies.sh --production` and `tools/check-audit.sh --production` — is green
on `rm -rf node_modules && npm ci`, `npm audit` included, zero advisories at any severity.

**Phase 8 gate: MET.** Every row in Part A and Part B is done. `npm run check:production`,
red by design since the phase opened, is green. Nothing in `src/` changed except the two
defects a real Chromium found and this phase's own tests now hold open: the Trusted Types
`DOMParser` sink (Phase 7) and the hidden-element focus blur (`src/dom/bell.ts`, B2 above).
`tools/run-live.sh` and `tools/run-live.sh --browser` pass exactly as they did before the phase
began: eleven live cases, twelve of thirteen browser cases, the same `Retry-After` skip.

---

## Phase 9 — edit an image

Crop, rotate, flip, resize and freehand drawing on a stored image. The design is
[`04-image-edits.md`](04-image-edits.md), and anvil's half is anvil `docs/21-image-edits.md`.
The client builds a canonical **recipe** and previews it as one SVG. The server renders the
recipe into a new object, so no edited pixel is ever produced in the tab.

| | Task | Notes |
|---|---|---|
| [x] | `docs/01-seams.md` §12 — `limits.edit`, the two routes | **Closed**, as a subsection of §12 landed with the generator change that emits `kEditLimits`. It says that the generator does not know which routes are the edit routes, and why |
| [x] | `codegen` — read `limits.edit` | **Closed.** Emitted as `kEditLimits`, and refused when a bound cannot travel in the recipe: a stroke count past one byte, an edge past sixteen bits, a narrowest edge above the widest. **The row's second half was not built as written.** Refusing a descriptor with `media.edit` and no bounds needs the generator to know which route is the edit route, and that is an application's route id. Instead, no `kEditLimits` is emitted without the block, so an editor mounted against such a server fails to type-check, which is the same failure moved to where hammer can see it |
| [x] | `edit/recipe.ts` — the codec and the validator | **Closed.** Passed all thirty golden vectors on its first run, field by field against what anvil's decoder read. `kFaultWire` names the field and reason for every fault. **One departure:** coordinates are not branded. `EncodedRecipe` is, and the encoder refuses any value that is not an integer in 0–65535, so a float cannot reach the wire whichever way it was made. The brand at the boundary is the one that pays |
| [x] | `edit/geometry.ts` | **Closed.** Beyond the row: `rotate` and `mirror` carry the crop and every stroke with the picture, as exact integer maps, and a case pulls a stroke back to source pixels through every orientation. Found while writing it: a clockwise turn of a MIRRORED picture is one turn fewer of the source, and the obvious `turns + 1` rotates a flipped photograph the wrong way |
| [x] | `edit/history.ts` | **Closed.** 100 entries, shared strokes asserted by identity, a drag is one entry and an undo mid-drag abandons it |
| [x] | `edit/submit.ts` | **Closed.** `detach` is required. A stored recipe that does not decode is refused rather than dropped |
| [x] | `edit/editor.ts` — `renderImageEditor` | **Closed**, and run in Chromium through `tests/dom/in_browser.test.ts`. Found by that run: `setPointerCapture` throws for a pointer that is not active, which is every script-dispatched event and every pointer that has already lifted, so capture is attempted and not required. **Two departures:** the preview address is the application's, handed in from the route builder, because an SVG `<image>` takes no `srcset`; and the stroke-point budget is checked when the stroke ends, because it cannot be known until the stroke is decimated, so an over-budget stroke is refused whole rather than stopped mid-gesture |
| [x] | The entry point — `hammer/edit` | **Closed.** 7.7 KB gzipped against a 9 KB ceiling. `tests/testapp/app/media_edit.ts` opens an editor with nothing but generated constants, and the editor's words are in both reference locales |
| [x] | Browser suite — the preview matches the render | **Closed**, without the reference server serving bytes: anvil's `testapp_emit_edit_renders` writes the renders as a fixture, as `testapp_emit_edit_vectors` writes the vectors, and `tests/edit/parity.test.ts` screenshots the real preview at each output size with `Page.captureScreenshot`'s `scale`, so the browser repaints the vector at output resolution rather than resampling a bitmap. Turn, flip and crop agree to the byte; strokes 0.02 mean; resize 0.19; all at once 0.28, outliers only on a stroke's edge. The flip-less control scores 42 and 81%. One pixel is inset from every edge: the browser filters an image's edge against transparency and libvips extends it, 50–70 levels along one whole edge, and nowhere else. It runs in `npm test`, needing Chromium and no anvil checkout |
| [x] | Live suite — against `anvil_reference_server` | **Closed**, as `tests/live/media_edit.test.ts`. The reference server now seeds one picture and prints `media <ns> <id>`. A recipe renders at exactly the planned size, the same edit twice is one object, an edit reopens on its source, a chained edit is refused, a detached edit is new every time, and a recipe sent past the client's check comes back with the field and reason `kFaultWire` predicted. **The scoping case was tried and dropped:** a second account's sign-in spent the run's login budget into a 429, and scoping a route away from a holder is `anvil.test.ts`'s assertion already |

---

## Phase 10 — chat, plaintext and polled (`hammer/chat`)

Direct conversations, groups and channels, as the client half of anvil §Phase 18. The design is
[`05-chat.md`](05-chat.md), and anvil's half is anvil `docs/22-chat.md`. This phase is a complete
plaintext client that polls. Phase 11 makes it live, Phase 12 draws it, and Phases 13–15 encrypt
it. Each is usable without the next, as anvil's are.

**A conversation is an ordered log with one writer of order, the server's sequence number,** and
the client never looks for a hole in it (05 §5.1).

**Not blocked.** anvil's routes, the descriptor's `limits.chat` and a seeded group in the
reference server have all landed. The upload rows wait on a cross-repo row (§Cross-repo, "a chat
upload route"), and nothing else in the phase needs it.

| | Task | Notes |
|---|---|---|
| [ ] | `codegen` — descriptor format 4, and `limits.chat` as `kChatLimits` | The fixture regenerated from anvil's `testapp_emit_descriptor` in the same commit as the reader change, because `check-descriptor.sh` regenerates the client from the committed descriptor and is green by construction. `kChatLimits` is `as const`, so a kind key, a shape, an encryption mode and a right name are each a union. `"chat": null` emits nothing, so `createChat` fails to type-check against a server without chat (05 §3.1). The generator refuses a kind that anvil's `kinds_are_well_formed` would have: a direct kind that is not two members, an encrypted channel, an encrypted kind with full history, an unknown right. Each refusal has a case against a hand-edited fixture |
| [ ] | `docs/01-seams.md` §24 — chat | In the same commit as the first code that reads a chat seam (`CLAUDE.md` §1, §11.1). Carries 05 §12's list, says that rights are affordances only, and says in so many words that `chat.create` is not retried and why |
| [~] | `chat/text.ts` — the composer's validator | anvil's `chat/text` restated (05 §4): code points against the kind's bound, `\n` the only break, C0/C1 except `\n` `\t`, bidi overrides and embeddings refused and isolates allowed, NFC REQUIRED rather than applied, blank refused, mention spans in code points inside the text, a reaction as one grapheme cluster of at most 8 code points, the preview URL's scheme. The one UTF-16 ↔ code-point conversion for mention offsets lives here. **Closes only against a shared vector file** (§Cross-repo, "golden vectors"), as the edit recipe's did. Until then the cases are written from anvil's `chat_text_test.cc` table and say so in their file's header **Built, open on the vectors.** Every one of anvil's cases ported in its order, all green on the first run; the invalid-UTF-8 bytes a JavaScript string cannot hold are asserted as the lone surrogate it can. `src/chat` is declared a layer (`core wire state`, no document, no `BroadcastChannel`, no `fetch`), and the ban was driven against a probe before it was trusted |
| [ ] | `chat/answers.ts` — the one decoder of every chat answer | Message, conversation, membership, list item, history page with tallies and `older`, `read_by`, presence, invite token. Narrowed once from `unknown`; a 2xx body that is not the shape is `chat-answer`. Grants become a branded `MediaGrant` and times a `ServerInstant`. An unknown message kind, system event or role decodes to `unknown`. **Written against recorded bytes**: the live row below records each answer the reference server writes, and this suite decodes the recording, because a decoder checked against a fixture written beside it is two copies of one belief |
| [ ] | `chat/calls.ts` — the calls, and the retry table | One function per route over `ChatRoutes`, each taking an `AbortSignal`. The retry decisions are 05 §3.4's table, asserted per route in one case each. `create`, `add_members`, `remove_member`, `revoke` and `create_invite` are never retried and surface a lost answer as `unknown-outcome`. A `404` on any conversation-scoped call is not-found and NEVER a permission error (`CLAUDE.md` §5) |
| [ ] | `chat/sync.ts` — cursors, catch-up, the held window | 05 §5. `head`, `cursor` and `shown` per conversation. The cursor moves only by a catch-up that paged to a short page. Catch-up is coalesced per conversation (2 s while open). The held window is 300 messages, evicting from the far end. Expiry is compared against the newest server instant seen and never against `Date.now()`. Cases: a burnt seq is crossed without a refetch; a message arriving in a wake with a lower seq than one already held is placed in order; the cursor never passes a seq it has not read. **The mutation fallback** (05 §5.2): the visible page is re-read on every sync trigger, with a case that an edit inside the page is shown after one trigger and a case that states, by asserting it, that one outside the page is not |
| [ ] | `chat/outbox.ts` — sends | 05 §6.1. The `cid` from `crypto.getRandomValues`, minted once. Serial per conversation, concurrent across them. `429` holds the conversation's queue for exactly `Retry-After`. A pending message is reconciled by its answer, a wake or a catch-up, whichever is first, matched by `cid`. A reply, edit or reaction to a pending message waits for the target's seq. Cases: a lost answer retried is one message with one seq; two sends pressed in order are stored in order under a slow first answer; a `404` makes the conversation past |
| [ ] | `chat/store.ts` — the chat list, the conversation, receipts, preferences | The list as a `Resource` paged by `{after_at, after_id}`, the unread count corrected on open by counting held messages from others (05 §5.4). Delivered posted within a second of holding; read from what the DOM reports as shown, only while visible and focused, coalesced to two seconds, flushed with `keepalive` on hide. Ticks from ONE `read_by` of the newest own message per open conversation per trigger, never one per message (05 §6.2). Mute computed from the newest server instant seen until the cross-repo duration lands, with the bound stated in the code. A past member's conversation is read-only |
| [ ] | `chat/media.ts` — upload, grants, forwarding | 05 §8. The `File` streamed through `wire/upload.ts` to the application's upload function, answered by a branded `UploadHandle`. A handle unspent within the hour is `upload-expired`. Grant addresses through the route builder; `<img>` with `referrerpolicy="no-referrer"`; a failed load re-reads the holding page, coalesced per page per minute, and re-points the element. The video metadata path is the one `createObjectURL` over a LOCAL file, revoked on `loadedmetadata`, with the `ban-exempt` reason on the line. **Waits on the cross-repo upload route** for its live case |
| [ ] | The staff view — a read-only conversation over the staff route | 05 §5.6. The same decoder, store and components as a member's view, with no composer, no receipts and no typing, and a case asserting that a staff read moves no member's watermark and adds nobody to `read_by`. The route is holder-scoped, so a case asserts its path is absent from a non-staff bundle. **Waits on the cross-repo row** "a staff read of a conversation, and reports" |
| [ ] | Invites | 05 §6.4. The token read from the fragment once, stripped with `history.replaceState`, posted in the body. A case asserts the address bar no longer holds it, and that every refusal is the one `404` |
| [ ] | The entry point — `hammer/chat` | `exports`, `tools/check-layering.sh` (`core wire state`, and no `document.` or `window.`), a gzip ceiling set from the first measured build against 05 §11's 12 KB target, and the public-surface and surface-coverage checks. `tests/testapp/` builds a chat over its generated routes and `kChatLimits`, so every seam is type-checked from outside |
| [ ] | Live suite — `tests/live/chat.test.ts` | Against `anvil_reference_server`'s seeded group and its two accounts, which is the whole login budget a run has (one server per run, two sign-ins). Send and retry are one message; history pages backwards and catches up forwards; an edit and a revoke are seen after a re-read of the page; a read receipt moves the sender's tick through `read_by`; a removed member's conversation goes read-only; a stranger's call is the stealth `404` byte for byte. **Records each answer's bytes** for `chat/answers.ts`'s unit suite |

**Phase 10 gate:** every row closed but the upload row's live case, and the reference consumer
showing a conversation polled from the reference server.

---

## Phase 11 — live

The socket, its fallback, typing and presence, as the client half of anvil §Phase 19. **A lost
wake costs latency, never a message**, because the cursor moves only by catch-up (05 §5.1).

| | Task | Notes |
|---|---|---|
| [~] | `chat/frames.ts` — the codec | anvil's `chat/frames.h` byte for byte (05 §7.2): canonical, big-endian, every refusal anvil names (`frame.size`, `frame.length`, `frame.version`, `frame.type`, `frame.direction`, `frame.inline`, `frame.nil`, `frame.value`, `frame.canonical`). A decoded wake COPIES its inline bytes. **Closes only against anvil's golden bytes as a printed fixture** (§Cross-repo). A fuzz-style case feeds every prefix and every single-byte mutation of each golden frame and asserts decode either refuses or re-encodes identically **Built, open on the vectors.** The client's half only: downstream decoded, upstream encoded, every accepted frame re-encoded and compared as anvil does. Anvil's golden bytes and refusal families ported, plus the `Mutation` frame anvil added for the edit catch-up (0x09, 26 bytes). Every one-byte change to every golden frame is refused by a NAMED rule, never by the canonical backstop. **One departure, asserted by its own case**: a counter past 2^53 is `frame.value` here, because a JavaScript number cannot hold it exactly and anvil would accept it |
| [ ] | `wire/socket.ts` — a binary socket the wire layer owns | `WebSocket` injected, as `fetch` is, so `tools/check-wire-discipline.sh` can keep the constructor inside `src/wire/` the way it keeps `fetch` there. The URL from the route builder with the scheme taken from the site origin. `binaryType = "arraybuffer"`. Reports close codes; decides nothing about reconnecting |
| [ ] | `chat/socket.ts` — the policy | 05 §7.1 and §7.3. Owned by the leader under `hammer.chat.socket`, fanned out over `BroadcastChannel`, followers through the same dedupe. No lock, no socket: `degraded`. The close-code table as code, one case per code: `4001` and `4006` never reconnect, `4005` refreshes through the leader first, `4004` falls back. Backoff from 1 s to 60 s with jitter from `crypto.getRandomValues`, consulting the circuit breaker. Every `Ping` answered; a `ClientPing` on `online` and on becoming visible, reconnecting after ten seconds without a `Pong`. The leader releases on `freeze` |
| [ ] | Sync on every moment wakes may have been lost | Socket open, `Sync`, `ChatSync`, visible after hidden, resumed from freeze or the back/forward cache: each catches every held conversation up, re-reads the visible page and, once Phase 14 lands, drains the device queue. One case per trigger |
| [ ] | The fallback — the stream, then polling | 05 §7.6. The application's stream-event decoder, answering a wake, a sync or nothing; no second stream, polling at 15 s while visible and never while hidden, the socket retried at the backoff ceiling and winning back when it connects |
| [ ] | Typing and presence | 05 §7.4–7.5. `ClientTyping` at most once per three seconds per conversation and never in a channel; a typing mark cleared six seconds after its last frame or by a message from that user, never announced. Presence asked only for the open direct conversation's peer, at the application's interval, never while hidden, `{online, lastSeen}` with no way to tell withheld from never seen |
| [ ] | Browser suite — two tabs, one socket | Two pages of one profile against the reference server: one socket between them (anvil would close a second with `4001`), the follower shows a wake the leader received, closing the leader hands the socket to the follower, and a frozen leader (`Page.setWebLifecycleState`) gives it up |
| [ ] | Live suite — wakes, typing, sync | A send by one account wakes the other's socket with the bytes history writes; an inline wake shows before any catch-up; a dropped socket recovers every message by catch-up; typing reaches the other member; with the socket refused, the same assertions pass on polling |

**Phase 11 gate:** the two-tab browser case and the live suite green, and a recorded number: wake
to screen, p50 and p99, in the browser suite, the way anvil recorded commit to wake.

---

## Phase 12 — the rendered surface (`hammer/chat-dom`)

Unstyled and wordless, by the component contract (`docs/01-seams.md` §19). The decisions are
05 §10.

| | Task | Notes |
|---|---|---|
| [ ] | `renderConversation` | `role="feed"` of `article`s, NOT a live log; a separate polite live region announcing arrivals only while at the newest message, through `Copy.arrivals(count)`, coalesced to two seconds. Windowed DOM, anchored on the message under the viewport. Bodies `dir="auto"`, names isolated, mentions from code-point spans. Pending, failed, past-member and every undecryptable class drawn from state the application words. Read reported through `IntersectionObserver` only while visible and focused. **No** `style`, no markup from a message, and links only through the application's matcher with `rel="noopener noreferrer"` |
| [ ] | `renderComposer` | The live validator on input; mentions inserted as spans and re-offset after NFC; reply and attachments; Enter and Shift+Enter as the application binds them, never hard-coded. Focus kept through arrivals; `Escape` clears a reply and nothing else |
| [ ] | `renderChatList` | Pinned, items and archived; unread as a number the application formats; typing markers; keyboard traversal as a listbox of links |
| [ ] | `renderMembers` | The member page, and the actions drawn from `mayI` for the viewer's role. A refusal from the server is shown, never pre-empted (00 §4.1) |
| [ ] | The entry point — `hammer/chat-dom` | `exports`, layering (`core state dom chat`, never `chat-e2ee`), a ceiling against the 14 KB target, and `tests/testapp/app/chat.ts` mounting every component with copy in both reference locales |
| [ ] | Browser suite — the surface | An arrival does not move the caret or the focus; a screen reader's view (the accessibility tree through CDP) carries one announcement for a burst of five; scrolling up stops the announcements; RTL names do not reorder the application's sentence; no element carries a `style` attribute after a session of sending, scrolling and typing |

**Phase 12 gate:** the browser suite green under the reference application's CSP, which has no
`unsafe-inline`.

---

## Phase 13 — devices and keys (`hammer/chat-e2ee`, part one)

The vault, the device lifecycle and verification: everything encryption needs before a message
is encrypted. **Blocked** for its first-device row on the cross-repo row "a fresh-authentication
refusal that is not `401`", which would otherwise sign a person out of every tab (05 §9.4.2).
Every other row can land first.

| | Task | Notes |
|---|---|---|
| [x] | `hammer/crypto` — incremental SHA-256 | The one hand-written primitive (05 §9.2), because WebCrypto's `digest` is one-shot and anvil compares a sealed upload's declared hash with the stream's. NIST CAVP short, long and Monte Carlo vectors. A hash: no secret passes through it **Closed**, against FIPS 180's four known answers (a million `a` among them, fed in uneven pieces) and against OpenSSL through Node's `createHash` at every length from 0 to 300 bytes and in eleven chunkings that cross a block boundary, rather than the CAVP files, which are not in this repository and would be a second download of the same oracle. The entry point's ceiling rises from 4 to 5 KB; the prehash bundles, which import only Argon2, did not change by a byte |
| [x] | `chat-e2ee/vault.ts` — the vault | 05 §9.3. One IndexedDB database per account; private keys as non-extractable `CryptoKey`s; everything else sealed under a non-extractable AES-256-GCM vault key with a fresh nonce per record. Every read-modify-write under `hammer.chat.vault.<account>`, in two transactions around the WebCrypto work, and a case that kills the step between them and finds the old state intact. Wiped on sign-out, identity change, a device the server no longer lists, and a session that is not the one the vault is bound to: the vault key first, then the database. No locks or no IndexedDB is `unsupported`, never degraded. `tools/check-source-bans.sh` learns to allow `indexedDB` in this file ONLY, in the same commit (`CLAUDE.md` §11.1, a rule and its script together) **The vault is built; the IndexedDB backing is not.** `Vault` over a narrow `VaultBacking`: open makes ONE key even when two tabs open a new vault at once; every sealed record is bound to its slot as associated data, so a record moved to another slot is `tampered`; an extractable key is refused at the write with nothing committed; a failed commit leaves the old state whole; twenty concurrent steps from two tabs on one counter end at twenty; the deadline bounds the wait and never a started step; a wipe forgets the key before destroying the store, goes ahead past a frozen holder, and no step ever recreates a forgotten key. `tools/check-source-bans.sh` now bans every `IDB*` type outside `src/chat-e2ee/idb.ts` as well as the global, because an injected factory would otherwise persist without spelling `indexedDB`. **Closed** with `idb.ts`: one database per account, every commit one readwrite transaction with `durability: "strict"` (a commit acknowledged before it is flushed is a chain index used twice after a crash), and every connection closing itself on `versionchange` so a wipe is never blocked and never reopens an empty database. Four cases in Chromium through the in-browser driver: a committed record read back over a fresh connection; an Ed25519 key that is still a usable, non-extractable key after a reload; a commit whose second put cannot be cloned lands nothing; a wipe deletes the database past another open connection, which then refuses. Detecting `unsupported` (no locks, no IndexedDB, no X25519) is the device row's |
| [x] | `docs/00-architecture.md` §7 | In the vault's commit, because §7 describes what the library does and not what it plans: "nothing is persisted" names the vault as its one exception, with the reason and the wipe. The "Offline and persistence" entry below already points here, and says the default is unchanged |
| [~] | `chat-e2ee/device.ts` — the lifecycle | The five states of 05 §9.4.1. Registration right after a sign-in, the refusal surfaced as re-authenticate. Confirming an ambiguous register or link by reading `my_devices` (05 §3.4). One-time prekeys replenished on `low`, in batches of 100, ids allocated in the vault before upload, a duplicate-id refusal of a retried batch read as success. Reset by unlinking every device. **Held to anvil's `testapp_emit_chat_vectors`**: every bundle encoding, both signed prekey messages, the 119-byte link message and its signature, and the five keys the server refuses, refused here first with the same field **The keys are built** (`chat-e2ee/device_keys.ts`): the three signed messages, signing, verification, and the bundle check in anvil's order with anvil's field names, against `testapp_emit_chat_vectors` committed as `tests/chat-e2ee/anvil_chat_vectors.json` — every derived public key, both prekey messages and signatures, the 119-byte link message and its signature, and all five refused keys refused here first. The client checks a peer's keys again because they reach it through the server. X25519 low order is asked of WebCrypto, whose spec requires `deriveBits` to throw on an all-zero secret; the five Ed25519 small-order y-coordinates are listed, and the suite recovers each point and multiplies it by eight in BigInt rather than trusting the list. Open: the states, registration, replenishment and reset, which wait on anvil's final device routes |
| [~] | `chat-e2ee/link.ts` — the ceremony | 05 §9.4.3. The link offer (the relay's token and a 128-bit digest of the request, 66 characters), and the approver's refusal of a relayed request whose digest is not the offer's, which is what stops a malicious server having the person's own phone sign its ghost device. **Not a six-digit check code**, which the design first had: 20 bits is ground in seconds by a server generating keypairs; the timestamp from the HTTP `Date` header of the approver's latest site-origin answer, NEVER `Date.now()`, with a case whose device clock is a day out **The offer is built**: encode, decode as untrusted input, and the approver's match, with a case where the server swaps one bit of the relayed signing key and the match refuses. Open: the four relay calls and the server-time timestamp, which wait on anvil's final routes |
| [ ] | `chat-e2ee/verify.ts` — chains, roots, the security code | 05 §9.5. A pinned root per peer account; a device trusted when its link chain reaches a trusted key, an unlinked approver's key included; `link: null` beside a pinned root is a reset. The security code as Signal's numeric fingerprint v2 over the root signing key, **held to the oracle's fingerprint vectors**. `trust` is required in `createE2ee`'s options and has no default, with a type-level case that omitting it fails to compile |
| [ ] | The review gate in the type | 05 §9.13. `unreviewedProtocol: "not for production"`, required and literal, with a type-level case. The row that removes it is the last row of Phase 15 |
| [ ] | Live suite — devices | A first device registered within five minutes of a sign-in, and refused after (once the cross-repo row lands, refused WITHOUT signing anyone out, which the case asserts across a second tab); a second browser context linked by a signature from the first; the device list carrying the chain; unlinking moves the conversation's `dsv`; signing out wipes the vault and the server lists the device no more |

**Phase 13 gate:** the vault's crash case and the device vectors green, and the first-device
live case green against an anvil that refuses stale authentication without a `401`.

---

## Phase 14 — the protocol (`hammer/chat-e2ee`, part two)

X3DH, the Double Ratchet, sender keys, the envelope, sending and receiving. **Written from Signal's
published specifications, with libsignal as the oracle and never as a source** (05 §9.14). The
oracle row is first, because a protocol written before its oracle exists is a protocol checked
against itself.

| | Task | Notes |
|---|---|---|
| [ ] | The oracle — a libsignal harness, outside this repository | 05 §9.14. A separate repository hammer never depends on links libsignal with a seeded random source and prints vectors: X3DH from fixed keys, a ratchet across skipped and out-of-order messages, message keys and their CBC ciphertexts, sender-key chains, and the numeric fingerprint. hammer commits the printed JSON as `tests/chat/oracle/`, and nothing else from it. **Risk recorded here**: current libsignal speaks PQXDH, and the harness needs a version or an internal entry point that still runs classic X3DH. The row closes when the vectors exist, however the harness got there, with the libsignal version named in the fixture |
| [ ] | The framing vectors, from a third implementation | 05 §9.15's four layouts and the MAC input, produced by a throwaway script written from that table alone, without reading hammer's code, as anvil's chat vectors were. Committed as a fixture beside the oracle's |
| [ ] | `chat-e2ee/x3dh.ts` | Bundle signatures verified before use; DH1–DH4 and Signal's KDF, constant for constant; the prekey message as 05 §9.15 lays it out; the one-time key's private half deleted in the same vault transaction that stores the new session. Held to the oracle's X3DH vectors and the framing vectors |
| [ ] | `chat-e2ee/ratchet.ts` | The Double Ratchet per the specification: root and chain KDFs, message keys, CBC with the 8-byte MAC over both devices' two keys. At most 2 000 skipped keys per session and 25 000 ahead, both refused past the bound. Held to the oracle's ratchet vectors, including the out-of-order and skipped cases. A message that fails is recorded, acknowledged and repaired (05 §9.6), with a case where one side's vault is restored from an older copy |
| [ ] | `chat-e2ee/sender_keys.ts` | One per (conversation, sending device), each with its own Ed25519 key, every message signed. Rotation when the device set shrinks: a member left or was removed, or a device was unlinked. A joining device gets the current iteration and nothing earlier. Held to the oracle's sender-key vectors |
| [ ] | `chat-e2ee/envelope.ts` — the content format | 05 §9.10. UTF-8 JSON, version 1, narrowed once; padded to 160 bytes after `0x80`; references as `(sender, cid)`; every field validated by `chat/text.ts` and the kind's bounds; cards by the application's validators; an unknown type is `invalid`. Revoke is an envelope AND the route |
| [ ] | `chat-e2ee/seal.ts` — the `ConversationSealer` | 05 §9.8. A ciphertext made once and persisted with the ratchet step in one vault transaction; retries send the same bytes. The fence's `409` handled by the six steps, with the same `cid`, at most three rounds, then `devices-unsettled`. Distributions in the same send when they fit and paged with `page: true` when they do not. Sessions for a group established in the background from opening, in pages, honouring every `Retry-After` |
| [ ] | `chat-e2ee/receive.ts` | 05 §9.9. The queue drained first, under the vault lock, each row decrypted and archived with its session state in one transaction, acknowledged `through` the last processed. Idempotent by `(c, seq)` before the ratchet is touched. The four classes: `before-device` by comparing two server instants, `awaiting-key` retried after each drain, `undecryptable`, `invalid` |
| [ ] | The entry point — `hammer/chat-e2ee` | `exports`, layering (`core crypto wire state chat`, no `document.`/`window.`), a ceiling against the 16 KB target, `tests/testapp/` turning encryption on with its words typed out |
| [ ] | Browser suite — two devices, two accounts | Three browser contexts (two devices of one account, one of another) against the reference server's encrypted kinds: a direct message read on both of the recipient's devices and on the sender's other device; a group message under a sender key; a member removed, the key rotated, and the removed device unable to read the next message; a device linked mid-conversation reading what follows and classing what precedes as `before-device`; a reload mid-send resuming with the same bytes; two tabs of one device never stepping one ratchet twice (a case with both tabs sending at once, asserting every message decrypts once on the peer) |

**Phase 14 gate:** every oracle and framing vector green, the browser suite green, and a recorded
number: encrypt and decrypt per message on a throttled CPU profile (CDP, 4× slowdown), since the
p95 device is the one that pays for WebCrypto.

---

## Phase 15 — sealed media, encrypted push, and the review

| | Task | Notes |
|---|---|---|
| [ ] | `hammer/chat-seal-worker` — `serveSealPool` | 05 §9.11. Chunked AES-256-GCM over 64 KiB chunks with the index and final flag in the nonce; the incremental SHA-256 of the ciphertext; a Blob of Blobs, a megabyte per part. One worker. Cases: a reordered, dropped, truncated or extended chunk refuses; peak heap stays under two megabytes for a 25 MB file (measured in the browser suite); the worker catches in every task body |
| [ ] | `chat-e2ee/sealed.ts` — the page half | Upload into the kind's sealed namespace with the declared hash; the descriptors inside the envelope; fetched with `credentials: "omit"`; nothing handed over unless the hash and every chunk check. One object URL per attachment on screen, revoked when it leaves the held window, the only module exempt from the `createObjectURL` ban, with the check taught so in the same commit. `03-deployment.md` gains `blob:` in `img-src` and `media-src`. **Waits on the cross-repo sealed upload route** |
| [ ] | `chat-e2ee/push.ts` — `chatPushNotification` | 05 §9.12. For the application's service worker: exactly `{c, seq}` or refused; the queue drained and the conversation caught up under the vault lock with a deadline; a refresh under `hammer.refresh`; the application's `wording`, or its generic wording on any failure and never nothing; the conversation as the tag. The "A service worker" entry below already says why hammer ships a function for one and still ships no worker |
| [ ] | Browser suite — the push path | A service worker registered by the reference application receives a real `{c, seq}` push and shows text decrypted from the archive; a lock held by another context past the deadline yields the generic wording; a frozen leader does not stop it |
| [ ] | **External review of both halves** | The one row no suite closes (anvil §Phase 20, its last row). Scope: 05 §9 and anvil 22 §7, the code of both halves, and the oracle and framing vectors. Findings get rows of their own here. When it is checked, `unreviewedProtocol` is removed in a major release, with the review's summary in the release notes |

**Phase 15 gate:** every row closed, the review included. Until then encrypted conversations are
built and tested, and the type says they are not for production.

---

## The first consumer — what an application found that the suite could not

**Not a phase, and it did not wait for one.** The phase gate above governs planned work; this
was a defect report from outside the repository, opened by the first application to build on
hammer. It reached for [`docs/02-getting-started.md`](02-getting-started.md), could not use it,
and reported four examples describing an API this library has never had: a `createClient` taking
a route table and three origins it has no members for, a `call` taking a route id rather than the
route value, a `useResource` taking a route and a query, a `useSession` taking nothing.

Every one of those was true of the document for its whole life, through seven phases, 1067
passing tests and nine green scripts. That is the thing worth recording: **a suite proves the
library works and proves nothing about what anyone is told it is.** The reference consumer is
compiled, so every seam is satisfiable — and the paragraphs beside it were satisfiable by
nothing.

### What was built

| | Task | Done when |
|---|---|---|
| [x] | **`tools/check-docs.sh`** | Every `ts`/`tsx` block in `docs/` and `README.md` names a region of a file the type-checker reads, and is diffed against it on every `npm run lint`. `--write` rewrites the blocks from source. A block naming no source fails the build; a signature sketch is spelled `ts sketch: <why>` and carries its reason on the fence |
| [x] | **`tests/testapp/app/minimal.ts`, `minimal_react.tsx`** | The compiled source those blocks are lifted out of: the narrow path a reader follows, beside the exhaustive seam proof that was already there. Type-checked by `npm run typecheck` like everything else under `tests/` |
| [x] | The guide and the README rewritten | Every TypeScript block in `02-getting-started.md` transcluded; the "cannot rot" claim in both made TRUE rather than removed |
| [x] | **`ClientConfig.origin` checked where the document said it was** | `createClient` takes the unvalidated pair and throws on one it cannot drive. `defineApiOrigin` still returns a `Result`, so the decision stays testable without constructing a client; `client.origin` exposes the validated value, so the `crossOrigin` preflight cost stays observable |
| [x] | **`Api` and `kApiTables` emitted** | The generator emits the type parameter and the four decode tables as one value, so `ClientConfig` has five required members rather than eight. The generated module still imports nothing and runs nothing, which `tests/codegen/emit.test.ts` asserts line by line |
| [x] | **An end-to-end run of `Client.upload`** | Done: `tests/wire/upload_client.test.ts`, nineteen cases over the whole pipeline — the body that reaches the wire, the refusals before the first byte, the budget, the dedupe, the queue, the key, the retry decision and the replay after a refresh. **It found a defect on its first run**, which is the answer to whether the row was worth opening: a progress-reporting upload sends a `ReadableStream`, a stream is spent by the attempt that sends it, and the body was built once before the pipeline ran — so every attempt after the first re-sent nothing, `fetch` rejected, and the call burned its whole attempt budget reporting a network failure for a network that was working. The body is now built per attempt and inside the one function that sends a request, which closed a second hole nobody had named: a shed, breaker-refused or already-aborted upload used to open a stream over the file for a request that was never made |
| [x] | **A pairing check between `Api` and `kApiTables`** | Done, and not with the phantom. `kApiTables` carries a sixth member nothing decodes with — `hash`, the descriptor's own — and `Api` declares `readonly hash: typeof kTablesHash`, so a tables object from another descriptor does not satisfy `ApiTables<A>` and `createClient` does not compile. The phantom `unique symbol` came OUT with it: it had to be optional, because the generated module imports nothing and cannot name a symbol hammer declares, and an optional phantom constrains no object literal at all — it was buying nothing and reading as though it bought something. What closes this had to be a value the generator can write down, and the descriptor hash already was one. Two generated modules from the same descriptor still pair, which is correct. `SessionStoreConfig.clientHash` stays a plain `string` deliberately: it is compared against the hash the SERVER sends, so a wrong one reports a stale client loudly on the first session read, where a crossed decode table is silent |

**State at close:** 1075 tests in 74 files, `tsc --noEmit` clean under every strict flag, and ten
scripts clean — `check-docs.sh` lifting ten blocks out of three files and diffing them to
nothing, with two signature sketches carrying their reasons. The two open rows above are both
things this work found rather than things it left undone, and neither blocks a consumer.

**Since:** both rows are closed. The upload row cost one fix rather than one test. The
suite was written against what the pipeline was supposed to do with a `File` body, ran, and
disagreed in three places at once — a retry, a backoff and a replay were all re-sending a
stream that the first attempt had drunk. That is the argument for the row in the shape it was
written: `uploadBody`, `uploadContentType` and `checkUpload` each had their own tests and each
of them passed, because the defect was not in any of the three. It was in the sentence nobody
had written down — *a body is built once per call* — and a unit test cannot disagree with a
sentence that is not in the code.

The pairing row closed by deleting the mechanism it was written about. The phantom `unique
symbol` on `ApiTables` was the shape everybody reaches for, and it could not work here for a
reason that is worth keeping: a brand has to be NAMED to be carried, naming it is an import, and
the generated module's two standing properties are that it imports nothing and runs nothing. An
optional phantom is the compromise that follows, and an optional phantom is not a constraint —
an object literal without the symbol satisfies it either way. What the check had to be was a
value the generator can write, and the descriptor hash already was one, published for a
different reason and carrying exactly the identity that was wanted.

### The design calls, and what each was weighed against

**The docs check transcludes rather than compiles.** Extracting fenced blocks into a scratch
file and running `tsc` over them was the obvious design and is worse where it matters: a snippet
in a guide is a fragment leaning on a `signal`, a `locale` and a `show()` that the surrounding
paragraph established. Making each one self-sufficient means either a per-block preamble that is
source nobody reads and nothing else checks, or examples contorted for a build step rather than
for a reader. A region is the other way round — ordinary source in the reference consumer, with
its imports and its neighbours, compiled for the same reason everything else there is, and the
document holds a copy the build refuses to let drift. It is the same bargain
`check-descriptor.sh` makes: regenerate, diff, print the command that fixes it.

**The scan stops at `docs/` and `README.md`, deliberately.** `CLAUDE.md` is not scanned, and the
boundary is what a consumer copies from. Scanning the rules document would force a `sketch:`
annotation onto every illustration of a rule, which is friction bought for a file no application
reads.

**The four tables became one member rather than staying four.** They describe one descriptor and
nothing required them to come from one: a rate-limit table from last month's regeneration, a body
cap typed in by hand, an error vocabulary narrowed to another application's unions — every one of
those type-checks, and the client decodes with it. Bundling them costs a breaking change to
`ClientConfig`, which is cheap at 0.1.0 with one consumer and expensive at every later moment.
`refreshRoute` could not join them and stayed a member of its own: the descriptor carries
`auth.refresh` and nothing that marks it as the refresh, so a generated table naming one would be
the generator guessing at an id — wrong silently, in the one call that decides whether a session
survives. It is a §Cross-repo row instead.

**`kApiTables` is an object literal and not a function call.** `errorVocabulary(...)` at module
scope in the generated file would have been the obvious spelling and would have made that module
import `hammer/wire` and run something on import. A bundler cannot drop a call expression it was
not told is pure, so every bundle touching any generated name would have carried the whole set.
The literal keeps the module's two standing properties — imports nothing, runs nothing — and the
vocabulary is built inside the client instead.

### Five more wrong examples, found by writing the source

The first consumer reported four. Transcribing the rest of the guide into compiled source found
five it had not reached, all the same class and none of them caught by anything:

- `mint` and `upload` each take **three** arguments, not two.
- `call` takes `params`, not `path`.
- `definitionsFrom` takes a control-to-field-type map, not the field-type table — the two are
  separate closed sets and the bridge between them is a product decision (§7 of
  [`01-seams.md`](01-seams.md)).
- `affordsRoute` and `holdsAll` take the session's `view`, not the session state.
- An error `code` exists only where `kind === "server"`. A transport failure and a locally shed
  request are the other two shapes, and neither has one because no server answered.

**Two documents were describing behaviour the library did not have, not one.**
[`01-seams.md`](01-seams.md) §16 and `CHANGELOG.md` had both said since 0.1.0 that a
misconfigured origin is refused at construction with a throw. It was not: `createClient` took an
already-validated origin and the unwrap-and-throw sat in the application, written identically in
each one. That is the same defect as the guide's, in a document nobody had reason to doubt — and
the reason it is repaired by moving the code rather than the sentence is that the sentence was
right about what the design wanted.

---

## The published surface — twenty-two names with no caller

**Not a phase, and not a row anyone had written down.** It came out of asking why the upload
row above existed at all. That row was opened by a person noticing that `client.upload` had no
caller; nothing in this repository could have told them, and nothing would have told anyone
about the next one. So the noticing became a script.

[`tools/check-surface-coverage.sh`](../tools/check-surface-coverage.sh) reads the committed
surface snapshot and fails the build on a published **value** that nothing under `tests/` so
much as names. It found twenty-two on its first run.

| | Task | Done when |
|---|---|---|
| [x] | **The gate** | `tools/check-surface-coverage.sh`, in `npm run lint` after the snapshot it reads. Values only, and no allow-list — the reason for an exemption would always be "this one is hard to test", which is the property that makes it worth testing. Its own weakness is stated in its header: it is a grep, so a name in a comment satisfies it |
| [x] | **The image pool's worker body** | `tests/state/image_worker.test.ts`. `serveImagePool` was exported and documented as a two-line application entry point and had NEVER RUN: every other image test drives the pool from the main thread against a stand-in that answers for the worker. So the scale arithmetic that decides what a person actually uploads, and the `close()` that decides whether the tab survives doing it, were both unexecuted. Thirteen cases now cover the five shapes (landscape, portrait, square, smaller-than-the-box, a panorama whose short edge rounds to nothing) and the `finally` on all three failure paths |
| [x] | **The bidi controls** | `tests/core/bidi.test.ts`. Five of the seven published control characters are used NOWHERE in the library — they exist for the positions an `isolate()` cannot reach — so a wrong code point would have been found by an Arabic name reordering a sentence in production. They are now asserted by code point, and, more usefully, tied to the predicates that police them: the constants are escapes and `isMark`/`isIsolate` are numeric ranges, which is two copies of one table with nothing holding them together |
| [x] | **The rest** | `isServerError`, `isRoutePath`, `uniqueId`, `supportsRequestStreams`, and the eleven published constants. Each is asserted against the behaviour it produces rather than against its own literal twice: `kDefaultBreaker` against the attempt the circuit opens on, `kRefreshLock` by naming it in the takeover case that used to hard-code the string, `kInboxHeld` by overflowing the list |

**What it did not find.** No defect. Every assertion written here passed the first time it ran,
which is worth saying plainly: the value of this work is not a bug it caught, it is that
twenty-two names can no longer stop being true without something going red. The one place that
came close is the bidi table, where the two copies were in agreement and nothing was holding
them there.

**What it cost.** 1144 tests in 76 files, up from 1098 in 75. No source change and no API
change — the gate is a test-side rule, and the only non-test edits are the dimensions added to
the bitmap stand-in and one existing case in `credentials.test.ts` that now names the constant
it had spelled by hand.

---

## Cross-repo — what anvil has to ship

Each of these was listed here rather than worked around, and each had a hammer-side fallback
that was deliberately worse, so the pressure stayed where the fix belonged. Nine have landed,
and five of those landed after being listed here — which is the only evidence that writing them
down rather than working around them was the right call.

The three rows phase 6 added are the first ones that are not about a table. Two are about the
bytes of a failure — a shape anvil documents and writes nowhere, and a recorder for the bytes
it does write — and the third is that there is no anvil to point a client at: the reference
application is a set of test executables, so the runs that only end-to-end can make have
nowhere to run.

The two rows phase 4 added are the same shape as each other: an address an application ends up
writing by hand because the descriptor withholds it or never carried it. Both fallbacks are
left uncomfortable on purpose (`docs/01-seams.md` §18), because a workaround in a library is a
workaround in every application built on it.

| | What anvil needs | Status |
|---|---|---|
| [x] | **A descriptor emitter** | Landed: `anvil/descriptor/`, with the reference application's thirty-line emitter built by anvil's suite. The route table is two tables there — `RoutePolicy` is scanned per request, so the build-time fields live beside it rather than in it |
| [x] | **A holder-scoped route table** | Landed: `accesscontrol/route_projection.h` projects the routes a holder reaches, filtered by `satisfies()` — **the same function the filter calls**, so the table cannot drift from the decision. anvil's suite asserts the two agree over every route and a range of holders |
| [x] | **The descriptor hash** | Landed: `hash` is SHA-256 over the `tables` object, so an application version bump does not invalidate every client. Putting it on the session response is the application's controller, using the emitted constant |
| [x] | **A name for every validation reason** | Landed: `wire_name(input::Reason)`. It had never existed — anvil documented the `fields` map from phase 0 and shipped nothing that produced its values |
| [x] | **`Retry-After` on 429 and 503** | Landed: `http/retry_after.h`, "in the one place it is spelled". The blocker recorded here was real and was solved rather than worked around — the window script returns what is left of the window alongside the count, so an honest value is still ONE Redis round trip. It is never zero, because `Retry-After: 0` means retry immediately, which is the stampede the header exists to prevent. `wire/retry.ts` honours it exactly and invents nothing |
| [x] | **An idempotency key honoured on non-idempotent routes** | Landed: `http/idempotency.h`. The client mints a key, the server records the response against it, and a repeat is answered from the record rather than performed again. It is safe WITHIN the retention window and not forever, which is the right trade: a store that never forgot would grow without bound. Two things hammer has to honour — a key is at most 255 bytes, and a route protected this way must also have a rate-limit rule, because the records one caller can create are bounded by the limiter and by nothing else |
| [x] | **The content tables** — field types, sections, topics, events, media roles | Landed, and they landed because the reader did: the generator is what they were held back for. Each is emitted in the vocabulary a client speaks rather than the one the server stores — named boolean flags instead of a byte, a `null` answer shape for a PII type rather than `"text"`. See §§7–10 and §12 of `docs/01-seams.md` for what each carries. hammer does not read them yet |
| [x] | **Response schemas from a response binder** | Landed: `http/response_writer.h`, and the descriptor carries a `response` per route. It is worth having only because of what it is — writing a key that is not next, one of the wrong type, or stopping early are `static_assert`s on the SERVER, so the schema describes the bytes rather than claiming something about them. hammer emits `RouteResponses` from it (`docs/01-seams.md` §4.3); module augmentation stays for the routes the binder does not cover |
| [x] | **The media types an upload route accepts** | Landed as `accepts` beside each namespace's roles, which is exactly what this row asked for. hammer emits `kMediaAccepts`, and the generator refuses a value that is not lowercase `type/subtype` with no parameters — the essence string a browser puts in `File.type`, so the comparison is `===` rather than a parse |
| [x] | **A route for the refresh itself** | Landed in anvil's reference tables, Public and POST, as `route_registry.h` said it had to be: its credential is the refresh cookie and the access token is expired at exactly the moment it is called, so gating it on a valid one would make refresh work only while it was unnecessary. It is described as NOT idempotent, and the reason is the one worth carrying over — repeating a refresh is safe only inside anvil's sixty-second `rotation_grace`, which exists so two tabs racing both succeed. Past it the same token is a REPLAY: the session is revoked and the epoch bumped. A client told `true` here would sign its user out by retrying after a backoff |
| [ ] | **A mark for the refresh route** | Open, and found by emitting `kApiTables`. Every other table `createClient` takes is now one generated value; `refreshRoute` cannot join it, because the descriptor carries `auth.refresh` as an ordinary public route and nothing that says it is the one a credential refresh goes to. A generator that guessed at the id would guess wrong silently, in the one call that decides whether a session survives, so the application names the `const` instead — one line, and a line whose absence is a compile error rather than a 401. What would close it is a flag on the route description, the way `idempotent` already is |
| [x] | **A method on every route description** | Landed: `auth.logout` describes POST. The policy may still be `Any`; what a client needed was the description naming one, and the live suite now signs out through it |
| [x] | **A session route reachable without a session** | Landed: `session.current` is described public, for the reason `auth.refresh` already was. The reference consumer's hand-written `/session` and its `visibility` override are gone |
| [x] | **A route for the media grammar** | Landed as `media.object`, `GET /media/{ns}/{id}/{role}`, public. `MediaConfig` takes the generated route and the application's copy of the pattern is gone |
| [x] | **The superadmin flag on the session response** | Landed as `append_holder_authority`, which writes `{"superadmin":…,"perms":[…]}` beside the route table. The live suite asserts the defect it closes from the side that was suffering it: the superadmin account holds ZERO bits, reaches every route, and `identity.me` answers `permissions: null` rather than an empty list. It also carries `perms` as NAMES rather than as a mask, which is what hammer's decode had to be corrected to read |
| [ ] | **A descriptor hammer's fixture can be refreshed from** | Half closed. The fixture is current — anvil's `testapp_emit_descriptor`, thirteen tables, format 2 — and refreshing it is still a person remembering to run that binary. Nothing catches the next drift: `check-descriptor.sh` regenerates the CLIENT from the committed DESCRIPTOR, so it is green by construction whatever release the descriptor is from. The honest fix is a step that runs the emitter rather than a note that says to, and it is not hammer's to write alone: hammer cannot make a sibling checkout of anvil a build dependency, so what would close this is anvil publishing the descriptor as a release artefact a fetch step can name |
| [~] | **A writer for the envelope anvil documents** | Half landed. `append_error_body` writes `request_id` now, and the live suite reads one off a real 401. It is NULL on a 404 and correctly so — anvil's 404 body is one constexpr string shared by a stealth drop, an unmatched route and a genuinely missing object, and a shared constant cannot carry a per-request id. What is still missing is `fields`, and that one is sharper: a form that cannot place a server reason on a field is a form that refuses to submit with nothing marked on it |
| [ ] | **A recorder for those bodies, owned by anvil** | Open, and the same shape as the descriptor row below. `tools/record-envelopes.cc` compiles against anvil's headers from a sibling checkout and reads `wire_name`, `http_status`, `is_stealth_hidden` and `kNotFoundBody` — which is real provenance for everything but the two lines of body ASSEMBLY, which it copies from `access_filter.cc` and `stealth.cc` because anvil has no function that returns one. A copy in a client is a copy that goes stale silently. What would close it is anvil emitting these the way it emits the descriptor, from the one place that builds them |
| [x] | **A reference application a client can be pointed at** | Landed: `anvil_reference_server`, loopback, ephemeral port, credentials drawn at boot and printed once, refusing to start against a database it did not create. It unblocked four written suites at once, and the first execution of one of them found that `decodeSessionView` was written against a payload anvil does not send — past 1,145 unit tests, all of which built their payload from the fixture the decode was written against. `tools/run-live.sh` starts it, reads its URL and its passwords off its own stdout, and runs both suites |
| [ ] | **An identity on the session response** | Open, and found by driving the two-tab browser run. `append_reachable_routes` writes the table and `append_holder_authority` writes the authority, and neither says WHO this is — so a client has nothing to key its caches by, and every cache must be keyed by the identity allowed to read it (`CLAUDE.md` §2.3) or it renders one user's documents to the next person on a shared device. The reference consumer works around it with a second call to `identity.me` on every session read, which is an N+1 on the bootstrap path and a workaround an application would copy. `SessionStore.identityOf` stays either way: it is the seam for an application whose session body does carry one |
| [ ] | **A rate limiter in the reference application** | Open, and found by running the slow-network browser row. The descriptor declares the buckets — `login`, `refresh`, `media` — and `anvil_reference_server` enforces none of them, so no route's limit can be reached deliberately and there is no way to observe a real `Retry-After` on a real connection. `wire/retry.ts` honours the header exactly and the unit suite covers it; the browser case is skipped rather than failed and says so, because the server being present and unable to answer is a different thing from the server being absent |
| [ ] | **A write route in the reference application** | Open, and it is what three rows have now been deferred around. There is no route that writes anything past the credential ones, so the versioned-write conflict, the idempotency key on a retried write and the freeze/discard row's duplicate-write claim have nothing to be observed against. Each is asserted in the half that IS reachable and named for that half rather than for the row — a write with no version read is refused rather than sent, and one call across a freeze records one outcome |
| [x] | **A session response that serves the holder-scoped table** | Landed: the reference application serves `GET /session` over a real listener, through the real access filter, and the recorded body is one envelope key over an object of route id to `"<METHOD> <path>"`. That is the shape `wire/session_view.ts` was written against, `ANY` included — anvil records `auth.logout` as `"ANY /session/logout"`, which is the entry hammer sorts into `unusable` rather than into `routes`. The route is in the descriptor as `session.current`, which is how this arrived here at all |
| [ ] | **One dependency policy for the SDK** | Open, and not a blocker for hammer. hammer and anvil ship as one SDK, and the SDK's release is as clean as the dirtier half. hammer's rule is `CLAUDE.md` §12 and its gate is `npm run check:production`. anvil's dependencies are vcpkg ports with a pinned `builtin-baseline`, which is the same bargain in another package manager. What would close this is anvil stating its allowed set and a release gate of its own, so the SDK's release runs both. hammer does not edit anvil to get there |
| [x] | **An edit route, the recipe codec and `limits.edit`** | Landed with its design rather than found by a consumer: anvil §Phase 17 ships `install_media_edit_routes`, `limits.edit`, the two response shapes and the golden vectors, and the reference server serves the routes over a seeded picture. Filed with no fallback, and none was needed |
| [ ] | **A fresh-authentication refusal that is not `401`** | Open, filed with the chat design, and **blocking** Phase 13's first-device row. anvil answers a first device whose sign-in is older than five minutes with `401 UNAUTHENTICATED` and no field (`devices.cc`, `service.cc`). To hammer that is an expired access token: the leader refreshes, the replay is refused with `401` again, and a second `401` after a successful refresh is a rejection, so **registering a device with a stale sign-in signs the person out of every tab**. What would close it is a code that means "prove yourself again": `428 CAPABILITY_REQUIRED` already means a second, deliberate act in hammer's error table, or a `403` with a `reason`. No fallback: special-casing a `401` on one route would also swallow that route's genuine expiries (05 §9.4.2) |
| [ ] | **A catch-up for edits, revokes and reactions** | Open, filed with the chat design. A plaintext edit, revoke or reaction changes a row without allocating a seq and publishes no wake, so a device holding the message never learns of it from `after=cursor`. anvil 22 §4.5 says a revoked row keeps its seq so "the next sync" can tell; a cursor sync cannot. What would close it is a per-conversation mutation counter stamped on the changed row, a catch-up by that counter, and a wake when it moves. The fallback is bounded on purpose: the visible page is re-read on every sync trigger, so a change on screen is at most one trigger stale and one off screen is corrected when scrolled to (05 §5.2). The encrypted half does not have the gap |
| [ ] | **Another member's delivered watermark** | Open, filed with the chat design. The `Receipt` frame is in the grammar and never sent, and no route reads a member's `dlv`, so a sender can be shown sent and read (one `read_by` of their newest message answers every tick, because `rd` is a watermark) and never delivered. Either the frame or a field beside `read_by` would close it. The store exposes no delivered state until then rather than inventing one (05 §6.2) |
| [ ] | **A chat upload route, plaintext and sealed** | Open, filed with the chat design. anvil ships no upload handler for chat and the reference application has none, so no attachment can be sent end to end. Plaintext: an upload into the kind's `media_ns` answering `mint_upload_handle` and nothing else. Sealed: the same into `sealed_ns`, taking the client's declared SHA-256 for `finish_sealed` and enforcing the per-account byte budget before the sink opens. Phase 10's media row and Phase 15's sealed row wait on it for their live cases; nothing else does |
| [ ] | **A batch prekey claim** | Open, filed with the chat design. A claim names one account and answers its devices' bundles, so a group's first encrypted send costs one request per member, and at the reference budget of 120 a minute a group of a thousand takes over eight minutes of sequential requests — an N+1 over the network (`CLAUDE.md` §7). A claim taking a bounded list of accounts, spending the per-target budget for each, would close it. The fallback establishes sessions in the background from the moment a conversation is opened and shows the send as pending until its distribution is complete (05 §9.6) |
| [ ] | **A batch presence read** | Open, filed with the chat design. `GET /chat/presence/{user}` is one account per call, so presence on a list of conversations is one request per row. The fallback asks only for the open direct conversation's peer (05 §7.5) |
| [ ] | **A mute duration** | Open, filed with the chat design. `muted_until` is an absolute instant, and the client's only "now" is the device clock (`CLAUDE.md` §6). A `mute_for_s` the server adds to its own clock would close it. The fallback computes the instant from the newest server instant the tab has seen, and the code states the bound: the mute ends late by however old that instant is (05 §6.3) |
| [ ] | **A client id on `chat.create`** | Open, filed with the chat design. `create` is described as not idempotent and takes no key, so a lost answer retried would be a second group, and hammer does not retry it. A `cid` with a unique index, as `send` has, would make it retryable. Until then a lost answer is surfaced as an unknown outcome and the next list read settles it |
| [ ] | **Which device is mine** | Open, filed with the chat design. A browser may evict a vault, and the device it held stays current on the server until the idle sweeper takes it thirty days later, with every sender encrypting to it meanwhile. The client cannot find it to unlink it, because `my_devices` deliberately does not expose the registering session. A boolean on the device the CALLING session registered would close it without exposing any session (05 §9.3) |
| [ ] | **A relay for the link approval** | Open, filed with the chat design. The new device must post its own link, since anvil binds the device to the posting session, so the approver's signature has to travel back to it, and a desktop browser usually cannot scan a code. A short-lived mailbox keyed by the request, written by the approver and read once by the requester, would close it. Until then the bytes travel however the application can carry them (05 §9.4.3) |
| [ ] | **Signed-prekey and last-resort rotation** | Open, filed with the chat design. anvil cannot rotate either yet ("nothing has asked for it"); hammer asks, because a signed prekey that never rotates weakens the forward secrecy of every session's first message. A route that verifies the new key against the stored signing key would close it, and the client rotates on a thirty-day period once it exists (05 §9.4.4). Wanted before the external review |
| [ ] | **Chat golden vectors as printed fixtures** | Open, filed with the chat design. `chat_frames_test.cc` says its golden bytes are the contract a client's decoder is written against, and `chat_text_test.cc` holds the validator's cases, but neither is printed the way `testapp_emit_chat_vectors` and the edit vectors are. An emitter for each would let `chat/frames.ts` and `chat/text.ts` close against the same file anvil's codec and validator are held to. `testapp_emit_chat_vectors` itself is already printed and is Phase 13's fixture |
| [ ] | **A staff read of a conversation, and reports** | Open, filed with the chat design after an application named the need: staff reviewing a reported marketplace dispute. Storage is not the gap, since anvil keeps plaintext for the kind's `retention_days`. The gap is the way in: a route reading a conversation the reader is not a member of, behind a permission the application names, audited on every read, holder-scoped; and a report a member files against a message, recording the reported range. Not for encrypted conversations, where the server holds nothing to show (05 §5.6). No fallback: a client-side copy cannot be read by a reviewer on another device |

### What the cross-repo work found

**The media role grammar contradicted a `srcset`, and the media table settled it.** anvil's
public grammar is `GET /media/{ns}/{id}/{role}` and the width ladder stayed server-side on the
stated grounds that a client which knows it will start building paths from it again; a
responsive `srcset` cannot exist without width descriptors, because without them the browser
has no basis on which to choose between the sources it is handed. It resolved the way the
objection was actually shaped rather than by either side giving way: the request is still by
role, the client still constructs no path, and the width is published beside the role so the
`srcset` can name it. Knowing a width was never the hazard — building a URL out of one was, and
the route builder is what makes that unspellable (`docs/01-seams.md` §12).

**A generated client cannot be told a route is safe to repeat when it is safe only for sixty
seconds.** `auth.refresh` is described as not idempotent, and the reason is the sharpest case
for the flag being a property of the route rather than of the verb: repeating a refresh
succeeds inside anvil's `rotation_grace`, which exists so two tabs racing both survive, and past
it the same token is a replay that revokes the session and bumps the epoch. That is the correct
answer to a leaked credential and a catastrophic one to a lost response. A client told `true`
would sign its user out by retrying after a backoff.

**`LocaleSpec` carries no digit system**, and should not: the client derives digit shaping from
the tag through `Intl`, and a second table would be a second thing to disagree with it.

## Deferred, deliberately

**Offline and persistence.** A persisted API response outlives the cookie that authorised it,
which is private data readable after the session ended, on a shared device. It returns as a
per-resource opt-in that names its own eviction and its own threat model — not as a default,
and not as a service worker that caches everything because it is easy.

Reopened once, for encrypted chat, and only there ([`05-chat.md`](05-chat.md) §9.3). A device's
keys, its ratchet state and what it decrypted cannot be fetched again, so the vault persists them,
names its threat model, and is destroyed with the session. It persists no API response, and
plaintext chat persists nothing. The default stands.

**A service worker.** It is a second, longer-lived origin-scoped program with its own update
lifecycle, and it makes the stale-bundle problem in `docs/00-architecture.md` §7.1 strictly
worse before it makes anything better.

Encrypted chat needs one, because only the device can word a push about a message the server
cannot read ([`05-chat.md`](05-chat.md) §9.12). So hammer ships a function the application's own
service worker calls, as it ships `serveImagePool` for the application's own worker, and still
ships no worker. The reason above is why it stays that way.

**A framework adapter beyond React.** The core is framework-agnostic and the adapter is thin
on purpose; a second one is written when a consumer needs it, against the same store surface,
and its existence is the proof the boundary held.

**A template or view layer.** anvil retired its own template engine for reasons that apply
here twice over (anvil `docs/19` §1). Components compose; a grammar is a policy.

**Request coalescing across routes and a normalised entity cache.** Both are real wins and
both are large; neither is worth designing before there is a consumer whose screens show what
gets coalesced.

**Client-side sampling of analytics.** The server samples whole sessions deterministically. A
second sampler produces a compound rate nobody can reason about.
