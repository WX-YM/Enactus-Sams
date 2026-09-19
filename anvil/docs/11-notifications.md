# 11 — Notifications

An inbox that is the system of record, and four transports that are best-effort on top of it.

anvil ships the machinery. The TOPICS and the TEMPLATES are yours — `docs/01-seams.md` §7a and
§7b — because a topic is a channel in your application and a template is a sentence in your
product, and a table anvil populated would be a table anvil had to guess.

---

## 1. The shape of the thing

```
   publish ──► notifications ──► fan out ──► notification_inbox ──► the reader's page
                    │                                                      ▲
                    │                                                      │
                    └──► enqueue one job ──► outbound ──► web push ─────────┘ (best effort)
                                                       ├─ email
                                                       └─ webhook
```

Three collections, named by the application (`docs/01-seams.md` §4):

| Collection | What it holds |
|---|---|
| `notifications` | The canonical event. **ONE row per publish, whatever the audience size.** Template id and parameters, never rendered text |
| `notification_inbox` | Targeted delivery plus read state: one row per recipient, and only for fan-out-on-write topics |
| `notification_clients` | Every delivery endpoint — an in-app inbox, a browser push subscription, a mailbox, an external webhook — because they share lifecycle, preferences and delivery bookkeeping |

Both `notifications` and `notification_inbox` carry `expires_at` TTL indexes. **A TTL index is a
garbage collector, not an access control:** the monitor runs roughly every 60 seconds, so an
expired notification stays readable for up to a minute after it should have gone. Every read
here filters `expires_at` explicitly as well, which is what `tools/check-db-discipline.sh`
enforces.

---

## 2. Two fan-out strategies, chosen at compile time

| | Publish cost | Read cost | Read state |
|---|---|---|---|
| `FanOut::Write` | O(subscribers) inserts | one indexed range scan | per row, exact |
| `FanOut::Read` | one insert | merge across subscribed topics | a watermark |

**Picking one globally is the mistake.** A broadcast under fan-out-on-write is 20 000 inserts
and a write storm that stalls `db_pool`; a targeted notification under fan-out-on-read makes
every reader merge a topic only one of them can see. So it is per topic, and it is
`constexpr` — a strategy chosen at runtime is a strategy that can flip under load, and the
whole point is that a broadcast to 20 000 costs what a broadcast to three costs.

---

## 3. Publishing

`anvil/notifications/publish.h`. The order is the design:

```
insert/coalesce ──► fan out inbox rows ──► enqueue transports ──► mark dispatched
               ▲                                                        │
               └──── sweep_outbox() re-runs everything after a crash ◄───┘
```

The canonical row is committed **first**. The reverse order — mail first, then record — loses
the notification entirely whenever the process dies in between, and the reader has no way to
discover that anything was meant to reach them. This way round the worst case is a
notification that is in the inbox and was never mailed, which a sweeper can find and finish.

**Every step between the insert and the mark is idempotent**, which is what makes re-running
the whole tail safe rather than merely tolerable: the fan-out through the `{uid, nid}` unique
index, the transport enqueue through the job queue's idempotency key, and the mark through a
`$set` of a constant.

### The audience is derived, never carried

A publish does not name its recipients. It cannot: the sweeper that re-runs a dispatch is a
different process holding only the stored row, so any list the caller passed would be gone
exactly when it is needed.

| Scope | Audience |
|---|---|
| `Account` | the subject, which is the reader's own id |
| `Global`, `Resource` | the owners of the in-app clients subscribed to (kind, subject) |

`Scope::Account` deliberately does **not** require a client row. An account that has never
registered one has never subscribed to anything, and losing its sign-in alert is the precise
state an attacker wants it in.

### Idempotency and coalescing

Every publish carries a dedupe key, and `ntf_dedupe_unique` is what turns a retried publish
into a no-op rather than a second notification. Two derivations, separated by a domain prefix
so one can never collide with the other:

```
coalescing      H("c" ‖ kind ‖ subject ‖ tpl ‖ window bucket)
otherwise       H("i" ‖ kind ‖ subject ‖ idempotency key)
```

The idempotency key is **required** for a non-coalescing topic. Every queue in this system is
at-least-once, so the caller that publishes is itself being retried; an optional idempotency
key is one nobody passes until after the duplicate reaches somebody's inbox.

