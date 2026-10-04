# 16 — Test plan

The security and resilience properties in this library are only facts to the extent the tests
assert them. Everything below states **what is asserted** and **why it is worth asserting** —
a test whose purpose is not written down is a test somebody deletes when it becomes
inconvenient.

## Structure

Eight suites, and the split is not arbitrary. Each exists because something in it cannot share
an environment with something in another.

| Suite | Environment | Label | Why separate |
|---|---|---|---|
| `core` | none — no DOM, no `fetch` | `unit` | Pure computation. Fast enough to run on every save, which is the point of the layer split; if it needs a DOM, the layering is wrong and this is where that shows |
| `wire` | a `fetch` stand-in and a fake clock | `unit` | Drives retry, backoff and expiry through simulated time. Real timers make a backoff test either slow or flaky, and flaky is worse |
| `state` | the `wire` stand-in plus a fake `BroadcastChannel` | `unit` | Multi-tab behaviour is two store instances in one process; that is the only way to make the refresh race deterministic |
| `dom` | a real Chromium page, **under the deployment CSP, no `unsafe-inline`** | `dom` | Needs a document, and needs a policy actually enforced rather than asserted against a stand-in that enforces nothing |
| `react` | the same Chromium page, plus `react` and `react-dom` as dev-only peers | `dom` | Needs a renderer, which no other suite may have: a store that can only be asserted through a framework is a store whose behaviour has moved into the adapter |
| `contract` | none — it reads two recorded artefacts | `unit` | The only suite whose fixtures came from anvil rather than from this repository. It asserts agreement with the other participant, so a stand-in in it would assert nothing at all |
| `browser` | a real Chromium, a real origin, a real CSP header | `browser` | Excluded from the default run. A policy is enforced by a browser or it is not enforced, and no fake document enforces one |
| `live` | a real browser, two contexts, a live anvil | `live` | Excluded from the default run. It is the only place cookies, origins and the leader lock are real |

Plus the scripts in `tools/`, which run as part of `npm run check`, because they enforce
source rules no unit test can express.

**The runner is `node:test`, not a third-party one** (`CLAUDE.md` §12, docs/15-tasks.md Phase 8
B1). `tests/support/register.mjs` resolves the `hammer/*` specifiers a real consumer writes
against this repository's own `src/`, the same map `tsconfig.json`'s `paths` carries, and
compiles `.ts`/`.tsx` through `esbuild` — the same two jobs a bundler-aware test runner did
before. `tests/support/test.ts` binds `describe`/`it`/the four hooks to `node:test` and adds
`it.each`, in one file, so a later swap of the *backend* touches one module rather than eighty.
Three `npm` scripts stand in for what were three configurations, one per suite grouping above
that cannot share an environment with the default run: `npm run test` covers every suite in the
table except `browser` and `live`; `npm run test:live` and `npm run test:browser` each name
their own directory and both **fail rather than skip** with no server or no browser found,
because a suite that quietly passes with neither is a suite whose green means nothing. `npm
run test` itself now needs a Chromium for the same reason: the DOM and `react` suites run in
one, driven by `tests/dom/in_browser.test.ts` (docs/15-tasks.md Phase 8 B2), which is why
`npm run test` and `npm run check` fail rather than skip with none found. Every DOM and React
test file is bundled by `esbuild` and served from a real origin under the deployment CSP
(`docs/03-deployment.md` §3), the same policy the `browser` suite serves — a fake document that
enforces nothing is what the `happy-dom` stopgap was, before Phase 8 B2 deleted it outright.

**No mocking framework.** Dependencies are injected — the fetch function, the clock, the
channel, the lock manager — so a test supplies a real object or a small hand-written
stand-in. A mock that is configured to return what the test expects asserts that the test
knows what it expects.

**Every published value has a caller, and `tools/check-surface-coverage.sh` fails the build
otherwise.** The rule generalises the one `tests/dom/registry.ts` already applied to
components — a component nobody mounted fails rather than being quietly uncovered — to
everything reachable from an `exports` entry point.

It exists because the gap it closes is where the expensive defects live. `Client.upload` was
exported for four phases; `wire/upload.ts` had tests for the three functions it is assembled
from and every one of them passed; nothing anywhere called the method. The first thing that
did found that a retried upload re-sent an empty stream — a defect none of the three unit
suites could see, because it was not in any of the three. A suite proves the parts work. Only
a caller proves the thing made of them does, and a published name with no caller has never
been the thing made of them.

The check is over **values**, not types: a type is exercised by being written in an
annotation, which a grep cannot tell from a mention in a comment. It has no allow-list, and
that is deliberate — the reason for an exemption would always be "this one is hard to test",
which is the property that makes it worth testing. A constant is covered by asserting it is
the value it claims to be, which is one line and catches the typo that ships to every consumer
at once.

## Fixtures

