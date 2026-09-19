# 00 — Architecture

## 1. Component map

```
                    Internet (TLS)
                          │
                   ┌──────▼──────┐
                   │    Nginx    │  TLS, HSTS, body cap, rate limit,
                   │             │  sendfile via X-Accel-Redirect
                   └──┬───────┬──┘
        internal;     │       │
   /protected_storage │       │ proxy_pass
        (disk)  ◄─────┘       │
                              ▼
                   ┌─────────────────────┐
                   │   Drogon (C++)      │
                   │  Trantor event loop │  ← never blocks
                   ├─────────────────────┤
                   │ db_pool   cpu_pool  │  ← all blocking work
                   │ hash_pool audit_pool│
                   │   analytics_pool    │
                   └──┬───────────────┬──┘
                      │               │
              ┌───────▼──────┐  ┌─────▼─────┐
              │   MongoDB    │  │   Redis   │
              │ (replica set)│  │ cache,    │
              │              │  │ queue,    │
              │              │  │ rate lim. │
              └──────────────┘  └───────────┘
```

MongoDB **must** be a replica set even at single-node scale. Transactions are required by
several paths, and adding a replica set later is a migration under load.

## 2. Layering

Dependencies point downward only. A lower layer never includes a header from an upper one,
and the three-way target split makes a violation a **link error** rather than a review
comment.

```
controllers/**            transport: parse, validate, map domain errors to HTTP
      │
services/**               business rules, transactions, invariants
      │
repositories/**           the ONLY place a BSON filter literal may appear
      │
foundation/**             primitives: text, crypto, validation, value types
```

| Target | Contains | Links |
|---|---|---|
| `anvil::foundation` | core, crypto, i18n, input, auth, config, http, fs | OpenSSL, ICU, simdutf, xxHash, argon2 — **no web framework, no driver, no event loop** |
| `anvil::platform` | db, redis, images, accesscontrol, timer, `http/rate_limit.cc`, `http/content_headers.cc`, `http/client_address_drogon.cc`, `analytics/gauges.cc` | + Drogon, mongocxx, redis++, libvips |
| `anvil::app` | identity, audit, media, sections, forms, notifications, analytics | + `anvil::platform` |

**Two subsystems straddle the boundary deliberately**, and the reason is the same in both
cases: a module that links nothing but OpenSSL and ICU has to be able to ship a piece of one.

| Lives in `foundation` | Rest of the subsystem | Why |
|---|---|---|
| `forms/validators.cc`, `forms/pii.cc` | `forms/repository.cc`, the two services, the export | so a locale module can ship a field-type validator without dragging bsoncxx in |
| `notifications/render.cc` | the repository, publish, inbox, outbound, the transports | the template renderer is a `constexpr` lookup and a bounded substitution — pure CPU, and it belongs beside the seam it serves rather than the repository that stores what it renders |
| `analytics/counters.cc`, `analytics/snapshot.cc`, `analytics/openmetrics.cc` | `analytics/buffer.cc`, `sessions.cc`, `ingest.cc`, the repository, the rollup, the query | a counter is incremented inside every layer, `auth/password.cc` included, so the cell array cannot live above the layer that increments it |

`analytics/gauges.cc` is a third case and a smaller one: it is in `platform` rather than in
either half, because the two gauges it samples need a thread pool and a `mongocxx::client`
respectively and neither can be reached from `foundation`.

`http/` straddles the line for the same reason and along one seam: a function that takes bytes
lives in `foundation` and one that takes a `drogon::HttpRequest` or `HttpResponse` lives in
`platform`. `json_writer`, `csv_writer` and `html_writer` append into a `std::string` and are
below the line; `content_headers` sets five headers on a response and is above it. That is what
keeps `anvil_foundation_tests` linkable without an event loop.

That is also why `db::TimeMs` lives on the `definition.h` side of the forms line and
`FieldSpec` does not (docs/13 §"The subsystem is split across two link targets").

- A controller contains no BSON and no business branch. It validates, calls one service, maps
  the result. Longer than ~60 lines means logic leaked in.
