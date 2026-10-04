# 04 — Access control

Every protected request runs the same ordered pipeline, and **the order is a
security property**: each stage is cheaper than the next, so hostile traffic is
dropped as early and as cheaply as possible. The full sequence is in
[`00-architecture.md`](00-architecture.md) §4. This document is about stages 4–6,
which complete with **zero database round trips**.

## 1. The permission set

128 bits, 16 bytes, on the stack. A membership check is two ANDs and two compares
— no branch per permission, no allocation.

The alternative, a `std::vector<std::string>` of permission names, is a heap block
plus a string compare per check on every authorised request. That is the whole
argument.

Not `std::bitset<128>`, for three reasons:

1. Its operations are not `constexpr` until C++23, so the compile-time route masks
   that make authorisation free would not compile.
2. It exposes no portable access to its underlying words. `to_ullong()` throws
   above 64 bits and there is no `to_bytes()`, so encoding one as a 16-byte BSON
   BinData would mean testing 128 bits one at a time.
3. **Its byte order is unspecified.** These bits are persisted. A value written by
   one build could be read differently by another.

`PermSet` fixes the wire format explicitly: little-endian words, bit *i* in word
*i*/64 at position *i*%64, low word first.

**Bit indices are stored** — in tokens and in the user row — so they are
append-only. Renumbering silently regrants or revokes access for every token
currently in flight, and nothing reports it. Which permissions exist is the
application's; see [`01-seams.md`](01-seams.md) §1.

## 2. The route table

One filter class, and the per-route policy is a compile-time table entry the
filter looks up by matched path pattern.

The obvious design — `AccessFilter(PermSet required, bool stealth)`, constructed
per route — cannot be registered. Drogon's `registerHandler` takes either an HTTP
method or a filter **name**, and names resolve through DrObject's class-name map;
a filter with a non-default constructor is deliberately not in that map, so there
is no name to reference it by.

The registry inverts it, and delivers something the per-route constructor could
not: the claim that **every registered route declares a policy**. With per-route
constructors that is unenforceable — a route registered without a filter simply
has none, and nothing knows it should have. With a registry it is a set comparison
between the framework's route table and the application's array.

**Read that paragraph narrowly: one half of it is true and the other was not.** The
set comparison proves every registered pattern *declares* a policy. It proves
nothing about enforcement — see *Declaring a policy is not enforcing one* below,
which is the half that had no guard at all until phase 12.

| `RouteAccess` | Meaning |
|---|---|
| `Public` | No token required. Normal HTTP semantics on failure |
| `Authenticated` | Valid token required; failure is a **real** 401. For routes where a 404 breaks the product — a login form that 404s cannot function |
| `Guarded` | Token plus permissions, real 401/403. Protected but not secret: their existence is public even though their use is not |
| `Stealth` | Token plus permissions, and every failure is the byte-identical 404 |

A method-specific entry beats an `Any` entry for the same pattern. A pattern with
no entry **fails closed** — but do not rely on that: `declared()` throws at boot if
a handler is registered for a pattern the table does not name, so the failure is a
startup crash rather than a route that quietly 404s in production.

### Declaring a policy is not enforcing one

The registry guarantees every registered pattern **declares** a policy. It does
not, and cannot, guarantee that the filter which **enforces** that policy was
attached to the handler.

Drogon resolves filters by class name in the `registerHandler` argument list, so
attaching one is a string an application has to remember to type — and nothing
can check that it did. `drogon::app().getHandlersInfo()` returns
`std::tuple<std::string, HttpMethod, std::string>`: pattern, method and
description, and **no filter information at all**. `declared()` throws for a
pattern the table does not name, and a boot-time sweep comparing the framework's
routes against the application's array compares those same two sets. An unguarded
`Stealth` route passes both, because declaring a pattern is exactly what an
unguarded route also does.

In the plainest terms: **a `Stealth` route registered without the filter in its
list is a public route, and every check in this library and in the application
reports green.** The first `/admin` anybody writes is where that happens.

