# 01 — Seams

A seam is a place where hammer stops and your application starts. hammer holds no
application's data — not one route, permission, field type, locale, section, topic, event
name or string — so everything of that shape arrives through the one mechanism below.

anvil has fifteen seams and each is a `constexpr` table an application writes by hand. hammer
has **one**, and it is not written by hand:

> **The application's tables are declared once, in C++, where anvil already validates them.
> hammer generates its copy from a descriptor that build emits.**

A second hand-written copy of the route table, the permission list, the field types and the
event names is a second copy that drifts, and the drift is silent in the direction that
matters: the client keeps calling a route the server retired, keeps offering a field type the
server no longer accepts, and keeps a permission bit that has been renumbered — which is not
a missing feature but a **wrong authority check**, rendered to a user as an affordance that
should not exist.

```
   your anvil application            build time                 your web application
  ┌──────────────────────┐      ┌──────────────────┐      ┌──────────────────────────┐
  │ perms.h  routes.h    │      │ hammer codegen   │      │ hammer.generated.ts      │
  │ field_types.h …      │─────►│  reads the JSON  │─────►│  types + constexpr tables│
  │      ↓ emitter       │ .json│  emits TypeScript│  .ts │  + the descriptor hash   │
  └──────────────────────┘      └──────────────────┘      └──────────────────────────┘
         one source of truth, validated by static_assert where it already is
```

Three rules, inherited from anvil's and adapted to a language with no `static_assert`:

1. **The generated tables are `as const`** and every derived type is a union of literals. A
   route that does not exist cannot be spelled; a permission that was renumbered is a type
   error at every call site that named it.
2. **A malformed descriptor fails the generator**, with the same checks anvil's
   `well_formed()` makes: duplicate bit, duplicate name, empty name, a bit out of range, a
   route with no method, a capability scope no route requires.
3. **A stale generated module fails the build.** The module carries the hash of the descriptor
   it came from, and `tools/check-descriptor.sh` regenerates and compares. A client generated
   from last month's server is the one failure this whole mechanism exists to prevent, and it
   is not allowed to be a runtime surprise.

---

## 1. The descriptor

One JSON file, versioned, emitted by the application's own build. Every seam below is a key
in it.

```jsonc
{
  "descriptor": 3,                    // the FORMAT version. Bumped by anvil, never by an app
  "emitted_by": "0.1.0",              // the anvil that emitted it
  "app": { "name": "example", "version": "2026.9.1" },
  "hash": "f3cc3172…",                // SHA-256 over the `tables` object, and nothing else

  "tables": {
    "error_codes": { "max": 14, "codes": [
      { "name": "VALIDATION_FAILED", "value": 6, "http": 400, "stealth_hidden": false },
      { "name": "FORBIDDEN", "value": 2, "http": 403, "stealth_hidden": true } ] },

    "validation_reasons": [ { "name": "BAD_FORMAT", "value": 4 } ],

    "permissions": [ { "name": "ContentWrite", "bit": 1 } ],

    "locales": [ { "tag": "en", "collation": "en", "rtl": false },
                 { "tag": "ar", "collation": "ar", "rtl": true } ],

    "routes": [ { "id": "content.delete", "method": "DELETE", "path": "/content/{id}",
                  "access": "guarded", "visibility": "holder",
                  "perms": ["ContentDelete"], "capability": "ContentDelete",
                  "rate_limit": "", "idempotent": false, "page": null,
                  "response": null } ],                   // §4.3, null where none is declared

    "capability_scopes": [ { "name": "ContentDelete", "value": 1, "single_use": true } ],

    "rate_limits": [ { "bucket": "login", "window_ms": 60000, "max_events": 20 } ],

    "limits": { "upload_max_bytes": 26214400, "body_max_bytes": 262144,
                "page_limit_max": 100 },

    // The content tables (§§7–10, §12), each described in its own section below.
    "field_types": [ … ], "sections": [ … ], "topics": [ … ],
    "events": [ … ],      "media": { … }
  }
}
```

**`tests/testapp/hammer.descriptor.json` is this document, emitted by anvil's reference
application, committed here, and the input to every generator test.** It is not an example
written against this section — it is the output, and when the two disagree this section is
wrong.

**The hash covers `tables` and nothing above it.** Three questions are being asked and no one
number answers all three: `descriptor` says whether the generator can read this file at all,
`app.version` says whose tables these are, and `hash` says whether a client was generated from
these exact tables. An application version bump changes no table and must not invalidate every
client running perfectly good code.

That makes determinism load-bearing rather than tidy: the same tables must produce the same
bytes on every run, or the staleness check reports drift that is not there and is ignored
inside a week. anvil's emitter iterates no unordered container and emits a route's permission
names in **bit order** rather than table order for exactly that reason.

**The content tables landed once there was a reader for them.** anvil held `field_types`,
`sections`, `topics`, `events` and `media` back on the grounds that a table emitted before
anything reads it is a format nobody has tested against a reader; the generator in §14 is that
reader, so they are in the descriptor now (§§7–10, §12) and the sections below describe what is
actually in them.

Each is emitted in the vocabulary a client speaks rather than the one the server stores — field
type flags are named booleans and not a byte, a PII type's answer shape is `null` rather than
`"text"` — because the alternative is a client carrying a copy of an enum, which is the thing
this whole document exists to remove.

**The generator reads all five.** What each emits is §14; where the emission withholds something
is §4.1, which now governs two tables rather than one — a holder route's path and a holder
topic's key are the same rule applied to the two things a client needs in order to *reach*
something.

**`descriptor` is the format version and `hash` is the content.** They answer two different
questions — "can this generator read this file" and "is this client built from this server" —
and a single number cannot answer both.

**Format 3 adds the two things an application was previously writing down itself**: a route's
declared `response` (§4.3) and a media namespace's `accepts` (§12). Both were rows under
`docs/15-tasks.md` §Cross-repo, and both closed the same way — the fallback here was left
deliberately worse than the fix, so the pressure stayed where it belonged. A descriptor of
format 2 is now **refused** rather than read for the tables it does have; a generator that read
an older file for what it recognises is a generator guessing at a shape nobody checked, which
is what this number exists to stop.

---

## 2. Error codes

### What hammer ships

The envelope decode, the union, and the retry policy per code
([`00-architecture.md`](00-architecture.md) §6). One place decodes; nothing else reads
`json.error`.

### What the descriptor carries

Every `ErrorCode` member with its numeric value and the HTTP status anvil maps it to, plus
`max`. The generator emits a string union of the **wire names, unchanged**, a value map, an
`Unknown` member and the bound.

`Unknown` is PascalCase against anvil's SCREAMING_SNAKE for a reason: the enum is append-only
server-side, so hammer's own member has to be one the server can never collide with. And the
success member — the code whose value is zero — is left out of the union, because an
application would otherwise owe a sentence for a case no error surface can reach.

**`ValidationReason` carries `Unknown` too, and for the identical reason.** `input::Reason` is
append-only on anvil's side as well, so a deploy can answer a tab that is already open with a
reason its bundle predates. The member is in the union and not in the value map — the server
stores no number for it — and the alternative to having one is dropping that field from the
`fields` map, which is a form that refuses to submit with nothing marked on it. A sentence that
is too general beats a field with no error on it.

### Notes that are not obvious

- **`ErrorCode` is append-only server-side, so the client must decode an unknown one rather
  than throw.** anvil stores the numeric value in audit rows read back much later and keeps
  `kMaxErrorCode` so a decoder has a bound it can state. A client that throws on a code added
  this morning converts a deploy into an outage in every tab that was already open — which is
  the same class of failure as §7.1's stale bundle and has the same answer: degrade, surface,
  do not crash.
- **`validation_reasons` exists so the application's copy map is exhaustive by type.** The map
  from a reason to a sentence is `Record<ValidationReason, string>`, so a reason added
  server-side is a **compile error in every client** until somebody writes the words. The
  alternative is a fallback string, and a fallback string is how "BAD_FORMAT" reaches a user in
  an interface that is otherwise entirely in Arabic. Its zero member is dropped the same way
  the error table's is, and for the same reason — and its `Unknown` member is added the same
  way, which is what makes the compile error safe to rely on at run time as well as at build
  time.
- **The names are never translated, in either table.** A second spelling is a translation table
  in every bundle that decodes an envelope, a second copy of an append-only enum to keep in
  sync, and a word on a screen that does not match the word in the log it is being read
  against — which is the argument §3 makes about permissions, and it does not stop being true
  for an error code.
- **The reason vocabulary did not exist until this seam needed it.** anvil documented
  `{"email":"INVALID_FORMAT"}` from its first phase and shipped no function that produced the
  string, so every application invented its own spelling of anvil's own enum — which is a
  second disagreement to keep in sync, on the one part of the error body a user actually reads.
  It is `wire_name(input::Reason)` now, and the names are the enumerators in SCREAMING_SNAKE so
  the mapping is mechanical in both directions.