- A repository takes and returns plain structs. It never sees an `HttpRequestPtr`, never
  throws an HTTP error, never logs.
- Services throw typed domain exceptions carrying an `ErrorCode`. Exactly one
  `code → HTTP status` table exists, in `http/errors.cc`.

`anvil::foundation` is separate because it is fast to build and trivially testable, and
because keeping a validator unable to reach a repository is worth a link boundary.

## 3. Thread pools

A **single** shared pool couples every workload's latency: one burst of image uploads
occupies every thread and every database query — including login — queues behind seconds of
CPU work.

Five independently sized pools, each with a bounded queue:

| Pool | Workload | Sizing rule | Queue full → |
|---|---|---|---|
| `db_pool` | All `mongocxx` calls (the driver is synchronous) | `== mongocxx::pool` max size. Threads beyond that just block on `acquire()` | `503`, `Retry-After: 1` |
| `cpu_pool` | libvips, EXIF, crop, encode | `hardware_concurrency()` minus loop threads | `503` |
| `hash_pool` | Argon2id **only** | Sized by **memory**: `RAM_budget / memory_per_hash`. Start at 4 | `503` — never queue unboundedly |
| `audit_pool` | The audit sink's flushes, and nothing else | Small and fixed — its work is one `insert_many` per second per batch | re-admit the changes to the buffer, drop the traffic, report the count once per flush, and stop ATTEMPTING a flush until the queue has room |
| `analytics_pool` | The event sink's flushes and the rollup's batches | Small and fixed — its work is one `insert_many` per flush interval | the same: re-admit, charge the compressible class, and defer the next attempt |

**`hash_pool` sizing is a security control, not a tuning knob.** Argon2id at 64 MiB per hash
with a 32-thread pool is 2 GiB of RSS reachable by one attacker sending 32 concurrent login
requests. The pool size *is* the memory cap.

**`audit_pool` is separate for a reason worth stating plainly.** Batching audit writes stops
the sink saturating `db_pool`; it does nothing about `db_pool` starving the sink. A batch
posted to the queue every request uses is refused precisely when that queue is full — which
is precisely the flood the rows describe. The forensic record goes blank for the one traffic
pattern it exists to capture. Capacity a request path cannot spend is the only fix.

**`analytics_pool` is separate from `audit_pool`, and that is not symmetry.** `audit_pool`
exists so a saturated request path cannot starve the record of what saturated it; putting a
second writer on it re-creates exactly that problem, and the second writer is the one that
floods — an analytics event fires on requests that are not interesting enough to audit. See
[`17-analytics.md`](17-analytics.md) §10.

**Rules, both of which are crash-or-corruption class:**

- Every task posted to any pool has its body wrapped in `try { … } catch (...)`. An exception
  escaping a pool task calls `std::terminate` and kills the process.
- Lambdas posted to a pool capture **by value**. `req` and `callback` are `shared_ptr`;
  capturing them by reference is a use-after-free the instant the handler returns.

An unbounded queue converts a login flood into an OOM kill, which is why every queue here is
bounded and "full" means shed.

**A shed answer carries `Retry-After`, and `http/retry_after.h` is the one place it is
spelled.** `kShedRetryAfterSeconds` is the constant this table names — a constant rather than
a measurement, because a bounded queue has nothing honest to measure: its depth is the work in
front of the caller, not the time that work will take, and a pool full now is almost never
full a second later. The stage-3 limiter's refusal gets a real number instead, from
`retry_after_seconds()`, because there the window is a fact Redis holds.

## 4. Request lifecycle

Every protected request runs the same ordered pipeline. **The order is a security property.**
Each stage is cheaper than the next, so hostile traffic is dropped as early and as cheaply as
possible.

```
 1. Nginx: TLS, client_max_body_size, connection/req rate limit
 2. Origin / Sec-Fetch-Site check         header compare, no I/O
 3. Per-IP rate limit                     one Redis op (or local token bucket)
 4. Access-token verify                   HMAC verify — NO database read
 5. perm_epoch check                      local cache hit; Redis GET on miss
 6. Permission bitset AND                 one CPU instruction — NO database read
 7. Schema validation                     bounded input only
 8. Capability-token consume              destructive routes only; one atomic op
 9. Idempotency claim                     non-idempotent routes only; one Redis op
10. Service                               first real business I/O
11. Response + explicit cache headers
```