- `tests/testapp/hammer.descriptor.json` — the reference application's descriptor, and the
  input to every generator test. It is committed (`docs/01-seams.md` §14). It is refreshed by
  running anvil's `testapp_emit_descriptor`, and **nothing in this repository can tell you when
  it needs to be**: `check-descriptor.sh` regenerates the generated client from this file, so
  the pair stays consistent with each other while both drift away from the server. The check
  that would catch it has to build anvil, which is why it is a cross-repo row rather than a
  script here.
- `tests/testapp/api/` — the generated client, committed, and diffed by
  `tools/check-descriptor.sh`.
- `tests/fixtures/envelopes/anvil.json` — anvil's error table and the two bodies it writes,
  **recorded out of anvil's own headers** by `tools/record-envelopes.sh`, which compiles
  `tools/record-envelopes.cc` against a sibling checkout. A hand-written fixture asserts what
  we believe the server sends, which is worth nothing; this asserts what `wire_name`,
  `http_status`, `is_stealth_hidden` and `kNotFoundBody` say, and the only hand-copied part is
  the two lines of body assembly, cited at the line that copies them.
  **It is not a response per code captured from a running server, and it cannot be yet**:
  anvil is a library plus test executables with no runnable reference application, which is a
  row in `docs/15-tasks.md` §Cross-repo. Refreshing it is a person running the script, the same
  way the descriptor is — and nothing in this repository can tell you when it needs to be.
- `tests/dom/registry.ts` — the list the accessibility, direction and CSP passes
  iterate. It is the reference consumer's own mounts rather than a set written for
  the suite, and it compares itself against what the entry points EXPORT: a
  component nobody mounted fails rather than being quietly uncovered.
- `tests/support/fake_fetch.ts`, `fake_clock.ts`, `fake_channel.ts`, `fake_locks.ts`,
  `fake_worker.ts`, `state.ts`. The worker stand-in counts live handles, because
  "two decoded bitmaps and never three" is a memory property and not something a unit test can
  observe directly — a stand-in that did not model a bitmap could not assert it.

---

## Phase 1 — core

| Assertion | Why |
|---|---|
| `PermSet` round-trips anvil's little-endian base64url encoding, against vectors taken from anvil's own suite | The two encoders are in different languages and different repositories. A disagreement is an authority check reading the wrong bit — silently, and only for bits above 63 |
| `has` on an empty set is false for every bit; `hasAll` of an empty mask is true | The identity cases are where a bitset is most often wrong, and both are reachable from a route table |
| `codePointLength` counts astral characters as one; `String.length` is never used for a bound | A limit written against `.length` silently halves for emoji and for every non-BMP script |
| Truncation never splits a code point or a surrogate pair | A split pair is an invalid string that survives to the server as a replacement character |
| NFC normalisation makes two visually identical strings compare equal | The composed and decomposed forms are different keys in every index on both sides |
| A bidi isolate wraps interpolated user text | Without it an RTL name reorders the Latin sentence around it, changing which words the sentence appears to contain |
| Digit folding converts Arabic-Indic input to ASCII before the wire | The server refuses the shaped form, and the browser gave no indication |
| A `Cursor` cannot be constructed from a number, and no public signature accepts an offset | Type-level. It is what makes `skip(n)` unspellable rather than merely discouraged |
| A `ServerInstant` has no arithmetic with `Date.now()` | Type-level. The device clock is user-settable and routinely minutes out |

---

## Phase 2 — the generator

| Assertion | Why |
|---|---|
| A descriptor with a duplicate permission bit, a duplicate name, an empty name, or a bit out of range fails generation | anvil's `well_formed()` makes exactly these checks; a descriptor that skips them ships the malformed table to the client instead |
| A route naming a capability scope, a permission or a rate-limit bucket no table declares fails generation | The emitted type would require a `Capability` of a scope with no minting call, so nothing downstream of it compiles. This is the direction that matters |
| A capability scope no route requires, and a rate-limit bucket no route names, are reported as **warnings** | Both are dead entries that read as coverage, so silence is wrong. Failure is wrong too: anvil limits by keys that are not route buckets and mints scopes for operations its emitter carries no route for, so the reference descriptor has six of them and a hard failure would reject anvil's own output |
| A path whose parameters cannot be parsed, and two names that land on one identifier, fail generation | Neither is anvil's check; both are the emission's. A colliding name silently shadows the first `const` that had it, and an unparseable path is a call site typed `never` |
| A route pattern, locale tag, collation or cursor field holding a quote, a newline or a backslash fails generation — and a hand-built descriptor that gets past that check is still escaped into a string literal | These four are the only descriptor strings that never become an identifier. The class is the first line and `JSON.stringify` is the second; a guarantee that rests on one call is one a refactor drops without noticing |
| **No holder route's path appears in the emitted module, and none appears in the BUILT public bundle** | The whole point of anvil's stealth 404 is that a probe cannot distinguish forbidden from absent. Asserted on the built output rather than the source, because the source assertion is the one a bundler configuration silently invalidates — and because a lazily-loaded chunk is a public URL, so code-splitting never was the control |
| No unreferenced permission name survives into the built bundle | It is what proves the emission shape — individual `const` exports rather than one object literal — is doing what it is there for |
| The reference application's published output contains no descriptor file | It carries every path the emission goes to trouble to withhold, and a JSON file in an output directory looks exactly like an asset |
| A source map built from a privileged bundle contains what tree-shaking removed — and still no holder path | Asserted rather than assumed, because it is the reason for the policy in Phase 7: a privileged bundle publishes no public source map. The second half is the sharper claim — the path is absent even there, because the emission never wrote one |
| Regenerating the committed fixture produces a byte-identical output | Determinism. A generator whose output depends on key order produces a diff on every run and is ignored within a week |
| The generated module's hash matches the descriptor's | The stale-client check has no value if the two can disagree locally |
| The identifier a permission or route `const` is bound to is asserted against a written-out name | The one defect this derivation has had — `kPermContentread` — survived a green suite, because every assertion in place checked the bit and not the name, and an expectation computed the way the emitter computes it agrees with the emitter's bugs |
| A missing copy entry for a new validation reason, a new error code or a new locale is a **type** error | It is the mechanism that stops `BAD_FORMAT` reaching a user in an interface that is otherwise entirely in Arabic |
| An invalidation naming a route id the descriptor does not carry is a **type** error, on both sides of the arrow | A mutation naming a resource the server retired is an invalidation that silently stops happening, and nothing renders wrongly until somebody notices a stale row |

