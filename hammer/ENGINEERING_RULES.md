# ENGINEERING_RULES.md — Engineering Rules for `hammer`

Stack: **TypeScript (strict) on the platform**, **zero runtime dependencies**, ESM only,
strict **UTF-8** end to end.

hammer is the browser half of an [anvil](../anvil/) application. It is a library, and it is
a library in the harshest distribution channel there is: applications are built on top of it,
a defect here is a defect in every one of them at once, and every byte of it is downloaded,
parsed and executed on hardware the author does not own and cannot profile.

Three non-negotiable axes, in priority order:

1. **Security** — the client is not a security boundary, and that is precisely why its
   defects are expensive. hammer cannot grant authority it does not have; what it can do is
   lose the session, and it loses it permanently.
2. **Performance** — bytes shipped, main-thread milliseconds, and resident memory on the p95
   device: a four-year-old mid-range Android on a congested network, not the laptop this is
   written on.
3. **Resilience** — every design must survive a response that never arrives, a tab that is
   frozen or discarded mid-flight, N tabs sharing one session, and a deploy that lands while
   a tab is open.

When they conflict, the order above decides. Never trade the session for a kilobyte.

---

## 0. Engineering Standards

Commit messages describe the change only, in the imperative mood, as a human engineer would write them.

---

## 1. Library rules

hammer holds no application's data. Not one route path, permission name, section key, field
type, locale, notification topic, event name, rate-limit constant, class name, colour —
**or user-visible string, in any language**.

- **A type hammer *ships* is machinery. A table hammer *populates* is a bug.** `FieldTypeSpec`
  is machinery; a table of eleven field types is an application's.
- **Everything an application supplies arrives through the generated descriptor module and
  nowhere else** ([`docs/01-seams.md`](docs/01-seams.md)). A table reaching hammer by any
  other route is a table nothing validated and nothing regenerates when the server changes.
- **Every seam is documented in `docs/01-seams.md` before it is used.** A seam an application
  cannot discover is a seam that gets worked around.
- **The published entry points are the ones in `exports`, and a module may only import
  downward.** `core` → `wire` → `state` → `dom` → adapters, never sideways and never up.
  In a language with no linker this is not automatic, so it is a script:
  `tools/check-layering.sh` makes a violation a build failure rather than a review comment.
- **No user-visible string, and no "sensible default" copy.** anvil's error responses carry a
  code and never a message, for exactly this reason: the words belong to whoever knows the
  audience and the locale. An English default is a string that ships to an Arabic user.
- **No styling.** hammer ships structure, behaviour, ARIA and direction. A class name, a
  colour, a font and a spacing scale are the application's.
- **A component may hold logic; it may not hold a name, a word or a look** (§9). The
  notification bell, the analytics chart and the login flow are mechanism every consumer
  needs and every consumer gets wrong once. The topic, the metric and the sentence are not.
- **`tests/testapp/` is the reference consumer and the proof.** Every seam is exercised by
  type-checking it. A seam that cannot be satisfied from outside hammer fails there, which is
  the only place it can fail cheaply.
- **Anything an application must supply fails at generate time or type-check time, never at
  runtime.** A route the descriptor does not declare cannot be spelled; a permission it does
  not carry is not a member of the union. A seam discovered by a blank screen at 3am is a
  design failure, not an operator error.

---

## 2. Bytes and memory

### 2.1 Bytes are the first cost, and they are paid before anything renders

A kilobyte of JavaScript is not a kilobyte of transfer. It is downloaded, decompressed,
parsed, compiled and held — and on a mid-range phone the parse-and-compile alone runs at
roughly **1 ms per kilobyte of uncompressed script**. Three hundred kilobytes is a third of a
second in which the device is doing nothing a user can see.

- **Every entry point has a declared gzipped ceiling and exceeding it fails the build**
  (`tools/check-bundle-budget.sh`). A budget on a dashboard is a budget nobody enforces; the
  number is in the repository and the build is what reads it.
- **Named exports only, no default export, no barrel that imports the world.** A consumer who
  imports one function must ship one function. A barrel that re-exports forty modules is forty
  modules in every bundle that touches any of them, because a bundler cannot prove a
  side-effect-free graph it was never given.
- **No top-level side effects.** No module-level `new`, no registration on import, no polyfill
  installed because a module was loaded. `"sideEffects": false` is a promise the code keeps.