- **`stealth_hidden` marks the codes the filter rewrites to a 404.** A client needs it to know
  that on a stealth route a not-found answer is not evidence of anything, and that rendering a
  denial there would rebuild the oracle the server removed.
- The HTTP status is carried for the tests, not for the runtime. hammer branches on the code;
  the status is what the contract suite asserts the server still sends.

---

## 3. Permissions

### What hammer ships

`PermSet` — the same 128 bits anvil has, as a `Uint8Array(16)`, with `has`, `hasAll`,
`hasAny`, and a base64url codec matching anvil's little-endian wire format byte for byte. A
membership test is one AND; there is no string anywhere in it.

### What the descriptor carries

Name → bit, for every permission. The generator emits one `const` per bit, the union of names,
and — separately — `kPermissionBits`, the whole map.

### Notes that are not obvious

- **The session carries a list of names, and hammer turns it back into the bitset.** This
  section used to say the opposite, and the correction is worth keeping: a live run against the
  reference application found that `append_holder_authority` writes
  `{"superadmin":…,"perms":["ContentRead"]}` — names, in bit order — and anvil's reasons are
  good ones. `for_each_name` skips the reserved gaps between an application's blocks, so a bit
  nothing names never reaches a client as a control that authorises nothing; and a superadmin's
  set is deliberately not all-ones, so a mask would put the very conflation anvil keeps apart
  onto the wire for every client to un-conflate.

  What hammer HOLDS is still `Uint8Array(16)`, because that half of the rule is about the
  client: one AND against a string compare per check (`ENGINEERING_RULES.md` §2.3). The mapping happens
  once, in `decodeSessionView`, through `kPermissionBits`.
- **`kPermissionBits` is its own `const` and is not in `kApiTables`.** It is the one table of
  names a client cannot avoid, so it is emitted where a bundler can drop it: a chunk that
  decodes no session references it and ships none of these names. Putting it in `kApiTables`
  would put every permission name into any bundle that constructs a client, including the one
  an anonymous visitor downloads — which `tests/codegen/built_output.test.ts` asserts against
  the built bytes.
- **A name with no bit is carried, not failed on.** `SessionView.unknownPermissions` holds the
  names this bundle has no bit for, which is a server newer than the client. Refusing the
  session would sign somebody out on every deploy that adds a permission; choosing a bit would
  be inventing an authority.
- **Bit indices are stored in access tokens and in the user document. They are never
  renumbered**, which is anvil's rule and which the client inherits without being able to
  check it: a renumber the generator cannot see is a silent regrant of authority.
- **The check governs affordances and never refuses a call**
  ([`00-architecture.md`](00-architecture.md) §4.1). The local copy goes stale by design;
  anvil's `perm_epoch` is the mechanism that bounds it, and the client's part of that bargain
  is to treat a `403` as "refetch", not as "deny".
- Names are never translated. An investigator comparing a staff screen against a server log
  needs the same word on both.

---

## 4. Routes

### What hammer ships

The route resolver, the typed `call`, the in-flight dedupe key, and the retry class per
method. Path parameters are interpolated through `encodeURIComponent`; query values through
`URLSearchParams`. **There is no `fetch(url)` in the public surface**, and
`tools/check-wire-discipline.sh` fails the build on a URL built by concatenation.

### What the descriptor carries

Per route: `id`, `method`, `path` with named parameters, `visibility`, the permissions it
requires, the capability scope it consumes, whether it is stealth, whether the method is
idempotent, its rate-limit bucket, and — for a list route — the cursor field and the maximum
limit.

`visibility` is the field this seam turns on, and it has two values:

| | What it means | Where the path lives |
|---|---|---|
| `public` | reachable with no credential at all — login, signup, public reads | compiled into the bundle |
| `holder` | everything else | delivered at run time, scoped to who is asking (§4.1) |

### 4.1 A privileged path is not a compile-time value

**The first draft of this section was wrong, and the error is worth keeping.** It said that
routes carry an audience, that the generator emits one module per audience, and that the
application code-splits so the public bundle holds no administrative path. The first two are
still true. The conclusion was not:

> **A lazily-loaded chunk is a public URL. Code-splitting is not access control.**

`/assets/staff-a3f2.js` is fetched by an anonymous visitor with `curl`, and the whole staff
route table is inside it. Splitting changes which bundle *downloads* the map in a browser; it
does not change who can *read* it.

So the generated module is split along a line where the split is free rather than
aspirational — the one TypeScript already draws between what is erased and what is emitted:

| | What it is | What it costs in a bundle |
|---|---|---|
| A route's **shape**: its id, params, body, capability requirement, response type | a `type` | **nothing.** Types are erased at build, so a shape cannot leak — at run time it does not exist |
| A **public** route's method and path | a `const` | a string in the bundle, naming a route anyone may call anyway |
| A **holder** route's method and path | **not emitted at all** | nothing. It is in no asset, in no chunk, and in no source map |

The path of a holder route arrives with the session, filtered by the server to what that
holder's permissions actually reach:

```jsonc
// GET /session
{
  "routes": {                                   // ONLY what these permissions may reach
    "content.publish": "POST /content/{id}/publish"
  },
  "authority": {                                // what is NOT a route (§3)
    "superadmin": false,                        // absent means false
    "perms": ["ContentRead"]                    // NAMES, in bit order
  },
  "hash": "f3cc3172…"                           // optional; see below
}
```

**The first version of this block was wrong, and the error is the second one worth keeping in
this section.** It showed `perms` as a base64url mask and `superadmin` and `hash` at the root,
and hammer's decode was written against it — for four phases, past 1,145 unit tests, every one
of which built its payload from the same fixture. The shape above is what anvil's two published
writers actually produce, and the first live run against `anvil_reference_server` is what told
the two apart.

**`hash` is optional, and the reference application sends none.** anvil publishes no writer that
puts the descriptor hash on this response: it is the application controller's job, using the
constant its generated module exports (§14). So `SessionView.serverHash` is `string | null`, and
null means *no claim* rather than *stale* — reporting every hashless session as stale would put
"this page is out of date" on every screen of an application whose controller has not been taught
to send it.

Three properties follow, and the third was not available before:

1. **The anonymous bundle contains no privileged path**, because the generator never emitted
   one — so a publicly fetchable chunk stops being a leak rather than being hidden better.
2. **The table is scoped per holder.** A content editor does not learn the paths of the routes
   above them, so a compromised account yields that account's map rather than the whole one.
3. **The scoping is the server's**, which is the only participant in this an attacker does not
   own. A client-side filter over a full table is a filter over a full table that shipped.

**A call to a route id with no entry is a missing address, not a refusal.** It is the one
place the client genuinely cannot "send it and let the server decide"
([`00-architecture.md`](00-architecture.md) §4.1), and the recovery is the one that mechanism
already uses: refetch the session, retry once, and if the entry is still absent report the
same not-found shape as everything else that is absent. **Never a permission-flavoured
error** — a client that says "forbidden" where the server would have said 404 rebuilds the
oracle in the one place the server cannot reach.

The cost is one indirection and a session payload carrying roughly fifty bytes per reachable
route, on a response the application already needs before it renders anything privileged. Its
`ETag` is keyed to `perm_epoch`, so a grant reaches the table at the next revalidation rather
than at the next login.

### 4.1.1 Decoding it is a trust boundary, and it is the interesting one

Every path in that table becomes the path of an authenticated request. A payload that is
malformed, truncated, corrupted in transit or served by something that is not the server
therefore reaches further than most responses do: **it chooses where the session's cookies are
sent.** So the paths are validated rather than trusted, against the same character class the
generator applies to a route pattern — and the check that matters is the one nobody writes:

> `//evil.example/x` is a protocol-relative URL. `new URL("//evil.example/x", origin)` is not a
> path on `origin` at all; it is a different host. It is the one malformed path that does not
> fail. It succeeds, somewhere else.

Two smaller rules follow from what anvil actually emits:

- **A method of `ANY` is refused, not guessed at.** `ANY` is not a method — it is a declaration
  that every method shares one policy — and anvil emits it, in its own words, "so a generated
  client fails loudly on a route whose description forgot to name one, rather than defaulting to
  GET and silently calling the wrong thing". It costs that route and not the session: a real
  table carries an `ANY` entry the day one description forgets a method, and discarding the
  session over it logs a person out of an application whose only defect is a logout route
  nobody can call.
- **The table is decoded into a `Map`, never an object.** The keys come off the wire, and in a
  plain object `__proto__` and `toString` are lookups that answer something nobody put there.

