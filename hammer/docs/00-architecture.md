# 00 — Architecture

## 1. Component map

```
                 One session, N tabs
  ┌──────────────────────────────────────────────────────┐
  │  application code                                    │
  │ ┌──────────────────────────────────────────────────┐ │
  │ │ hammer/react    adapter: hooks over the stores   │ │
  │ │ hammer/dom      unstyled components, with state  │ │
  │ │ hammer/chart    scales, marks, a data-table fallback│ │
  │ │ hammer/state    stores, bounded cache, streams   │ │
  │ │ hammer/wire     client, credentials, retry, SSE  │ │
  │ │ hammer          core: types, errors, text, bits  │ │
  │ └──────────────────────────────────────────────────┘ │
  └────────┬─────────────────────┬───────────────────────┘
           │                     │
    navigator.locks        BroadcastChannel
    (one leader tab)       (identity, invalidation, logout)
           │                     │
  ┌────────▼─────────────────────▼───────────────────────┐
  │ SITE_ORIGIN      fetch + __Host- cookies             │──► Nginx ──► Drogon
  │ CONTENT_ORIGIN   assembled HTML — navigate or frame  │        (anvil)
  │ MEDIA_ORIGIN     <img>, srcset, never credentials    │
  └──────────────────────────────────────────────────────┘
```

**Three origins, three postures, and the difference is the security design** — anvil's, which
hammer inherits rather than invents ([anvil `docs/19` §6](../../anvil/docs/19-server-side-rendering.md)).

| Origin | What lives there | Credentials | What hammer does |
|---|---|---|---|
| `SITE_ORIGIN` | the API and the application | `__Host-` session cookies, sent automatically | every `fetch` in the library |
| `CONTENT_ORIGIN` | HTML assembled from partially-trusted content | a path-scoped preview cookie, set by a `303` | **navigates to it or frames it. Never fetches it** |
| `MEDIA_ORIGIN` | image and file bytes | none, ever | emits `<img>`/`srcset` and nothing else |

**Content-origin HTML is never rendered inside the site origin's document.** Fetching that
HTML and inserting it into the application's DOM undoes the entire origin split in one line:
the split exists so that an XSS in staff-authored content executes somewhere that holds no
session cookie, and moving the markup into the site's document moves the XSS with it. It is
an `<iframe src>` pointed at the content origin, or a top-level navigation. This is the one
rule in this document that a well-meaning performance change is most likely to break.

## 2. Layering

Dependencies point downward only, and in a language with no linker that is not free. Every
layer is a published entry point, and `tools/check-layering.sh` parses every import
specifier and fails the build on an edge that points the wrong way — the closest thing
available to anvil's link error, and for the same reason: a boundary enforced in review is a
boundary that holds until the week someone is busy.

| Entry point | Contains | May import | Needs |
|---|---|---|---|
| `hammer` | `Result`, the error model, brands, `PermSet`, `Uuid`, text and code-point bounds, locale, bidi, digits, validators, cursors | nothing | **no DOM, no `fetch`** |
| `hammer/wire` | the session view, route resolution, the affordance gate, the client, the route builder, envelope decode, retry, idempotency, the credential lifecycle, the leader, SSE, upload | `hammer` | `fetch`, `EventSource`, `BroadcastChannel`, `navigator.locks` |
| `hammer/state` | stores, the bounded cache, resources, sessions, forms, sections, inbox, media, analytics, consent | `hammer`, `hammer/wire` | — |
| `hammer/dom` | `SanitizedHtml`, the insertion site, and the stateful components: the login flow, the session and permission gates, the notification bell and inbox, the form and section renderers, the upload control, the pager, the error surface, the consent gate | `hammer`, `hammer/state` | a `document` |
| `hammer/chart` | scales, ticks, marks, hit regions, keyboard traversal, and the data-table fallback. No colours, no formatting, no charting dependency | `hammer`, `hammer/state` | a `document` |
| `hammer/react` | hooks binding the stores to a framework's lifecycle | all of the above | `react` as a peer |
| `hammer/codegen` | the descriptor reader and the TypeScript emitter | `hammer` | **node only — never in a browser bundle** |

**`hammer/dom` and `hammer/chart` need a document and may not reach the global
one.** It arrives through the element a component was asked to mount in, which is
§3.3's injection rule applied to the one singleton this layer cannot avoid
needing — a test supplies its own instead of racing every other test in the file,
and the same component renders into a document that is not the tab's (a preview,
a print view, a frame) with no branch for it. `tools/check-layering.sh` fails the
build on a bare `document.` or `window.` in either layer.