---

## Phase 3 — the wire

| Assertion | Why |
|---|---|
| Every error code in `tests/fixtures/envelopes/` decodes to its union member, and an unknown code decodes to `Unknown` without throwing | `ErrorCode` is append-only server-side. A client that throws on a new code turns a deploy into an outage in every open tab |
| A `404` produces no permission-flavoured error anywhere in the union | Rendering "forbidden" for a 404 reconstructs the oracle anvil removed |
| A route id absent from the session's table refetches the session, retries once, then reports **not-found** | A missing address is not a refusal, and reporting it as one rebuilds the oracle in the one place the server cannot reach |
| No error in the library can say a permission refused something locally | Type-level, and it is how the row above stays true: there is no cause to say it with, so a client cannot report a denial it invented |
| Resolving a public route never reads or fetches a session | It is reachable with no credential at all. A client that needed a session to find the login route could not log anybody in |
| A route affordance is answered from the session's table, and a superadmin with no bits set is still afforded everything | anvil's `satisfies()` short-circuits on user type and a superadmin's set is deliberately not all-ones. A client counting bits would hide the whole application from the one account that reaches all of it |
| A session path that is protocol-relative, absolute, backslashed, whitespaced or carries a query is refused | Every path in that table becomes the path of an authenticated request. `//evil.example` resolved against an origin is a different host — the one malformed path that does not fail, but succeeds somewhere else |
| A method of `ANY` costs its own route and not the session | anvil emits `ANY` so a client fails loudly instead of defaulting to GET; discarding the session over it logs a person out of an application whose only defect is an uncallable logout |
| The route table decodes into a `Map`, and a `__proto__` key is an ordinary entry | The keys come off the wire. In a plain object two of them are lookups that answer something nobody put there |
| `Retry-After` is honoured to the second, on both `429` and `503` | A client that invents a backoff retries into the outage it was told to wait out |
| Retry jitter comes from `crypto.getRandomValues` | Jitter from a predictable source synchronises, which is the thundering herd it exists to prevent |
| A non-idempotent route is never retried without an idempotency key, and a replay reuses the same key | A retry without one is a duplicate write — a second charge, a second message, a second row |
| **Two tabs, one expiry, exactly one `POST /auth/refresh`** | anvil rotates the refresh token as a compare-and-swap. Two concurrent refreshes log the user out of a valid session, in the tab they were using |
| A second `401` after a successful refresh clears identity and broadcasts logout | It is a rejection, not a race, and looping on it is how a client hammers a server that has already said no |
| A single-use capability is never auto-retried | The server consumed it whether or not the response arrived; a retry reports failure for an operation that succeeded |
| The queue sheds rather than growing when in-flight is at its bound | An unbounded client queue turns a slow network into unbounded memory growth |
| The breaker opens after N transport failures and lets exactly one probe through | Twenty tabs retrying independently is a self-inflicted denial of service |
| SSE resumes from `Last-Event-ID` and a duplicate event is dropped by id | Reconnects replay. An event handled twice must be a no-op, not a second notification |
| `upload` refuses an oversized file before a byte is sent, and never calls `readAsArrayBuffer` | The check saves a round trip; the absence of the read is what keeps a 40 MB video out of the tab |
| Constructing a client against a cross-**site** API throws at construction | `SameSite=Lax` cookies are not sent there. The session is already gone; the only question is whether it fails loudly |

---

## Phase 4 — state

