# 05 — Authentication and sessions

Who somebody is, how they prove it, and how long that proof is worth anything.
[`04-access-control.md`](04-access-control.md) covers what a proven identity may
then *do*; this document stops at the point where a `UserContext` exists.

The whole design is one split: **the credential that lasts and the credential
that authorises are two different things.** Conflating them produces four
contradictions at once — a token long enough to be convenient is a token too
long to revoke, and a token short enough to revoke is one the user re-earns
every fifteen minutes.

## 1. Two credentials

| | Refresh token | Access token |
|---|---|---|
| Form | 32 CSPRNG bytes, opaque | the fixed binary layout in `anvil/auth/token.h` |
| Verified by | a database lookup | an HMAC tag, no I/O |
| Grants | nothing by itself | the permissions it carries |
| Lifetime | days | minutes to an hour |
| Stored | as `SHA-256(token ‖ pepper)` | not stored at all |

Worst-case stale authority is therefore the ACCESS lifetime rather than the
refresh lifetime, and `perm_epoch` (§6) cuts even that to one cache TTL — about
ten seconds.

**The access token is not a JWT**, which is what a first pass at this usually
specifies. A JWT keeps all of JSON's costs and none of its benefits here: a parse
per request on the hottest path in the system, a header that advertises its own
algorithm, and a field set that grows because nothing stops it. A fixed 64-byte
payload plus a 32-byte tag is parsed by casting, is impossible to grow by
accident, and has no algorithm field to confuse.

**The refresh token is not a JWT either**, for a different reason: it is looked
up in the database anyway — that lookup is what makes revocation work — so a JWT
would add parsing cost and a second expiry that can disagree with the row's.

Two rules about verification, both of which are the difference between a working
scheme and a decorative one:

- **The tag is verified before any field is read**, including the expiry. Reading
  a claim first and checking the signature after is branching on unauthenticated
  data.
- **An unknown key id fails identically to a bad signature.** A forged token must
  not confirm which key it was aimed at.

## 2. Where the credentials live

Both travel in cookies, both carry the `__Host-` prefix, and the prefix is
load-bearing rather than cosmetic. `__Host-` is the only cookie form a browser
refuses to accept with a `Domain` attribute, which means a sibling subdomain
cannot set it. Without it, control of `anything.example.com` is control of the
session cookie for `example.com`.

Set with `Secure`, `HttpOnly`, `SameSite=Lax`, `Path=/`. `Lax` rather than
`Strict` because a session that does not survive following a link from an email
is a session people work around.

Cookies alone are not CSRF protection, so every state-changing request also
passes an **origin check**: `Origin` or `Sec-Fetch-Site` against a configured
allow-list, refused before any I/O. See `anvil/http/origin_check.h`.

The origin that serves media is deliberately NOT in that list. Media is served
from a host that never issues state changes, which is what stops an executable
response there from reaching an administrative session.

## 3. What a password has to be

A length floor and nothing else. Long, and checked against a list of known-breached
passwords — that is the whole policy.

Character-class rules (an upper, a digit, a symbol) are worse than nothing: they
*reduce* entropy by pushing people to `Password1!`, they make passwords harder to
remember and therefore likelier to be reused, and they do not stop any attack.
What actually stops an attack is length, a breach list, and the rate limits in §4.

Bounds are in **code points, never bytes**. A byte limit halves the allowance for
any non-Latin script, so a passphrase in Arabic would be refused at half the
length of the same passphrase in English.

anvil ships the mechanism (`anvil/input/breach_filter.h`) and not the list: which
passwords a deployment refuses is its own decision, and a list compiled into a
library is a list nobody can update.

## 4. The login path

Ordered so that hostile traffic is refused before it costs anything:

1. **Origin check.** A header compare, no I/O.
2. **Per-IP rate limit.** One Redis operation, against the address from
   `anvil/http/client_address.h` and never from `peerAddr()` — see
   [`00-architecture.md`](00-architecture.md) §4.1 for why that distinction
   collapses every bucket in the system into one.
3. **`hash_pool` saturation check.** Advisory, and it happens BEFORE the user
   lookup: a request that is going to be shed should not first spend a database
   round trip.
4. **One indexed lookup**, on ONE field. The identifier's shape decides which of
   email, username or phone it is, before the query is issued. An `$or` across
   three cannot use a single index and turns the login path — the one path an
   attacker can drive hardest — into a collection scan.
5. **Per-identity rate limit.** Distinct from step 2, because an attacker with
   many addresses and one target defeats a per-IP limit, and an attacker behind
   one address with many targets defeats a per-identity one.
6. **Argon2id verify, on `hash_pool`.**
7. **Status check**, and the failure counters.

### Everything that is not a successful login answers identically