- A feature that is used by a minority of consumers is its own entry point, not a branch
  inside a shared one.

### 2.2 Never materialise what you can borrow

- **Binary never enters the JS heap.** An image is an `<img src>` pointed at the media origin
  — never `fetch(...).blob()`, never `createObjectURL` over a fetched body, never a base64
  `data:` URI. anvil serves image bytes with `sendfile()` precisely so they never enter *its*
  heap; decoding them into a JS `Blob` to display them puts the whole file in the one heap
  with the least room, on the device least able to spare it, and loses the CDN cache besides.
- **An upload streams from the `File`.** A `File` is a handle, not bytes, until something
  reads it. `readAsArrayBuffer` on a 40 MB video is 40 MB of tab memory and a frozen main
  thread; the body goes to `fetch` as the `File` itself, and the size check happens against
  `file.size` before a byte is sent.
- No intermediate arrays on a hot path. `.map().filter().reduce()` over ten thousand rows is
  three arrays nobody asked for; one loop is one.
- `structuredClone` over a `JSON.parse(JSON.stringify(...))` round trip — the round trip is a
  serialise, a parse, two allocations and silent corruption of every `Date`, `Map` and
  `undefined` it touches.

### 2.3 Fixed-size data is a typed array

- **A permission set is a `Uint8Array(16)`, not a list of names.** anvil stores 128 bits, and
  a membership test is one AND. On the wire the set is 24 base64url characters; the same set
  as JSON names is several hundred bytes, on every session response, forever. In memory it is
  one object against one per name plus a string compare per check.
- **A UUID is 16 bytes.** It is rendered for a human and parsed for a wire, and it is never
  stored as a 36-character string to be compared with `===`.
- **Objects have exactly one shape.** Declare every property in the literal or the
  constructor; never add one later and never `delete` one. A shape change deoptimises every
  call site that has already seen the old shape, and the cost lands nowhere near the line
  that caused it.
- **Every cache is bounded, evicting, and keyed by the identity allowed to read it.** A tab
  stays open for days: an unbounded cache is a leak with a slow fuse. A cache that survives a
  logout is worse than a leak — it is one user's data rendered to the next on a shared device.

---

## 3. API rules

### 3.1 Types carry the guarantees

- `readonly` on every field and every array that is not deliberately mutable. `as const` on
  every table. No `any`. `unknown` at each boundary, narrowed exactly once.
- **No `!` non-null assertion and no `@ts-ignore`.** `@ts-expect-error` with a reason on the
  line is the only escape hatch, because it fails when the reason stops being true.
- **Anything whose provenance is a guarantee is a branded type with a module-private
  constructor** — `SanitizedHtml`, `Capability<Scope>`, `Cursor<Route>`, `IdempotencyKey`,
  `Uuid`. A value that asserts something about itself must not be constructible by a caller
  who has not done the work. This is the one trick that converts a rule people remember into
  a rule the compiler checks, and it is the same trick anvil uses for `SanitizedHtml` and
  `CapabilityScope`.
- **Failure is in the return type**: `Result<T, HammerError>`. `throw` is reserved for
  programmer error — a violated precondition, a misconfigured client — and never for a
  network failure or a server error, both of which are certainties rather than exceptions.
  An exception thrown for an expected condition is a `catch` somebody forgets to write.
- Discriminated unions over an options object of optional booleans. Three booleans are eight
  states, of which five are usually meaningless.
- **Every public async function takes an `AbortSignal`.** A request nothing can cancel is a
  request that outlives the screen that wanted it.

### 3.2 Parameter passing

| Kind | Pass as |
|---|---|
| A scalar, an id, an enum member | by value |
| A sequence the callee only reads | `readonly T[]` or `Iterable<T>` |
| A sequence the callee stores | a frozen copy made at the boundary |
| More than three parameters | one `readonly` options object |
| A callback that may outlive the call | with an explicit unsubscribe in the return |

### 3.3 Ownership and lifetime

- **Every subscription returns its own unsubscribe**, and the return is `[[nodiscard]]` in
  spirit: a listener with no way off is a leak and a double-render.
- One owner per store. A store is created by the thing that will dispose it.
- No global mutable singleton. Where the platform already imposes one — the document, the
  broadcast channel, the lock manager — it is injected, so a test supplies its own rather than
  racing every other test in the file.