The coalescing key identifies the **event class**, not the event instance — that is what makes
three form submissions inside the window one row saying three. The cost is stated rather than
hidden: a retried publish inside a coalescing window increments the count a second time,
because a key that distinguished the retry would also distinguish the two submissions and
nothing would ever coalesce. A caller that needs an exact count needs a topic that does not
coalesce. The window is a **fixed bucket**, not a sliding one; a sliding window would need a
read before the write, which is the check-then-act the atomic upsert exists to avoid.

### Publishing inside a transaction

`publish(client, session, …)` writes the row inside the caller's transaction and **does not
dispatch** — the row is invisible to another process until the commit, and a transport that
fired for a transaction that then aborted is a notification about something that never
happened. Use it whenever the notification must not outlive the row it is about: "your form
was submitted", published for a submission that failed to save, is worse than no notification
at all.

**Coalescing inside a transaction is refused.** The upsert's whole purpose is that two
concurrent publishes land on one row; inside a transaction the second is a write conflict that
aborts the caller's transaction, so the notification would take the row it is about down with
it. Publish that topic outside the transaction, or declare one that does not coalesce.

### The outbox sweeper

`sweep_outbox()` is the reconciliation pass. Redis is not the system of record
(`docs/10-timer-jobs.md` §1): anything that must happen records its intent in MongoDB, and
this is the pass that finds intents with no completion.

The 60-second grace is not caution, it is a race: a publish that has committed its row and is
three instructions from enqueuing its transports is indistinguishable from one whose process
died there. Sweeping immediately would duplicate the work of every in-flight publish in the
deployment — harmless, because the tail is idempotent, and a pointless doubling of the
fan-out load under exactly the traffic that can least afford it.

The grace is measured against the row's UUIDv7 `_id` rather than its `created_at`: the leading
48 bits of a v7 id **are** the creation millisecond, so an upper bound on the id is an upper
bound on age, answered by the index the sweep already walks.

---

## 4. Reading

`anvil/notifications/inbox.h`. A reader does not know which of the two storage strategies a
notification came from, so the merge is the whole job.

Both halves are ordered by a UUIDv7 `_id` on the same wall clock, which is what lets **one
cursor** bound both: an inbox row's id is minted at fan-out and a broadcast row's at publish,
and "everything older than this id" means the same thing in each.

### Rendered at read, never at publish

A row stores a template id and parameters. Storing `{title, body}` per row copies the same two
strings once per recipient under fan-out on write **and freezes the language at send time** —
a reader who switches to Arabic still reads the English notifications they already received.
Rendering here means storage per notification drops from a few hundred bytes to a few dozen,
the language is the reader's *current* locale, and a typo in a template is fixed by a deploy,
retroactively, for every notification ever sent.

`kCountParam` (`n`) is reserved on a coalescing topic and bound at render from the row's own
`count`. Coalescing is an upsert whose `$setOnInsert` keeps the first publish's parameters, so
a count stored as a parameter is frozen at 1 while the row's count climbs — "1 new
submissions", forever.

### Preferences apply to the broadcast half at read

The targeted half had `should_deliver` applied when it fanned out. The broadcast half writes
no per-reader row at all, so there was never a moment on the way in at which a preference
could be consulted. **If it does not take effect at read, muting a broadcast topic does
nothing whatsoever.** It is applied by filtering the subscription list before either the page
or the count sees it, so the two cannot drift apart.

### Suppression is total, never redaction

A notification whose resource the reader can no longer see is hidden **entirely**. Not shown
with the title removed, not shown as "a post you can no longer view" — a redacted placeholder
still discloses that something existed, and on a `stealth_on_denial` topic the existence is
the secret.

- For the **targeted** half the row is also deleted: one that will never be visible again is
  dead weight in the index the unread count rides.
- For the **broadcast** half the row is left alone. It is one row shared by everybody, and
  another reader may still be entitled to it.
- A `ref` with **no visibility probe supplied** is hidden rather than shown. Showing what
  nobody checked cannot be walked back once it has been read.

Suppression runs **after** the merge and the truncation. Probing rows the page was never going
to show is up to `limit` extra round trips spent hiding something nobody would have seen. And
`next_cursor` is taken **before** suppression removes anything — a cursor derived from a
surviving entry would re-serve every suppressed row on the next page, and the page after that,
forever.

A suppressed row leaves the page **shorter** than the limit rather than triggering a backfill.
Backfilling means re-querying until the page is full, which is a loop with no bound against a
reader who lost access to a thousand resources.

