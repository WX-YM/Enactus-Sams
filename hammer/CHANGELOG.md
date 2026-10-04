# Changelog

hammer is a library, so this file is what a consuming application reads to decide whether it
can upgrade. Two rules follow from that, and they are the whole format:

- **Every change to anything reachable from an `exports` entry point names the migration** an
  application has to perform. A line saying what changed and not what to do about it is a line
  that costs every consumer the same afternoon.
- **Semver, and a breaking entry-point change is a major bump, never a patch.** The published
  surface is committed in [`tools/public-surface.txt`](tools/public-surface.txt) and diffed by
  `npm run lint`, so a change to it cannot land unnoticed — but deciding whether it is breaking
  is still a person's job.

Entries that require something of the **deployment** rather than of the code are marked
**Requires**. They are the ones that pass every test in a consuming repository and fail in
production; [`docs/03-deployment.md`](docs/03-deployment.md) is the long form.

---

## Unreleased

### Added — `Sha256` and `sha256` in `hammer/crypto`, not breaking

**An incremental SHA-256**, for hashing what cannot be held whole. WebCrypto's `digest` takes its
input at once, and encrypted chat media is hashed chunk by chunk as it is encrypted, because the
server compares the hash the client declares with the hash of what arrived
(`docs/05-chat.md` §9.11). Anywhere the whole input is already in memory, WebCrypto's `digest`
remains the call to make.

No migration. The entry point's ceiling rises from 4 KB to 5 KB gzipped; an application that
imports only `argon2` tree-shakes this away, and the prehash bundles did not change by a byte.

### Added — `hammer/edit`, a new entry point, not breaking

**Crop, rotate, flip, resize and freehand drawing on a stored image.** `renderImageEditor` draws
one SVG over a role of the source and produces a canonical recipe; `encodeRecipe` validates it
against the generated `kEditLimits` with the server's own fault names; `submitEdit` sends it to
the application's edit route and `reopenEdit` reads an edit back as its source and its recipe.
No edited pixel is ever produced in the tab: anvil renders the recipe into a new object
(`docs/04-image-edits.md`).

Pass `aspect: { num: 16, den: 9 }` when the result is going into a place of fixed shape, such
as a section's image slot: the crop then opens on the largest centred box of that shape and keeps
it through every drag, key and rotation, so the picture is not refused there for a ratio a hand
could not hit.

*Requires* an anvil with the edit routes (anvil `docs/21-image-edits.md`), two route
declarations and one unique index in the application (anvil `docs/01-seams.md` §17), and a
regenerated client: `kEditLimits` is emitted only from a descriptor that carries `limits.edit`,
so an editor mounted against an older server fails to type-check. The canvas class needs
`touch-action: none` in the application's stylesheet, or touch scrolls the page under a stroke.

### Added — `hammer/dom`, not breaking

**`renderAccountForm`, and words for a refusal made before anything was sent.** The discipline
`renderLogin` and `renderSignup` share — one submit at a time, aborted on close, every secret and
code dropped once the call has succeeded — is now available to every other account screen:
verifying an address, a reset, a password change. Its `send` is `accounts.submit(flow, …)`. A
failure that never reached a server is worded by `kind.cause` first and then by `kind`, so
`"account.secret-too-short"` and `"prehash.out-of-memory"` can each have a sentence of their own.

*Requires* nothing. An `errors` table keyed by `kind` alone reads exactly as before.

### Added — `hammer/accounts`, a new entry point, and `AccountError` in `hammer`, not breaking

**The client of anvil's built-in account flows.** `Accounts` drives registration, contact
verification, sign-in, password reset and change, and sign-out from the generated `kAccounts`
table; `accountFields(table, flow)` gives each screen its fields, keyed, bounded and with their
platform purpose; `accounts.submit(flow, form.body(), signal)` sends one. Under client hashing —
anvil's default — the password never leaves the tab, and which salt each credential is derived
under is fixed here rather than left to an application (`docs/01-seams.md` §23).