Stages 2–7 complete with **zero database round trips** on the common path. That is the
performance thesis: an authenticated, authorised, well-formed request pays for exactly the
I/O its business logic needs and nothing more.

Stage 9 runs only where `RouteDescription::idempotent` is false and the client sent a key, and
it is placed after stage 8 rather than before it because a claim is a write: admitting a
request that the capability check is about to refuse would leave a marker behind for a request
that never happened. It is one Redis round trip and it may answer the request outright — see
[`01-seams.md`](01-seams.md) §7. A route that is safe to repeat never reaches it, which is why
the column that says so is part of the route description rather than derived from the verb.

Stages 1–3 complete with **zero allocations proportional to body size**.

Steps 4–6 run inside the Drogon `HttpFilter` and are pure CPU, which is also what makes the
stealth 404 timing-safe: a denied request and a nonexistent route both return without
touching the database, so response time does not distinguish them.

### 4.1 Whose address stage 3 counts

Every per-IP mechanism reads the client address from `http/client_address.h`, never from
`req->peerAddr()` directly.

Behind a reverse proxy, `peerAddr()` is `127.0.0.1` on every request. Reading it collapses
**every per-IP bucket into one bucket shared by the whole internet** — a five-signups-per-hour
limit becomes five signups per hour for all users combined, and the security log names the
loopback as the source of every denial. Nothing fails and nothing warns.

`X-Forwarded-For` is attacker-supplied on any request that reaches the application directly,
so reading it unconditionally replaces a collapsed per-IP layer with an absent one — strictly
worse, because an attacker then picks a fresh bucket per request. The rule is therefore:

1. `TRUSTED_PROXIES` is a list of CIDRs, **empty by default**. Empty means this process is the
   edge and the address is the socket's.
2. If the peer is not in that list, `X-Forwarded-For` is ignored **entirely**.
3. Otherwise the header is walked **right to left**, and the first entry not itself in the
   list wins. Each proxy appends, so everything left of the last trusted hop was written by
   whoever spoke to it — up to and including the client, inventing it.
4. A header of nothing but trusted hops falls back to the innermost peer. Never to nothing:
   the all-zero address is what an unattributable request packs to, and collapsing a real
   deployment into it is the same failure under another name.

The list is parsed and validated **at boot**. A malformed CIDR is a boot failure, because an
empty list behaves exactly like a correct edge deployment and a typo would restore the
collapsed-bucket bug in silence.

One function, one header, called by every site — not a fix applied eight times, which is how
the ninth call site gets it wrong.

## 5. `UserContext` injection

On success the filter attaches exactly **one** attribute to the request. Drogon's
`req->attributes()` is a string-keyed map of `shared_ptr<void>` — every attribute costs a
string hash, a map insert, and a control-block allocation. Five attributes cost five of each.

```cpp
// sizeof == 64: exactly one cache line. Verified by static_assert.
struct UserContext final {
    Uuid          user_id;      // 16  UUID bytes, not a string
    Uuid          session_id;   // 16
    PermSet       permissions;  // 16  one AND per check
    std::uint64_t perm_epoch;   //  8  revocation channel
    UserType      user_type;    //  1
    Locale        locale;       //  1
    std::array<std::uint8_t, 6> reserved;  // 6 — keeps sizeof stable across additions
};
static_assert(sizeof(UserContext) == 64);
static_assert(std::is_trivially_copyable_v<UserContext>);
```

The token's `iat` is deliberately **not** a member. Carrying it would push the struct to 72
bytes and across a second cache line, and nothing on the request path reads it — expiry is
enforced during token verification, before the context is built.