`register_route` (`accesscontrol/route_registration.h`) closes it by construction:
it resolves the policy, builds the constraint list itself, and attaches the filter
for every class the filter enforces — which is every class but `Public`. An
application never types the filter name, so it cannot omit it. `route_constraints`
is exposed beside it because that decision is otherwise untestable: nothing can
read the attachment back afterwards, so it is checked before the fact, and one
case pins `kAccessFilterName` against `AccessFilter::classTypeName()` so the
string cannot drift from the class it names.

`Authenticated` is enforced too, though the report that prompted this named only
`Guarded` and `Stealth`: `evaluate_token` denies an absent token on it by the same
branch, so omitting it would have been the same defect one class over.

Found by the first application built on this library, while reasoning about a
route it had not yet written.

That guard takes the **method**, and it answers by calling `policy_for`, so it keys
on exactly what the filter keys on. An earlier version asked only whether some
entry mentioned the pattern — so a handler registered under a verb the table did
not declare booted clean and then denied every request to itself.

## 3. Stealth

> Stealth is not "return 404 instead of 403". It is the claim that a **denied**
> admin route and a **nonexistent** one are indistinguishable.

Three channels, handled in three places:

**Timing** — structurally, in `decision.cc`. Token verify plus a bitset AND. No
database, no Redis on the **denial** path, whatever the epoch cache holds.
*Jitter is not a fix* — averaging removes it.

That qualifier is the whole of the property, and it was once missing. The mask
used to be checked *after* the epoch, so a denial on a route whose authority was
uncached returned `ResolveEpoch` and the filter paid a Redis GET before answering
404 — while a genuinely nonexistent route was answered by the framework for
nothing. Milliseconds against microseconds, on the one comparison stealth exists
to prevent. Any token holder could have walked a URL list and read the admin
surface off the latency, and an uncached authority is not a corner: it is every
user's first request after a deploy, an eviction or a 10 s TTL expiry.

The mask is therefore evaluated **before** the epoch, and a stealth route that
fails it answers on the loop thread. Nothing is given up by answering early —
`resume_after_epoch` re-checks the mask carried by the same token, so resolving
the authority can only ever turn an allow into a denial, never a denial into an
allow. The round trip could not have changed the outcome, only the `ErrorCode`,
and stealth renders both codes as the same 404.

Two consequences, both deliberate:

- The audit row reads `Forbidden` where a resolved epoch might have said
  `Unauthenticated`. `Forbidden` is the stronger of the two — it records what was
  actually checked rather than what was assumed.
- The short-circuit is **stealth-only**. On a `Guarded` route the client can tell
  401 from 403, and the 401 is load-bearing: a permission that was just *granted*
  reaches the user as 401 → refresh → retry. Collapsing that to 403 would strand
  them behind a stale token until it expired. A guarded route's existence is
  already public, so it has nothing to protect by answering early.

One residual, stated rather than hidden: a holder whose token *satisfies* the mask
but whose epoch is stale — a revoked staff member inside their token's remaining
lifetime — still pays the round trip before the 404. Closing that would mean
denying every cold-cache request, which is the feature rather than a fix. The
population that can observe it is the population that already knew the route.

**Response bytes** — `not_found_response()` returns one shared `HttpResponsePtr`,
and `install_as_framework_404()` installs **the same object** as the framework's
unmatched-route page. Byte-identity is then a property of construction rather than
of two code paths agreeing.

**Headers** — no `WWW-Authenticate`, no `Set-Cookie`, no request id.

There is a deployment obligation: the reverse proxy's own 404 page must serve
exactly these bytes, or the proxy distinguishes what the application refused to.

Rate-limit buckets are keyed by **identity plus rule, never by route**. A
per-route bucket is an existence oracle, which defeats the whole thing.

The cost is real and is paid back by logging: a denial still records the **true**
code internally, so losing intrusion signal to stealth is mitigated rather than
accepted. The count of stealth 404s per source address is the highest-signal
intrusion metric in the system.

## 4. The decision

`decision.cc` is a pure function. No framework type, no I/O — which is what makes
the timing property structural rather than aspirational.

### The budget: under 15 microseconds, at most one allocation

It runs on **every protected request**, so it is a latency floor for the whole
system rather than a cost paid by one endpoint. The number is why several
decisions elsewhere look over-careful:

- **The token is a 96-byte binary layout, not a JWT.** A JSON parser on the
  hottest security path is attack surface and allocation both.
- **`base64url_decode_into` writes into a caller's buffer.** The allocating form
  exists and is documented as *not* for this path.
- **`cookies.h` scans the raw header** rather than calling Drogon's
  `getCookie(const std::string&)`, which would construct a `std::string` per
  lookup.
- **The DENY path allocates nothing at all.** A denial that is measurably more
  expensive than an accept is a timing oracle, and the cheapest way to keep the
  two indistinguishable is for neither to allocate.

Stated here because it is the reason for all four, and a constraint that lives
only in scattered comments is one the next person optimises away.

- A public route allows an empty or bad token, and yields a context for a valid
  one. The epoch is deliberately **not** consulted.
- Otherwise: empty is `Unauthenticated`; every token error maps to
  `Unauthenticated` so a forged token's `kid` is never confirmed; then the epoch
  check; then the mask.

`satisfies()` is `constexpr`, and superadmin is an **explicit `UserType` check,
not an all-ones mask**. "Holds every permission" and "is a superadmin" stay
distinguishable in an audit record, and no bit-fiddling can synthesise the second
from the first.

Two orderings in the deny path are security properties: the response is sent
**before** the audit row is enqueued (the attacker's stopwatch stops at the
response), and the denial record carries the **true** code even when the client
received a 404.

## 5. The epoch — revocation without a database read

A permission change that waits out a token lifetime is a permission change the
screen lied about. But checking the database on every request is exactly the cost
stages 4–6 exist to avoid.

Three tiers:

| Tier | Cost | Role |
|---|---|---|
| Local cache | pure CPU | The common case |
| Redis `pe:{uid}` | one GET | On local miss |
| The user row | one query | The authority |

The local cache is **direct-mapped and fixed-capacity** — 1024 slots, 40 KiB, 64
mutex shards. Three properties, each deliberate:

- **Fixed capacity.** This is a cache on a security path, fed by a user id an
  attacker chooses. An unbounded map is memory exhaustion via forged tokens.
- **Short TTL.** The TTL *is* the revocation latency.
- **Fails closed.** A resolve failure is a denial, never an allow.

Direct-mapped rather than LRU because an LRU needs a list splice — two shared
writes per authorised request.

The filter sees a narrow interface, `EpochResolver`, with `check_cached`
(`noexcept`, no allocation, safe on a loop thread) and `resolve_async`. That
interface exists so a low-layer header never includes a service.

## 6. `UserContext`

Exactly one request attribute, because the framework's attribute map costs a
string hash, a map insert and a control-block allocation per entry.

64 bytes, one cache line, trivially copyable — so it crosses thread-pool
boundaries by value with no synchronisation. The layout and both `static_assert`s
are in [`00-architecture.md`](00-architecture.md) §5.

## 7. CSRF and cookies

`origin_check` is a byte comparison against the allowed origins, with no I/O. It
runs at stage 2, before anything expensive.

It is a rule about the **method**, and that is correct for every request this
repository currently serves — but it means a GET requires no `Origin`, and a
WebSocket handshake is a GET. Same-origin policy does not constrain WebSockets and
the browser sends the cookie anyway, so on an upgrade the origin check is not
defence in depth, it is the only CSRF defence there is — and by method it would
answer `NotRequired`.

`OriginRequirement::Always` is the answer, at the call site that knows what the
request is rather than in `is_state_changing`, which stays a statement about
methods. It is a **defaulted parameter**: every existing call site keeps the
behaviour it had, and an upgrade path passes `Always` to get a `Missing` or
`Mismatched` where it would have got a pass.

It landed before there was a route to use it, which is the point. The hole is in
shipped code, the wrong answer is a PASS — no log line, no metric, no failing
test, a control that is present, called and answering the wrong thing — so it
would have armed itself silently on the first upgrade route somebody added. What
passes it is `register_websocket_route`, in §8.

Session cookies carry the `__Host-` prefix, which forbids `Domain` — so two
origins are separate cookie jars by construction rather than by configuration.
That is what stops a session cookie reaching untrusted rendered content when an
application serves uploaded bytes from a second origin.