*Requires* an anvil server with built-in accounts; `kAccounts` is `null` otherwise, and nothing
here is used.

- Replace a hand-written sign-in and registration with `new Accounts({ table: kAccounts, call:
  accountCall(api), prehash: { pool, bounds } })` and its methods.
- Add words for `"account"` failures (`secret-too-short`, `secret-too-long`) to your error copy.

### Fixed — `hammer/state` and `hammer/dom`, not breaking

**A password field is drawn as one.** `renderForm` drew every text-like field as
`type="text"`, so the login form's password was on the screen of everybody standing behind the
person typing it, and with no `autocomplete` token no password manager could fill or save it.
`FieldDefinition` gains an optional `purpose` — `current-password`, `new-password`, `email`,
`username`, `tel`, `one-time-code`, `given-name`, `family-name` — which `renderForm` maps to the
platform's own input type and standard `autocomplete` token, masking a secret and turning off
correction and capitalisation where they would change what was typed.

*Requires* nothing; a field with no `purpose` is drawn exactly as before.

- Set `purpose: "current-password"` on a sign-in form's password field, `"new-password"` on a
  registration's or a reset's, and the identifier purposes where they apply.

### Added — the generated module, not breaking

**`kAccounts`, from anvil's built-in account lifecycle.** anvil's descriptor now carries a
`tables.accounts` (`docs/01-seams.md` §22) — hashing mode, activation, the contact channel,
declared identifiers, the profile fields, the secret bounds, the verification code length and
the routes for each role in the lifecycle. The generator emits it as `kAccounts`, `null` for an
application that declares none, with every route role bound to that route's own generated
`const` rather than to its id as a string — so a role naming a route anvil later retires is a
compile error at the reference rather than a call that 404s at run time. `AccountsTable` names
the shape.

*Requires* nothing: a descriptor written before this table existed carries no `tables.accounts`
key at all, which reads the identical way an explicit `null` does.

- Regenerate the client (`hammer codegen`) to pick up `kAccounts` from a newer anvil descriptor.
- There is no client here yet that reads it — that is separate work, on top of this table.

### Added — `hammer/state`, not breaking

**An analytics dimension may now be an entity: a foreign id rather than a member of a closed
set.** anvil's descriptor carries a `kind` per dimension now (`docs/01-seams.md` §10); an
`"enum"` dimension is unchanged, and an `"entity"` one takes no values at all. `report()` takes
an optional `Uuid` for it instead of a string from a union, encoded to the wire's canonical
36-character form and omitted entirely when the id is not yet known — never an empty string and
never an index, because there is nothing to index into. `EntityDimension` is the new exported
marker a generated `EventSpec` carries for one.

*Requires* nothing from an existing application: an event declaring no entity dimension is
unaffected, and an enum dimension's wire encoding is unchanged.

- Regenerate the client (`hammer codegen`) to pick up an entity dimension a newer anvil
  descriptor declares.
- Pass a `Uuid` where an event's dimension is an entity, and omit the key rather than the value
  when the id is not yet known — `Dimensions<E>` makes it optional rather than nullable.

### Added — `hammer/dom`, not breaking

**`renderSignup`, and a `prepare` step on both credential forms.** `renderSignup` is the login
form's discipline — one submit at a time, aborted on close, the secret dropped after a
successful call — for the screen where a password is chosen. Both it and `renderLogin` take an
optional `prepare(body, signal)`, run before the call; pass a `Prehasher`'s `prepare` from
`hammer/prehash` and the password never leaves the page (`docs/01-seams.md` §21).

*Requires* nothing. `prepare` is optional and an existing `renderLogin` behaves as before.

- For client-side prehashing, pass the same `prehasher.prepare` to both forms.
- Add words for `"prehash"` to the `errors` table of either form that has a `prepare`: it is
  the key a failed prehash is reported under.