**`superadmin` is carried because the route table cannot answer every question.** anvil's
`satisfies()` short-circuits on user type, and a superadmin's permission set is deliberately
*not* all-ones so that "is superadmin" and "holds every permission" stay distinguishable in an
audit log. The route table already accounts for it — the server built that table with
`satisfies()` — so a **route** affordance is answered from the table and is exact. An affordance
that is *not* a route has only the bits, and a client counting bits alone would hide the whole
application from the one account that reaches all of it. Absent means false, which hides more
rather than less.

### 4.2 What this does not buy

**Path secrecy is defence in depth and never a boundary**, and a design that begins to rely on
it has already lost. A staff user's browser holds that user's paths, and so does anything
running inside it. anvil's stealth 404 is the control; it is enforced on the server, and it is
worth exactly as much after this section as before it.

What §4.1 buys is narrower and real: hammer does not hand the map to people who have not
authenticated, and does not hand the whole map to people who have.

Where the **existence** of a feature is itself sensitive — a moderation tool, an impersonation
route, an unreleased surface — what remains exposed is the route *id* and the call site, which
live in whichever chunk calls them. The answer there is deployment rather than codegen:
**serve that entry point from a protected location** — `auth_request` in Nginx, or a separate
host behind the session — so the asset is a protected resource like every other one. It costs
one location block, and it is the only thing here that actually stops `curl`.

And one that is missed almost every time:

> **A published source map re-leaks everything tree-shaking removed**, because the generated
> module's full contents are in it. A privileged bundle publishes no public source map: it
> uploads one to the error reporter privately, or it ships none.

### 4.3 What a route answers with

**This section used to say that response shapes were not derivable, and it was right at the
time.** anvil validated *requests* through a schema binder and wrote *responses* by hand, so
the only thing a generator could have emitted was a guess. The application declared a response
type by augmenting `RouteResponses`, and a route with no declaration resolved to `unknown`.

anvil now has a response *writer* (`http/response_writer.h`), and that changes the answer —
but only because of **what kind** of thing it is. A schema beside a handler is a comment; this
one is enforced on the server, where writing a key that is not next, writing one of the wrong
type, or stopping before the last field are `static_assert`s. So the descriptor's `response`
is a description of the bytes rather than a claim about them, and that is the only condition
under which generating types from one is better than generating none:

> **A response type nothing enforces is a lie that type-checks**, and it is worse than
> `unknown` — because `unknown` makes the call site narrow, and a wrong type makes it confident.

```jsonc
"response": { "kind": "object", "fields": [           // or "array", for a list of them
  { "name": "id",          "type": "uuid",    "nullable": false },
  { "name": "permissions", "type": "strings", "nullable": true  } ] }
```

Six types, and they are not the JSON types. Each is a distinction a **client** has to make:
`uuid` and `string` are both JSON strings and one of them is parsed into a typed id; `time` is
a JSON string a client turns into an instant. The generator emits them as the types a
**decoded body** has, which is why none of them is a branded type — what a call site holds is
the output of `JSON.parse`, and a brand asserts work that nobody here has done.

| descriptor | emitted | what turns it into the value hammer wants |
|---|---|---|
| `string` | `string` | — |
| `int` | `number` | — |
| `bool` | `boolean` | — |
| `uuid` | `UuidText` (an alias for `string`) | `Uuid.parse` — a 36-character string compared with `===` is what that type is for (`ENGINEERING_RULES.md` §2.3) |
| `time` | `ServerTimeText` (an alias for `string`) | `ServerInstant.fromServerIso` — the device clock is user-settable, so a duration is never the difference of two clocks (`ENGINEERING_RULES.md` §6) |
| `strings` | `readonly string[]` | — |

**`nullable` is carried because a nullable field is a different type.** A consumer told a field
is always present and handed a `null` crashes on the field it was told it could trust. anvil's
`/me` is the case in the reference application: `permissions` is `null` for a superadmin and a
list for everyone else, and an empty list would have read as "holds nothing", which is the
opposite of true.

**A route with no declaration is still absent, and still resolves to `unknown`.** The binder is
opt-in per handler, so most routes emit no type — and module augmentation remains the way an
application declares one, exactly as it did before. What changed is that the routes anvil
describes no longer need it.

### Notes that are not obvious

- **Tables are emitted as individual `const` exports, never as one object literal.** A bundler
  can drop an export nothing references; it cannot drop one member of an object that something
  imported. The shape of the emission is what makes "the public bundle does not contain it"
  true rather than hoped for, and it applies to permission names as much as to routes.
- **`RouteResponses` is an `interface` and not a type alias**, which is what lets an application
  augment it for the routes anvil describes nothing for. Emitting a closed alias would have
  made the generated half and the hand-written half mutually exclusive.
- **`idempotent` is a property of the route, not of the method.** A `POST` that is safe to
  repeat says so and skips the idempotency key; a `PUT` that is not says so too. Deriving it
  from the verb is how a retry becomes a duplicate write.
- A list route names its cursor field, which is what makes an offset **unspellable** in the
  client API rather than merely discouraged.

---

## 5. Locales

### What hammer ships

`Locale`, the direction and digit-shaping rules, NFC normalisation, the bidi isolation helpers,
and code-point counting (`ENGINEERING_RULES.md` §8).

### What the descriptor carries

The locale list **in the server's order**: `tag`, `collation` and `rtl`.

### Notes that are not obvious

- **Order is persisted and append-only.** anvil stores a locale as a one-byte index into this
  table. The client never sorts it, never renumbers it, and never assumes index 0 is a default.
- **That order is now load-bearing in a second place**: a section field's `labels` is an array
  PARALLEL to this one (§8). `labels[i]` is the label in `locales[i]`, so a client that sorted
  this table would relabel every field in the application — silently, and correctly-looking, in
  whichever locale happened to move.
- **The collation string is carried so the client sorts the way the server does**, when it
  sorts at all. `Intl.Collator` with the same locale tag is the only way a client-side sort of
  a page and a server-side sort of the collection agree; they are otherwise two different
  orderings of the same rows, and the disagreement shows up exactly at a page boundary, where
  a row appears twice or never.
- **Digit shaping is display-only**, and the descriptor does not carry a digit system.
  `LocaleSpec` has none, and adding one would be a second table to disagree with the one the
  platform already has: the client derives shaping from the tag through `Intl`. An
  Arabic-Indic digit on the wire is a validation failure at the server either way.

---

## 6. Capability scopes

### What hammer ships

`Capability<Scope>` — a branded value with a module-private constructor, produced by the call
that mints it and by nothing else. A route declaring a scope takes one in its parameters, so
**the requirement is discharged by the type system rather than by a reviewer**.

### What the descriptor carries

Scope name, its stored value, and whether it is single-use. **Not a TTL** —
`CapabilityScopeSpec` carries none, because the lifetime is set where the token is minted
rather than declared per scope.

### Notes that are not obvious

- **hammer never mints a capability implicitly on a `CapabilityRequired` error.** The scope
  exists so that a destructive action is a second, deliberate act — usually a confirmation the
  user gave. Minting one automatically to satisfy an error is an elaborate way of removing the
  control and leaving its costs.
- **A single-use capability is consumed by the server whether or not the response arrives.**
  So the call that carries one is never auto-retried: a retry re-sends a token that is already
  spent, gets `CapabilityInvalid`, and reports a failure for an operation that succeeded. The
  recovery is a re-read, not a retry.
- **An expiry the interface wants to show comes from the mint call, not from this table.** A
  scope declares whether redemption burns the token; how long a particular token lives is a
  property of that token.
- **A capability reaches the server in a header, and the handler is what reads it** (§17). anvil
  hands its *preview* capability over in a query string exactly once and exchanges it for a
  path-scoped cookie with a `303`, which is a deliberate one-shot; for every other scope a URL
  is the wrong carrier, because a URL is in the address bar, the history, the `Referer` and
  every analytics payload built from `location.href`.

---

## 7. Field types

### What hammer ships

The form renderer: a state machine over a definition, per-field validation in **code points**,
error placement, ARIA wiring, and submission with the version it read. `FieldTypeSpec` is
machinery.

### What the descriptor carries

Each field type an application declared: `name`, its stored `code`, whether it is `pii`, the
shape of the answer it produces, a `default_code_points` bound, and a `flags` object of named
booleans — `options`, `attachment`, `ranged`, `code_point_capped`, `multi_line`, `multi_select`.

```jsonc
{ "name": "TEXT_LONG", "code": 1, "pii": false, "answer": "text",
  "default_code_points": 4000,
  "flags": { "options": false, "attachment": false, "ranged": false,
             "code_point_capped": true, "multi_line": true, "multi_select": false } }
```

### Notes that are not obvious

- **`answer` is `null` for a PII type, not `"text"`.** A PII field produces no answer a client
  ever reads back, and a renderer handed `"text"` would render a control for a value that has
  silently emptied itself. `null` is the shape that cannot be mistaken for an empty one.