`hammer` is separate for the same reason `anvil::foundation` is: it is fast to check, trivially
testable with no DOM and no network, and keeping a validator unable to reach the client is
worth a boundary.

**`hammer/codegen` runs at build time and must never be reachable from a browser entry point.**
It reads a file from disk and writes TypeScript; a bundle that contains it contains a path
walker and a code emitter that the application ships to every device for no reason.

## 3. Worker pools and the request queue

A single shared worker couples every workload: one twelve-megapixel decode occupies the only
thread and the export parse that renders a table waits behind it.

| Pool | Workload | Sizing rule | Queue full → |
|---|---|---|---|
| `imagePool` | downscale and re-encode before upload | **memory budget**, start at 2 | reject with `PoolError { cause: "queue-full" }`; never queue |
| `decodePool` | large JSON/CSV decode for exports and long lists | `min(hardwareConcurrency - 1, 2)` | queue to a bound, then reject; the caller retries with a smaller page |
| the request queue | every outbound request | bounded in-flight per origin (start at 6) | shed locally with `TooManyRequests` |

**`imagePool`'s size is a memory cap, not a tuning knob.** A 12 MP photo decoded to RGBA is
`4032 × 3024 × 4 ≈ 48 MB`. Three concurrently is 144 MB against a tab budget frequently around
350 MB on a mid-range phone, and the failure mode is not slowness — the tab is killed, with
whatever the user had typed in it. Two workers, one `ImageBitmap` each, `close()` on the
instant it is consumed.

**The request queue is a pool for the same reason the others are.** HTTP/2 accepts a hundred
concurrent streams without complaint, which is how a list view fires a hundred requests and the
one the user is waiting for arrives last. Bounded in-flight, FIFO, and a full queue sheds
locally rather than growing — an unbounded client queue turns a slow network into an
unbounded memory growth curve, which is the browser's version of the OOM kill anvil's bounded
queues exist to prevent.

**The worker itself is injected, and that is not a testing convenience.**
`new Worker(new URL("./x.js", import.meta.url))` is a bundler contract, and a library with a
zero-dependency rule has no bundler and takes no dependency on one. The application hands over
a factory and writes a two-line worker entry against `serveImagePool` / `serveDecodePool` —
functions rather than modules with top-level listeners, because a module that registered a
handler on import is a module a bundler cannot drop and a promise `"sideEffects": false` stops
keeping (`ENGINEERING_RULES.md` §2.1).

**Two rules, both crash-or-hang class:**

- Every task body catches. An unhandled rejection in a worker leaves a promise nobody settles,
  and a spinner that never stops is worse than an error message.
- Nothing posted to a pool captures a DOM node. The screen that asked may be gone; the result
  goes back through the store that owns it.

## 4. Request lifecycle

Every request runs the same ordered pipeline, and **the order is the point**: each stage is
cheaper than the next, so a call that cannot succeed is abandoned before it costs a round trip.

```
 1. Route resolution                compiled table for a public route; the
                                    session's holder-scoped table otherwise (§4.2)
 2. Permission pre-check            one AND over 16 bytes — affordances only (§4.1)
 3. Schema validation               code-point bounds, from the descriptor
 4. Local rate-limit budget         token bucket per declared bucket — advisory
 5. Capability requirement          satisfied at the TYPE level; cannot be forgotten
 6. Cache read / in-flight dedupe   a hit sends nothing at all
 7. Idempotency key                 minted for anything retryable and not idempotent
 8. Queue admission                 bounded; a full queue sheds here
 9. fetch(credentials: "include", signal)
10. Envelope decode                 exactly one place; a typed value or a typed error
11. Retry decision                  per code and per method; Retry-After honoured exactly
```

Stages 1–8 complete with **zero network**. That is the client's version of anvil's thesis: a
request the descriptor can already prove will fail never leaves the device, and the cheapest
round trip is the one that is not made.

Stage 5 is the one worth stating twice. anvil's destructive routes require a capability token
minted by an earlier request and consumed exactly once; hammer models the requirement in the
*type* of the call, so a caller cannot reach the route without holding a `Capability<Scope>`
that only the minting call can produce. A rule the compiler checks is a rule that survives the
refactor that was not thinking about it.

### 4.1 What the permission pre-check is for, and what it must never do

The session payload carries the 128-bit permission set, so the client can answer "may this
user do X" with one AND and no round trip. That answer governs **affordances** — whether a
button is rendered, whether a menu entry exists.