### Added — `hammer/prehash` and `hammer/prehash-worker`, new entry points, and `PrehashError` in `hammer`, not breaking

**Client-side password prehashing: the password never leaves the device.** Against an anvil
server in prehash mode (anvil `docs/05` §12), a `Prehasher` turns a login or signup form's body
into the one to send — the password removed, a credential derived from it with Argon2id in its
place — using an `Argon2Pool` whose worker runs `serveArgon2Pool` from `hammer/prehash-worker`.
The server stops paying ~100 ms and 64 MiB per sign-in, and nothing between the tab and the
server ever holds the password. `docs/01-seams.md` §21 is the whole contract.

*Requires* an anvil server serving a salt route; a server in plain mode is unaffected and an
application talking to one uses none of this.

- Build one worker entry: `serveArgon2Pool(self)`, and hand the pool a factory for it.
- Give the `Prehasher` your salt route call — it is passed a `SaltPurpose`, `sign_in` or
  `enroll`, to send as the body's `purpose` — your field names and the cost bounds your
  audience's devices can afford. hammer ships no default for any of them.
- Send `prehasher.prepare(body, signal)` instead of the form's body, on every screen that sends
  a password — the same `Prehasher` for sign-in and signup.
- `PrehashError` is new in `hammer` and is not a member of `HammerError`; nothing that switches
  over `HammerError` changes.

### Fixed — `hammer/state`, not breaking

**A real `Worker` now satisfies `WorkerLike` without a cast.** The pool's worker type promised
that the platform's `Worker` satisfied it structurally, and it did not: `postMessage`'s
transfer list was declared `readonly` and optional, where the platform declares a mutable
array. An application writing `imageWorker: () => new Worker(url)` got a type error and had
to cast, and nothing noticed because nothing in this repository had ever handed a pool a real
worker. `WorkerScope`, the worker-side half, had the same mismatch against a dedicated
worker's global scope.

Both now declare `postMessage(message, transfer: Transferable[])`, as the platform does.

- Remove any `as WorkerLike` cast around a `new Worker(...)`; it is no longer needed.
- A hand-written `WorkerLike` or `WorkerScope` keeps compiling: one whose `postMessage`
  accepted an optional or `readonly` list still accepts what is now passed.

### Added — `hammer/crypto`, a new entry point, not breaking

**BLAKE2b and Argon2 (argon2d, argon2i, argon2id), in plain JavaScript and byte-for-byte what
libargon2 computes.** `argon2(params)` returns a `Result`; `blake2b(input, outBytes, key?)` and
the incremental `Blake2b` are the hash beneath it. It is the algorithm half of client-side
password prehashing (`docs/01-seams.md` §21), and it is its own entry point so that nothing else
an application ships pays for it: 3.2 KB gzipped.

*Requires* nothing. Nothing existing imports it.

- Run it in a worker. At the parameters a password deserves it takes most of a second on a
  desktop and several on a phone, and on the main thread that is a frozen page.
- It needs no CSP change: it is not WebAssembly, so `'wasm-unsafe-eval'` stays out of
  `script-src`.

### Added — `hammer/state` and `hammer/dom`, not breaking

**A form option can carry the word a person reads, separately from the value that is stored.**
`FieldDefinition` gains an optional `choiceLabels`, a map from each entry in `choices` to its
label, and `renderForm` uses it for both controls a closed set can draw — the `<option>` text of
a `<select>` and the text beside a checkbox.

anvil constrains an option value to `[A-Za-z0-9_-]` so that it is safe in a CSV cell, a JSON
string, a URL and a BSON value without any downstream stage having to know where it came from.
That constraint is right and it means the value cannot be a word in Arabic, in Greek or in
Chinese — so a renderer with only `choices` to work from printed `new_site` on screen in every
edition of a bilingual site, and there was no seam through which an application could supply
anything better. Found by the first application to put a localised `SELECT_SINGLE` on a public
page.