### Read state

Targeted rows carry `read_at`. Broadcast rows have a **watermark** on the reader's in-app
client: everything at or below it is read. A row per reader per broadcast would reintroduce
exactly the fan-out-on-write cost the read strategy exists to avoid.

`mark_read` advances both with the same marker. The marker is one the **client sent** — the
highest id it has actually rendered — and never a server-side `now`: a notification that
arrived between the client's render and the request must stay unread rather than be silently
buried. The watermark write is a `$max`, so a stale request carrying an older marker cannot
walk it backwards.

The unread count is **capped**, not exact. A badge shows "99+" anyway, and an uncapped count
grows with a reader who has ignored notifications for a year.

---

## 5. The broadcast read is a union, not an `$or`

This is worth its own section because the obvious implementation loses notifications silently.

The read merges one indexed range per subscribed fan-out-on-read topic. Written as a single
`$or` with one limit, **every available ordering is the wrong one**:

- Sorted `{kind, subject, _id}` the server merges the branches correctly but in **topic**
  order, so the page fills entirely from the lowest topic code and `next_cursor` is an id from
  inside that one branch. Applied as a `$lt` to every branch on the next page, it excludes
  every newer row of every other topic **permanently**.
- Sorted `{_id: -1}` the order is right, but only if the planner chooses a `SORT_MERGE` — and
  that is a cost-based decision. `explain` shows it walking the primary key with the topic
  predicate as a filter against a small collection, which is unbounded for a scoped topic the
  reader rarely matches. Hinting the index turns it into a blocking `SORT` instead.

So the limit moves to the branches. Each topic gets its own `$match`/`$sort`/`$limit`, merged
with `$unionWith` in one round trip. Every stage is a `{kind, subject}` equality plus an `_id`
range — an `IXSCAN` with the sort provided by the index, verified against an **empty**
collection as well as a populated one, because a plan that depends on collection size is a
plan that changes in production. The closing sort sees at most branches × (page + 1) rows.

**`subject` is stored as a nil UUID, never as null.** In MongoDB `{subject: null}` matches
missing *or* null, so it is not a point equality and the index bounds cannot be elided to
provide the sort — with null, every stage above degrades to a blocking sort. It also makes the
row agree with `Subscription::subject` and `TopicRef::subject`, which already spell "unscoped"
as nil.

---

## 6. Live streams

`anvil/notifications/sse.h`. The registry, the per-connection ring, and the ceiling — not the
HTTP, which is the application's.

**A full ring drops the connection; it does not buffer.** A client that stops reading is a
client whose events accumulate somewhere, and every answer except "nowhere" is a memory leak
with a network trigger. Each connection gets 64 slots — 2 KiB, so ten thousand streams is
twenty megabytes and that is the whole exposure.

Dropping is safe rather than merely tolerable, because the stream is a latency optimisation
and the inbox is the system of record: a dropped client reconnects and re-reads, which is what
it already does after a deploy or a laptop lid. Overwriting the oldest slot was the other
candidate and is worse *invisibly* — the connection stays up, the client believes it is
current, and it has silently missed something.

**The ceiling is derived from `RLIMIT_NOFILE`**, not chosen. An SSE connection holds a
descriptor for its whole life, so the descriptor budget is the real limit: half of it, after a
512 reserve for the database pool, Redis, open files and ordinary requests in flight. A
deployment that raises its limit gets the streams; one that cannot serve them answers a clean
**429** at the door rather than failing `accept()` for every other kind of request — including
the ones a client would use to recover. A per-reader limit sits under it so a hundred tabs on
one account cannot hold the process ceiling open.

No I/O happens under any lock. The wake callback fires outside the stream's mutex, and the
registry lock is released before any push — a stream's mutex is never taken while the
registry's is held, which is the one ordering that could deadlock a delivery against a
concurrent close.

### The hub is PROCESS-LOCAL, and that is the part to design around

A reader's connection lives on one process; a publish happens on whichever process took the
request. `deliver()` therefore reaches the streams **this** process holds and no others, and a
deployment that assumes otherwise ships a feature that works on one instance and stops working
the day a second one is added — which is also the day nobody is looking for a notification
bug.