- Nothing outlives what created it. A component that unmounts aborts its requests, closes its
  streams, and cancels its timers.

---

## 4. The main thread

- **Nothing blocking ever runs on the main thread.** A frame is 16.7 ms and script's share of
  it is about 8. Anything longer is a dropped frame, and a dropped frame during a scroll is
  the performance defect users actually report.
- **Separate worker pools per workload class**, each with a bounded queue. A single shared
  worker means one twelve-megapixel decode blocks the parse that would have rendered the page.

| Pool | Workload | Sizing rule | Queue full → |
|---|---|---|---|
| `imagePool` | Client-side downscale and re-encode before upload | **By memory.** Start at 2 | reject with a typed error; never queue |
| `decodePool` | Large JSON and CSV decode for exports and long lists | `min(hardwareConcurrency - 1, 2)` | reject; the caller falls back to a smaller page |
| the request queue | Every outbound request | Bounded in-flight per origin | shed with `TooManyRequests`, locally |

- **`imagePool`'s size is a memory cap, not a tuning knob.** A 12 MP photo decoded to RGBA is
  48 MB. Three in flight is 144 MB against a tab budget that is frequently around 350 MB on a
  mid-range phone — the tab does not slow down, it is killed. Two workers, one bitmap each,
  `close()` called the instant it is consumed.
- **The request queue is a pool for the same reason.** HTTP/2 will accept a hundred concurrent
  streams without complaint, which is how a list view fires a hundred requests and the one the
  user is waiting for arrives last.
- **Every task body catches.** An unhandled rejection in a worker or a handler leaves a promise
  nobody settles, and a UI that waits forever is worse than a UI that reports an error.
- **One leader per session owns the single refresh and the single stream.** `navigator.locks`
  elects it, `BroadcastChannel` fans the result out. anvil rotates the refresh token as a
  compare-and-swap, so N tabs refreshing concurrently is a rotation race whose loser is logged
  out — and a user with twelve tabs is also twelve SSE connections against a ceiling anvil
  derives from `RLIMIT_NOFILE`.

---

## 5. Security

**The client is not a security boundary.** Every check in this library exists to save a round
trip or to shape a surface. The server's check is the control. Nothing here may be described,
in code or in a comment, as preventing anything — a sentence claiming it does is how the
server-side check gets dropped as redundant two years later.

- **Never hold a credential.** anvil's tokens are `__Host-` cookies with `HttpOnly`, and
  hammer cannot read them: that is the property being bought, not a limitation being worked
  around. No token in `localStorage`, `sessionStorage`, IndexedDB, a module variable or a
  store. An XSS against a cookie it cannot read is bounded by the lifetime of the page it
  landed on; an XSS against a token in storage is a credential exfiltrated forever.
- **One function inserts markup, and it takes only `SanitizedHtml`.** `innerHTML`,
  `outerHTML`, `insertAdjacentHTML`, `document.write`, `eval`, `new Function` and the string
  forms of `setTimeout`/`setInterval` are banned outright and enforced by
  `tools/check-source-bans.sh`. Trusted Types are installed where the browser has them, with a
  default policy that throws.
- **URLs are built by the generated route builder**, never by concatenation. Every path
  segment goes through `encodeURIComponent`; every query value through `URLSearchParams`.
- **No credential, capability or secret id in a URL the application then keeps.** anvil hands
  a preview capability over in a query string exactly once and exchanges it for a path-scoped
  cookie with a `303`; the client's job is to follow that and never to store, log or re-share
  the first URL. A URL is in the address bar, the history, the `Referer` and every analytics
  payload ever built from `location.href`.
- **`crypto.getRandomValues` and `crypto.randomUUID` only.** `Math.random` is banned for ids,
  keys, nonces and for retry jitter — jitter from a predictable source is jitter that
  synchronises, which is the thundering herd it was added to prevent.
- **A 404 is not a permission error.** anvil answers a denied request on a stealth route with a
  byte-identical 404 so that a probe cannot distinguish a missing object from a forbidden one.
  A client that renders "you do not have permission" for a 404 hands back the oracle the server
  spent a whole design on removing.
- **Never render a server value as markup, and never echo a submitted value into an error.**
  Same reasons anvil gives for keeping it out of the response: reflected XSS, log injection,
  and an encoding hazard on non-Latin input.