A disabled account, a locked one, an unverified one, a wrong password and an
address that has never been registered all produce the **same response** after
the **same work**. Any difference between them is an account-enumeration oracle,
and the two that leak most easily are not the response body:

- **Timing.** Skipping the Argon2 verify when no account matched makes a missing
  account answer in microseconds and an existing one in ~100 ms. `PasswordService`
  therefore verifies a dummy hash in its place — identical work, identical
  answer, identical elapsed time.
- **Round-trip count.** A branch that does one query and a branch that does two
  is measurable. Where the two paths genuinely differ, the cheaper one is padded
  rather than the expensive one optimised.

### Lockout is a counter, not a policy

anvil records consecutive failures and stores whatever lock instant the caller
computed from them. The BACKOFF is the application's: how many failures, how
long, and whether it escalates are product decisions, and a library that chose
them would be a library every deployment fights.

## 5. Sessions

A session row is found by its id or by a **refresh token hash**, never by the
access token's. Keying on the access token makes rotation impossible: a rotated
token has a new hash, and the write budget below means the row is not rewritten
per request — so the session becomes unfindable the first time a token is
re-minted.

### The rotation budget

An active client causes **one session write per rotation interval**, not one per
request. `last_seen` is written only by the rotation, which is what keeps write
volume proportional to users rather than to traffic.

The TTL index is on `expires_at` and **not** on `last_seen`. Putting it on the
observational timestamp expires an active user as soon as the budget stops
rewriting it, which is up to a whole interval early.

### The two-tab race and the replay, which are one mechanism

Rotation is a **compare-and-swap**: the hash being replaced is in the filter. Two
tabs refreshing at the same instant produce exactly one rotation, and the loser
matches nothing — which is not an error. It reads the winner's **grace window**
and carries on.

The row keeps the previous hash and the instant its grace ends. Presenting the
previous hash is then two different facts depending on when:

| When | Meaning | Response |
|---|---|---|
| inside the window | a concurrent tab | succeed; do not rotate again |
| after it closed | a rotated credential was **replayed** | revoke the session, bump the epoch, audit |

The second is the single highest-signal event this layer produces. A replayed
credential means it leaked. The grace lookup therefore deliberately does NOT
filter on the window's end: matching nothing would report an intrusion as an
ordinary expired login.

### Lifetimes

Privileged sessions do not slide: their refresh window equals their absolute cap,
so at the interval they re-authenticate. Client sessions slide, but never past an
absolute cap — a sliding window with no cap means "requires full
re-authentication" is never actually enforced, because the window simply moves
forward forever.

The user type used for that decision is the one **freshly read**, never the one
frozen on the session row at sign-in. A promotion changes the account without
touching the row, so reading the row would let a session that began as a client
keep sliding on the client window after its owner became staff.

### The concurrent-session cap

Unbounded session rows are unbounded write amplification and a storage leak an
attacker controls. On overflow the least-recently-seen is revoked, in the request
that caused it rather than at some later sweep nobody is watching.

### What a sessions listing may disclose

A coarse network (`/24` for v4, `/48` for v6) and a **device label** — one word
from a fixed table, derived from a hash of the user agent.

Never the full address and never the user-agent string. The question the screen
answers is "is that one me?", which a consistent label answers; echoing the raw
user agent back is a stored-XSS vector in whatever renders it and tells an
attacker exactly what is being fingerprinted.

The coarsening rule lives in exactly one function, shared with the audit sink's
fold key. An audit row and a screen that disagreed about what "one source" means
would be impossible to line up, which is the one thing they exist for.

## 6. Revocation: `perm_epoch`

Permissions live in the token, so authorisation costs no database round trip —
and so a token minted before a permission change keeps the old permissions until
it expires. For a privileged account that is unacceptable.

A monotonic counter per user fixes it. Every token carries the epoch it was
minted with; the filter compares that against the authority; a mismatch denies
and the refresh endpoint re-mints. Three tiers answer the comparison:

| Tier | TTL | Cost | When |
|---|---|---|---|
| local `EpochCache` | ~10 s | pure CPU, no allocation | almost always |
| Redis mirror | 1 h | one GET | local miss |
| MongoDB | — | one indexed read | mirror expired |

**It fails closed.** A Redis error is a denial, never an allow. Treating an
unreachable revocation channel as "no revocations" turns an outage into a silent
restoration of every credential the system has ever revoked, and does it without
a single error reaching anybody who could notice.

Revocation latency is one cache TTL on OTHER instances — that is the documented
window, not a defect, and it is why the TTL is ten seconds rather than ten
minutes. The instance that performs the bump drops its own entry and honours the
change immediately.

Every write that changes authority bumps it: a permission change, a promotion, a
demotion, a disable, a sign-out, a detected replay. A revocation that did not
bump it is a revocation that looks like it worked.