Cookies are read through `req->getCookie`, never by parsing the `Cookie` header:
the framework's parser consumes that field into its own map and does not store it,
so a header read returns nothing and every request is unauthenticated. That failure
is total and silent.

## 8. Authorizing a connection, not a request

Everything above decides a **request**. A connection is decided once and then
lives for hours, and two mechanisms that work because requests are short stop
working the moment it outlives the decision:

| | Why it stops working |
|---|---|
| The epoch | `EpochCache`'s TTL *is* the revocation latency (§5). For an open connection it is meaningless — the permission was checked at the handshake and nothing checks it again, so a staff member dismissed at 09:00 keeps their live feed until they close the laptop |
| The token's expiry | A connection that outlives it is a session with no end |

This is not a WebSocket concern waiting on a WebSocket.
[`notifications/sse.h`](../include/anvil/notifications/sse.h) already ships a
registry of connections held open across the same gap, and anvil owns neither its
HTTP nor its authorization — so the re-check is a function an application calls
on whatever it is holding open.

### 8.1 The re-check, beside the decision it must not disagree with

`still_authorized` lives in
[`accesscontrol/decision.h`](../include/anvil/accesscontrol/decision.h), next to
`evaluate_token`, because two authorization paths that can disagree will — and
the way this one would is silent: a connection keeping an authority a request
would refuse is a revocation that did not happen, and nothing logs it.
`ConnectionVerdict` maps onto `Step` one for one and one table drives both.

It returns a verdict rather than a `bool`, because the third answer is that the
authority is not cached — which a `bool` cannot express, and which the design's
own batched resolve gives the caller no way to ask for. A resolve that **fails**
closes the connection: an unreadable revocation channel is not permission to skip
revocation.

There is no `token_epoch` parameter. `ctx.perm_epoch` *is* the token's epoch, and
a second parameter carrying it is a second spelling of one number that the first
caller holding only one of them passes inconsistently.

The expiry is checked first and for every class, `Public` included. Agreement is
"both produce the same authority", not "both allow": a public route with an
expired token evaluates to an **empty** context, so a connection still holding a
populated one disagrees with what a fresh request would build. It uses the same
comparison `auth::decode` makes, tolerance included — a re-check one minute
stricter would close a connection whose token a fresh request still accepts, and
the client would reconnect, hand over that same token, and be let straight back
in.

### 8.2 The handshake: registered, not remembered

```cpp
anvil::accesscontrol::register_websocket_route(
    kRoutes, "/ws/feed", FeedSocket::classTypeName());
```

The constraint list is built from the policy, so an application never types a
filter name — the same move `register_route` makes and for the same reason.

That call also **installs the upgrade gate and records the path with it** (§8.3).
Registering the route *is* installing the gate, so there is no route it can be
missing from: the only way to get one is to call the function that installs it.
The converse is the deny-by-default half — an upgrade to a path this function did
not register is refused before Drogon's router is consulted, which makes a
WebSocket controller wired straight into the framework unreachable rather than
unguarded.

Then two filters, in this order:

1. **The origin check** (`OriginRequirement::Always`, §7). A header compare with
   no I/O, so a cross-origin handshake is refused before the process decides
   whether the caller was signed in.
2. **The access filter**, under the rule every other route follows.

An upgrade is a GET, always, so the policy is looked up under `Get` and there is
no method parameter to pass inconsistently.

**The pattern must be lowercase, and registration refuses one that is not.**
Drogon keys its WebSocket map by the lowercased path and reports that spelling
back as the matched pattern, so an uppercase pattern is registered under one name
and looked up under another — and the filter's `policy == nullptr` branch then
denies every upgrade, closed and silent. This is specific to WebSocket routes;
`registerHandler` stores an HTTP pattern as given.

The filter is named by its **type** and never as a string.
`DrObject<T>::alloc_` registers the class in its constructor and is instantiated
only when something ODR-uses it, so a literal would leave nothing referencing the
class, the filter's object file would not be pulled out of the static library,
and Drogon's answer to a middleware it cannot find is a log line and a chain that
runs *without* it — the route registers, the handshake succeeds, and the CSRF
check silently is not there.