- **No log line may contain a body, a cookie, a header, a form value or a URL with a query
  string.** Enforced by a redacting logger, not by discipline.
- **Zero runtime dependencies is a security control**, not minimalism. Every dependency is an
  npm account whose compromise ships a credential-stealing patch straight into the session.
  `tools/check-dependencies.sh` fails the build on a non-empty `dependencies`.
- **hammer sets no inline style and no inline script.** The application serves a CSP with no
  `unsafe-inline` (anvil `docs/19` §7), so a component that writes `style.cssText` from data,
  injects a `<style>` element or emits a `javascript:` URL is a component that silently does
  nothing in production and works in every test. A component that needs a style needs a class
  name the application supplies.
- `target="_blank"` always carries `rel="noopener noreferrer"`.
- Every `postMessage` names an explicit target origin, and every received message has its
  `origin` checked against the allow-list **before its data is read**.

---

## 6. Correctness on an unreliable network

- **A retry without an idempotency key is a duplicate write.** Every non-idempotent request
  hammer may retry carries a client-minted key, so at-least-once on the wire is at-most-once
  at the server. This is the same bargain anvil makes with its queues, in the other direction.
- **Every read-modify-write carries the version it read.** anvil's optimistic concurrency
  returns `VersionMismatch` on a lost update; the client's job is to reconcile — re-read,
  re-apply, re-present — and never to retry the same body in a loop, which converts a detected
  conflict into a silent overwrite with extra steps.
- **An optimistic update is reconcilable or it is absent.** Apply locally, confirm against the
  document the server returns, and never let an unconfirmed value become the input to another
  request.
- **Nothing scheduled in a tab is durable.** `setTimeout`, `setInterval`, a pending promise and
  an open stream all die when the tab is frozen, discarded or reloaded. Anything that must
  happen happens server-side; a client timer is a hint about the UI, never a guarantee about
  the work.
- **Every stream is at-least-once.** SSE resumes with `Last-Event-ID` and a reconnect replays;
  every handler is idempotent and dedupes on the event id.
- **Never compare a server timestamp with `Date.now()`.** The device clock is user-settable and
  is routinely minutes out. Expiry belongs to the server. A countdown is rendered from a
  *duration* the server sent, never from the difference between two clocks.
- **Retry policy belongs to the library, decided per error code and per method** — never to a
  call site. `Retry-After` is honoured exactly: a client that invents its own backoff against a
  server that named one retries straight back into the outage it was told to wait out.
- **A circuit breaker per origin.** After N consecutive transport failures it opens and every
  call fails fast until a probe succeeds. Twenty tabs retrying independently is a
  self-inflicted denial of service against a server that is already down.

---

## 7. Wire discipline

- **Cursor pagination only.** hammer has no way to express an offset, because `skip(n)` is
  O(n) server-side (anvil `ENGINEERING_RULES.md` §7) and no client convenience is worth putting it back.
- Every list request states a limit, bounded by the descriptor's maximum for that route.
- **Ask for the projection the route offers.** Fetching a 40 KB document to render one boolean
  costs transfer, parse and resident memory — on the device with the least of all three.
- **No N+1 over the network.** One batched call, never one request per row: a round trip is
  60–200 ms on mobile, and forty serialised is the entire page.
- Respect the server's `Cache-Control`. Never invent a longer freshness than was granted and
  never store a `private` response anywhere a second user can reach.
- **One request per (route, params) in flight.** A duplicate is deduplicated onto the first,
  not sent.
- **The envelope is decoded in exactly one place.** A call site that reaches into
  `json.error.code` itself is a call site that will miss the next code anvil appends.

---

## 8. Text, locale and direction

- **Bounds are in code points — never bytes, and never `String.length`.** `.length` counts
  UTF-16 code units: an emoji is two, every astral character is two, and a limit written
  against it silently halves the allowance for the scripts that need it most. anvil's limits
  are in code points, and the client's must be the identical number or the two disagree at the
  boundary — which a user experiences as a form that accepts what the server then refuses.
- Grapheme clusters are a **caret** concern, not a **limit** concern. `Intl.Segmenter` answers
  "how many characters did the user see"; code points answer "will the server take this".
- **Normalise to NFC before comparing, hashing or sending.** Two visually identical strings
  that differ by composition are unequal to `===` and to the server's index.