anvil does not paper over it, because the fix is a deployment decision rather than a library
one. Either fan the event out across processes yourself (a Redis pub/sub channel keyed by
reader, with `deliver()` called on each subscriber), or accept per-process delivery and let
the periodic `broadcast_ping` carry the unread count — an idle tab then corrects itself within
one ping interval instead of instantly, and the inbox is still exact because it is the system
of record. What is not an option is calling `deliver()` on the publishing process and
believing every reader saw it.

---

## 7. Outbound

`anvil/notifications/outbound.h`. **One job per notification, never one per subscriber** — a
broadcast to 20 000 endpoints must not become 20 000 queue entries. The audience is paged
inside the call.

What a verdict costs the endpoint:

| Verdict | What happens | Why |
|---|---|---|
| `Delivered` | the streak resets | a transient failure that recovers must not accumulate across weeks into a disable nobody can account for |
| `Gone` | disabled immediately | 410/404 from a push service, 410 from a webhook. The endpoint said it is finished; a streak would only delay believing it |
| `Rejected` | disabled immediately | an unusable address, a missing key, an SSRF refusal. Retrying cannot fix a configuration |
| `Transient` | counted; disabled at `kMaxSoftFailures` | a permanently broken webhook that retries forever is a self-inflicted outbound flood aimed at somebody else's infrastructure |

Rendering is **per endpoint**, not per notification: two subscribers to one topic do not
necessarily read the same language, and a render is a table lookup and a bounded substitution
— nowhere near the cost of the round trip it precedes.

A channel with **no transport configured is skipped, not failed**. A deployment with no SMTP
has not got a broken mailbox, it has no mail, and recording failures would disable every
mailbox in the system after five notifications.

The topic's permission is rechecked here as well as at fan-out. A subscriber who lost the bit
in between must not be mailed about a topic whose existence is what the permission protects.
An endpoint with **no account behind it fails closed** on a gated topic — nobody is not
everybody.

**Redelivery re-sends, and that is accepted rather than prevented.** The alternative is a
per-endpoint completion record, which is a write per endpoint per notification — more storage
and more write load than the duplicate it avoids. Web Push and email are both already
at-least-once to the client, and a webhook receiver has to be idempotent anyway.

---

## 8. Web Push

`anvil/notifications/webpush.h`. VAPID (RFC 8292) and aes128gcm payloads (RFC 8291).

The payload is encrypted to a key pair the **browser** generated and whose private half never
leaves the device. The push service relays ciphertext it cannot read, and neither can we
afterwards — there is no stored key that decrypts a delivery. That property is what makes it
acceptable to put a notification's text through somebody else's infrastructure at all.

```
ecdh  = ECDH(our ephemeral private, the subscription's p256dh)
IKM   = HKDF(salt = auth secret, ikm = ecdh,
             info = "WebPush: info\0" ‖ ua_public ‖ as_public, 32)
salt  = 16 CSPRNG bytes, FRESH PER MESSAGE
CEK   = HKDF(salt, IKM, "Content-Encoding: aes128gcm\0", 16)
nonce = HKDF(salt, IKM, "Content-Encoding: nonce\0", 12)
body  = salt ‖ rs(4) ‖ idlen(1) ‖ as_public(65) ‖ AES-128-GCM(plaintext ‖ 0x02)
```

The ephemeral key pair and the salt are generated **inside** `encrypt_payload`, and there is
deliberately no signature by which a caller can supply either. AES-GCM under a repeated key
and nonce is not merely weakened: it leaks the XOR of the two plaintexts and the
authentication key with it.

A subscription key that is not a point on P-256, or is the point at infinity, is refused.
Multiplying our ephemeral scalar by an off-curve point is an **invalid-curve attack** that
leaks the scalar a few bits at a time, so the conversion goes through `EC_POINT_oct2point`
rather than a memcpy of the coordinates.

**`decrypt_payload` ships alongside**, and the tests use it with the subscription's private
key — the browser's side of the exchange. Re-deriving the key our own way instead would pass
with the two public keys in the wrong order, with every label's trailing NUL missing, and with
the record delimiter dropped: three mistakes that each produce a body no browser can read
while looking entirely correct. HKDF is checked against RFC 5869's own vectors for the same
reason.

VAPID identifies the **sender**; it does not authorize the delivery. The token is signed over
the push service's own origin as `aud`, which is what stops one service replaying our token to
another — and the origin includes the **port**, which is the single most common VAPID failure
and surfaces as an opaque 401. The JWS signature is converted from OpenSSL's DER to raw
`r ‖ s` with `BN_bn2binpad`: a DER integer whose leading byte happens to be below `0x80` is 31
bytes, and passing the DER through produces a signature that verifies nowhere roughly one time
in 256.