It is the first member of `http::RequestScope`, which is what the single attribute now holds —
the id §8.1 defines is the other member — stored under a single `constexpr` key,
`anvil::http::kRequestScopeKey`, declared beside that struct and nowhere else. There were two
constants naming this attribute once: `accesscontrol/access_filter.h` carried a second one
holding a different string, and that was the one the filter wrote, so anything reading the key
the public header names got a null context at run time with no error. Handlers read the context
through `accesscontrol::user_context()`, which hands back a `shared_ptr` **aliasing** the scope;
nothing mutates it after the filter has filled it. Trivially copyable and `const` means it may
cross thread-pool boundaries by value with no synchronisation.

Note that `UserContext` is trivially *copyable* but not trivially *default-constructible* —
`Locale` has a user-provided default constructor because only validating constructors may
produce one. Nothing asserts triviality, and nothing should start.

## 6. Configuration

Read once at boot into a frozen struct, validated, and **exit non-zero on failure** — a
server that starts with a missing signing key is worse than one that does not start. Never
read the environment outside `config/`.

anvil ships the machinery — typed readers, validator combinators, `SecretBuffer<N>` (zeroed
on destruction, cannot be streamed or formatted by accident), `path_is_within` (by path
component, not string prefix: `/srv/storage-evil` starts with `/srv/storage` as a string and
is a different directory), and an injectable `EnvLookup` so a test supplies a map instead of
racing `setenv` against every other test in the binary.

Your application declares its own `Config` struct. See `docs/14-config.md`.

## 7. Deployment invariants

Assumptions the code may rely on. Each is enforced by deployment and asserted at startup.

1. **The process may die at any instant.** No state that matters lives only in process memory.
   Local caches are derived and reconstructible.
2. **There may be N instances.** Any "run this once" work uses a Redis lease, never a local
   timer. Rate limits, `perm_epoch` and job claims are all shared state.
3. **Clocks skew between instances.** Expiry is compared against the local clock with a ±60 s
   tolerance wherever a client-supplied timestamp is involved. Never compare two
   client-supplied timestamps.
4. **`STORAGE_ROOT` is never web-served.** The Nginx location serving it is marked
   `internal;`. Without that keyword, protected storage is world-readable.
5. **`TRUSTED_PROXIES` names every hop in front of the application** (§4.1). Both failure
   directions are silent; only the boot summary's count distinguishes them.
6. **Graceful shutdown.** On `SIGTERM`: stop accepting, drain in-flight with a deadline,
   release held job leases, close the `mongocxx::pool`, flush logs, exit 0. Killing the
   process with open upload streams leaves partial files the sweeper must reap.
7. **Migrations run from exactly one place**, as a process that exits — never at boot, never
   on the job queue, never from N instances. The step lock is a safety net, not a scheduler,
   and the property it protects is idempotence rather than exclusion
   ([`18-data-migrations.md`](18-data-migrations.md) §4).
8. **`CONTENT_ORIGIN` is a different registrable host from `SITE_ORIGIN`**, never a path on
   the same one. `__Host-` session cookies are host-only, so an XSS in assembled HTML cannot
   read them or ride the session — and for the same reason the render path cannot read one
   either, which is why it takes a path-scoped capability cookie instead
   ([`19-server-side-rendering.md`](19-server-side-rendering.md) §6). Asserted with
   `config::require_origin_split`, which compares the HOST rather than the origin: cookies are
   not port-scoped, so two origins differing only by port are one host to every browser.

## 8. Error model

```cpp
enum class ErrorCode : std::uint16_t {
    Ok = 0,
    Unauthenticated, Forbidden, NotFound,
    CapabilityRequired, CapabilityInvalid,
    ValidationFailed, Conflict, VersionMismatch,
    RateLimited, PayloadTooLarge, UnsupportedMedia,
    ServiceUnavailable, Internal, InsufficientStorage,
};
```

Wire format, identical for every failure:

```json
{ "error": { "code": "VALIDATION_FAILED", "request_id": "01J…", "fields": { "email": "BAD_FORMAT" } } }
```

- `fields` appears only for `ValidationFailed`, and names the field plus a **fixed reason
  enum**. It never echoes the submitted value — that is a reflected-XSS and log-injection
  vector, and with non-Latin input an encoding hazard too.
