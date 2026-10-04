# hammer

The browser half of an [anvil](../anvil/) application. Strict TypeScript, **zero runtime
dependencies**, ESM only.

anvil is the reusable half of a production backend. hammer is the half that talks to it: the
transport, the credential lifecycle, the state and the unstyled components that every
application on top of anvil needs and nobody should write twice.

| | |
|---|---|
| **Credentials** | No token is ever held, stored or read. anvil's credentials are `__Host-` cookies with `HttpOnly`, and hammer works by *not having an implementation* — one leader tab owns the single refresh, so N tabs cannot lose a rotating refresh token to their own race |
| **Errors** | One decode site for anvil's envelope, a literal-typed code union that degrades on a code the server added this morning, and a policy per code that says what the client does — including the rule that a `404` is never rendered as a permission error |
| **Retries** | Per code and per method, never per call site. `Retry-After` honoured exactly, jitter from a CSPRNG, a client-minted idempotency key on anything retryable, and a per-origin circuit breaker so twenty tabs are not a denial of service against a server that is already down |
| **Wire** | Cursor pagination with no way to express an offset, one request per `(route, params)` in flight, a bounded request queue that sheds rather than growing, and URLs built by a generated route builder rather than by concatenation |
| **Seams** | Every application table — routes, permissions, locales, field types, sections, topics, events, limits — is **generated** from a descriptor the server's own build emits, so the client cannot drift into a wrong authority check |
| **What ships to whom** | A route's *shape* is a type and is erased at build; a *public* path is compiled in; a **privileged path is never emitted at all** and arrives with the session, scoped by the server to the permissions the holder actually has. A lazily-loaded chunk is a public URL, so code-splitting was never access control |
| **Permissions** | The same 128-bit set anvil has, as 16 bytes and one AND, 24 base64url characters on the wire instead of a list of names — and a local check that governs affordances and never refuses a call |
| **Text** | Bounds in code points, never `String.length`; NFC before comparing or sending; bidi isolation for interpolated user text; digit shaping for display and ASCII on the wire |
| **Memory** | Binary never enters the JS heap — images are `<img>`, uploads stream from the `File` — caches are bounded, evicting and keyed by identity, and the image worker pool's size is a memory cap rather than a tuning knob |
| **Streams** | One SSE connection per session across every tab, resumed by `Last-Event-ID`, deduplicated by event id, with a bounded ring |
| **Components** | Stateful and complete — the login flow, the session and permission gates, the notification bell with its count reconciled against an at-least-once stream, the form and section renderers, the upload control, the inbox, the pager, the consent gate, and charts that ship scales, keyboard traversal and a data-table fallback. Unstyled, ARIA-complete, RTL-correct, and carrying no name, no word and no colour of the application's |
| **Markup** | Exactly one function inserts markup and it takes only a `SanitizedHtml` whose constructor is module-private — anvil's `append_sanitized` trick, in the type system rather than in a review comment |

Every mechanism above exists in a specific shape for a specific reason, and [`docs/`](docs/)
says what that reason is. Read the reason before changing the mechanism.

---

## What hammer refuses to hold

Not one route path, permission name, section key, field type, locale, topic, event name,
rate-limit constant, class name, colour — **or user-visible string, in any language**. anvil's
error responses carry a code and never a message for the same reason: the words belong to
whoever knows the audience and the locale.

Everything of that shape arrives through the generated descriptor module
([`docs/01-seams.md`](docs/01-seams.md)) and nowhere else.

## Prerequisites

| | Minimum | Notes |
|---|---|---|
| An anvil application | current | Its tables are the source of every seam |
| Node | 22 | Build and test only; hammer ships nothing that needs it at run time |
| A Chromium | any recent | Build and test only. `npm run test` runs the DOM and React suite in it, and fails rather than skips with none found (`HAMMER_BROWSER`, else the system binary) |
| A bundler | any | Must honour `exports` and `"sideEffects": false` |

[`CHANGELOG.md`](CHANGELOG.md) is what to read before upgrading: every entry-point change
names the migration, and the **Requires** entries are the ones a deployment has to satisfy
rather than the code.

The application and the API must be served from the **same origin**. A cross-origin API pays a
CORS preflight on every mutating request; a cross-**site** one does not work at all, because
`SameSite=Lax` cookies are not sent there — and hammer refuses to be constructed against one
rather than failing later and confusingly.

## Build

```sh
npm install
npm run check        # type-check, suite, and every script in tools/
```