- **Digits shape for display and never on the wire.** An Arabic-Indic digit in a numeric field
  is a validation failure at the server, and a silent one in the browser.
- **User-authored text interpolated into a sentence is isolated** — `dir="auto"` on the element
  that holds it, and the isolate characters where an attribute cannot reach. Without it an RTL
  name reorders the Latin sentence around it, and the result is not a cosmetic defect: it
  changes which words the sentence appears to contain.
- Direction is a property of the document *and* of every element holding text. Logical CSS
  properties, `dir` where the text is, and no layout that assumes left.
- Every locale comes from the descriptor. **Locale order is persisted server-side and is
  append-only**; the client never renumbers it and never assumes index 0 means anything.

---

## 9. The rendered surface

**Components here hold logic and state, and that is the reason they are worth shipping.** A
notification bell is not a widget. It is an unread count reconciled against an at-least-once
stream and against the server's own count, a popover with focus management and a return path,
a mark-read that is idempotent because the stream is, and a live region that announces an
arrival without stealing focus from whatever the person was typing. Every application built on
anvil needs exactly that, and every one of them gets some part of it wrong the first time.

So the rule is not "no logic". It is one line, and it is the same line §1 draws everywhere
else:

> **A component may hold any amount of mechanism. It may hold none of the application's
> names, words or look.**

| What hammer's bell ships | What the application supplies |
|---|---|
| the unread count, reconciled against the stream and the server's count | the word for "notifications", in every locale |
| dedupe by event id, because the stream is at-least-once | which topics deserve a badge at all |
| the popover: focus, `Escape`, focus return, `aria-expanded`, a live region for arrivals | every class name, every icon, every colour |
| mark-read as one idempotent call carrying the version it read | what "read" means in this product |

The same split holds for the analytics chart, the login form, the upload control and the
inbox. What stays banned is narrow and absolute: **a topic name, a permission name, an event
name, a route, a sentence, a colour, a class name, a font, or a default that encodes a product
decision.**

- **Charts ship geometry and accessibility, not appearance.** Scales, ticks, paths, hit
  regions, keyboard traversal of the series, and a data-table fallback — because a chart that
  exists only as pixels is a chart part of the audience cannot read. Colours, number and date
  formatting and the labels come from the application. No charting dependency: a linear scale
  and a path string are arithmetic, and §5's zero-dependency rule does not have an exception
  for convenience.
- **A surface a minority of consumers use is its own entry point**, not a branch inside a
  shared one (§2.1). `hammer/chart` is separate for that reason: an application with no
  dashboard pays nothing for one.
- **Unstyled, structural, and complete**: semantics, ARIA, keyboard, focus order, direction.
  **An unlabelled control is a defect, not a polish item** — and one shipped from here is
  shipped to every consumer at once.
- Degrade to what the platform already does. A `<form>` posts, a `<details>` opens, a link
  navigates; a component replacing one of those inherits the responsibility for everything it
  replaced.
- No focus trap without an escape. No `aria-hidden` over anything focusable. No `tabindex`
  greater than 0.
- Animation honours `prefers-reduced-motion`, and every transition is interruptible.

**The largest piece of logic in this library renders nothing at all.** The credential
lifecycle — login, the single leader-owned refresh, the replay, the logout fan-out, the
permission gate over affordances — is mechanism that belongs here precisely because it is
where applications go wrong and where going wrong costs the session (§4, §5). It holds no
token while doing it, and cannot: anvil's credentials are `HttpOnly` cookies, and every screen
hammer ships around them is a screen driving a credential it is unable to read.

---

## 10. Style

- Explicit imports of what is used. No barrel imports inside the library.
- **Comments explain *why*, never *what*.** A comment restating the code is deleted. Where a
  mechanism exists because something failed, say so — that reasoning is the reason this code
  is worth reusing, and it is the first thing lost to a refactor that does not know it.
- Names carry the unit and the frame: `ttlSeconds`, not `ttl`; `expiresAtUtc`, not `expiry`;
  `maxBytes`, `widthCssPx`, `retryAfterMs`.
- A function that can fail returns `Result<T>`. Never `null` to mean an error, never `-1`,
  never a bare `false`.
- Time is a number of milliseconds inside the library and a `Date` only at the edges. A date
  held as a string is a timezone bug that has not happened yet.
- One concept per module, named for what it holds.

---

## 11. Git