A controller registered this way must not also declare its own paths with
`WS_PATH_ADD`: that registers the same path with whatever constraints the macro
carried, Drogon merges into one map entry, and the registration without the
filters is the one nothing reports.

### 8.3 Stealth, and four bytes that are not in the response

A refused stealth upgrade came back as the byte-identical 404 followed by
`88 02 03 e8` — a WebSocket close frame. `WebSocketConnectionImpl`'s destructor
writes one onto a socket that is still connected, and a filter refuses long after
that object exists. That is the existence oracle §3 exists to close, arriving
*after* the response rather than in it, where no assertion about headers or
bodies could have seen it.

**It is closed, by moving the refusal one stage earlier.**
[`accesscontrol/upgrade_gate.h`](../include/anvil/accesscontrol/upgrade_gate.h)
is a Drogon *sync advice*, and `HttpServer::onRequests` runs those before it
builds anything:

```cpp
if (requestParser->firstReq() && requests.size() == 1 && isWebSocket(req)) {
    if (passSyncAdvices(req, requestParser, false, false)) {
        auto wsConn = std::make_shared<WebSocketConnectionImpl>(conn);
        ...
```

An advice that returns a response short-circuits that branch. The connection
object is never constructed, so there is no destructor and no frame — not rarely,
never. It is the only such point: every pre-routing advice, every middleware and
every filter runs past the `make_shared`, and so does Drogon's own
unmatched-upgrade path.

The gate takes exactly the refusals whose purpose is to be indistinguishable:

| Refusal | Answered by | Why there |
|---|---|---|
| Upgrade to a path no `register_websocket_route` registered | The gate | It is the baseline everything else is compared against, and it includes every ordinary HTTP route and every WebSocket controller wired straight into Drogon — **deny by default, applied to the transport** |
| Cross-origin or absent `Origin` on a `Stealth` route | The gate | §7, and the answer must be the 404 above |
| A `Stealth` route the holder's token does not satisfy | The gate | `evaluate_token` reaches `Step::Deny` without I/O, which is the branch the filter would take a moment later with the identical `Evaluation` |
| Anything on a route whose class is not `Stealth` | The filters | Its existence is not a secret, so the trailing frame discloses nothing — and a sync advice runs before a request id has been minted and its response never reaches the advice that emits one, so a `401` answered there would lose the id that joins a complaint to the row explaining it |

It is **not a second authorization decision.** It calls `evaluate_token`, the same
pure function, with the same policy from the same table, the same keys and the
same resolver. It answers only on `Step::Deny`. `Step::ResolveEpoch` falls
through to the filter, because resolving an epoch touches Redis and an advice
cannot wait — and that residual is not an oracle, because reaching it means the
caller presented a validly signed, unexpired token that *carries the route's
permission bit*, and a holder of the bit already knows the route is there. The
cost is one extra token verify on a handshake that is going to be accepted, once
per connection.

The gate's answer is a **fresh** copy of the shared 404, because the framework
calls `setVersion` and `setCloseConnection` on whatever an advice returns and the
shared object is read by every other event loop in the process.

**The measurement that said this could not be closed was wrong, and how it was
wrong is worth keeping.** The residual used to be recorded here as a frequency —
0/2/1 refusals carrying the frame against 12/5/3 unmatched, over forty handshakes
each. The test helper read the socket once, and a single `recv` sees the frame
only when TCP coalesced it into the same segment as the response, so one server
behaviour measured two different ways and the difference was read as a property
of the two paths. Reading until the peer goes quiet says what happens: the frame
follows **every** refusal a filter makes, and `setCloseConnection(true)` — which
this library added to suppress it — never suppressed anything. That call is gone.

**And the same reading found something larger, in the response itself.** The
refusal that carried `setCloseConnection(true)` rendered a `Connection: close`
header; Drogon's unmatched-upgrade path set the same flag, but on the *per-IO-
thread copy* of the shared 404 (`newNotFoundResponse` hands back that copy on an
event-loop thread), so whether an unmatched upgrade carried the header depended
on whether that thread had ever served one before. Two refusals whose headers
differ are separable by reading the response, with no timing and no trailing
bytes involved at all — a plain break of §3, worse than the frame it was added to
fix.