*Requires* nothing. The member is optional and a value with no entry is drawn as itself, which
is what every existing caller already gets: a **section** field's `choices` arrive from the
descriptor as bare values with nowhere to put a word, and a blank option would be worse than an
unlocalised one.

- To adopt it, pass `choiceLabels` beside `choices` when you build a `FieldDefinition` from a
  form definition your server sent. `definitionsFrom` is unchanged and does not set it — it
  builds definitions from a **section**'s rows, which have no labels to offer.

### Fixed — `hammer/wire` and `hammer/state`, breaking

**`decodeSessionView` was written against a session payload anvil does not send.** It expected
`perms` as 24 base64url characters, `hash`, `superadmin` and `routes`, all at the root. anvil
writes two keys — `routes` from `append_reachable_routes`, and `authority` from
`append_holder_authority` carrying `superadmin` and a list of permission **names** in bit order
— and no hash at all.

Every one of this repository's 1,145 unit tests passed over it. They all built their payload
with the same fixture the decode was written against, which is two copies of one belief; the
first run against `anvil_reference_server` is what told them apart. Against a real anvil the
decode failed, `SessionStore` reported an anonymous session, and every holder route resolved to
no address — a client that signs in successfully and can then call nothing.

*Requires* nothing of the deployment and everything of the client:

- **`SessionStore` takes a new required `permissionBits`**, and the generated module exports
  `kPermissionBits` for it. anvil sends names and hammer holds a `Uint8Array(16)`
  (`CLAUDE.md` §2.3), so something has to map one to the other and only the application has the
  table. Pass `permissionBits: kPermissionBits`. It is required rather than optional because a
  store with no table decodes every session to no permissions, and the symptom is a screen with
  every non-route affordance missing and nothing to explain it.
- **`decodeSessionView(body)` is now `decodeSessionView(body, permissionBits)`.** An application
  calling it directly — `tests/testapp/app/staff_screen.ts` is the reference for this — passes
  the same table.
- **`SessionView.serverHash` is `string | null`.** Null means the response carried no hash, so
  no staleness claim can be made; `staleClient` returns null for it rather than reporting every
  such session as stale. anvil publishes no writer that puts the descriptor hash on this
  response — it is the application controller's job, using the constant its generated module
  exports — and the reference application does not.
- **`SessionView` gains `unknownPermissions`**, the names the server sent that this client has
  no bit for. A server newer than the bundle, carried rather than failed on: refusing the
  session would sign somebody out on every deploy that adds a permission, and choosing a bit
  would be inventing an authority.
- `SessionDecodeError` gains `bad-authority`.

**Regenerate the client** for `kPermissionBits`. An application that renders its own
"this page is out of date" banner from `staleClient` should note that it now stays silent
against a server that sends no hash, which is the honest answer rather than a weaker one.

### Added — the generated module

**`RouteResponses` is emitted for the routes anvil declares a response for**, with `UuidText`
and `ServerTimeText` beside it. Module augmentation still works and is still how a route anvil
describes nothing for gets a type. See [`docs/01-seams.md`](docs/01-seams.md) §4.3.

**`kMediaAccepts`** — the media types each namespace will take, which an application was
previously writing down itself. See §12.

**`kPermissionBits`** — see above. It is its own `const` and deliberately not a member of
`kApiTables`: putting it there would put every permission name into any bundle that constructs
a client, including the one an anonymous visitor downloads.

*Requires* a descriptor of **format 3**. Format 2 is refused, because a generator that read an
older file for the tables it recognises is a generator guessing at a shape nobody checked.

### Changed — `hammer/wire` and the generated module, breaking

**`Api` and `kApiTables` are now paired by the descriptor hash, so two generated modules cannot
be crossed.** Bundling the four decode tables into one member closed the gap between the tables
and left one between the set and the type parameter: every generated tables object is
structurally the same shape — four string-keyed records and a number — so an application with
two generated modules could write `createClient<OneApi>({ api: kOtherApiTables })` and have it
compile. What was holding was that both names come out of one file, which is a convention rather
than a check.