The VAPID `sub` reaches the JWT's JSON, so a quote or backslash in it would forge a claim. It
is refused rather than escaped — a `mailto:` or `https:` address contains neither.

**The application server key pair is generated once and stored.** Rotating it silently
invalidates every existing subscription: the browser pinned the public half at subscribe time
and the push service checks it.

---

## 9. Webhooks

`anvil/notifications/webhook.h`.

```
X-Anvil-Signature: v1=HMAC-SHA256(secret, "<timestamp>.<body>")
X-Anvil-Timestamp: <unix seconds>
X-Anvil-Delivery:  <notification id>
```

The signature covers a **timestamp**, not just the body. A signature over the body alone is
replayable forever — an attacker who observes one valid delivery can resend it a year later
and it still verifies. Binding the instant into the signed string lets the receiver reject
anything outside `kReplayTolerance` (five minutes each way), and the attacker cannot move the
window without invalidating the signature. The separator is what makes the encoding
unambiguous: without it, `(1, "23x")` and `(12, "3x")` sign the same bytes.

### What a receiver must do

anvil ships `verify()` as well as `sign()`, because the receiver is the half that gets this
wrong and gets it wrong in ways that pass every test:

1. **Reject outside the tolerance.** A tolerance nobody applied accepts a year-old replay.
2. **Recompute over the RAW body**, not a reparsed and reserialised copy. A JSON round trip
   reorders keys and changes spacing, and the signature is over bytes.
3. **Compare in constant time.** A receiver using `==` leaks the expected signature one byte
   at a time to anyone who can measure it.

### The secret, and where the URL is checked

The signing secret is 256 bits of CSPRNG output, stored sealed, and **shown once**. There is
no read path that decrypts it back to an operator's screen — a signing secret that can be
re-read is one that appears in a support ticket. `list_webhooks` returns a projection that has
**nowhere to put it**, which makes that a property of the type rather than of a handler.

A webhook URL is an operator-supplied address this process will make a request to, which is
server-side request forgery by design. `check_webhook_url` runs at **registration**, where
refusing is free; a check at delivery time runs after a URL that resolved to a private address
yesterday has already been trusted. It refuses:

- anything that is not `https://` — the signature proves who sent a delivery, not that nobody
  else read it;
- literal loopback, link-local, unique-local and RFC 1918 addresses, **including
  `169.254.169.254`** — the cloud metadata address, which hands out instance credentials to
  anything that can make a plain GET;
- an IPv4 address wearing an IPv6 mapped spelling (`::ffff:10.0.0.1`);
- any port but 443 and 8443, or a webhook is a port scanner whose results are visible in the
  delivery log;
- credentials, a fragment, a control character or a non-ASCII host. The `user@host` form is
  **refused rather than parsed**: several HTTP clients read it differently from the host that
  ends up in a log, and a check that disagrees with the client about which host it is checking
  is not a check.

**This is not the last line of defence.** It decides about literals; a hostname that resolves
to a private address is only caught after resolution, which is the HTTP client's job.
`is_private_address` is exposed for exactly that second call.

---

## 10. Mail

`anvil/notifications/email.h`. The message and the conversation are separated because they
fail differently: building a message is a pure function whose mistakes are injections, and the
conversation is I/O whose mistakes are hangs.

**CRLF in a header is not a formatting bug.** SMTP delimits headers with CRLF and ends them
with a blank line, so a subject carrying one produces whatever headers the attacker wrote
after it and then a body of their choosing. A `Bcc:` is the cheap version. Every header value
is refused rather than sanitised — sanitising invites the question of which spelling of a
newline was meant, and enough mail libraries have answered it wrongly.

**Dot-stuffing is the same problem one layer down.** DATA is terminated by a line containing
exactly `.`, so a body line beginning with one ends the message early and the server reads the
rest of the body **as SMTP commands**. Anyone who could put such a line into a notification
could then send mail as this server. It happens once, in `build_message`, so nothing reaches
the socket without it.

**Implicit TLS on 465**, not STARTTLS, which is negotiated in cleartext and can be stripped by
anybody on the path. The certificate chain *and* the hostname are verified: a chain that
verifies for some other host is one an attacker can obtain, and the SMTP password goes over
this connection. `AUTH` is only reachable after the handshake because there is no other state
the session can be in.