- `Internal` carries no detail; `request_id` correlates to a server log line holding the stack.
- `ServiceUnavailable` is distinct from `Internal` so a circuit breaker and a client can tell
  a fault from an outage.
- **`Unauthenticated` and `Forbidden` never reach the client on a `Stealth` route.** The
  filter maps both to a byte-identical `404`. They still exist internally so the audit log
  records the true reason — losing intrusion signal to stealth is a real cost of the design
  and is mitigated by logging, not by accepting it.

`ErrorCode` is stored as int32 in audit rows read back much later, so it is **append-only**:
`kMaxErrorCode` exists so a decoder has a bound it can state rather than a belief about how
long the list is.

### 8.1 The writer, and the id it carries

**This shape was published in phase 0 and nothing in this library wrote it until phase 12.** The
access filter assembled `{"error":{"code":"X"}}` inline with a string concatenation, the stealth
path was a `constexpr` with neither of the other two fields, and the string `request_id` appeared
in no writer anywhere in the source. A contract a client is generated against and nobody keeps
is how two applications come to spell one failure two ways — which is exactly how
`wire_name(input::Reason)` came to be missing, and why the example above named `INVALID_FORMAT`
for eleven phases: a reason this library has never been able to produce.

So there is one writer, `http::append_error_body(out, code, request_id, fields)`, in
`http/errors.h` beside the status table that header already owns. `fields` is
`std::span<const input::FieldError>` — the type the validators already return, so a key in that
map is a schema constant and never a key taken from the request — and it is written only where
`carries_field_detail()` says so, which keeps "only `ValidationFailed` explains itself" a
property of one function rather than of every call site. The map is the half with the sharper
consequence for a client: **a form that cannot place a server's reason on a field is a form that
refuses to submit with nothing marked on it.**

`request_id` is 16 bytes — a 48-bit millisecond timestamp and 80 CSPRNG bits — rendered as 26
Crockford base32 characters, which is the `01J…` above. It sorts into occurrence order in a log
without being parsed, carries no MAC and no host identity (which is why it is not UUIDv1), uses
a transcription-safe alphabet because its whole purpose is that a person reads it back to
support, and authorises nothing. It is minted in the earliest pre-routing advice, so a request
refused before the filter still has one, and it travels in the single request attribute
`UserContext` already occupies rather than in a second one. The same value goes out as
`X-Request-Id`, so a client holding a body it cannot parse still has an id to quote.

**Except on the stealth path, where there is neither.** `kNotFoundBody` stays the `constexpr` it
is: a correlatable value that a genuine 404 does not carry is one of the tells
`accesscontrol/stealth.h` enumerates beside `WWW-Authenticate` and `Set-Cookie`. Non-stealth
denials from the filter do carry one — `deny()` has written an audit row for every denial since
phase 3, so there has always been something to correlate, and the id is what joins a user's "I
cannot get in" to that row.

The wiring is `http::install_request_scope()`, called once before the first listener starts. It
registers both halves together and they install together on purpose: one pre-routing OBSERVER
that mints the id and creates the `RequestScope`, and one pre-sending advice that puts the same
value out as `X-Request-Id`. Half of that pair is a half-kept contract in either direction — an
id nobody can quote, or a header that is always empty — and neither failure produces a symptom
the deployment that has it would notice.

`RequestScope` is the single attribute `UserContext` used to occupy, with the context first at
offset 0 so the filter's read still starts on the allocation's first cache line and the id sits
in the next one, allocated on every request and touched on none of the hot ones. The filter
fills the context through `http::attach_user_context`, on the loop thread that owns the
request, before any handler runs; readers get `const UserContext&` through a `shared_ptr` that
**aliases** the scope rather than copying it, so a context crossing a thread-pool boundary costs
no second allocation and keeps the id reachable.