It must never **refuse a call the user has managed to make**. The local set is a copy, and
anvil's `perm_epoch` exists precisely because a copy goes stale: a grant made thirty seconds
ago is not in it. A client that refuses locally turns a stale copy into a permanent denial
that no server-side change can clear, and the user's bug report is "the button does nothing".

So: **hide it locally, but if it is invoked, send it and let the server decide.** A `403` in
return means the copy is stale, and is the signal to refetch the session — which is also what
makes the stale copy self-healing rather than sticky.

The rule is kept by the error type rather than by care. There is no cause anywhere in
`HammerError` meaning "denied here", so a local permission refusal cannot be constructed,
returned or reported by this library at all.

**Where the affordance is a route, the bits are the wrong question.** The session's route table
is the server's own answer — anvil builds it with `satisfies()`, the same function its request
filter calls, superadmin short-circuit included — so "is this route in my table" cannot
disagree with the filter, and counting bits can: a superadmin's set is deliberately not
all-ones, so a bitset check would hide every guarded control from the one account that reaches
everything. The bits answer the affordances that are *not* routes — a column of personal data, a
bulk action — and for those the payload carries the superadmin flag alongside them
([`01-seams.md`](01-seams.md) §4.1.1).

### 4.2 A route whose path the client does not have

A privileged route's path is **not compiled into the bundle**. It arrives with the session,
filtered by the server to what the holder's permissions reach, for the reasons in
[`01-seams.md`](01-seams.md) §4.1 — the short version being that a lazily-loaded chunk is a
public URL, so code-splitting was never access control.

A route id with no entry is therefore a **missing address**, not a refusal, and it is the one
place §4.1's "send it and let the server decide" cannot apply. The recovery is the same
mechanism: refetch the session, retry once, and if the entry is still absent report the
not-found shape — **never a permission-flavoured error**, which would rebuild in the client
the oracle the server removed.

## 5. The credential lifecycle

hammer never sees a token. anvil's credentials are `__Host-` cookies with `HttpOnly`, and
every property that makes them worth using — host-only scope, unreadable by script, cleared by
the server — is a property hammer gets by **not having an implementation**.

```
  request ──► 401 ──► is this tab the leader?
                         │                    │
                        yes                   no
                         │                    │
              navigator.locks("hammer.refresh")│
                         │              await the broadcast
              POST /auth/refresh  (once)       │
                    │        │                 │
                   200      fail               │
                    │        │                 │
     broadcast "identity" ◄──┴─► broadcast "logout"
                    │                          │
        replay the in-flight requests ◄────────┘ drop caches, close streams
```

- **One refresh, ever, per expiry.** anvil rotates the refresh token as a compare-and-swap, so
  two tabs refreshing concurrently is a rotation race whose loser is logged out — with a valid
  session, in a tab the user was using. The lock is not an optimisation.
- **A follower never refreshes.** It waits for the leader's broadcast and replays.
- **A replay is a retry, and obeys §6 of `ENGINEERING_RULES.md`.** A `POST` replayed after a refresh is
  exactly the case the idempotency key exists for; a replay without one is a double charge.
- **Replays are capped and are not retried a second time.** A second `401` after a successful
  refresh is not a race, it is a rejection: clear identity, broadcast logout, stop.
- **Logout is a fan-out.** Every tab drops its caches, closes its streams and releases the
  lock. A tab left rendering a signed-in shell after another tab signed out is one shared
  device away from being a disclosure.

### 5.1 What "token handling" means in a library that holds no token

This is the largest piece of logic in hammer and it renders nothing: login, refresh, replay,
logout, the permission gate, the capability flow, and the session payload that carries the
permission bitset and the holder's route table (§4.2). It is here because it is where
applications go wrong, and because going wrong costs the session rather than a screen.

It handles the credential **without ever holding it**. There is no token in a variable, a
store, a header hammer sets, or storage of any kind. What hammer owns is the *orchestration*:
who refreshes, when, exactly once, what replays afterwards, and what every other tab does
about it.