| Assertion | Why |
|---|---|
| The cache evicts at its ceiling, least-recently-used first | A tab open for days with an unbounded cache is a leak with a slow fuse |
| An identity change drops every entry | A cache that survives a logout renders one user's data to the next on a shared device |
| Nothing is written to `localStorage`, `sessionStorage` or IndexedDB | A persisted response outlives the cookie that authorised it |
| A mutation's invalidation reaches a second store instance over the channel | Tab B rendering the row tab A just changed is the defect multi-tab coherence exists for |
| A `VersionMismatch` surfaces a reconciliation and never re-sends the same body | Re-sending is a lost update with extra steps — the exact bug anvil's optimistic concurrency detects |
| An optimistic value is rolled back on failure and is never read by a second request | An unconfirmed value that becomes an input is a fabrication that propagates |
| An event requiring consent is **not queued** before consent | A buffer that flushes on consent is a buffer of pre-consent data, which is the thing consent was about |
| `imagePool` holds at most two bitmaps and closes each on consume | 48 MB per 12 MP decode; three concurrent is a killed tab, with whatever was typed in it |
| A rejected pool task settles its promise | An unsettled promise is a spinner that never stops, which is worse than an error |
| A response with no `Cache-Control` is read again rather than cached briefly | Saying nothing is not permission. A client that invented thirty seconds would hold a row through the permission change that was supposed to remove it |
| A stale entry is served only where `stale-while-revalidate` granted it, and the revalidation is not optional | Serving stale without refreshing is a cache that has quietly extended the freshness it was given |
| `hasMore` comes from the server's cursor, and a full last page does not imply another | `items.length === limit` is wrong in both directions: a full page at the end shows a control that loads nothing, and a filtered short page in the middle ends the list early |
| A bound is counted in code points, and an emoji counts as one | `String.length` halves the allowance for every script that needs it most, and the disagreement surfaces as a form that accepted what the server then refused |
| A `VALIDATION_FAILED` naming a field the form does not have is kept rather than dropped | A form that refuses to submit with nothing marked on it is the worst failure this surface has |
| A PII field is never populated and is emptied on accept | anvil answers `null` for one, so a form that re-read its own PII field would empty it on the next render anyway — with the value having been on screen in the meantime |
| A draft and a published section are different cache keys | A draft read into the published entry is a draft rendered to every visitor, with no request having gone wrong anywhere |
| The reference consumer constructs, in one order, with every seam supplied from outside | Type-checking proves each seam can be satisfied. It does not prove they can be satisfied at the same time, and a construction order that only works one way is a seam an application discovers at run time |

---

## Phase 5 — the rendered surface

| Assertion | Why |
|---|---|
| The insertion site is **not callable with a string**, and `SanitizedHtml` is **not constructible from bytes** | Type-level, both. The second is the one that matters: without it the first is a speed bump, which is the same reasoning anvil gives for closing `SanitizedHtml`'s constructor |
| No component produces `innerHTML` assignment anywhere in the emitted bundle | Asserted on the built output, not the source, because the source check is what a refactor routes around |
| Every component renders under a CSP with no `unsafe-inline` and no `unsafe-eval` with zero violations | A component that sets an inline style works in every other suite and is silently blank in production |
| Every component is enumerated by the accessibility suite: labelled control, visible focus, keyboard operable, no `aria-hidden` over a focusable node, no positive `tabindex` | Enumeration is what makes a new component fail until it is accessible, rather than being tested if somebody remembers |
| Every component renders correctly under `dir="rtl"` and isolates user text | Direction is not a stylesheet concern; a component that assumes left is wrong for a whole audience |
| An image element carries intrinsic dimensions | Without them every image is a layout shift, and layout shift is the one performance defect that is also an accessibility defect |
| The error surface renders the application's copy and the `request_id`, and nothing else | There is no server message to render, by design |
| The bell's unread count survives a duplicate stream event, a reconnect and a mark-read replay | The stream is at-least-once and the mark-read is retryable; a count that drifts is the defect users notice first and report last |
| The bell announces an arrival in a live region without moving focus | An arrival that steals focus interrupts whatever the person was typing, which is a worse defect than a missed notification |
| Every chart renders its data-table fallback, and the series is traversable by keyboard | A chart that exists only as pixels is a chart part of the audience cannot read |
| No component contains a user-visible string | Over the SOURCE, by `tools/check-vocabulary.sh`, across `src/dom` and `src/chart`. It is §1's rule, and the only one a component author breaks by being helpful. It is **not** enumerated over the built output, and the reason is recorded rather than dropped: finding string literals in minified JavaScript needs a real parse — a regex literal can contain a quote, and `escapeAttribute` contains exactly one — so a hand-written scanner mistakes the code between two strings for a string, which is what it did. A dependency-free repository does not get a parser in its suite for one assertion, and a scanner wrong in the direction of passing is worse than none |
| ~~happy-dom's `DOMParser` executes scripts and a browser's does not~~ (history — Phase 8 B2) | The DOM suite ran against happy-dom's `DOMParser`, which attaches a browsing context to a window, so a `<script>` ran as the parser appended it; the sanitiser's dangerous cases asserted the STRING `sanitize` produced rather than round-tripping through the document for exactly that reason. Since Phase 8 B2 the suite runs in real Chromium, whose `DOMParser` builds a document with no browsing context — the bound this row recorded no longer applies, and `tests/dom/sanitized.test.ts`'s own cache-busting re-import cases now exercise the real parser directly |