The header is withheld from **every 404**, and that rule is keyed on the status rather than on
the identity of the shared not-found object. Drogon hands back that object itself on an
event-loop thread and a *copy* of it anywhere else, so a rule that recognised it by address
would hold until the first not-found built off the loop and would then produce a 404 with an id
beside a 404 without one, on the same path. Deleting the guard to see what happens produced
something sharper still: Drogon caches the rendered form of that shared object, so two
consecutive stealth drops came back carrying the **same** id — the first request's. A
per-request value written onto it is not merely a stealth tell, it is one visitor's correlation
id handed to every later 404 for the life of the process.

## 9. Observability

One structured line per request: `request_id`, `route`, `status`, `duration_us`, `db_ops`,
`redis_ops`, `user_id` (never the email), and on a stealth drop the *true* code and the
missing permission bit.

`request_id` is the value §8.1 defines, and it is the same one the error body carries. That is
the only thing that makes a user-reported id answerable: a line and a body with independently
generated ids correlate to nothing.

The metrics that matter are named rather than wished for, and they are anvil's own table —
`kInternalMetrics`, which [`17-analytics.md`](17-analytics.md) §3 defends against the rule
that anvil populates no table:

```
anvil_authz_cache_hits_total          anvil_pool_queue_depth
anvil_mongo_pool_wait_microseconds    anvil_ttl_collection_rows
anvil_orphan_files_swept_total        anvil_stealth_denials_total
anvil_audit_rows_dropped_total        anvil_transactions_aborted_total
anvil_idempotency_claims_total        anvil_rate_limit_decisions_total
```

The pool wait is in **microseconds** rather than seconds because bucket boundaries are
integers in the declared unit ([`17-analytics.md`](17-analytics.md) §5) and a wait expressed
in whole seconds has one useful boundary.

`anvil_stealth_denials_total` is the highest-signal intrusion metric available, and it is
labelled by the true reason and by whether the client was stealthed — **not by source
network**. The value space of a network label is the internet, which is precisely the
cardinality explosion [`17-analytics.md`](17-analytics.md) §6 refuses; the coarsened network
is carried on the audit ROW instead, where it is bounded by a retention window rather than by
resident memory, and the row is what an intrusion view groups by anyway.

`anvil_rate_limit_decisions_total` is labelled by the counter that answered — `shared` for the
Redis one every instance counts into, `local` for the per-process bucket the limiter falls back
to — and by what that counter decided. The ratio is the point: **`local` climbing means every
limit in the deployment has quietly become N times looser**, once per instance, and nothing in
a response says so. `RateLimitVerdict::degraded` carried that fact from the first commit and
said in its own comment that it was surfaced so it would appear in metrics; for a phase no
metric consumed it, and what a deployment got instead was one `LOG_WARN` per request for the
duration of the outage — which is precisely what the rule above forbids, and unlike the
idempotency store nothing refuses the request earlier, so every request reached it. The line
survives as one per transition into the degraded state and one on the way out, because a
counter cannot carry the reason and a log line must not carry the rate.

The BUCKET is not a label on it. The rules are the application's
([`01-seams.md`](01-seams.md) §7) and they arrive as a `std::span` handed to a service rather
than from a config header, so unlike `anvil_ttl_collection_rows` the value space could not be
closed at compile time even if the names were anvil's to use.

`anvil_idempotency_claims_total` is labelled by what the claim found, and every one of the
five is invisible from outside: a replay is a 200 with a body, indistinguishable to an
operator from the request that produced it, so without the counter there is no way to learn
whether clients are retrying at all — which is the only reason the store exists. `mismatch` is
the value to alert on, because it is a client reusing one key for two different requests,
which is a bug in that client rather than a property of the traffic. The sixth value,
`unavailable`, is not an answer a claim can give — it is the store refusing the request
because Redis could not be asked — and it is counted rather than logged for the reason the
paragraph below gives.

Two of them exist because their absence once hid a real failure: **audit rows dropped** and
**MongoDB transactions aborted**. Both are zero in a healthy deployment and neither is visible
from outside — a dropped audit row produces no error response, and a correctly retried
transaction produces none either. Alert on the first at all, and on the second when it stops
tracking request volume.

**A log line per occurrence is not a metric.** Anything that can happen once per request must
be counted and reported periodically, never logged per event: under the load that makes it
fire, a per-event line is itself the outage.