The conversation runs over an abstract `SmtpStream`, which is what makes the state machine
testable against a scripted server — the only way to assert what a broken or hostile one does
to this client. A multiline reply with no end, a line with no newline, a close in the middle
of DATA and a reply arriving one byte at a time are all unreachable against a working mail
server, and every one is a hang or a memory exhaustion if it is not handled. Both the
continuation-line count and the length of an unterminated line are bounded.

A refused credential is `Rejected` and not `Transient` whatever the code says. Retrying one
repeatedly is how an account gets locked out by its own mail server.

---

## 11. Notes that are not obvious

- **`user_optional: false` is a security decision, and the exemption lives in the RULE.**
  `should_deliver` ignores the stored preference mask for such a topic, so it cannot be muted
  by a client row an older build wrote or by a hand-edited document. An account that can
  silence its own "new sign-in" alert has no alert.
- **`subs[].since` must be a `uuid::v7_boundary`, never a generated UUIDv7.** A v7 id carries
  74 random bits after its 48-bit timestamp, so two ids from the same millisecond order by
  those random bits rather than by time: a generated watermark makes `_id > since` a coin flip
  for anything published in the millisecond somebody subscribed, and loses the notification
  outright whenever it comes up the wrong way. The failure presents as an occasional
  unreproducible "I never got that notification" in production and as an unrelated test being
  flaky in CI.
- **The fan-out scan uses `$elemMatch`, not two dotted predicates.** `subs.kind == k AND
  subs.subject == s` matches a client whose array holds `{k, other}` and `{otherk, s}` — a
  different client from the one that subscribed to `(k, s)`, and on a staff-only topic that is
  the disclosure.
- **`channels` is stored on the row.** A publish that narrowed its channels — a security alert
  that must not be mailed to an address the attacker may already control — is re-dispatched by
  the sweeper after a crash, and a sweeper falling back to the topic's defaults would *widen*
  it on the replay. The row is the only place the decision can be recorded.
- **Every parameter key is a single ASCII letter.** `params` is a subdocument whose keys come
  from the template's placeholders, so they reach a document's key space; the single-letter
  grammar is what makes `$set` and `a.b` unrepresentable there rather than merely rejected.
- **A topic code is stored and is also a bit position** in the preference masks. That is why
  the ceiling is 64 and why exceeding it is a build error rather than a topic that silently
  cannot be disabled.
- **Parameters are re-validated at render**, not only at publish. A stored row is data from
  another process and gets the treatment a request would, including the bidi-override check —
  U+202E in a notification body spoofs the whole sentence and the reader cannot see that it
  did.

---

## 12. The storm breaker

Shedding was never absent. `BoundedThreadPool::try_post` refuses a full queue and the caller
sheds, so a publish storm cannot grow the heap. What was missing is **priority**: the queue
refuses whatever arrives while it is full, in arrival order, so a password reset published
during a like-storm was refused exactly as readily as a like. The pool protected the process
and lost the one message that mattered.

This section stood for a long time as a refusal, on the grounds that shedding wants a real
traffic shape to be tuned against rather than a guessed threshold. That is still true of the
**number** and it was never true of the **rule**, which is the part that has to be right.

### Two gates, because a publish is two things

The write path already separates them, and they have completely different loss semantics:

| Gate | Where | Effect | Legal for |
|---|---|---|---|
| Admission | before the row is written | **drop** | `user_optional` topics only |
| Dispatch deferral | after the row is committed | **delay** | every topic |

The row is the system of record, so shedding it *drops* the notification. The dispatch tail is
idempotent and already has a recovery path — shedding it leaves `dispatched_at` unset, which is
byte-for-byte the state a process killed between commit and enqueue leaves, and `sweep_outbox()`
finishes exactly that. Shedding it *delays*.

> **A topic the reader is not allowed to silence is a topic the storm breaker is not allowed to
> drop.** Under any pressure whatsoever it is only ever delayed, and the delay is bounded by the
> sweep interval plus `kOutboxGrace`.

Conflating the two gates is how a storm breaker turns into data loss, which is why that property
is asserted as a **loop over the whole topic table** rather than as a case about one topic: a
case names a topic and keeps passing after somebody adds the next one.