| Script | Purpose |
|---|---|
| `npm run typecheck` | `tsc --noEmit` |
| `npm run test` | The unit, contract, DOM and React suites. The DOM and React suite runs inside a real Chromium (`tests/dom/in_browser.test.ts`) and fails rather than skips with no browser found |
| `npm run test:browser` | The Trusted Types, credential and resilience browser suite, against the same Chromium. Excluded from `npm run check`, and it fails rather than skips with no browser |
| `npm run test:live` | The live suite, against a server you started. Excluded from `npm run check`, and it fails rather than skips with no `HAMMER_LIVE_ORIGIN` |
| `npm run lint` | Every check in `tools/`: the source bans, wire discipline, vocabulary, layering, dependencies, the generated client's staleness, **every TypeScript block in the docs against the source it names**, the per-entry-point byte ceilings, what the package publishes, and the published API surface |
| `npm run check` | All of the above. This is the gate before a commit |
| `npm run check:production` | `check`, plus the dependency policy with every warning as an error and `npm audit` over the whole tree. This is the gate before a release or a deploy, and it fails closed when the registry cannot be reached (`CLAUDE.md` §12) |

## Using hammer in an application

```sh
# In your anvil application's build:
./build/tools/emit-descriptor > web/hammer.descriptor.json

# In your web application:
npx hammer codegen --descriptor hammer.descriptor.json --out src/api
```

The generated client is committed and diffed in CI, because a client generated from last
month's server is the one failure the whole mechanism exists to prevent.

[`docs/02-getting-started.md`](docs/02-getting-started.md) walks through a minimal application
end to end. [`tests/testapp/`](tests/testapp/) is that application; it is type-checked on every
build, and every TypeScript block in the guide is lifted out of it by
[`tools/check-docs.sh`](tools/check-docs.sh), which diffs them on every `npm run lint`. That
sentence used to stop at "type-checked", and the guide's examples were prose beside compiled
source rather than compiled source — which is how four of them came to describe an API this
library has never had.

[`docs/03-deployment.md`](docs/03-deployment.md) is what to get right when serving it: the
entry point whose existence is sensitive and therefore belongs behind `auth_request`, the
source map that re-leaks everything tree-shaking removed, the CSP the DOM suite is written
against, and the cache headers that decide what an open tab does across a deploy.

## Layout

```
src/
  core/      types, errors, Result, brands, PermSet, text, locale, bidi, validators, cursors
             hammer            (no DOM, no fetch)
  wire/      session view, resolution, affordances, client, routes, envelope, retry,
             idempotency, credentials, leader, sse, upload
             hammer/wire       (+ fetch, EventSource, BroadcastChannel, navigator.locks)
  state/     stores, bounded cache, resources, session, forms, sections, inbox, media,
             analytics, worker pools
             hammer/state
  dom/       SanitizedHtml and the single insertion site; the login flow, the gates, the
             bell, the inbox, the form and section renderers, the upload control, the
             pager, the error surface, the consent gate
             hammer/dom        (+ a document)
  chart/     scales, ticks, marks, keyboard traversal, a data-table fallback
             hammer/chart      (its own entry point: an app with no dashboard pays nothing)
  react/     hooks binding the stores to a lifecycle: useStore, useResource,
             useMutation, useSession, useInbox, useForm — and no behaviour of its own
             hammer/react      (+ react, as an optional peer)
  codegen/   the descriptor reader and the TypeScript emitter
             hammer/codegen    (build time only — never in a browser bundle)
tests/       core, wire, state, dom, react, contract, browser and live suites, and testapp/
docs/        the reasoning
tools/       the checks that turn the rules into build failures
```

Dependencies point downward only. In a language with no linker that is not automatic, so
`tools/check-layering.sh` parses every import specifier and makes a violation a build failure
rather than a review comment.

## Conventions

[`CLAUDE.md`](CLAUDE.md) is the engineering contract. Nine scripts run as part of
`npm run check` and fail the build rather than a review:

- `tools/check-source-bans.sh` — `innerHTML`, `eval`, `new Function`, `document.write`,
  `Math.random`, credential storage, `any`, `@ts-ignore`, non-null assertions
- `tools/check-wire-discipline.sh` — no `fetch` outside `src/wire/`, no URL built by
  concatenation, no offset pagination, no credential in a query string
- `tools/check-layering.sh` — the layer graph above, enforced per import
- `tools/check-dependencies.sh` — zero runtime dependencies, which is a security control
  rather than minimalism
- `tools/check-vocabulary.sh` — no application's feature names in `src/`
- `tools/check-descriptor.sh` — the committed client is what the descriptor generates, and no
  descriptor reached the build output
- `tools/check-docs.sh` — every TypeScript block in `docs/` and this file is a region of
  compiled source, diffed against it. A block naming no source fails; `--write` rewrites them
- `tools/check-bundle-budget.sh` — a declared gzipped ceiling per entry point, in
  `tools/bundle-budget.json`, raised only in a commit that says what bought the bytes
- `tools/check-build-output.sh` — builds, then asserts the package publishes no source map,
  resolves every `exports` target, and keeps the CLI's shebang
- `tools/check-public-surface.sh` — the published surface of every entry point, committed and
  diffed. A change to it is an API change and needs a `CHANGELOG.md` entry naming the migration