## 7. Identifiers

Every login identity is stored twice: the **normalised** form, which is the
lookup key and carries the unique index, and the **display** form, which is what
the person typed. Storing one and re-deriving the other gives either logins that
fail on a capital letter or a screen that renders an address in a spelling nobody
wrote.

Email validation is a hand-written linear scanner: at most 254 bytes, exactly one
`@`, a local part and a domain that each satisfy their own rules. Not a regular
expression — `std::regex` is banned on any request path (ENGINEERING_RULES.md §5), it is
ReDoS-prone, and the "correct" RFC 5322 pattern is famously several kilobytes
long and still wrong.

A phone number is E.164 or it is not stored. It is **absent** rather than empty
when an account has none: a unique index treats a missing field as null, so
without a partial filter the first account without a phone would lock out every
other one — and an empty string is a value every phoneless account shares, which
defeats the partial filter just as thoroughly.

## 8. Argon2id

`m = 64 MiB, t = 3, p = 1`, and the memory parameter is the one that matters:
it is what makes a GPU attack uneconomic, and it is why the pool that runs this
is sized by **memory budget rather than core count**.

**`hash_pool`'s size IS the memory cap.** Size × 64 MiB is the worst-case
resident memory one attacker can pin by opening concurrent logins, so a
thirty-two-thread pool is two gigabytes reachable from the internet. Its queue is
bounded and a full queue sheds `503` — never queues, because a queued request is
still holding its 64 MiB when it finally runs.

Shedding is reported as a return value rather than through the callback, so a
caller that sheds simply returns and never has to reason about whether its own
continuation already ran on the stack beneath it.

Passwords are NFC-normalised and never trimmed or case-folded, identically at
registration and at login. Normalising on one path and not the other means the
same passphrase typed on two keyboards produces two different hashes and the
person simply cannot sign in.

A successful verify against below-policy stored parameters triggers a rehash, in
the **same task**: rehashing needs the plaintext, and the plaintext is zeroed the
moment the task ends. The write back is conditional on the hash that was verified
still being the stored one, so a password change racing the rehash cannot
resurrect the credential it replaced.

## 9. Contact verification

One live row per address, holding a peppered digest of a six-digit code and an
attempt counter. A six-digit code is about twenty bits, so the rule that permits
a fast hash for token storage — "tokens carry ≥128 bits of CSPRNG entropy" —
**does not apply**. Four controls make it acceptable, and all four are
load-bearing:

1. **Bounded attempts, incremented atomically.** Never read-compare-write, which
   N concurrent guesses defeat outright.
2. **A short lifetime**, backed by an explicit expiry predicate on every read.
   The TTL monitor lags about a minute, so the index alone leaves an expired code
   verifying for that minute.
3. **Two rate-limit buckets** — one on guesses, one on re-issues. Without the
   second, minting a fresh row resets the attempt counter for the price of one
   request.
4. **Byte-identical failure** for a wrong code, an expired row, exhausted
   attempts and no such address.

Remove any one and the scheme fails. The third is the one anvil cannot enforce
for you: it ships the limiter, and the buckets are the application's to declare.

The row carries no user id. The duplicate-registration branch has to write a
byte-identical row WITHOUT first reading the user, or the cost of that read is
itself an enumeration oracle — a fresh registration would do one more query than
a duplicate, measurably.

## 10. Capability tokens

A short, scoped grant redeemed by a **later, separate** request: an upload slot,
a destructive-action confirmation, a preview of unpublished content.

It exists because those cases genuinely need a credential. It does **not** exist
to re-tokenise a decision the access filter already made two stack frames up.

- **Consumption is one atomic `find_one_and_update`.** Check-then-act here is a
  double-spend: two concurrent confirmed deletes, or one irreversible operation
  performed twice.
- **The binding — user, scope, subject — is in the FILTER**, not checked against
  the document that came back. A token issued to one person and presented by
  another matches nothing, so it is rejected *and not consumed*; a token for one
  object presented against another likewise.
- **There is no unscoped scope.** An unscoped capability is a bearer token with
  authority over everything and a replay window as long as its lifetime.
- **Only a digest is stored**, peppered, so a database dump alone is not enough
  to replay one.

Which scopes exist is the application's; see [`01-seams.md`](01-seams.md) §8.

## 11. What is audited

Every event in this document that changes what a credential can do, and every
denial. See [`04-access-control.md`](04-access-control.md) §5 for the sink and
its shedding policy; the point relevant here is that a stealth `404` hides the
true reason from the client and the audit row is the only place it survives.
Losing the 401/403 signal from client-visible responses is a real cost of
stealth, and it is paid for by logging rather than accepted.