The history is the only durable record of *why* the code is the way it is. A defect found in
six months is diagnosed through `git log` and `git blame`, not through the current diff. In a
library, that history is also the changelog every consuming application reads to decide
whether it can upgrade. Treat a commit as a permanent artefact, not as a save point.

### 11.1 What one commit is

- **One commit is one logical change.** If the subject line needs "and", it is two commits.
- **Every commit type-checks and passes the suite on its own.** `git bisect` is worth exactly
  as much as the worst commit in the history.
- Size: a diff a reviewer cannot hold in their head is two diffs. Roughly 400 changed lines is
  the point to start splitting; past ~1000, split it or explain in the body why it is
  genuinely atomic (a mechanical rename, a regenerated descriptor).
- **Mechanical changes travel alone.** A rename, a reformat, a file move, a regeneration gets
  its own commit with no behaviour change in it.
- Some things are *required* to be in the same commit, because splitting them lands a broken
  invariant on the mainline: a seam and its entry in `docs/01-seams.md`; a rule and the script
  in `tools/` that enforces it; an entry-point change and the `tests/testapp/` update that
  proves it is satisfiable; a generator change and the regenerated fixture.
- **Never commit** `node_modules`, `dist/`, a generated client outside `tests/testapp/`,
  editor state, `.env`, key material, or any production data. A secret that reaches a commit
  is a leaked secret the moment it is pushed — rotate it; the object stays in the history and
  on every clone.

### 11.2 The message

```
<subject: imperative, ≤ 50 chars, capitalised, no trailing period>
                            <- blank line, always
<body: why before what, wrapped at 72 columns>

<footers>
```

- **Imperative mood**, completing "Applied, this commit will …": `Add the inbox stream`, not
  `Added` or `Adds`.
- **Subject ≤ 50 characters**, hard ceiling 72.
- **No type prefixes.** No `feat:` / `fix:` / `chore:`.
- **The body explains why** — the problem, the approach, what was rejected and the reason.
- Wrap the body at **72 columns**.
- When a change exists for a security, performance or resilience reason, **say so in the body,
  with the number**. "Two image workers — a 12 MP decode is 48 MB of RGBA and three concurrent
  is a killed tab" is the sentence a future reader cannot reconstruct from the diff.
- Footers, last, one per line: `Refs: #123`, `Fixes: #123`, or for a regression,
  `Fixes: 58ae496 ("Add the inbox stream")`.
- Banned subjects: `wip`, `fix`, `fixes`, `update`, `cleanup`, `address review comments`,
  `misc`.
- **No attribution trailers of any kind — see §0.** Human co-authors may be credited with a
  normal `Co-Authored-By: Name <email>` trailer. No tool, assistant, generator or model is
  ever named in a message, footer, PR body or changelog.

### 11.3 History and branches

- **Committing straight to the mainline is allowed**, and the protection is §11.1 and §11.4 —
  every commit is one logical change that is green on its own.
- Branch when the work is genuinely provisional. Branch names are `area/short-description`.
- **Rebase onto the mainline; do not merge it into your branch.**
- **Rewrite only what has never been pushed.**
- `git push --force` is banned. `--force-with-lease`, on your own branch, after a rebase.
- Because hammer is a library, a change to anything reachable from an `exports` entry point is
  an API change and **must be called out in the PR title and body**, with the migration an
  application has to perform. Releases are tagged with semver; a breaking entry-point change is
  a major bump, never a patch.

### 11.4 Before the commit

- `npm run check` — type-check, suite, and every script in `tools/`.
- **Stage with `git add -p`, never `git add -A`.** Reading your own diff hunk by hunk is where
  the stray `console.log`, the commented-out block and the pasted token are caught.
- Read `git diff --cached` in full before committing.

### 11.5 When the commit happens

- **Finished work is committed without being asked.** When a task leaves the tree changed and
  the §11.4 gates pass, commit it. An uncommitted change is work that exists in exactly one
  place and dies with the machine.
- Split the task's work into the commits §11.1 describes before committing, not after.
- **Do not commit when the gates cannot run or do not pass.** A red type-check, a failing
  test, a red `tools/` script, a change the author flagged as exploratory — these stop at a
  dirty tree and a plain statement of what is left. Never commit to "save progress".
- **Pushing is not automatic.** Push when asked.
- Say what was committed and give the subject line.