### The discriminator is a flag that already exists

`TopicSpec::user_optional`, and no new field. A security topic the reader cannot disable must
also be one the storm breaker will not shed, and both are the same question — so a topic that is
not `user_optional` says so once and means it everywhere. A second `sheddable` bit would let the
two drift, and the first time they drifted it would be a security topic somebody marked
droppable to make a graph look better. It would also push `TopicSpec` past its asserted 48 bytes.

### Watermarks are a fraction, which is why a default is defensible

`ShedPolicy` is two `float`s, both read as a **fraction of the queue's capacity**: above
`defer_dispatch_above` (0.50) the tail is skipped, above `drop_optional_above` (0.75) an
optional topic is refused admission. The absolute number depends on a traffic shape anvil cannot
know; the fraction is read against a queue the operator already sized, so it scales with their
tuning instead of overriding it.

The deferral watermark is the LOWER of the two, and `shed_policy_is_well_formed` refuses a
policy where it is not. Deferring is cheap and loses nothing, so it has to be the response that
engages first — a policy that dropped before it deferred would throw a notification away while
the outbox path it could have used was still idle.

`shed_verdict` is **pure**: it samples nothing and can acquire nothing later, because it has
nothing to call. The caller samples the pressure, once per publish, through
`PublishHooks::pressure`. An absent probe means no shedding at all, which is the configuration
every consumer that predates this has. A NaN pressure — `0 / 0`, from a pool reporting no
capacity — proceeds, and so does a probe that throws: the safe answer to a number nobody can
read is never to throw a notification away over it.

### Coalescing does more than this, and does it first

`TopicSpec::coalesce_window_s` collapses repeats of one event class into a single row with a
count. Against the storm that actually happens — thousands of instances of *one* event class —
coalescing removes the load and loses nothing at all. Shedding is for a storm of **distinct**
events, which is rarer.

> **A topic that can storm gets a non-zero `coalesce_window_s` first.** The storm breaker is the
> second line, and it is the lossy one.

### It ships with its counters

A shed nobody counts is silent data loss, so these are part of the feature rather than a
follow-up:

- `anvil_notifications_shed_total{outcome="dropped"}` — a reader did not get something. The one
  to alert on.
- `anvil_notifications_shed_total{outcome="deferred"}` — expected to be non-zero under load, and
  not an alert on its own.
- `anvil_notifications_outbox_rows` — a **gauge**, and the honest cost of the design. The
  dispatch gate converts queue pressure into outbox backlog, which is the correct trade because
  a backlog is durable and bounded by disk rather than by RSS — but a backlog growing faster
  than the sweeper drains it is the failure mode this introduces, and it is invisible without
  the gauge. `PublishService::sample_outbox_depth` samples it from the recurring job that runs
  the sweep, never at scrape: a sampler that queries the database turns every scrape into a load
  test that fires every fifteen seconds.

### What it does not do

**The transactional `publish` overload is not shed**, and the reason is arithmetic rather than
caution: admission control that runs after the caller already holds a transaction and a pooled
client has nothing left to save. The pressure has been paid. Refusing there would add a failure
mode to a transaction in flight and buy back only the row write it was about to do — and the
dispatch gate has nothing to skip, because that overload never dispatches.

**There is no sixth thread pool for publishes.** The resource under contention is the MongoDB
connection budget, not thread time, and a sixth pool draws its clients from the same
`mongocxx::pool` — renaming the contention rather than reducing it. `audit_pool` and
`analytics_pool` earn their separation because their work is *batched*, so a small pool is real
headroom; a publish is not batched and never will be, because the row must be committed before
the caller returns.

**Shedding is not by topic rate.** A per-topic token bucket sounds fairer and is worse: it needs
shared mutable state keyed by a value the storm is varying, and under the storm the bucket map
*is* the allocation. The flag is a compile-time constant read from `.rodata`.

---

## 13. Deferred, deliberately

Recorded rather than silently dropped:

- **Digests.** `DigestMode` is stored on every client row and nothing reads it yet. The
  mechanism it needs is a recurring job that aggregates a window per client and sends one
  message, which is a scheduling problem rather than a notification one — and the shape of the
  aggregate depends on what an application wants a digest to say.
- **Per-endpoint delivery receipts.** §7 states why: the write cost exceeds the duplicate it
  would avoid. Revisit only if a transport appears that is not already at-least-once to the
  client.