---

## Phase 6 — the adapter and the contract

| Assertion | Why |
|---|---|
| A resource opened by a hook is released on unmount, and under `StrictMode` the live-handle count nets to one while mounted and zero after | StrictMode's double invoke is the mount/unmount asymmetry React will eventually really do (an offscreen tree, a restored back/forward cache). A hook that leaks one handle per mount leaks a request and a store in a tab that stays open for days |
| An address that changes releases the previous entry before opening the next | Otherwise a component renders one commit of the old address's body under the new address's parameters, which is the defect that looks like a caching bug and is not |
| A mutation aborts what is in flight on unmount, and publishes nothing afterwards | Nothing outlives what created it (`CLAUDE.md` §3.3): a request whose screen has gone holds a slot in the bounded queue that something visible is waiting for |
| A second run supersedes the first, and the older answer does not land last | A person who edited twice is looking at the second edit. The adapter decides what is on screen and nothing else |
| The adapter's bundle is under a gzipped ceiling, imports nothing but `react`, and reaches the layers below it through `import type` and one constant | The rule is "it binds stores and holds no logic", and a sentence is not a check. Behaviour arrives in an adapter one helpful commit at a time, and each arrival is a second implementation of something the store below already does — reachable only by consumers who chose this framework |
| No layer below `react` names `react` | A store that imported a hook would make `hammer/state` unusable without a framework, which is the whole premise of the adapter being a separate entry point |
| Every hook is driven through the reference consumer's own generated tables | A field spec or a route invented for a test is one nothing regenerates when the server's tables move |

Everything else about a hook is React's suite to run (§What is deliberately not tested), and
everything else about a store is asserted in `tests/state/**` with no framework in the process.
That split is the reason the adapter is worth this little code.

### The contract

Every other suite here asserts hammer against hammer: the fetch stand-in answers what the test
wrote, and the decode agrees with it because the test made it up. `tests/contract/` is the one
place the other participant is present.

| Assertion | Why |
|---|---|
| Every code anvil can write is a member of the generated union, with the status and the stealth flag anvil gives it | `ErrorCode` is append-only server-side. A client missing a code decodes it to `Unknown` — correct behaviour, and a silent downgrade of every surface that had a sentence for it. The status is all the retry policy has left to read on a code it does not know |
| The recording and the descriptor agree, code for code and reason for reason | They are two paths out of one enum, produced by two anvil binaries. A disagreement means one of them came from a different checkout, which is the staleness `check-descriptor.sh` cannot see |
| `kStealthHiddenErrorCodes` is exactly the set anvil rewrites to a 404 | A list that disagreed would describe a server behaviour that does not happen, to an application deciding what to render |
| Every recorded failure body decodes to its code, with a null `request_id` and a null `fields` | Those are the bytes anvil assembles. A surface that assumed a string renders the word `undefined` on the most common failure there is |
| The stealth 404 and the genuine 404 are byte-identical | The oracle the server spent a whole design removing is given back by one byte of difference, and the two paths that write them are in different translation units |
| The documented envelope — `request_id` and a `fields` map — decodes | The shape is anvil's `docs/00-architecture.md` §8 and **no writer in anvil's source produces one**. The decode handles the contract a client is written against; that nothing produces it yet is a cross-repo row |
| A code appended after this bundle was built decodes to `Unknown` and keeps its status; a body that never reached anvil does too | A deploy answers a tab that has been open since this morning. A decode that threw would turn that deploy into an outage in every one of them |
| Every route anvil describes decodes out of a session table and builds, with every parameter percent-encoded and no offset expressible | The holder paths are never emitted, so the addresses a real client uses arrive at run time. A pattern this client cannot parse is an address a person cannot reach, discovered on a deploy rather than at generation |
| A route described with `ANY` stays out of `routes` and resolves as unusable | anvil emits `ANY` so a generated client fails loudly rather than defaulting to GET. The reference application describes `auth.logout` that way, which means it has a logout route no client can call — recorded, not worked around |

---

## Phase 7 — browser

Run against a real Chromium, driven from Node over a real origin that serves a real CSP header.
`npm run test:browser` is the only thing that runs it; it is excluded from `npm run check`
because it needs a browser binary on the machine, and it **fails rather than skips** when it
cannot find one.

`tests/browser/cdp.ts` is an in-repo DevTools Protocol client, over
`--remote-debugging-pipe` rather than a `ws://` port (`docs/15-tasks.md` Phase 8 B3):
nothing here downloads a browser or opens a socket anything else on the
machine could connect to. `HAMMER_BROWSER` names an executable, or the
harness finds the system Chromium.