- **The flags are named booleans rather than the byte anvil stores.** A client carrying a copy
  of a bitmask is a client carrying a copy of an enum, and the second one to be renumbered is
  the one nobody is reading.
- **`default_code_points` is code points, as its name says**, and it is the same number the
  server checks against — which is the whole point of generating both sides from one table.
- **Client-side validation is a round-trip saver and never a control.** The server validates
  the same input with the same bounds, because the server is the only participant an attacker
  does not own. The value of generating both from one descriptor is that the two *agree* — a
  form that accepts what the server refuses is a user staring at a field with no error on it.
- **A control is a mapping, not a component.** `"control": "text"` says what kind of input it
  is; what it looks like is the application's (`ENGINEERING_RULES.md` §1, §9).
- An unknown field type is a **generation** failure, not a runtime fallback. A fallback renders
  a text box for a signature pad and posts a string the server rejects.
- **`pii` and `answer` are checked against each other at generate time.** They say the same
  thing, a renderer branches on both, and a descriptor where they disagree is one whose form
  draws a control for a value it can never read back.
- **This is the table that is emitted as a table** (§14). A field type is resolved by the `code`
  a definition carries at run time, so there is no bundle in which a subset of it is the useful
  part.

---

## 8. Sections

### What hammer ships

The section renderer, and the only markup-insertion site in the library (§19).

### What the descriptor carries

Each section's `key` and the `site_path` it renders at, its `fields` and its `images`. A field
carries its `key`, its `type`, its `labels`, a `max_code_points` bound, and whether it is
`localized`, `required`, and which `choices` it admits.

```jsonc
{ "key": "home.about", "site_path": "/", "images": [ … ],
  "fields": [ { "key": "title", "type": "Text", "labels": ["Title", "العنوان"],
                "max_code_points": 80, "localized": true, "required": true,
                "choices": [] } ] }
```

### Notes that are not obvious

- **`labels` is the one place the descriptor carries words a person reads, and it is not an
  exception to §13.** They are the application's own labels for its own editors, declared in
  the application's own C++ table, arriving through the one seam everything else arrives
  through. What §13 forbids is *hammer* shipping a string and an application keeping a second
  copy of a table; a section label written once server-side and generated into the client is
  the opposite of both. It is indexed by locale order, so see §5.
- **Rich text inserted into the DOM is a `SanitizedHtml` and nothing else can reach the
  insertion site.** This is anvil's `append_sanitized` trick in TypeScript: a brand whose
  constructor is module-private, so a raw string cannot be made into one by a refactor that was
  not thinking about it.
- **The insertion site needs a Trusted Types policy, and the application's CSP has to name
  it.** `DOMParser.parseFromString(..., "text/html")` is a Trusted Types sink — this library
  claimed otherwise until a browser said so — so a page enforcing
  `require-trusted-types-for 'script'` must serve `trusted-types hammer default`, where
  `hammer` is the policy the sanitiser creates for its one parse and `default` is the refusing
  one `installTrustedTypes()` installs. Without the first, every rich-text render throws; see
  [`03-deployment.md`](03-deployment.md) §3. That a policy is the audited place a string
  becomes markup is the same claim the brand above makes, stated to the platform instead of to
  the compiler.
- **The client sanitiser is not the server's.** anvil sanitises on write and re-sanitises at
  render because a stored value that fails re-sanitisation means something bypassed the write
  path. The client's pass is a third one, and it exists for the narrower reason that markup can
  reach the DOM from a cache, a broadcast or a replayed stream event that no server render
  touched.
- A published section and a draft section are two documents to anvil. They are two cache
  entries here, and a draft is never served from the published key.
- **A label list that is not one per declared locale fails generation.** The array is indexed by
  the locale table's order, so a short one is an index out of range the moment an editor
  switches language — the defect nobody sees in the locale they develop in.
- **A label is the one descriptor string with no character class of its own**, because a class
  tight enough to be worth having would refuse a script. What it is held to is the characters
  that are not text at all: the C0 and C1 controls, and U+2028 and U+2029, which
  `JSON.stringify` does not escape and which a parser older than the JSON-superset rule reads as
  a line break inside a string literal. The same holds for a `choices` value, which is a stored
  value rather than a name the generator spells.
- **A section is emitted as its own `const`, and the registry as a table beside it** (§14). A
  section's rows carry words in every declared locale, so a screen that edits one section
  shipping all of them is the difference between a few hundred bytes and all of them.

---

## 9. Notification topics and templates

### What hammer ships

The inbox store, the SSE subscription with `Last-Event-ID` resumption, dedupe by event id, a
bounded per-connection ring, and **one connection per session across all tabs**.

### What the descriptor carries

Each topic's `key` and stored `code`, its `fanout` and `scope`, its `default_channels`, a
`coalesce_window_s`, a `retention_days`, whether it is `user_optional`, whether a denial is
`stealth_on_denial`, the `perms` it requires, and its `visibility`.

```jsonc
{ "key": "form.submitted", "code": 1, "fanout": "read", "scope": "resource",
  "default_channels": ["in_app", "email", "webhook"], "coalesce_window_s": 600,
  "retention_days": 90, "user_optional": true, "stealth_on_denial": true,
  "perms": ["FormRead"], "visibility": "holder" }
```

### Notes that are not obvious

- **A permission-gated topic carries `visibility: holder`, and its KEY is subject to §4.1.**
  Subscribing to a topic is the disclosure — the same way calling a route is — so a holder
  topic's key may not be emitted as a value into a bundle any more than a holder route's path
  may. It is the same rule, applied to the other table, and the emission honours it the same
  way: `key` is `null` on a holder topic's `const`, the key stays in the erased `HolderTopicKey`
  union, and `publicTopics` aggregates the public tier because it is the only tier that has a
  key to aggregate.
- **The `code` is emitted for every topic, and it is what a holder topic is matched on.** It is
  a bit position in a stored preference mask, append-only the way a permission bit is — and with
  the key withheld it is also the identity: a preferences response carries both, filtered by the
  server to what that holder may subscribe to, and the client re-attaches the entry to the
  generated `const` by the half it has.
- **The withholding is stricter here than it is for a route**, and the difference is worth
  knowing. A route id is in whichever bundle calls it; a holder topic's key is in NO bundle,
  including the one holding the `const`, because the key is what subscribing takes. That is not
  a boundary either — anvil refuses the subscription, and refuses a `stealth_on_denial` topic as
  though it were not there.
- **A descriptor whose `visibility` disagrees with its `perms` fails generation, in both
  directions.** anvil derives one from the other, so a disagreement is not anvil's output:
  `public` with a permission is a key emitted into a bundle the permission was meant to gate,
  and `holder` with none withholds a key nothing protects, which reads as a gate that is not
  there.
- **A user with twelve tabs must not be twelve connections.** anvil derives its SSE ceiling
  from `RLIMIT_NOFILE`; the client's share of keeping that ceiling meaningful is the leader
  election. The follower tabs receive the events over `BroadcastChannel`.
- **Every handler is idempotent and dedupes by id.** A reconnect replays from `Last-Event-ID`,
  so an event arriving twice is normal operation, not an error.
- **Placeholders are carried so the application's copy can be checked against them.** A
  template rendering `{order_ref}` into a sentence that never uses it is a silent blank in one
  locale, which is anvil's reason for checking placeholders across all locales at compile time.

---

## 10. Analytics events and consent

### What hammer ships

The event sink: consent gate, batching, and delivery on `pagehide` with `sendBeacon`.

### What the descriptor carries

Each event's `name` and stored `code`, its `class`, whether it `requires_consent`, and its
`dimensions` — each a name and the closed set of `values` it admits.

```jsonc
{ "name": "PageViewed", "code": 0, "class": "behaviour", "requires_consent": true,
  "dimensions": [ { "name": "surface", "values": ["web", "ios", "android"] } ] }
```

### Notes that are not obvious

- **A dimension's values are a closed set, so a typo is a compile error.** The generated union
  is the values the server will accept; an event sent with a value outside it is a row the
  server drops, and dropped rows are the analytics defect nobody notices for a quarter.
- **Consent is at the door, and an event that requires it is not queued.** Not queued and
  filtered later, not buffered pending a decision — anvil's `ingest.h` makes exactly this
  argument server-side, and "recorded and then excluded" is a policy one forgotten filter away
  from being no policy at all. A buffer that flushes when consent arrives is a buffer of
  pre-consent data, which is the thing consent was about.
- **The client does not sample.** anvil samples whole sessions, deterministically, above a
  high-water mark; a client that also sampled would produce a compound rate nobody can reason
  about.
- **`sendBeacon` and not `fetch` on `pagehide`.** A `fetch` from an unloading document is
  cancelled by the browser without an error, which is how a funnel loses precisely its last
  step — the one every drop-off analysis is about.