`kApiTables` now carries a sixth member nothing decodes with — `hash`, the descriptor's own —
and `Api` declares `readonly hash: typeof kTablesHash`. Crossed, the two literals disagree and
`createClient` does not compile. Two modules generated from the same descriptor still pair,
because they are the same tables.

**Regenerate the client** — `hammer codegen --descriptor … --out …`. A module generated before
this release has neither member, and passing its `kApiTables` to `createClient` now fails with
`Property 'hash' is missing`. There is no other migration: nothing an application writes
mentions either member.

An application that declares `ApiTypes` by hand rather than from the generated module adds
`readonly hash: string` to it. `tests/testapp/` declares none, and neither should anything else.

*Removed with it:* the phantom `unique symbol` on `ApiTables`. It was never reachable — the
generated module imports nothing and so cannot name a symbol hammer declares, which forced the
member to be optional, and an optional phantom constrains no object literal at all. It is
unexported and erased, so nothing can have depended on it.

### Fixed — `hammer/wire`

**A retried upload re-sent the empty remains of the first attempt's body.** An upload that asks
for progress sends a `ReadableStream`, and a stream is consumed by the attempt that sends it.
The body was built once, before the pipeline ran, so every attempt after the first — a transport
retry, a backoff retry, the replay after a credential refresh — was handed a stream that had
already been drunk. `fetch` rejects on that, this client cannot tell the rejection from a
dropped connection, and the call spent its whole attempt budget re-sending nothing before
reporting a network failure for a network that was working.

Found by driving `client.upload` end to end for the first time. `tests/wire/upload.test.ts`
covered `uploadBody`, `uploadContentType` and `checkUpload`, and nothing anywhere called the
method the three of them are assembled into (`docs/15-tasks.md` §The first consumer).

**Nothing to migrate for a caller of `client.upload`.** The body is now built per attempt, so a
retried upload sends a second one. Progress restarts from zero on each attempt, which is the
honest number: the bytes really are being sent again, and a counter carried across attempts
reports an upload as half done while the server has received none of it.

A second property arrived with the fix. The body is built inside the one function that sends a
request, so an upload that is shed by a full queue, refused by an open circuit or handed an
already-aborted signal now reads **no byte of the file at all** — each of those used to open a
stream over it for a request that was never made.

### Changed — `hammer/wire`, breaking

**`SendOptions.body` takes a factory for a one-shot body, and no longer takes a `ReadableStream`
directly.** A stream passed by value is a call that retries itself into its own attempt cap, and
the type is where that is cheap to find rather than a sentence in a comment. Only `client.send`
is affected; `call`, `upload`, `stream` and `mint` are unchanged.

```
- client.send(route, { body: readableStream, contentType, signal });
+ client.send(route, { body: () => readableStream(), contentType, signal });
```

Every other `BodyInit` — a `File`, a `Blob`, a `Uint8Array`, a string, `FormData`,
`URLSearchParams` — is re-readable, so it is still passed as itself and still sent again as
itself on a retry.

### Added — `hammer/wire`

- **`RequestBody`** — `() => BodyInit`, the body of one attempt.
- **`SendBody`** — what `send` accepts: a re-readable `BodyInit`, or a `RequestBody` for one
  that is not.

### Changed — `hammer/wire`, breaking

**`ClientConfig` takes one `api` member in place of four.** `vocabulary`, `rateLimits`,
`singleUse` and `bodyMaxBytes` were all the generated module's, and handing them over one at a
time was four chances to give a client a table from a descriptor other than the one its routes
came from. They are now emitted together as `kApiTables` and passed together.

**Regenerate the client first** — `Api` and `kApiTables` are new output, so a module generated
before this release does not have them. `hammer codegen --descriptor … --out …`, then:

```
- import { createClient, errorVocabulary } from "hammer/wire";
- import {
-     kBodyMaxBytes, kCapabilitySingleUse, kErrorCodeValues,
-     kRateLimits, kValidationReasonValues, routeAuthRefresh,
- } from "./api/hammer.generated.js";
-
- type Api = {
-     readonly params: RouteParams;
-     readonly responses: { readonly [Id in RouteId]: ResponseOf<Id> };
-     readonly code: ErrorCode;
-     readonly reason: ValidationReason;
- };
-
- createClient<Api>({
-     vocabulary: errorVocabulary<ErrorCode, ValidationReason>({
-         codes: kErrorCodeValues, reasons: kValidationReasonValues,
-     }),
-     rateLimits: kRateLimits,
-     singleUse: kCapabilitySingleUse,
-     bodyMaxBytes: kBodyMaxBytes,
-     refreshRoute: routeAuthRefresh,
-     origin, fetch, session,
- });

+ import { createClient } from "hammer/wire";
+ import { kApiTables, routeAuthRefresh } from "./api/hammer.generated.js";
+ import type { Api } from "./api/hammer.generated.js";
+
+ createClient<Api>({
+     api: kApiTables,
+     refreshRoute: routeAuthRefresh,
+     origin, fetch, session,
+ });
```

`errorVocabulary` and `ErrorVocabulary` are unchanged and still exported, for a consumer that
decodes an envelope itself. A client no longer makes anyone spell them: the two type parameters
were the application restating what `Api` already says.

`refreshRoute` stays a member of its own. The descriptor carries the route and nothing that
marks it as the refresh, so a generated `kApiTables` naming one would be the generator guessing
at an id — wrong silently, in the one call that decides whether a session survives.

### Added — the generated module

- **`Api`** — the type parameter `createClient` takes, assembled from `RouteParams`, `RouteId`,
  `ResponseOf`, `ErrorCode` and `ValidationReason`. It was written by hand in every application
  until the first one to consume hammer copied a stale version out of the getting-started guide.
- **`kApiTables`** — the decode tables as one value. A plain object literal over the constants
  above it: no call and no import, so the module still depends on nothing and a bundle that
  references none of it still drops all of it.

### Added — `hammer/wire`

- **`ApiTables<A>`** — the type of the member above, exported so a consumer can name it.

### Fixed — documentation

**`docs/02-getting-started.md` described an API this library has never had.** Found by the first
application to consume hammer, which reached for the guide and could not use it: `createClient`
shown taking a route table and three origins it has no members for, `call` shown taking a route
id rather than the route value, `useResource` shown taking a route and a query, `useSession`
shown taking nothing.

The document's own opening sentence is why nobody caught it. It claimed the examples "cannot
rot" because they are compiled as `tests/testapp/` — the testapp is compiled, and these were
paragraphs beside it. A claim that drift cannot happen is exactly the claim that stops anyone
checking, and the same sentence was in `README.md`.

So the claim is now true rather than removed. Every TypeScript block in `docs/` and `README.md`
names a region of a file the type-checker reads, **`tools/check-docs.sh`** lifts that region out
and diffs it on every `npm run lint`, and `--write` rewrites the blocks from source. A block
that names nothing fails the build; a signature sketch is spelled `ts sketch: <why>`, which
costs a sentence the way `@ts-expect-error` does.

`tests/testapp/app/minimal.ts` and `minimal_react.tsx` are the compiled source those blocks come
from — the narrow path a reader follows, beside the exhaustive seam proof that was already
there. Five further blocks in the guide were wrong in the same way and had not been found:
`mint` and `upload` both take three arguments rather than two, `call` takes `params` rather than
`path`, `definitionsFrom` takes a control-to-field-type map rather than the field-type table,
`affordsRoute` and `holdsAll` take the session's `view` rather than the session state, and an
error `code` only exists on the failures a server answered.

### Changed — `hammer/wire`, breaking