| Assertion | Why it must be a browser | Has run |
|---|---|---|
| Every component mounts under `require-trusted-types-for 'script'` with no policy violation and nothing thrown | A policy is enforced by the browser or it is not enforced. `tests/dom/csp.test.ts` traps every route to a sink, which is the strongest thing a fake document can say; whether Chromium agrees is a different claim | **yes** |
| A deliberate violation IS reported, so the empty list above is an absence | The lesson `tests/dom/csp.test.ts` learned when a trap on `Document.prototype` turned out to be shadowed: a check that cannot fail reports clean for the wrong reason | **yes** |
| Two tabs of one session: neither can read the credential, the tab that did not sign in reaches a protected route, at most one refresh happens and `unelected` is zero | `tests/state/` runs two store instances over a lock manager the test wrote, so it asserts the algorithm is right GIVEN that manager. Whether Chrome's is shared between two tabs of one origin, whether the rotated cookie reached the second tab, and whether `__Host-` did what it says are the three things that actually break | no — needs anvil |
| A logout in one tab empties the other | The disclosure it prevents — a tab rendering a signed-in shell after another signed out — needs two real contexts to exist | no — needs anvil |
| A tab frozen mid-request resumes without duplicating the write; a reloaded tab loses nothing the server holds | Freeze is the browser taking the event loop away and giving it back. A fake clock cannot produce it, because the fake clock is the thing that would have kept running | no — needs anvil |
| Throttled to 3G, in-flight requests stay bounded | The queue's job is a shape. Under a fake clock every request resolves in the order the test resolved it, which is the one arrangement that cannot show a queue misbehaving | no — needs anvil |

The three that have not run need the page to be **same-origin** with anvil, which is not a
convenience: hammer refuses a cross-site API because `SameSite=Lax` cookies are not sent there,
so a page served off a second port would assert an anonymous session. anvil serves no
application, so the harness fulfils its own bundle at one path on the live origin and touches
nothing else — a run that intercepted an API call would be a stub, which is the thing this
suite exists not to be.

Freeze and throttling are driven over CDP (`Page.setWebLifecycleState`,
`Network.emulateNetworkConditions`), which is why the suite is Chromium and not a browser
matrix.

**What the first run found, on its first execution.**
`DOMParser.parseFromString(..., "text/html")` is a Trusted Types sink. `src/dom/sanitized.ts`
said in its own header that it was not, and used that as the reason hammer needed no permissive
policy of its own — so under an enforcing CSP every rich-text render threw, on exactly the
deployment that had been careful enough to serve one. All 1064 tests passed while it was
broken, and every one of them would have kept passing. The fix is the shape Trusted Types is
asking for: one named policy at the one audited place a string becomes markup, which is the
same claim `SanitizedHtml`'s private constructor makes to the compiler, made to the platform
instead. The application's CSP has to carry `trusted-types hammer default`
(`docs/03-deployment.md` §3).

---

## Phase 6–7 — live

Run against a real browser and a live anvil. Everything here is a property that **cannot** be
observed from a unit test, and the list is short on purpose.

anvil ships `anvil_reference_server`, so this runs:

```
tools/run-live.sh              # the Node live suite
tools/run-live.sh --browser    # the browser runs as well
```

The script starts the server, reads its base URL and its two passwords off the server's own
stdout — both are drawn at boot and printed once, so nothing here is a constant — hands it a
database of its own, and drops that database afterwards. It is a script rather than a paragraph
for the reason `tools/check-descriptor.sh` gives about the descriptor: the honest fix for "a
person has to remember to run that binary" is a step that runs it. `npm run test:live` still
works with the variables set by hand, is excluded from `npm run check`, and **fails rather than
skips** when `HAMMER_LIVE_ORIGIN` is absent — a live suite that quietly passes with no server is
a suite whose green means nothing.

**What the first execution found is the argument for the whole section.** `decodeSessionView`
was written against a session payload anvil does not send, and all 1,145 unit tests passed over
it: they built their payloads with the same fixture the decode was written against, which is two
copies of one belief. Against a real anvil the client signed in and could then call nothing.

Two things bound what a Node run of it can claim, and they are the reason the rows below say
*browser* rather than *server*. The cookie jar is in the harness, because Node's `fetch` keeps
no cookies — so `__Host-` scoping and `HttpOnly` are not enforced by anything in that run, which
is the whole property being bought. And one process is one tab: the leader lock, the rotation
race and the logout fan-out need two real contexts.

Three of phase 6's own six rows still cannot be written against the reference application: it
has no upload route, no HTTP stream route — `live.feed` is a WebSocket upgrade — and no write
route past `auth.login`, `auth.refresh` and `auth.logout`. So `wire/upload.ts` and `wire/sse.ts`
are driven by the unit suites and by nothing else, and the versioned case asserts the half that
is reachable: a write with no version read is **refused rather than sent**, which is the lost
update the whole mechanism exists to detect, and a `send` that is never called is the only
observable difference.