**A bearer token is a different credential model and is deliberately not supported.** It needs
somewhere to live, and in a browser every such place is readable by script — which converts an
XSS bounded by one page's lifetime into a credential exfiltrated permanently. The trigger that
would reopen it is a client that genuinely cannot use cookies (a native application, or a
cross-site deployment that has accepted §8.4's costs), and it would arrive as a second
credential mode with its own threat model written down, never as a fallback inside this one.

## 6. Error model, and what the client does with each code

anvil answers every failure with the same envelope and no message:

```json
{ "error": { "code": "VALIDATION_FAILED", "request_id": "01J…", "fields": { "email": "BAD_FORMAT" } } }
```

It is decoded in exactly one place, into a discriminated union whose `code` member comes from
the descriptor. `ErrorCode` is **append-only** server-side, so the generated union carries the
bound as well as the members and an unknown code decodes to `Unknown` rather than throwing —
a client that crashes on a code the server added is a client that turns a deploy into an
outage.

**The members are anvil's wire names, unchanged.** An earlier draft of this table spelled them
in PascalCase, which cost a fifteen-entry translation table in every bundle that decodes an
envelope, a second spelling of an enum that is append-only on the other side, and — the part
that is not about bytes — a word on a screen that does not match the word in the log somebody
is reading it against. `Unknown` is hammer's own member and is PascalCase precisely so that no
enumerator anvil appends can collide with it. The success member, `OK`, is not in the union at
all: an application would otherwise owe a sentence for a case no error surface can reach.

The HTTP column is `http_status()` in anvil's `include/anvil/http/errors.h`, which that header
calls the single mapping in the codebase — "a second one drifts, and drift in this table is how
a 403 leaks from an admin route that was supposed to stealth-404". Two rows here were wrong
until it was read: `CAPABILITY_REQUIRED` is a 428 rather than a 403, and `VALIDATION_FAILED` is
a 400 rather than a 422. The same header calls `wire_name()` stable API — "clients map these to
their own bilingual messages, so renaming one is a breaking API change" — which is the other
half of why nothing is translated on the way in.

| Code | HTTP | hammer's behaviour | What the application must not do |
|---|---|---|---|
| `UNAUTHENTICATED` | 401 | one leader-owned refresh, then replay once (§5) | treat it as a login form trigger before the refresh has failed |
| `FORBIDDEN` | 403 | never retried; refetch the session, because the local permission copy is stale (§4.1) | render it where a stealth route would have answered 404 |
| `NOT_FOUND` | 404 | never retried | **say "no permission" — that is the oracle anvil removed** |
| `CAPABILITY_REQUIRED` | 428 | surfaced to the flow that must mint one; never minted implicitly | mint-and-retry automatically; a capability exists to be a second, deliberate act |
| `CAPABILITY_INVALID` | 403 | never retried — it is spent or expired | re-send the same capability |
| `VALIDATION_FAILED` | 400 | maps `fields` onto the form's field ids | render the reason enum verbatim; the copy is the application's (§1) |
| `CONFLICT` | 409 | never retried | resolve it silently |
| `VERSION_MISMATCH` | 409 | surfaced as a reconciliation with the current document | retry the same body — that is a lost update with extra steps |
| `RATE_LIMITED` | 429 | honours `Retry-After` **exactly**, and holds back the whole bucket | invent a backoff; the server named one |
| `PAYLOAD_TOO_LARGE` | 413 | never retried | reach it at all — the cap is in the descriptor and is checked before the first byte |
| `UNSUPPORTED_MEDIA` | 415 | never retried | guess a second format |
| `SERVICE_UNAVAILABLE` | 503 | retried with `Retry-After`; opens the circuit breaker after N | retry per call site |
| `INSUFFICIENT_STORAGE` | 507 | never retried | — |
| `INTERNAL` | 500 | retried at most twice, **idempotent methods only** | show the `request_id` and nothing else — there is no detail, by design |

`request_id` is surfaced because it is the only thing a user can report that a server-side log
can be found by. It is displayed; it is never interpreted. **It is also nullable, and the null
is the common case rather than a defensive one**: anvil's 404 body is a single constexpr
string, `{"error":{"code":"NOT_FOUND"}}`, shared byte for byte by a stealth drop, an unmatched
route and a genuinely missing object — and Nginx's `error_page` serves those same bytes with no
request behind them to have an id. A surface that assumed a string renders the word `undefined`
on the most common failure there is. It is validated on the way in, too: it reaches a screen and
a log line, so a control character in it is log injection in the one field that is meant to be
safe to echo.

**A failure may never have reached anvil at all**, and the decode has to answer for that as
well. A proxy's 502 is an HTML page; a gateway timeout may be empty. There is no envelope in
either, so both decode to `Unknown` — and the decoded error carries the HTTP **status**
alongside the code for exactly this reason: without it a 502 from the proxy and a 200 whose body
did not parse are one indistinguishable failure with two different right answers. The code is
what policy branches on; the status is what says what happened when there is not one.

## 7. State, cache and multi-tab coherence

A `Resource<T>` is keyed by `(route id, canonical params, user id)`.

- **Freshness comes from the server.** `Cache-Control` decides; hammer revalidates while
  serving stale where the server permitted it, and invents no freshness of its own.
- **Bounded and evicting.** Every resource class has an entry ceiling and evicts
  least-recently-used. A tab stays open for days: an unbounded cache is a leak with a slow fuse.
- **Keyed by identity, and dropped on identity change.** A cache that survives a logout is one
  user's data rendered to the next on a shared device.
- **Nothing is persisted.** No IndexedDB, no `localStorage`, no service-worker cache of an API
  response by default. A persisted response outlives the cookie that authorised it, which
  means private data readable after the session is gone — the offline case is a per-resource
  opt-in that names its own eviction, not a default.
- **A mutation declares what it invalidates**, and the invalidation is broadcast: tab B must
  not keep rendering the row tab A just changed. The mechanism is hammer's; the mapping is the
  application's.
- **The session payload is memory-only and dies with the tab.** It carries the permission
  bitset and the holder's route table; both are scoped to one person and neither is written
  anywhere a second person could read.

### 7.1 The bundle can be older than the API

A tab open across a deploy is running last week's client against this week's server. The
session response carries the descriptor hash the server was built from; a mismatch sets a
`staleClient` flag on the session store.

**hammer never reloads the page by itself.** An automatic reload discards whatever the user
had typed, on the deploy that is most likely to be happening during working hours. The flag is
surfaced and the application decides — a banner, a reload at the next navigation, or nothing.

## 8. Deployment invariants

Assumptions the code may rely on. Each is enforced by deployment and asserted where it can be.
What the person holding the Nginx configuration has to do so that they are true is
[`03-deployment.md`](03-deployment.md).

1. **The tab may be frozen or discarded at any instant.** Nothing that matters lives only in a
   tab. Client timers are hints; anything that must happen is server-side.
2. **There may be N tabs on one session.** One leader owns the refresh and the stream; every
   other coordination is a broadcast.
3. **The device clock is wrong.** Expiry is the server's. A countdown is rendered from a
   duration, never from a difference of two clocks.
4. **The API is same-origin with the application.** Not merely same-*site*: same origin. A
   cross-origin API costs a CORS preflight on every mutating request — one extra round trip,
   60–200 ms on mobile, on exactly the requests a user is waiting for — and it puts the
   session's survival at the mercy of a third-party-cookie policy that changes without notice.
   A **cross-site** deployment is refused outright at client construction: `SameSite=Lax`
   cookies are not sent on a cross-site subresource request, so the session is already gone
   and the only question is how confusingly.
5. **The CSP carries no `unsafe-inline` and no `unsafe-eval`.** hammer sets no inline style and
   no inline script (`ENGINEERING_RULES.md` §5), and its DOM suite runs under a policy that would catch it
   if it did.
6. **`CONTENT_ORIGIN` is a different registrable host, and its HTML is never inlined into the
   site's document** (§1).
7. **`MEDIA_ORIGIN` never receives credentials** and never issues a state change. anvil keeps
   it out of the origin allow-list for that reason; a `fetch` from hammer to it with
   `credentials: "include"` would be a client-side attempt to undo a server-side control.

## 9. Observability

One structured record per request: `requestId` (the server's), `routeId`, `status`,
`durationMs`, `queueWaitMs`, `retries`, and the transport error class. **Never a URL with a
query string, never a body, never a header** — the same redaction rule as the server, applied
where the data is most casually available.

Counted, not logged per event, and for the same reason anvil gives: **a log line per occurrence
is not a metric.** Under the load that makes it fire, a per-event line is itself the problem.
The counters that matter here are the ones that are invisible from outside:

```
hammer_refresh_races_total        hammer_requests_shed_total
hammer_replays_total              hammer_circuit_open_seconds
hammer_stale_client_detected      hammer_image_worker_rejections_total
hammer_sse_reconnects_total       hammer_optimistic_rollbacks_total
```

`hammer_refresh_races_total` is the highest-signal one and should be zero: a non-zero value
means the leader election is not holding and users are being logged out by their own second
tab. `hammer_optimistic_rollbacks_total` is the second: it is the count of times the interface
told the user something had happened that had not.

They are reported through the application's own analytics seam — hammer declares no metric
names of its own in an application's namespace, and ships no reporter.