**`ClientConfig.origin` takes the unvalidated pair and `createClient` throws on one it cannot
drive.** `docs/01-seams.md` §16 and the 0.1.0 entry below have both said this since the first
release. It was not true: `createClient` took an already-validated `ApiOrigin`, so the
unwrap-and-throw sat in the application, written identically in each one.

```
- const origin = defineApiOrigin({ pageOrigin, apiOrigin, site: null });
- if (!origin.ok) throw new Error(origin.error);
- createClient<Api>({ origin: origin.value, … });

+ createClient<Api>({ origin: { pageOrigin, apiOrigin, site: null }, … });
```

`defineApiOrigin` still returns a `Result` and is still exported, for a deployment that would
rather render the reason than crash on it, and so the decision stays testable without
constructing a client. The validated value is readable afterwards as **`client.origin`** — new,
and the reason it is there: `crossOrigin` records a CORS preflight paid on every mutating
request, and nothing else in a browser can observe it.

### Changed — `engines`

**`engines.node` is now `>=22`.** Node 20 reached end-of-life on 2026-04-30 and receives no
further security fixes (`CLAUDE.md` §12). This changes nothing at the published surface —
hammer ships nothing that runs on Node at all — but `npm install` and `npm ci` both read
`engines` before they read anything else, and `.npmrc`'s `engine-strict=true` is what makes
that read a refusal rather than a warning.

*Requires* Node 22 or later to install, build or test hammer.

---

## 0.1.0 — 2026-09-14

First versioned release. The package is private and internal: it is consumed by this
organisation's applications, alongside [anvil](../anvil/), and has no registry name.

### Entry points

`exports` is frozen at seven, and each has a declared gzipped ceiling that the build enforces
([`tools/bundle-budget.json`](tools/bundle-budget.json)):

| Entry point | What it is | Ceiling |
|---|---|---|
| `hammer` | types, errors, `Result`, brands, `PermSet`, text, locale, bidi, validators, cursors | 8 KB |
| `hammer/wire` | client, routes, envelope, retry, idempotency, credentials, leader, SSE, upload | 12 KB |
| `hammer/state` | stores, bounded cache, resources, session, forms, sections, inbox, media, worker pools | 12 KB |
| `hammer/dom` | `SanitizedHtml` and the single insertion site, and every unstyled component | 18 KB |
| `hammer/chart` | scales, ticks, marks, keyboard traversal, a data-table fallback | 9 KB |
| `hammer/react` | hooks binding the stores to a lifecycle, and no behaviour of its own | 3 KB |
| `hammer/codegen` | the descriptor reader and the TypeScript emitter — build time only | not shipped |

### Requires

- **A CSP carrying `trusted-types hammer default`** where the page enforces
  `require-trusted-types-for 'script'`. `hammer` is the policy the sanitiser creates for the
  one place this library turns a string into markup; `default` is the refusing policy
  `installTrustedTypes()` installs. **Missing the first throws on every rich-text render**, and
  only on a page that enforces the policy — so it passes every test and fails on the most
  careful deployment. `docs/03-deployment.md` §3.
- **The API is same-origin with the application.** Not merely same-site: hammer refuses to be
  constructed against a cross-site origin, because `SameSite=Lax` cookies are not sent there
  and the session would already be gone.
- **No public source map for any bundle that is not public.** A map carries each module's full
  contents, so it hands back everything tree-shaking removed. `docs/03-deployment.md` §2.
- **An entry point whose existence is sensitive is served from a protected location.** Holder
  route paths are never emitted, but the route id and the call site live in whichever chunk
  calls them. One `auth_request` block; `docs/03-deployment.md` §1.

### Not yet verified end to end

The two-tab credential run — one refresh, one replay, one logout fan-out, zero rotation races —
**has never executed**, because anvil serves the reference application on no port. The suite is
written and fails rather than skips without a server (`docs/16-test-plan.md` §Phase 7). Every
unit test of the client, the store and the renderer passes while that path is broken, which is
the whole reason the run exists. Treat the credential lifecycle as unverified against a real
server until it has run.