The suite needs two accounts and says so. The projection is only visible as a projection when
two holders are handed two different tables, and `authority.superadmin` only means anything
against an account whose permission set is deliberately not all-ones. A suite with one account
asserts that a table arrived, which is the half that was never in doubt.

| Assertion | Why it must be live | |
|---|---|---|
| Login sets `__Host-` cookies that script cannot read, and the client works without ever seeing them | Every unit test passes with a token in a variable. The property being tested is the absence of that variable | ✅ |
| **Two real tabs, one expiry: one refresh, one replay, zero rotation races** | Two store instances in one process share a fake channel. Real tabs share a real lock manager, a real cookie jar and a real network | ✅ |
| A logout in tab A empties tab B's caches and closes its stream | The disclosure this prevents needs two real contexts to exist | ✅ |
| A capability minted on the site origin is redeemed once, and a replay is refused | The single-use property is the server's; the client's part is not retrying | ✅ |
| A preview URL exchanges its capability for a path-scoped cookie and a `303`, and the content renders on the content origin | This is the exact path that shipped broken in anvil's own history: a render requiring a cookie the origin split guarantees will never arrive. It has never been caught by a unit test anywhere | the reference application mints no capability |
| Content-origin HTML is never present in the site origin's document | The origin split is undone by one well-meaning `fetch` | the reference application serves one origin |
| A frozen tab resumed mid-request completes | Freeze and discard cannot be simulated meaningfully | ✅ |
| …without duplicating a **write** | as above | no write route (§Cross-repo) |
| Throttled to 3G, the queue bounds in-flight and every request is answered | Concurrency behaviour under real latency is not the same shape as under a fake clock | ✅ |
| `Retry-After` is obeyed exactly, and the breaker opens | A backoff a client invented against a server that named one retries straight back into the outage | no rate limiter (§Cross-repo) |

**The ticks are what a run produces, not what the code claims.** Three rows are marked with the
reason they cannot be produced rather than left blank, because a row nobody can explain is a row
somebody eventually deletes. Each is a line in `docs/15-tasks.md` §Cross-repo.

---

## Phase 8 — dependencies

The tree that builds hammer is under test the same way hammer is (`CLAUDE.md` §12). The
checks live in `tools/`, not in a suite, because they judge the repository and not the
library. Each one is driven against a violation before it is trusted.

| Assertion | Development | Production |
|---|---|---|
| `dependencies` is empty, every peer is optional, no lockfile entry is non-dev | error | error |
| Every `devDependencies` key is in a tier of `tools/dependency-policy.json` | error | error |
| Every `devDependencies` spec is an exact version | error | error |
| Every lockfile package under an allowed root is named in that root's `tree` | error | error |
| `.npmrc` sets `ignore-scripts=true` | error | error |
| A package under a tolerated root | warning, naming the root and its task row | **error** |
| An install script not listed in the policy | warning | **error** |
| `npm audit`, any severity | warning | **error** |
| `npm audit` could not run | warning | **error**, because a gate that passes when the registry is unreachable passes whenever somebody wants it to |

`npm run check` runs the development column and must stay offline-safe. `npm run
check:production` runs the production column, and it was **red by design** until Phase 8 Part
B emptied the tolerated tier. It named `vitest`, `happy-dom` and `playwright-core` until row B1
retired the first, B3 the third and B2 the second — `tools/dependency-policy.json`'s
`tolerated` object is now `{}`, and `npm run check:production` is green on a clean `npm ci`.

Part B changed what ran the suites, not what they asserted. The test count is stated before
and after each replacement and had to be equal. A test that could not be carried across is
named in the commit body with the reason, never dropped silently. After B2 the DOM suite runs
in Chromium, so the happy-dom caveats recorded in Phase 5 above stopped being bounds on the
claim and became history — including one Phase 5 never anticipated: `src/dom/bell.ts`
restored focus by reading `document.activeElement` AFTER hiding the popover, and a real
browser blurs a hidden element's focus to `document.body` synchronously, which happy-dom never
did. `tests/dom/bell.test.ts`'s own Escape case caught it on the suite's first run against
Chromium.

---

## Phases 10–15 — chat

The design is [`05-chat.md`](05-chat.md). Chat has more contracts with anvil than any other part
of hammer (a frame grammar, a text validator, eleven response shapes, a device encoding, and a
protocol), so this section opens with where each contract's truth comes from. None of them is a
file written beside the code it checks.

| Contract | Held to | From |
|---|---|---|
| `limits.chat` | `tests/testapp/hammer.descriptor.json`, format 4 | anvil's `testapp_emit_descriptor` |
| Every chat answer's shape | bytes recorded off the reference server by the live suite | a running anvil |
| The frame codec | anvil's golden frames, printed | anvil, once the cross-repo emitter lands |
| The text validator | anvil's validator cases, printed | the same |
| Device encodings, the link message, refused keys, the push payload | `testapp_emit_chat_vectors` | anvil, already printed |
| X3DH, the ratchet, sender keys, the fingerprint | `tests/chat/oracle/` | a libsignal harness in a separate repository |
| The framing around them | `tests/chat/framing/` | a third implementation written from 05 §9.15 |
| Incremental SHA-256 | NIST CAVP | NIST |