- **An event is emitted as its own `const` and there is no table of them** (§14), for the reason
  there is no table of routes: a call site names the event it reports, and an aggregate would
  put every event's name into any bundle that reported a single one.
- **A duplicate dimension value fails generation.** The row stores the index into the set, so
  two names for one stored number are two readings of every row already written.

---

## 11. Rate limits

### What hammer ships

A local token bucket per declared bucket, checked before a request is admitted.

### What the descriptor carries

Bucket name, `window_ms` and `max_events`. Milliseconds are named in the key, because a duration whose unit lives in a comment is a duration that gets read as seconds.

### Notes that are not obvious

- **It is advisory, and it is not the limit.** The server's is. The client's copy exists so an
  interface does not spend a user's budget on its own retries and then present them with a
  lockout they did not cause.
- **A `429` holds back the whole bucket, not the one call.** Every queued request against that
  bucket waits out the `Retry-After` the server named. Retrying the other nine calls while one
  waits is how a rate limit becomes a lockout.

---

## 12. Media namespaces, and the limits

### What hammer ships

The image element (`srcset`, `sizes`, `loading`, `decoding`, intrinsic dimensions to prevent
layout shift), and the upload: size and type checked **before the first byte**, streamed from
the `File`, progress from the stream.

### What the descriptor carries

A `default_role`, and one entry per namespace: its `ns`, the media types it `accepts`, and
the `roles` it serves, each with the `width` that role resolves to. The global `limits` block
is separate (§1).

```jsonc
{ "default_role": "card",
  "namespaces": [ { "ns": "content",
                    "accepts": [ "image/jpeg", "image/png", "image/webp", "image/avif" ],
                    "roles": [ { "role": "thumb", "width": 320 },
                               { "role": "card",  "width": 1024 } ] } ] }
```

The address of a stored image is a route like any other — `media.object`,
`GET /media/{ns}/{id}/{role}`, `public` — so it comes out of the route builder and not out of
this table. It did not always: this section published the role and the width and said the
address still came from the route builder, while the reference emitter carried no route to
build it with, so the pattern was written out in the application. That was a row under
`docs/15-tasks.md` §Cross-repo and it is closed.

### Notes that are not obvious

- **Bytes never enter the JS heap** (`ENGINEERING_RULES.md` §2.2). Format negotiation is `Vary: Accept`,
  which is the browser's job and not a branch in the client.
- **The public grammar is still a ROLE, and the width is published beside it.** This section
  once recorded a tension and left it open: anvil's `fs/namespace_spec.h` kept the width ladder
  server-side because "a client that knows the ladder is a client that will start building
  paths from it again", and a responsive `srcset` cannot exist without width descriptors —
  without them the browser has no basis on which to choose between the sources it is handed.
  The media table settles it, and it settles it the way the objection was actually shaped: the
  request is still `GET /media/{ns}/{id}/{role}`, the client still constructs no path, and the
  number is published so the `srcset` can name it. Knowing a width was never the hazard.
  Building a URL out of one was, and the route builder is what makes that unspellable.
- **`Content-Length` is a hint to the server; `file.size` is a fact to the client.** The check
  here saves an upload that was going to be refused, and it is not the enforcement — anvil
  enforces during the stream, because a client-side check is a check an attacker skips.
- **A byte cap is in bytes and a text bound is in code points, and they are never conflated.**
  Uploads are bytes; everything a human typed is code points.
- **`default_role` has to be a role every namespace serves**, and a descriptor where it is not
  fails generation. It is what a request with no role segment resolves to, so a namespace that
  does not serve it turns forgetting the segment into a 404 found in production.
- **What is emitted is the role, the width beside it, and nothing else** — no extension and no
  rule leading from a width back to an address. The width is there so a `srcset` can name it;
  the address still comes from the route builder.
- **`accepts` is the server's list, and the client's is not a second one.** It reaches two
  places that have to agree: the `accept` attribute on a file input, which decides what the
  picker offers, and the check that runs against `File.type` before a byte is sent. An
  application that wrote it down itself kept a copy of something the server already enforces,
  and the copy is the one that goes stale — the symptom being an upload the picker offered and
  the server refused, or a file the picker hid and the server would have taken. This too was a
  §Cross-repo row, listed rather than worked around for exactly that reason.
- **The accept list is `type/subtype`, lowercase, with no parameters**, which is the essence
  string a browser puts in `File.type` — so the comparison is `===` rather than a parse. A
  descriptor carrying a value with a parameter or a capital fails generation, because a value
  that never matches what it is compared against refuses uploads for a reason nobody can see.
- **It still is not the enforcement.** A namespace's accept list saves an upload that was going
  to be refused and shapes a picker; anvil sniffs the bytes, and a client-side type check is a
  check an attacker skips (`ENGINEERING_RULES.md` §5).
- **`onProgress` goes BACKWARDS when an attempt is retried**, and the number is the honest one:
  a retry sends the file again from the start, so the bytes really are leaving the device a
  second time. A bar that latches its maximum is drawing a claim about what the server holds
  that is not true. Each attempt gets a fresh stream because a stream is spent by the attempt
  that sends it — reusing one made every retry after the first a re-send of nothing, which is
  the defect the first end-to-end run of `client.upload` found (`CHANGELOG.md`, Unreleased).
- **`client.send` takes a one-shot body as a factory, not as a value.** `client.upload` is the
  only caller inside hammer and it already does; an application shaping its own streamed body
  passes `() => stream()`, and the type refuses a bare `ReadableStream` rather than letting a
  call retry itself into its own attempt cap. Everything else a `BodyInit` can be is
  re-readable and is passed as itself.

---

## 13. What the application still writes

The descriptor carries what the server knows. Three things it cannot know, which the
application declares in TypeScript and hammer requires:

| | What it is | Why it cannot be generated |
|---|---|---|
| **Copy** | every user-visible string hammer's own surfaces need, per locale — the error-code and validation-reason maps above all | words are an audience decision, and hammer ships no string in any language (`ENGINEERING_RULES.md` §1) |
| **Class names** | what each component's parts are called | styling is the application's, and a library that ships class names ships a design system |
| **Invalidation** | which resources a mutation invalidates | the server knows what a write touches; it does not know what a screen is showing |

Each is a plain object checked against a generated type, so **an addition server-side is a
compile error in the client until it is answered** — a new validation reason, a new locale, a
new error code. That is the same bargain anvil's `static_assert`s make, in the only currency
TypeScript has.

**Not every word a person reads is in these three.** A section field's labels are declared in
the application's own C++ table and are generated into the client with the rest of the section
schema (§8), so they are written once rather than twice. That is not a hole in the rule — the
rule is that hammer ships no string and that an application keeps no second copy of a table,
and a label generated from the one place it is declared satisfies both. What lands here is what
the server has no opinion about: the words for an error code, a validation reason, and whatever
a component needs to name itself.

---

## 14. The generation contract

```sh
hammer codegen --descriptor path/to/hammer.descriptor.json --out src/api
```

- **One file, `hammer.generated.ts`, in the directory `--out` names.** Not one module per
  audience: §4.1 retired that idea along with the belief that code-splitting was access
  control. The split that survives is the one TypeScript already draws between what is erased
  and what is emitted, and it needs no second file to express.
- **The output is committed** in the consuming application, and in `tests/testapp/` here. A
  generated file nobody reads is a generated file nobody reviews, and this one contains the
  authority model.
- **`tools/check-descriptor.sh` regenerates into a temporary directory and diffs.** A
  difference is a build failure with the command to fix it. Because the committed bytes were
  written by an earlier run in another process, that diff is also the determinism assertion:
  a generator that iterated an unordered container would fail it on every run.
- The generated module is marked `DO NOT EDIT`, carries the descriptor hash, and is
  side-effect-free so a bundler can drop every table an entry point does not use.
- **Nothing above `tables` reaches the output** — not `app.version`, not `emitted_by`. It is
  the same split the hash makes: a release that changes no table must not regenerate every
  client, or the diff it produces is one nobody reads.
- **A permission is a `const` holding its bit, and there is no table of names.** A membership
  test is `permissions.has(kPermContentRead)`, so the names live in a bundle only where code
  spells one — and since the identifier is minified and the union is erased, a built bundle
  carries no permission name at all. An application that has to *show* a name writes
  `Record<Permission, string>`, which is copy, which is §13's, and which is then total by type.
- **What decides whether a table is emitted as `const`s or as one table is how a consumer
  resolves a row.** A row a call site SPELLS is its own `const` — a permission, a route, a
  section, an event — because a bundler can drop an export nothing references and cannot drop
  one member of an imported object. A row that arrives at RUN TIME is a table: a field type
  resolved by the `code` a definition carries, a media width resolved by the namespace an object
  was stored in. There is no bundle in which a subset of those is the useful part, so the table
  is offered whole and the consumer is told it is paying for all of it. `publicRoutes` and
  `sections` are that same offer over tables whose rows can also be spelled.