It was invisible because the case asserting byte-identity ran in a process of its
own: `gtest_discover_tests` gives every case its own ctest entry, so the pair
never shared a listener with anything else and always landed on the same fresh
IO thread. Running the whole listener binary in one process — which is what a
server is — fails on the mainline as it stood. Both refusals now come from the
gate, from one object built by one call, so there is nothing left for a thread's
history to change.

The gate closes a second thing nobody was looking for. Drogon's unmatched-upgrade
branch calls `newNotFoundResponse(req)` and then `setCloseConnection(true)` on
it, and on an event-loop thread that function hands back the *per-thread copy* of
the custom 404 rather than a new object — so the first unmatched upgrade a thread
ever handled gave every later 404 from that thread a `Connection: close` that no
other thread's 404 carried. With the gate in front of it, that branch is
unreachable.

### 8.4 What an open connection costs

[`http/upgrade.h`](../include/anvil/http/upgrade.h). Frames are not requests and
the rate-limit table never sees them: a client that connects once and then sends
ten thousand frames a second has consumed exactly one rate-limit event.

| | Value | Why |
|---|---|---|
| `kMaxFramesPerWindow` / `kFrameWindow` | 120 per 10 s | Twelve a second sustained is far above any interface and far below a loop. Per connection, because the cost it bounds is paid per connection |
| `kMaxFrameBytes` | 64 KiB | CLAUDE.md §2.4: a frame is neither streamable nor worth 256 KB of inbound buffer per connection. Over it is a close, never a truncation — a truncated frame hands a handler a message that is not the one that was sent |
| `kRecheckPeriod` | `auth::kDefaultEpochTtl` | Faster cannot produce a different answer; slower makes a connection a way to outlive a revocation |

Exceeding a budget **closes** rather than answering `429`. There is nobody to
answer on a socket the client is still flooding, and reconnecting is what a
client already does after a deploy, a network blip or a laptop lid — SSE's policy
reused rather than reinvented. A sweep that fell behind schedules its next check
from *now*, so a process stalled for an hour does not then owe hundreds of checks
that all read the same cache entry.

`UpgradedConnection` is 88 bytes and asserted: ten thousand connections is 880 KB
of state and a member added carelessly is another 80 KB.

### 8.5 What anvil does not ship here

**No connection registry and no outbound ring**, although the design said it
would. That was right for `sse.h` and is wrong here for one reason: SSE's ring
exists because `publish()` is a producer *inside* this library that has to
deliver into it. A WebSocket carries the application's own messages and anvil has
no producer, so a ring here would be a container with no writer in this library —
twenty lines an application can write, permanently part of the ABI, and one more
thing to keep correct under concurrency.

**Chat reverses this, and only chat.** The refusal's reason was that anvil had no
producer. Chat is one: every send publishes wakes, and the subscriber that
receives them has to hand them to a socket. So
[`chat/hub.h`](../include/anvil/chat/hub.h) ships a registry and a per-socket
ring under SSE's policy — a full ring drops the connection — with its slots out
of `kUpgradeShare` and one socket per device. Which half of the refusal still
stands: **the generic one.** There is still no registry or ring for an
application's own WebSocket, because for those anvil still has no producer, and
the reasoning above applies to them unchanged. An application's socket gets the
budget, the re-check and the descriptor share, and nothing else; the chat hub is
not a general-purpose connection registry and is not shaped to become one — its
ring holds chat frames, its key is a device, and its only writer is the wake
subscriber.

What is genuinely anvil's is the **descriptor budget**, because it is one
process-wide number that two subsystems would otherwise each spend in full.
`kUpgradeShare` in [`core/descriptor_budget.h`](../include/anvil/core/descriptor_budget.h)
is the share upgrades may take, its sum with the stream share is `static_assert`ed,
and `descriptor_ceiling` turns it into a count.

**No in-band re-authentication.** Parsing a credential out of a socket frame is a
second authentication path with a second set of bugs, to avoid a reconnect the
client already implements. When the token expires the connection closes and the
client reconnects.