**Where each suite runs.** The protocol runs under `node:test`, because Node's WebCrypto has
X25519, Ed25519, HKDF and AES, and every oracle vector is a pure function of its inputs. The
vault does not: Node has no IndexedDB, and a stand-in for one is a database written for the
suite. So the vault's transaction discipline is behind a narrow store interface with an
in-memory implementation for the unit suite, and the real IndexedDB, the real locks and the real
crash between two transactions run in the browser suite.

| Assertion | Why |
|---|---|
| A burnt seq is crossed with no request beyond the catch-up that found it | A client that read a hole as loss would refetch forever around a seq that will never exist |
| The cursor never moves on a wake, and a wake's seq above a dropped one does not skip it | The wake is a hint and the catch-up is the record. Moving on the hint skips the one message Redis dropped, forever |
| Two catch-ups are never in flight for one conversation, and a burst of fifty wakes makes at most one catch-up per window | anvil inlines messages in wakes so a busy group does not turn into a database read per member per message. A client that caught up per wake would undo that |
| A pending message, its answer, its wake and its catch-up converge on one row by `cid` | Three paths deliver one message. A count of rows is the visible symptom of getting it wrong |
| Read is never reported for a hidden tab, and is the highest seq shown rather than the head | A read receipt from a tab nobody is looking at tells the sender a lie |
| One `read_by` per open conversation per trigger, whatever is on screen | Ticks per message would be an N+1. The watermark makes the newest one answer all of them |
| `create` is not retried after a lost answer, and `send` is, with the same body | One is a second group. The other is the same message |
| A `4001` and a `4006` never reconnect; a `4005` refreshes through the leader before it does | The first loops two tabs against each other for ever, the second loops a broken client against the server, and the third reconnects into the same refusal |
| Two tabs of one profile hold one socket | anvil closes a second socket of one session, and two tabs that each reconnected would trade it for ever |
| Every reconnect's jitter comes from `crypto.getRandomValues` | A predictable jitter synchronises clients after a deploy, which is the herd it is there to scatter |
| A message decrypted once is never decrypted again, by any tab or the service worker | The second decrypt fails, because the key was deleted after the first. A client that tried would mark a readable message undecryptable |
| Two tabs sending at once never step one ratchet twice: every message decrypts exactly once on the peer | Two writers from one chain index reuse a message key, which breaks both messages' confidentiality |
| A retried encrypted send is the same bytes, before and after a reload | Encrypting again would step the ratchet twice for one message |
| A stale fence resends with the same `cid`, gives up after three rounds, and rotates the sender key when a device left | The `409` spent no seq, so the `cid` is unused. A device that left holds the old chain |
| Registering a device with a stale sign-in signs nobody out | The defect the cross-repo row exists for. A second tab is open in the case and must still be signed in after |
| A link whose timestamp came from a device clock a day out still links | The timestamp is the server's, from a `Date` header. The device clock is never asked |
| A sign-out leaves no readable record in the vault, including when the tab closes mid-wipe | The vault key is deleted first, so every record is unreadable even if the database deletion never finishes |
| No private key is extractable: `exportKey` on every stored key is refused | The property that ends a compromise with the page, as the `HttpOnly` cookie's does |
| A decrypted message with a U+202E, an out-of-bounds mention or text past the kind's bound is `invalid` | Under encryption the server validates nothing, so the recipient's check is the only one there is |
| Sealed media refuses a reordered, dropped, truncated or extended chunk, and a whole-file hash mismatch | Integrity has to be checked as the bytes arrive, without the whole file in memory |
| The encrypted push shows the application's generic wording on every failure path, never nothing | A user-visible push that shows nothing is a push the browser holds against the origin |
| `trust` and `unreviewedProtocol` are required, and omitting either fails to compile | Type-level. The first is a security property no default should hand an application. The second makes an unreviewed protocol something an application has to type out |

---

## What is deliberately not tested

- **That the server enforces anything.** anvil's suite does that. A client-side test asserting
  a permission is denied is a test of a stand-in, and its green is worth nothing.
- **Visual appearance.** hammer ships no styling; a snapshot of unstyled markup asserts the
  markup, which the accessibility and direction suites already assert with a reason attached.
- **Framework internals.** The React adapter is tested for subscription lifecycle and
  StrictMode safety. That hooks work is React's suite.
- **Every browser.** The suites run on one engine. The platform features hammer depends on —
  `navigator.locks`, `BroadcastChannel`, streamed request bodies — have a documented degraded
  path each, and the degraded path is tested; a browser matrix is a release activity, not a
  per-commit one.