- **Every vocabulary anvil owns is emitted as a union of exactly what the table holds** — the
  answer kinds, the section field types, the event classes, the media roles. None of them is
  written down on hammer's side, because a copy of an enum in a generator is the copy nobody
  updates: a member anvil appends would be a generation failure in every application on the day
  the server started sending it.
- **A topic is a `const`, and a holder topic's `const` has no key in it.** `publicTopics` is
  the same offer `publicRoutes` is, over the only tier that can be a table (§9).
- **A route is a `const`, and there is no table of routes.** `publicRoutes` exists because the
  public tier is all-or-nothing anyway; there is deliberately no holder equivalent, because an
  aggregate would put every route's id, permissions and rate-limit bucket into any bundle that
  touched one of them. A client that resolves an id at run time is handed the routes the
  application chose to give it.
- **What the generator refuses.** The checks of §"Three rules" above, plus three the emission
  itself needs: a path whose parameters cannot be parsed, two names that land on one identifier,
  and any string bound for the output that holds a character its kind cannot. The content tables
  add four of their own, each of them a contradiction rather than a typo: a PII field type that
  also declares an answer shape (§7), a label list that is not one per declared locale (§8), a
  duplicate dimension value (§10), and a `default_role` a namespace does not serve (§12). Every emitted
  string is escaped with `JSON.stringify` as well, so a descriptor cannot write source into the
  module — but that is the second line and not the first, because a guarantee resting on one
  call is a guarantee a refactor removes without noticing. The reverse of a route naming something absent — a capability scope no route
  requires, a rate-limit bucket no route names — is a **warning**, not a failure. anvil limits
  by keys that are not route buckets and mints scopes for operations its emitter does not carry
  a route for, so the reference descriptor has six of them; failing there would reject anvil's
  own output.
- **The emitter is anvil's, not hammer's**, and it exists: `anvil/descriptor/descriptor.h`
  with `tests/testapp/emit_descriptor.cc` as the reference application's copy of it, which is
  thirty lines because every table it reads is already validated by a `static_assert` above it.
  It links `anvil::foundation` and nothing else — generating a client needs no driver, no event
  loop and no database.
- **The route table is two tables on anvil's side, and that is not an accident of this
  format.** `RoutePolicy` is scanned linearly on every protected request, so the build-time
  fields — the id, the capability, the bucket, the cursor, the idempotence — live in a separate
  `RouteDescription` table rather than as six more fields pulled through L1 on a path whose
  whole design is that it does no work. `descriptions_match()` keeps the two in agreement at
  compile time.

## 15. What the descriptor must never carry

Any of the descriptor's content may reach a browser bundle, so it is written as though all of
it will. It carries **names, shapes, bounds — and the application's own labels for its own
content (§8)** — and nothing else.

What decides whether a given entry may be emitted as a VALUE is §4.1, not this section: a
holder route's path and a holder topic's key are in the descriptor and are in no bundle. The
descriptor is the input to that decision, not the output of it.

- No secret, pepper, signing key, connection string or internal hostname.
- No collection or database name. The client has no business knowing the storage layout, and a
  name in a bundle is a name in an attacker's notes.
- No index definition, no query, no migration step.
- Nothing that only exists to be filtered out again. Audiences were the first draft of §4 and
  are gone: the descriptor carries every route, and the EMISSION is what withholds a path
  (§4.1). A table that had to be trimmed before it was safe to hand over would be a table
  somebody eventually hands over untrimmed.
- Nothing about another tenant, another application, or a feature flag whose existence is the
  secret.

And one rule about the file rather than its contents:

> **The descriptor is a build artefact and is never served.** It carries every route's path,
> `visibility` included, which is the whole table §4.1 goes to trouble not to emit. A copy in
> the application's published output directory hands it over in one request, and it is an easy
> mistake to make because a JSON file looks like an asset. The consuming application asserts
> its absence from the built output, in the same check that diffs the generated client.

---

## 16. What the application supplies at construction

The descriptor carries what the server knows. A deployment is not in it — anvil emits tables,
not the hostname it happens to be reachable at — so the facts below are handed to
`createClient` by the application, which is the participant that knows them.

`ClientConfig` has **five** required members. Everything else has a default, and a single-tab
application needs none of them.

| | What it is | Why it is not generated |
|---|---|---|
| **`api`** | `kApiTables` from the generated module: the error and reason value maps, `rateLimits`, `singleUse`, `bodyMaxBytes`, and the descriptor `hash` that pairs them with `Api` | It IS generated — it is passed in rather than imported, because a library that imported one application's module would carry that application's tables (§1) |
| **`origin`** | `pageOrigin` (`location.origin`) and `apiOrigin`, plus `site` when the two differ, **unvalidated** — `createClient` is what checks them | A descriptor is a build artefact shared by every environment; an origin is a property of one deployment of it |
| **`refreshRoute`** | the route a credential refresh is made to | Neither hammer nor the generator can know which route it is: the descriptor carries the route and nothing that marks it as the refresh, so it is the one table member an application still names by hand (`docs/15-tasks.md` §Cross-repo) |
| **The platform's singletons** | `fetch`, `navigator.locks`, a `BroadcastChannel`, the monotonic clock | Injected rather than read, so a test supplies its own instead of racing every other test in the file (`ENGINEERING_RULES.md` §3.3). A null lock manager is a degraded election, not an error |
| **`session`** | how to read the current session and how to refetch it | The store that holds a session is the thing that will dispose it; a client with a session of its own would be a second opinion about who is signed in |
| **`telemetry`** | where one record per request and hammer's own counters go | This library declares no metric name in an application's namespace and ships no reporter (`docs/00-architecture.md` §9) |
| **`onLogout`** | what this tab drops when the session ends | hammer knows the session ended; the application knows what is on the screen |

**The four tables travel as one member, and that is the point of `kApiTables`.** They are one
descriptor's, they are emitted together, and passed one at a time they were four places to hand
a client a table from a descriptor other than the one its routes came from — a rate-limit table
from last month's regeneration, a body cap typed in by hand, an error vocabulary narrowed to
another application's unions. Nothing in the type system was looking at any of it. The
vocabulary is now built inside the client from the value maps the module already exports;
`errorVocabulary` stays exported for a consumer that decodes an envelope itself (§17).

**The tables carry the descriptor's hash, and that is what pairs them with `Api`.** Bundling
the four closed the gap between them and left one between the set and the type parameter: an
application with TWO generated modules could still write `createClient<OneApi>({ api:
kOtherApiTables })`, because every generated tables object is structurally the same shape — four
string-keyed records and a number — and nothing in the type distinguished them. What had been
holding was that both names came out of one file, which is a convention rather than a check.

So `kApiTables` carries a sixth member nothing decodes with: `hash`, the descriptor's own, and
`Api` declares `readonly hash: typeof kTablesHash`. Crossed, the two literals disagree and
`createClient` does not compile. Two modules generated from the same descriptor pair freely,
which is correct — they are the same tables. **The check is a hash rather than a brand** because
the generated module imports nothing and can therefore not name a `unique symbol` hammer
declares: a phantom that has to be optional in order to compile constrains no object literal at
all, which is what the first attempt at this was. The descriptor hash was already the identity
of the table set (§14), and a generator can write it down.

What it does not catch is a hand-written tables object that copies the literal, and nothing in a
type system would. The consequence of getting `SessionStoreConfig.clientHash` wrong is different
in kind and is left as a plain `string` for that reason: it is compared against the hash the
SERVER sends, so a wrong one reports a stale client, loudly, on the first session read. A
crossed decode table is silent.

**The type parameter is generated too.** `createClient<Api>` takes one type assembled from the
generated names — the parameters map, the responses map, the error code union and the validation
reason union — and every call is then typed by the route `const` it names. A route that does not
exist cannot be spelled, a parameter cannot be omitted, and a route declaring a capability scope
will not compile without a `Capability` for that scope. It is `export type Api` in the generated
module rather than four imports and a mapped type in each application, because a hand-assembled
copy of it is a copy that goes stale: the first application to consume this library wrote one
from a guide that predated the library it described. `tests/testapp/app/client.ts` is the proof
that all of it is satisfiable from outside hammer.

**A route's response type is the application's to declare**, by augmenting `RouteResponses` in
the generated module. anvil validates requests through a schema binder and writes responses by
hand, so there is nothing in the descriptor to emit that would not be a guess; an undeclared
route answers `unknown`, which forces the call site to narrow loudly rather than inherit a lie.

### The origins, and what is refused

`docs/00-architecture.md` §8.4 states the invariant: the API is same-**origin** with the
application. What construction does about a deployment that is not:

| | Verdict |
|---|---|
| Identical origins | accepted, no preflight |
| Same host, different port | accepted, `crossOrigin` recorded — every mutating request pays a CORS preflight |
| Different host inside a declared `site` | accepted, `crossOrigin` recorded |
| Different host with no `site`, or one the declared site does not contain | **refused** |
| Same host, different scheme | **refused** — site comparison is schemeful, and a cookie set on one is not sent to the other |
| `http:` on anything but loopback | **refused** — `__Host-` cookies require `Secure` |

**A refusal here is a throw**, not a `Result`, and it is the one place in this library that is
true: a misconfigured client is programmer error (`ENGINEERING_RULES.md` §3.1), and the alternative is a
deployment whose every request is silently anonymous. `SameSite=Lax` cookies are not sent on a
cross-site subresource request, so the session is already gone; the only question is whether it
fails at construction with a reason or at the first login with a 401.

`createClient` is what throws, and it takes the **unvalidated** pair. `defineApiOrigin` is still
exported and still returns a `Result`, for two reasons: the decision can be made and tested
without constructing a client, and a deployment that would rather render the reason than crash
on it can. What is gone is the unwrap-and-throw that sat between them in every application,
written identically each time. The validated value is readable afterwards as `client.origin`,
because `crossOrigin` records a preflight paid on every mutating request and nothing else can
observe it — an `OPTIONS` is invisible in a waterfall unless somebody is looking for it.

**`site` is a claim the application makes and hammer checks.** Deciding "same site" for two
arbitrary hosts needs the Public Suffix List: thousands of rules, revised monthly, and a stale
copy is wrong in the direction that matters — it calls two hosts on a shared hosting suffix one
site. A library with a zero-dependency rule does not carry that table and must not guess, so
the application names the registrable domain it believes the two share and both hosts are
required to lie within it. A wrong claim fails loudly at construction rather than quietly at
run time; a claim that is right costs nothing.

---

## 17. The headers hammer sets

anvil's framework never reads a request for these: `IdempotencyStore::claim` takes the key as an
argument and says so — "whether a request with NO key reaches here at all is the caller's
decision" — and a capability is redeemed by the handler that consumes it. So the spelling below
is hammer's, and an application's handler is what reads it. **A handler that does not read one
is not broken; it is a route that has not opted into the mechanism.**

| Header | On | What it carries |
|---|---|---|
| `Idempotency-Key` | every request to a route the descriptor marks `idempotent: false` | A key minted per call and reused by every replay of it, so at-least-once on the wire is at-most-once at the server |
| `Capability` | a call to a route whose descriptor entry names a scope | The opaque token the mint call returned |
| `Content-Type` | a request with a body | `application/json; charset=utf-8`, or the file's own type for an upload |
| `Last-Event-ID` | a stream reconnect | Where the replay resumes from. A header rather than a query value because it is not part of the address: the same stream resumed twice is one resource |
| `Accept` | every request | `application/json`, or `text/event-stream` for a stream |

Three rules about all of them:

- **No `X-` prefix**, per RFC 6648. The prefix was deprecated because a header that later gets
  standardised has to be renamed, and renaming a request header is a breaking change for every
  deployment that reads it.
- **A header is not a URL**, and that is why the capability travels in one: a header is in no
  address bar, no history entry, no `Referer` and no analytics payload, and the redacting logger
  drops every header rather than choosing between them (`ENGINEERING_RULES.md` §5).
- **A custom header makes a cross-origin request preflight.** It is one more reason §16's
  same-origin invariant is worth keeping; it costs nothing at all when it holds.

---

## 18. What the state layer needs

`hammer/state` holds no application's data either, and the things it cannot know are a
different list from §16's. §16 is about a DEPLOYMENT — where the API is, what the platform
gives you. These are about the shape of an application's own screens and its own vocabulary,
and every one of them is supplied to a constructor rather than imported by it.

`tests/testapp/app/state.ts` supplies all of them, and is the proof that each can be satisfied
from outside hammer — at the same time, in one construction order, which is the half that
type-checking alone does not prove.

| | What it is | Why it cannot be generated |
|---|---|---|
| **The identity reader** | where the user id is in a session body | The payload past the three fields `decodeSessionView` reads is the application's (§4). The cache is keyed by whatever comes back, so this is the function that decides whose entries are whose |
| **The session read** | how to fetch a session, and at what address | Two reasons, and the second is a gap. The client takes the session store as its `SessionSource`, so the two cannot each be constructed first; and anvil describes the session route as `authenticated`, so §4.1 withholds its path and there is nothing to resolve on a cold load. See the note below |
| **Resource classes and their ceilings** | which bounded cache a route's entries live in, and how many fit | A ceiling is a judgement about a screen — a list view paging through five hundred rows is not a session response. hammer requires only that a class exists and is bounded, because a tab stays open for days |
| **The page query** | how the server spells a limit and a cursor | anvil's list routes read them from a query whose names are the application's route contract. The descriptor names the cursor FIELD, not the parameter |
| **The section query and stage** | how a section key and its stage reach the request | The same reason. What hammer insists on is that the stage is never optional, so a draft cannot be read into the published key |
| **The section control map** | which stored field type each section control writes into | `SectionFieldType` and `FieldTypeName` are two closed sets in the descriptor, and the bridge is a product decision: whether a rich-text control stores `TEXT_LONG` is an application's answer. Written `satisfies Record<SectionFieldType, FieldTypeSpec>`, so it is total by type |
| **The validation-reason names** | which member of the descriptor's enum each form failure is | The enum is append-only server-side and the words for it are §13's. hammer takes the names as a parameter so it spells none |
| **The inbox decode and badge rule** | what a notification event's payload is, and which topics deserve a badge | A notification's shape is a response shape (§4), and which topics count is a product decision (`ENGINEERING_RULES.md` §9) |
| **The media origin and grammar** | `MEDIA_ORIGIN`, and the route an image is addressed by | The origin is a deployment fact. The route should come from the descriptor and does not yet — see the note below |
| **The worker factory** | a function returning a `Worker` | `new Worker(new URL("./x.js", import.meta.url))` is a bundler contract, and a library with a zero-dependency rule has no bundler and takes no dependency on one. The application's worker entry is two lines against `serveImagePool` / `serveDecodePool` |
| **The beacon target and the analytics delivery** | where a batch goes, and where the LAST batch goes | An ingest route is an application's route, and the wire shape of a batch is a response shape from the other direction |
| **`pagehide`** | when to send the last batch | `hammer/state` names no document (`tools/check-layering.sh`), and a library adding a listener to a document it did not create is a listener nobody can remove |
| **The count sink** | where this layer's tallies go | hammer declares no metric name in an application's namespace and ships no reporter (`docs/00-architecture.md` §9) |

### Two addresses an application should not have been writing — closed

Both were the same shape of gap, both were rows in `docs/15-tasks.md` §Cross-repo, and both
were left uncomfortable rather than papered over, because a workaround in a library is a
workaround in every application built on it. The pressure stayed where the fix belonged and the
fix arrived there.

**The session route** was described as `authenticated`, so the generator withheld its path the
way it withholds every holder route's — and the address of the session arrived *with* the
session, which on a cold load does not exist. The application wrote `/session` out by hand, with
a `visibility` override to get it past the type. anvil describes `session.current` as public
now, for the reason `auth.refresh` already was: its credential is a cookie, and gating a route
on the credential it exists to establish makes it work only while it is unnecessary.

**The media grammar** was published here as a role and a width, with the address said to come
from the route builder — while the reference descriptor carried no route to build it with, so
the pattern was written in the application. anvil describes `media.object` now, and `MediaConfig`
takes the generated route.

**What is left is one address, and it is not of this shape.** `refreshRoute` is still named by
the application, because the descriptor carries `auth.refresh` as an ordinary public route and
nothing that marks it as the one a credential refresh goes to. A generator that guessed would
guess wrong silently, in the one call that decides whether a session survives — so the
application names the `const`, which is one line whose absence is a compile error rather than a
401. See §16.

### Notes that are not obvious

- **A resource handle must be released.** It is refcounted: two components reading one address
  share one entry, one store and one request, and the entry's request is aborted and its store
  closed when the last watcher lets go. A handle nobody releases is a request that outlives the
  screen that wanted it (`ENGINEERING_RULES.md` §3.3).
- **Freshness has no default and no constant to tune.** A response with no `Cache-Control` is
  one the server said nothing about, and saying nothing is not permission — it is read again on
  the next open. Stale is served only where `stale-while-revalidate` granted it, and serving
  stale without revalidating is not a state the code can be in.
- **An optimistic value reaches the store and never the cache.** That is what stops a second
  reader of the same address inheriting a value the server has not seen, and it is why both
  write mechanisms refuse to start while one is outstanding rather than trusting a call site to
  remember.
- **An invalidation re-reads what is watched and drops what is not.** What refetches is bounded
  by what is on a screen rather than by everything the tab has ever read, which is what keeps a
  broadcast from being a thundering herd across twenty tabs.
- **The unread count is the server's.** The stream adjusts it so a badge appears on arrival, and
  the next server answer replaces that adjustment outright rather than reconciling against it.
- **`imagePool`'s size is a memory cap.** Two workers, one bitmap each, zero queue. A 12 MP
  photo decoded to RGBA is about 48 MB, and the third concurrent one is a killed tab rather than
  a slow one. A refusal is handed back to the caller, because the answer — send the original,
  ask again later, say so on screen — is one only a caller has.

---

## 19. What the dom layer needs

§18 is what a *store* needs: an address, a shape, a ceiling. These are what a *component* must
be handed before it can draw anything, and there are more of them for one reason — a component
is the layer where a person reads words and sees a design, and hammer ships neither.

`tests/testapp/app/screen.ts` supplies all of them, and is the proof that each can be satisfied
from outside hammer for every component at once.

| | What it is | Why it cannot be generated |
|---|---|---|
| **The mount point** | the element to build in | The document arrives *through* it and never from the global, so a component renders into a preview, a print view or a frame with no branch for it, and a test supplies its own (`ENGINEERING_RULES.md` §3.3) |
| **The class names** | `ClassNames<Part>`, where the `Part` union is the component's and the strings are the application's | A library that ships class names ships a design system (§13). The union is hammer's because only the component knows what its parts are; the spelling is the application's because only it knows its design |
| **The words** | one copy object per component, total over its own slot union | §13's rule, at component granularity. `Copy<Locale, …>` is the *failure* vocabulary — an error code and a validation reason — and a component's own words are a second seam rather than a growth of that one. A slot that embeds a number is a function, because plural rules and digit shaping belong to the locale, not to the count |
| **The store** | `Readable<T>` — the read side only | A component handed a store it can write is a component that can decide somebody consented, marked a notification read, or is signed in. It renders a value and calls back to change one |
| **The action callbacks** | `markRead`, `signIn`, `signOut`, `upload`, `more`, `setConsent` | Every one of them is a request against a route, and `hammer/dom` may not import `hammer/wire` (`docs/00-architecture.md` §2). The component owns *when* and the application owns *what* |
| **The locale index** | which entry of the descriptor's label arrays to read | Locale order is persisted server-side and append-only, and index 0 means nothing in particular (§5). Which locale a person is reading is the application's answer |
| **An image's intrinsic box** | `width` and `height`, in CSS pixels | The media table publishes a width per role and no height (§12), and the aspect ratio of a particular image is not in any table. Without both attributes every image is a layout shift |
| **The upload bounds** | `maxBytes` and the accepted media types | `maxBytes` is the descriptor's; the accept list is the application's because anvil has no table of them to emit — a cross-repo row in `docs/15-tasks.md` |
| **The monotonic clock** | for anything that counts down | Expiry is the server's and a countdown is rendered from a *duration* (`ENGINEERING_RULES.md` §6). The clock is injected so a test does not wait out a real one |

### The component contract

Every renderer in this layer has one signature and one lifetime:

```ts sketch: the shape every renderer returns, not a call site
renderX(mount: Element, options: XOptions): Mounted      // { element, close }
```

- **`close` is not optional to keep.** A component subscribes to stores, opens resources that
  are refcounted, and may hold a timer; a caller that drops the handle leaks all three in a tab
  that stays open for days. It is idempotent, and it releases in reverse order of construction
  so a subscription taken out over a resource comes off before the resource does.
- **A component holds mechanism and never a name, a word or a look** (`ENGINEERING_RULES.md` §9). The bell
  owns the unread count reconciled against an at-least-once stream, the popover's focus and its
  return, and a live region that announces without stealing focus. It owns no topic name, no
  sentence and no colour.
- **A gate hides an affordance and cannot refuse a call.** There is no error in this library
  meaning "denied here" (`docs/00-architecture.md` §4.1), and a component may not invent one: a
  local refusal turns a stale permission copy into a denial no server-side change can clear.

### Notes that are not obvious

- **The one insertion site takes a `SanitizedHtml` and is not `innerHTML`.** It parses into an
  inert document and imports the nodes, which is what lets the built-output assertion be that
  the string `innerHTML` appears nowhere in a bundle at all — a `ban-exempt` line would satisfy
  the source check and leave the bundle claim untestable. It also means hammer needs no
  permissive Trusted Types policy, because the parser it uses is not a Trusted Types sink.
- **A Trusted Types default policy is installed by the application, never by importing a
  module.** A library that installed a throwing default policy on import would break the
  application's own markup from a side effect it never asked for (`ENGINEERING_RULES.md` §2.1). It takes
  the scope as a parameter and hands back the removal.
- **`"unknown"` is a state every gate has to render differently from a refusal.** A session that
  has not answered yet is not a signed-out session — rendering one is a login form that flashes
  at everybody who is already signed in — and consent that has not been asked for is not consent
  that was declined.
- **A 404 renders as not-found and never as a denial.** anvil answers a denied request on a
  stealth route with a byte-identical 404 so a probe cannot tell a missing object from a
  forbidden one, and a client that renders "no permission" there hands back the oracle the
  server spent a whole design removing (§2).
- **`request_id` is displayed and never interpreted, and it is usually absent.** anvil's 404 body
  is a single constexpr string with no request behind it to have an id, so a surface that assumed
  a string renders the word `undefined` on the most common failure there is.
- **A component is enumerated by the accessibility suite rather than covered by it.** The suite
  iterates a registry and asserts that every `render*` export has an entry, so a new component
  fails until it is labelled, reachable and operable — rather than being tested if somebody
  remembers (`docs/16-test-plan.md`).

---

## 20. What the react adapter needs

Almost nothing, and that is the seam.

`hammer/react` binds the stores of §18 to one framework's lifetime and holds no behaviour of
its own — `tests/react/boundary.test.ts` asserts it as a gzipped ceiling and as an import
graph in which every edge into a layer below is `import type` but one. So there is no table
here to match §19's: an application supplies the stores it already constructed, and the hooks
supply the only thing React can: when a subscription starts and when it stops.

| | What it is | Why it cannot be generated |
|---|---|---|
| **The store** | `Readable<T>` — a session, a form, an inbox, a pager, a consent, or an application's own `Store` | `useStore` is the primitive the other four are typed wrappers of. The stores are constructed by whoever will dispose them (`ENGINEERING_RULES.md` §3.3), which is never a hook |
| **The `open` factory** | a `useCallback` returning `resources.open(route, …)`, stable over the address | The address is the application's, and its identity is what decides the lifetime — exactly as `useSyncExternalStore`'s own `subscribe` argument does |
| **The `perform` call** | `(signal) => api.call(route, { body, signal })`, handed to `run` at the moment of the write | Passed at call time rather than held from the render that declared it, so there is no closure to go stale: a callback captured at render time sends the value the field held two keystrokes ago |

### Notes that are not obvious

- **`useResource`'s factory must be stable, and an inline arrow is a request storm.** A new
  identity means release this entry and open the next, so an arrow rebuilt every render aborts
  and re-issues the read on every keystroke anywhere on the screen. A dependency array of
  hammer's own was considered and rejected: a caller who forgets a dependency gets an entry
  that never re-opens, which is the same footgun spelled so that it fails silently instead.
- **The handle arrives one render after the mount.** `view.resource` is what `optimistic()` and
  `writeVersioned()` take, and it is null until the subscription exists — which is one render on
  mount and never again, so no event a person can fire observes the null. `release` is on it and
  belongs to the hook: calling it from a component drops a watcher React still believes it has.
- **The stream is not opened or closed by `useInbox`.** It is leader-owned across every tab on
  one session (`wire/sse.ts`) and `close` is not scoped to one component — a bell unmounting
  while an inbox screen is open would take the connection out from under it. The lifetime
  belongs to whoever constructed the inbox.
- **A second `run` supersedes the first and aborts it.** What the adapter decides is only what is
  on SCREEN: a person who edited twice is looking at the second edit, and an older answer landing
  last is an interface showing a result that has been superseded. Whether a second write may be
  sent at all is not its decision — an unconfirmed value refuses the write built on it
  (`state/optimistic.ts`), and a double submit is a control the application disables from
  `state.status`.
- **A `perform` that throws is programmer error and is rethrown.** Failure belongs in the return
  type (`ENGINEERING_RULES.md` §3.1), so there is no error value of the caller's own vocabulary to publish
  and none is invented: the mutation goes back to `idle`, where it can be retried from, and the
  throw continues to whoever wrote it.
- **React is an optional peer and nothing below this layer names it.** An application that does
  not use React installs nothing and ships nothing; `tests/react/boundary.test.ts` asserts the
  edge in both directions, because a store that imported a hook would make `hammer/state`
  unusable without a framework.
