# 22 — Chat

Direct conversations, groups and one-to-many channels, with media, receipts, live delivery
and opt-in end-to-end encryption.

**Status: built through phase 20's server half; encryption NOT for production.** The rows
are [`15-tasks.md`](15-tasks.md) phases 18–20, and each phase is usable without the next.
Phases 18 and 19 are met. Phase 20's server rows are closed; its gate is not, because two of
its rows cannot be closed in this repository: hammer's design document, and the external
review of both halves, before which no application ships an encrypted kind. The client half —
the socket client, the key store and every byte of cryptography — is hammer's. Where the
build departs from this design, the section says so under "As built".

Prior art is WhatsApp, which settled most of the product questions here at a scale nothing
built on this library will reach. It corroborates; it does not decide. Where this design
differs, the section says why.

Two sentences govern all of it:

> **A conversation is an ordered log with one writer of order: the server's sequence
> number.** Everything else (receipts, unread counts, sync, history visibility, disappearing
> messages) is a comparison against that number.

> **Encryption is decided when a conversation is created and never changes afterwards.** A
> flag that can be flipped later is a downgrade switch, and whoever holds the server holds it.

---

## 1. What anvil owns, and what the application does

| anvil ships (machinery) | The application declares (seams) |
|---|---|
| the conversation log, its sequence, its rows and indexes | which **conversation kinds** exist, and every knob on them (§2) |
| membership, roles and the rights vocabulary | which **rights** each role holds, per kind |
| message validation, history, edits, revocation, reactions, receipts | the **collections**, the **routes**, the **rate-limit rules** and the **audit actions** |
| the socket hub, cross-process wakes, typing, presence | whether **presence** is visible at all, and to whom (a hook) |
| the device and key directory, ciphertext relay, sealed media | which **namespaces** hold chat media (§6) |
| the push nudge, through the existing Web Push transport | the push **wording** for plaintext conversations (a template) |
| | who may **start** a direct conversation with whom (a hook; contacts, requests, spam policy) |

Nothing in the first column names a product. There is no "group of 1024", no "admins only",
no "24 hours" in anvil. Every one of them is a value in somebody's kind table, and the
reference application's table (`tests/testapp/chat_kinds.h`) is where the WhatsApp-shaped
defaults live, as an example and not as a default.

---

## 2. Conversation kinds, the seam

A kind is a `constexpr` row: what shape the conversation is, how big it may get, who may do
what in it, and whether it is encrypted. It is looked up by a stored one-byte code and never
by a string from a request, for the reason topic codes are (01 §7a): **joining is the
disclosure**, so a kind a caller can name freely is a kind a caller can name as anything.

```cpp
struct ConversationKindSpec final {
    std::string_view               key;                   // never parsed from a request
    std::span<const std::uint32_t> timers_s;              // the disappearing timers allowed
    PermSet                        create_requires;       // the account bit to create one
    std::uint32_t                  max_members;           // ≤ kMaxMembers; exactly 2 for Direct
    std::uint32_t                  retention_days;        // 0 = until revoked or expired
    std::uint32_t                  edit_window_s;         // 0 = no edits
    std::uint32_t                  revoke_window_s;       // 0 = no delete-for-everyone
    std::uint32_t                  max_text_code_points;  // ≤ 4 096
    RoleRights                     rights;                // a RightMask per role
    KindCode                       code;                  // STORED; the table's index
    Shape                          shape;                 // Direct, Group, Channel
    E2ee                           e2ee;                  // Never, Optional, Required
    History                        history;               // FromJoin, Full
    Receipts                       receipts;              // Off, Delivered, Read
    bool                           mentions_break_mute;
    std::optional<fs::Ns>          media_ns;              // plaintext attachments (§6)
    std::optional<fs::Ns>          sealed_ns;             // encrypted blobs (§6.4)
};
static_assert(sizeof(ConversationKindSpec) == 88);
```

As built (`anvil/chat/kind_spec.h`, 01 §18). The design had a timer *mask* over a ladder
anvil would ship; the build takes the timers as a span of seconds instead, because a ladder of
durations is a product's data, and a table anvil populates is a bug (CLAUDE.md §1).

### 2.1 What the shapes mean

| Shape | Members | Who posts | Notes |
|---|---|---|---|
| `Direct` | exactly two, fixed at creation | both | unique per pair **and per encryption mode** (§3.2); cannot be joined, left or renamed |
| `Group` | 2…`max_members` | whoever holds `Right::Post` | an "announcements only" group is a kind whose member role lacks `Post` |
| `Channel` | followers, up to `max_members` | owner and admins only | no receipts, no typing, no member list visible to followers, never encrypted |

`Channel` is not a big group. A follower has a member row, because the chat list needs one,
but it never has its activity bumped (§5.3), has no receipts and cannot see who else follows.
That is what lets `max_members` on a channel be six figures while a group's stays in four.

### 2.2 Rights

`Right` is anvil's vocabulary, because the handlers check it: `Post`, `React`, `AddMember`,
`RemoveMember`, `EditInfo`, `SetTimer`, `ManageAdmins`, `CreateInvite`, `RevokeAny`, `Pin`.
Which role holds which, per kind, is the application's. A role is stored as two bits on the
member row, and the rights are looked up from the kind at every check rather than copied
onto the row. Changing the table changes every existing conversation of that kind at the
next deploy, which is what a rule should do; a copied mask would freeze every conversation
at the rights it was created with.

### 2.3 What `kinds_are_well_formed` refuses

Every one of these is a conversation that would misbehave at runtime with nothing in the
table looking wrong in review:

- a `Direct` kind whose `max_members` is not 2, or which grants `AddMember`, `RemoveMember` or
  `ManageAdmins` to anyone;
- a `Channel` kind with `e2ee != Never` (§7.9 says why), with receipts on, or granting a
  follower `Post`;
- `e2ee != Never` with `History::Full`. A joiner cannot decrypt ciphertext from before they
  joined, so serving it to them is bytes they cannot read and metadata they were not owed;
- `e2ee != Never` with no `sealed_ns`, or a `sealed_ns` that is not `Private`, deduplicates at
  all, or accepts anything but exactly the `Sealed` class (§6.4);
- a `media_ns` that accepts `Sealed`;
- a `media_ns` that is not `Private`, or that deduplicates across owners (§6.1, §6.3);
- `E2ee::Required` with a `media_ns`, which would be a namespace no conversation of that kind
  could ever use;
- rights that are not monotone (owner ⊇ admin ⊇ member). A member who can do what an admin
  cannot is a role table with the names swapped;
- `max_members` above `kMaxMembers`, a duplicate or non-dense code, an empty key, a
  `retention_days` shorter than the longest timer it allows.

### 2.4 The bounds anvil sets, and why there are any

| Bound | Value | Why it is anvil's and not the kind's |
|---|---|---|
| `kMaxMembers` (non-channel) | 1 024 | the per-process member cache (§5.4), the per-message activity bump and, under encryption, a sender-key redistribution that is O(members) on every membership change |
| `kMaxFollowers` (channel) | 1 000 000 | a channel writes nothing per follower per message, so the bound is the membership index, not the send path |
| `kMaxMembershipsPerUser` | 4 096 | bounds the device-set propagation of §7.4 and the chat list's worst page |
| text | 4 096 code points | code points, never bytes (CLAUDE.md §8). The kind can lower it |
| ciphertext per message | 64 KiB | the socket's frame bound (`http/upgrade.h`), so a message that arrives by wake arrives whole |
| attachments per message | 10 | each one is a reference `$inc` inside the send transaction |

---

## 3. Conversations and membership

### 3.1 Rows

Collection names are the application's (01 §4); the field names are anvil's.

| Collection | One row per | Holds |
|---|---|---|
| `chat_conversations` | conversation | kind, `e`, head `seq`, membership version `mv`, device-set version `dsv`, title, description, icon, timer, the direct-pair key |
| `chat_members` | (conversation, user) | role, `js` joined-at seq, `ls` left-at seq, receipt watermarks `dlv` and `rd`, activity `act`, mute, pin, archive, `hide_before` |
| `chat_messages` | message | `c`, `s` seq, sender, sending device, client id `cid`, kind byte, body **or** ciphertext, attachment ids, `ref` seq, expiry, revoked flag |
| `chat_reactions` | (message, user) | one reaction |
| `chat_invites` | invite link | `SHA-256(token ‖ pepper)`, conversation, uses, cap, expiry |
| `chat_blocks` | (blocker, blocked) | nothing else |

Phase 20 adds `chat_identities`, `chat_prekeys` and `chat_device_queue` (§7).

As built (phase 21), a group or a channel is created with a **client id**, as a message is
sent with one (§4.2): `cid`, 16 bytes the client minted before its first attempt, required by
the route. A unique index on `{by, cid}`, partial on `cid` existing, makes a retry a read of the
first conversation, answered `200` rather than `201`, and two racing attempts converge on one
row the same way two sends do. Without it a lost answer retried was a second group, so a client
could not retry a create at all. A direct conversation needs none: it is already found by its
pair key. A caller that will never retry (a seed at boot) may pass none through the service.

### 3.2 A direct conversation is found, not created

`dpk = SHA-256("d" ‖ min(a, b) ‖ max(a, b) ‖ e)`, under a **unique partial index** on
direct kinds. "Open a chat with B" is an upsert on that key, so two people opening each other
at the same instant get one conversation, and a retried request gets the same one. No
idempotency key is involved.

The encryption bit is in the key on purpose. With `E2ee::Optional`, A and B may hold one
plaintext and one encrypted conversation, and they must stay two: merging them would be the
mode switch the opening of this document forbids, with extra steps.

`hooks.may_start_direct(a, b)` is asked first, and a block in either direction (§3.6) refuses
before it. Contacts, message requests and "only people who share a group" are all policies
that belong in that hook. Spam from strangers is the most common abuse a messenger gets, and
the application's notion of a stranger is not something anvil can guess.

### 3.3 History visibility is a range, and the range is the whole rule

A member may see messages with `js ≤ seq < ls`, and with `ls` absent while they are still a
member. `History::Full` sets `js` to 0 for a new joiner, and `FromJoin` sets it to the head.
Every read filters on the range. There is no second rule for "can this user see message N",
so there is nothing for two code paths to disagree about.

The range is also what makes removal correct under a race. A message sent the instant before
somebody was removed has a seq below their `ls`, so they were entitled to it, and a wake
already on its way to them (§5) carries something they may read.

As built, a past member is shown the conversation with its head at `ls − 1`, in the chat list
and the conversation alike, and may no longer list its members: how far a conversation went
on after you, and who is in it now, are not yours to read once you are out of it. Phase 18
let a past member list the members, and in an encrypted group that is the very set the
removal rotated the keys away from. A past member keeps their own row's settings.

Rejoining writes a new `js` and clears `ls`. The gap between leaving and rejoining stays
invisible, which is the behaviour every messenger converges on: what was said while you were
out was not said to you.

### 3.4 Membership changes are messages

Add, remove, leave, promote, rename, timer change: each one is a **system message** in the
log, with its own seq, written in the same transaction as the member row and the `mv` bump.
Clients converge on the member list by reading the log, the same way they converge on
everything else. "A was removed" sits between the right two messages on every client,
because it holds a position in the same order rather than arriving on a side channel.

This is the one place the conversation document is written inside a transaction. Membership
changes are rare and messages are not, so the conflicts land on the rare side: a membership
transaction that collides with a send retries under `repo::in_transaction`'s backoff (09
§5.1), and the send never waits on it.

The last owner leaving promotes the longest-standing admin, or failing that the
longest-standing member, inside the same transaction. A group with no owner is a group
nobody can ever administer again. Choosing the successor by `js` makes the choice
deterministic, so a retry promotes the same person.

### 3.5 Invite links are digests

The token is 128 CSPRNG bits and only `SHA-256(token ‖ pepper)` is stored (CLAUDE.md §5).
The link is **multi-use** with a cap and an expiry, so it is not a capability token (05 §10):
those are consumed exactly once. A join is one `find_one_and_update` that filters on the hash,
the expiry and `uses < cap`, and `$inc`s `uses`. Revoking a link deletes the row, and the next
join with that token matches nothing.

A token the server does not know, an expired one, a revoked one and one whose conversation is
full all answer the same `404`. A link is shared in places the inviter does not control, so
telling a holder *why* it stopped working tells them whether the group still exists.

### 3.6 Blocks

A block stops direct conversations in both directions (creation and send) and nothing else.
It does not remove anyone from a shared group: a member who blocks the admin would otherwise
be able to stop the admin from administering. The blocked party is not told, and their sends
to the blocker's direct conversation are accepted and **never delivered**, which is what
every messenger does. The blocked party's client shows one tick (sent) forever.

---

## 4. Messages

### 4.1 Sending: the sequence is allocated once, outside any transaction

```
loop      bound the body, rate-limit, Origin, decode
db_pool   1. member row  {c, u, ls absent}  → role, or the stealth 404
          2. (c, u, cid) already stored?    → answer with it (a retry)
          3. find_one_and_update conversation {$inc: seq}  → seq, mv, kind, timer, dsv
          4. transaction: insert message, $inc each attachment's reference count,
             the push nudge's job asked for just before it (§8.4)
after     hooks.on_message, wakes (§5), activity bump (§5.3)
```

**Step 3 is not in the transaction, and that is a decision.** The conversation document is
the one hot document in this subsystem. A transaction that `$inc`s it conflicts with every
other send to the same conversation and aborts, so a busy group would turn into a retry
storm on the one row everyone writes. A plain `find_one_and_update` is serialised by the
server and never conflicts.

The cost is that a crash between steps 3 and 4 burns a seq and leaves a **gap**, and that is
the contract: **sequence numbers are monotonic, not dense.** A client finds out what it has
missed by asking for `seq > cursor`, never by looking for a hole. A client that treated a
hole as loss would refetch forever around a seq that will never exist.

Step 1 runs before step 3, so a non-member never burns a number. A burnt number would cost
nothing on its own, but a refusal arriving after the `$inc` would take longer than one
arriving before it, and that difference is a timing oracle on whether the conversation
exists.

### 4.2 The client id is the idempotency key, and it is durable

Every send carries `cid`: 16 random bytes minted by the client before the first attempt. A
unique index on `{c, u, cid}` turns a retry into a read of the first attempt's row. This is
used instead of the Redis idempotency store (01 §7) for two reasons:

- A chat client retries for **days**. A phone that queued a message on a train sends it when
  the train comes out of the tunnel, and the Redis key's lifetime is the wrong order of
  magnitude.
- Replies, reactions and edits inside an **encrypted** message have to name their target
  before the server has answered with a seq, so the client's own id is the only name it has.
  The index makes that name unique.

### 4.3 Plaintext validation

UTF-8, strictly (03). The bound is in code points. C0 and C1 controls are refused except
`\n` and `\t`. Bidi overrides and embeddings (U+202A–U+202E) are **refused**, not stripped:
11 §11 gives the reason, U+202E spoofs the whole sentence and the reader cannot see that it
did. Isolates (U+2066–U+2069) are **allowed**. The design first refused them too; the existing
`i18n::TextClass::Prose` policy allows them because mixed Arabic and English sentences
genuinely need them, and message text is prose, so it takes that policy rather than a second
one.

Three rules the build settled that this section did not name:

- **`\n` is the only line break.** `\r`, CRLF and U+2028/U+2029 are refused. The code-point
  bound and the mention offsets have to mean the same thing on every platform, and a bare
  `\r` is a log-line spoof.
- **The text must already be NFC, and is refused if it is not.** It is not normalised on
  the way in, as section content is: mention offsets are code points into the client's own
  text, and normalising can change the count (U+0958 is two code points in NFC), which
  would shift every later span off the name it marked.
- **A message that renders as nothing is refused as empty.** White space and
  default-ignorable code points (zero-width characters, Hangul fillers) do not count as
  content.

**Mentions are ids, never parsed out of the text.** A message carries up to 32
`{offset, length, user}` spans in code points. Every user must be a current member, and every
span must sit inside the text. Parsing `@name` would make a display name a key, and display
names are neither unique nor stable.

**Link previews are never fetched by the server.** A server that fetches a URL a user typed
is SSRF by design (11 §9 covers it at length for webhooks), and in an encrypted conversation
the fetch would also disclose the URL. The client sends the preview's title, description,
URL and an optional thumbnail attachment, all validated as untrusted input. The URL goes
through the same scheme allow-list the HTML writer uses, so a `javascript:` preview cannot
exist.

### 4.4 Application message kinds

A poll, a location, a contact card and a product are each a product's own idea, so they come
from a table the application declares: a stored code plus a field schema using the existing
`input/schema.h` machinery. The server binds and validates a plaintext card exactly like a
form submission. Inside an encrypted conversation the kind is part of the ciphertext and the
server validates nothing; the client half is where that card's rules live.

### 4.5 Replies, edits, revocation

| | References | Plaintext | Encrypted |
|---|---|---|---|
| Reply | `ref` = target seq | target must be inside the sender's visible range | inside the ciphertext |
| Edit | `ref` | same sender, inside `edit_window_s`; the row keeps the latest body and an edit count | a new ciphertext; the server checks sender and window on the cleartext `ref` |
| Revoke (delete for everyone) | `ref` | sender inside `revoke_window_s`, or any holder of `RevokeAny`; body and attachments removed in one transaction, media references released | ciphertext removed; same checks |
| Delete for me | none | `hide_before` for "clear chat"; a single message is the client's | the client's |

A revoked message keeps its row and its seq, with the content gone and the flag set. If the
row were removed, every client that had it would hold a message the server can no longer
account for.

As built (phase 21): **this section used to say "the next sync" tells such a client, and it
could not.** An edit, a revoke and a reaction change a row without allocating a seq, and a
device syncing `seq > cursor` never re-reads a message it already holds. So a conversation
carries a second counter, `mut`, and every plaintext edit, revoke and reaction change:

- **allocates the next mutation number inside its own transaction**, first, by `$inc` on the
  conversation, and stamps it on the changed message as `mu`. Unlike a send's seq (§4.1) this
  is in the transaction, deliberately: a client's mutation cursor moves past every number it
  reads, so a number allocated and committed out of order would be skipped for good, while two
  transactions that both write the conversation conflict and commit in the order they
  numbered. A send's `$inc` waits on the lock for the length of a short transaction, and the
  conflict lands on the rare side, as a membership change's does (§3.4). A refused mutation
  gives its number back with its transaction; the same reaction twice, or taking back one that
  is not there, allocates none.
- **is read back by `GET …/messages?changed_after=<n>`**: every visible message with `mu > n`,
  oldest change first, a message changed twice once at its latest number, in the history
  route's shape (`messages`, `reactions`, `older: null`). A device pages until a page is short
  and moves its mutation cursor to the last message's `mutation`. Every rendered message
  carries `"mutation"` (zero for one never changed), and every conversation `"mutations"`, the
  counter, so a chat-list read says whether there is anything to catch up.
- **is announced by a `Mutation` frame** (`0x09`, §8.2): the conversation and the new counter,
  to every current member at the head, or to the sender alone for a message hidden past a
  block (§3.6). No seq: which message changed is the catch-up's to say. On the stream fallback
  it is `ChatWake`, so a client catches up both cursors on a `ChatWake`. A lost frame costs
  latency, as a lost wake does.
- **stops for a past member where they left**: `lmu`, the counter at their departure, is
  written with `ls`, so their counter and their catch-up stop there, as the head does (§3.3).

The counter is the conversation's, so a member who joined after a changed message learns that
the counter moved and nothing about what moved; the conversation they are shown already says
as much. Encrypted conversations do not need it: there an edit, a reaction and a revoke are
messages with a `ref` inside the ciphertext, each with its own seq.

Edits are **not chained**: the row holds the current body and the count, not a revision
history. A history would be one more copy of text the author has just asked to take back.

### 4.6 Reactions

Plaintext: one row per (message, user) under a unique index, so a reaction is one upsert or
one delete. A page of history fetches its reactions with one `$in` over the page's seqs,
bounded by the page. A reaction is at most one grapheme cluster of at most 8 code points.
Whether a given cluster is "an emoji" is not something to decide on the server, and an
application that wants a fixed palette supplies the list.

Encrypted: a reaction is a message with a `ref`, as in WhatsApp, because a cleartext reaction
row would be content stored beside ciphertext.

### 4.7 Disappearing messages, and why there is no TTL index

A conversation timer, chosen from the kind's `timers` mask, stamps `expires_at_utc` on every
message sent while it is set. Changing it is a system message (§3.4), so every client shows
the change at the same point in the log.

**Expiry is a sweeper, not a TTL index.** The TTL monitor deletes a document without running
any code. A message with an attachment holds a media reference, and a message the monitor
deleted would leave that reference counted forever: a leak the media sweeper cannot tell from
a live reference (07 §7). So a recurring job claims expired messages in pages under a Redis
lease, and releases their references in the same transaction as the delete. Every read also
filters `expires_at_utc` explicitly, because the sweeper runs on a schedule. That is the same
rule a TTL index needs (09 §6) for the same reason.

Retention (`retention_days`) uses the same sweeper. It is a kind-level ceiling on how long
the server holds anything, and the timer is a member's choice under that ceiling.

---

## 5. Receipts, the chat list, and wakes

### 5.1 Receipts are watermarks

`dlv` and `rd` on the member row: "delivered up to seq N" and "read up to seq N", each
advanced with `$max`. A receipt per message would be a write per member per message. A
watermark is one write per member per **batch**, and a stale or reordered receipt cannot walk
it backwards. The client sends the highest seq it has actually shown, never a server-side
"now", for the reason the notifications watermark gives (11 §4).

"Read by" for one message in a group is `{c, rd ≥ seq}` over an index, bounded by the group.

As built (phase 21), the readers route answers both watermarks for one message:
`{"read_by":[…]|null,"delivered_to":[…]}`. `delivered_to` is every current member whose `dlv`
covers the seq, over a `{c, dlv}` index beside the `{c, rd}` one; `read_by` is null for a kind
whose receipts are `Delivered`, and an `Off` kind answers `403`. Because both are watermarks,
one call for a sender's newest message answers every tick on the screen. The `Receipt` frame
stays in the grammar and unsent, deliberately: a frame per watermark move is a publish per
member per receipt batch, which in a group of a thousand where everyone posts receipts is a
million publishes for one message being read, while the route is one indexed range read per
sender per look. Delivered is not withheld by private reads: it says a device holds the
message, which is what a delivered tick has always said.

Read receipts are a privacy setting. A member who turns them off still advances `rd` (their
own unread count needs it), and the flag on their row withholds it from everybody else. Prior
art makes the setting reciprocal (you stop seeing others' read receipts too), and whether to
copy that is a product rule, so it is a kind knob, off by default.

As built (phase 21), a mute is asked for as a **duration on the server's clock**:
`{"mute_for_s": N}`, at most 366 days, zero to unmute, or `{"mute_indefinitely": true}`,
stored as `9999-12-31T23:59:59.999Z`. The absolute `muted_until` is still taken, and at most
one of the three may be named (`mute_for_s`, `NOT_ALLOWED`). A client's only clock is the
device's, which is wrong by an amount nothing on the device can measure, and a mute computed
from it ends early or late by exactly that. The preferences route answers `200` with the
membership after the write, as receipts do, so the client learns the instant without
computing it.

### 5.2 Unread counts are subtraction

`unread = conversation.seq − member.rd`, minus nothing else. It counts the sender's own
messages and system messages as unread. Fixing that exactly needs a count query per
conversation per list page, so the count is **approximate and capped at 999**, and the client
corrects it when it opens the conversation. A badge shows "99+" anyway, which is the argument
11 §4 makes for capping the inbox count.

### 5.3 The chat list, and the activity bump

The chat list is a range scan on `chat_members {u, arch, pin, act}`, newest first, paged by
cursor, then one `$in` over that page's conversations for heads, titles and (plaintext only)
last-message previews. For that to work, every send has to bump `act` on **every member's
row**.

For a direct conversation that is two writes. For a group of 1 024 it is 1 024 index updates
per message, and for a busy group that cost is the whole send path. So the bump is
**coalesced**: a Redis `SET NX` with a two-second expiry per conversation decides whether this
send performs the `update_many` or a recent one already did. The chat list orders by the
bump, so it can be up to two seconds behind on a busy group, and that is invisible on a
screen that sorts by the minute. A channel never bumps its followers at all (§2.1), which is
what makes the follower bound possible.

If Redis is down the debounce fails **open**: every send bumps. A chat list that stops
reordering is a visible fault, and a write amplification that lasts only for the outage is
not.

### 5.4 Wakes: cross-process, content-light, lossy by design

A member's socket lives on one process and the send happened on another. 11 §6 refused to
paper over this for SSE because anvil had no consumer to design for. Chat is that consumer,
so this time anvil ships the fan-out:

- After the commit, the sending process publishes a **wake** per recipient user on a Redis
  **sharded** pub/sub channel (`SPUBLISH`, Redis 7) keyed by user id. Every process
  `SSUBSCRIBE`s the channels of the users it currently holds sockets for, subscribing on a
  user's first local socket and unsubscribing on their last.
- The recipient list comes from a per-process **member cache** keyed by conversation and
  validated by `mv`. Step 3 of the send returns `mv` anyway, so the validation costs no
  round trip. A 1 024-member conversation is 16 KiB of ids, and the cache is bounded in
  bytes, not entries.

  As built (`anvil/chat/member_cache.h`). An entry is used only when its version **equals**
  the one the allocate returned: a newer entry can hold somebody who joined after this
  message, and an older one somebody who has left. A miss reads the list after the
  allocate, so it can include somebody who joined in between; it is filtered to members
  whose `js` is at or below the message's seq, because under `FromJoin` that person cannot
  see the message and a wake may carry it inline. A conversation with more than
  `kMaxMembers` current members is refused rather than cached, so **a channel's followers
  are not woken**: their clients learn of a post from the chat list, and a broadcast wake
  would need a per-channel subscription rather than a per-user fan-out.
- A wake is `{c, seq}` and, when the message is at most `kInlineWakeBytes` (2 KiB), the
  message itself, so a busy group's members do not each turn a wake into a database read. A
  larger message's wake makes the device fetch it.

**A lost wake costs latency, never a message.** Pub/sub is fire-and-forget and Redis is not
the system of record (10 §1). A socket that missed a wake is a device whose next sync asks
`seq > cursor` and gets the message from MongoDB. That is the same bargain SSE makes (11 §6):
the live channel is a latency optimisation and the log is the record.

Sharded pub/sub is used rather than one channel per process for one reason. A
per-process channel means the publisher has to know which process holds each recipient's
socket. That is a presence registry consulted on every send, and it is wrong for as long as
the registry lags behind a reconnect.

---

## 6. Media

Nothing in storage is public today, and nothing here changes that. `STORAGE_ROOT` sits
behind nginx's `internal;` and every byte goes out through a handler that chose to issue
`X-Accel-Redirect` (07 §6). The server always has to agree. What chat changes is **what the
server can check when it agrees**, and the first constraint is where the request arrives.

### 6.1 The agreement is made where the user is known, and carried as a grant

The serving route lives on the media origin, and the `__Host-` session cookie never reaches
it (00 §7, invariant 8). The handler there cannot ask "is this caller a member", because it
has no idea who the caller is. The two obvious fixes are both refused:

- **Serve chat media from the site origin, with the session.** A chat file is bytes a user
  chose. Serving it from the origin that holds the session is the XSS the origin split exists
  to prevent.
- **A capability cookie on the media origin**, as the preview does (19 §6). That works for
  the preview because the preview is a top-level navigation. An `<img>` or `fetch` on the
  site's page is a **cross-site subresource request** to the media origin, and browsers do
  not send a `SameSite=Lax` cookie on one. A `SameSite=None` cookie is a third-party cookie,
  which browsers are removing.

So the agreement is made on the **site origin**, where the user *is* known. Every response
that shows a message with an attachment (history, a wake, a search hit later) carries a
**grant** per attachment instead of an id: `MEDIA_ORIGIN/m/{grant}/{role}`. The history
read has already checked membership and the visible range, so minting the grant is that
decision written down. The media origin verifies it.

- **The grant is sealed, not signed.** It is `ns ‖ id ‖ expiry` under **AES-SIV**
  (`EVP_aes_256_siv`), a deterministic AEAD. The client cannot read the object id out of it
  (§6.2), and it cannot be forged or altered. It is deterministic, so the same object in the
  same expiry bucket is the same URL and the browser's cache works. A random-IV AEAD would
  make every mint a new URL and every scroll a re-download. The only thing deterministic
  encryption reveals is that two grants are equal, and that equality is exactly what a cache
  needs to see.
- **The expiry is bucketed**: the end of the next ten-minute boundary, so a grant lives 10–20
  minutes. A member removed from a conversation keeps the grants they were already handed for
  at most that long, for objects they were entitled to when they got them. That is the
  revocation latency, stated rather than hidden.
- **Delete-for-everyone is immediate anyway.** Serving reads the media row to resolve the role
  (08 §6), so the handler also refuses a row with no references left. Revoking the last
  message that held an object stops its grants on the next request. If a forward still holds
  it, it is still entitled to be served.
- Verifying is one AEAD open and an expiry comparison: no session, no membership read, and no
  round trip beyond the row read that serving already makes. Responses carry
  `Cache-Control: private, max-age` up to the grant's expiry. Chat pages send
  `Referrer-Policy: no-referrer`, so a grant does not leave in a `Referer` header.

**anvil enforces it with a type, not a convention.** `NamespaceSpec` gains `visibility:
Public | Private`. For a `Private` namespace, `accel_redirect_response` takes a `MediaGrant`,
and the only thing that produces one is `open_grant`. An application's own media handler
cannot serve a chat object by accident, because it has no way to construct the argument.
That is `append_tel_attr`'s shape (15, phase 13) applied to serving.

### 6.2 A client is told it can pull, and nothing else

Every response about stored media carries an **opaque handle or a grant**, and nothing taken
from storage. That means no object id, content hash, owner, stored size, reference count, or
"this already existed" flag. Each of those is a fact about other people's uploads or about
the store's internals, and a client that never receives one cannot leak it, log it, or build
on it.

- **Upload** answers with an **upload handle**, sealed the same way as a grant: `ns ‖ id ‖
  uploader ‖ expiry`, valid for an hour, and redeemable only by a send from the same account.
  The send opens it to get the id. The client never holds the id itself.
- **What the composer needs** (dimensions, duration, a waveform, a file name) is content, not
  storage metadata. The sending client already has it, because it had the file. It travels in
  the message as validated, bounded fields, inside the ciphertext when the conversation is
  encrypted, and the server derives none of it for the client.
- **Forwarding** names a message the forwarder can see (`c`, `seq`, attachment index), never
  an object. The server checks visibility and attaches a second reference to the same object.

The handle also closes something that raw ids would leave open. If a send accepted an object
id, anyone who learned one (from a log, a shared screenshot of devtools, or another
application on the same namespace) could attach it to a conversation with themselves and get
a grant for it. A handle bound to the uploader cannot be spent by anyone else.

### 6.3 Deduplication is still scoped, because the clock is metadata too

07 §4 deduplicates uploads per namespace by content hash. With §6.2 in place the response
says nothing about a dedup hit, but **the timing still does**. A hit skips probing,
normalising and transcoding, so it answers in milliseconds where a new file takes seconds.
Upload a guessed document and the clock says whether somebody in this namespace already has
those bytes. For a leaked contract or a medical letter, that is the disclosure. A response
shape cannot hide a clock. Padding the answer to a fixed time is refused: it holds a request
open for nothing, and under load it is never actually constant.

So `NamespaceSpec` gains `dedupe: Namespace | Owner | None`. Existing namespaces keep
`Namespace` and behave exactly as before. Chat's plaintext namespace uses `Owner`, so the
only thing a fast answer can reveal is that *you* uploaded those bytes before. `Sealed`
namespaces use `None`, because every ciphertext is unique anyway.

### 6.4 Three object classes

| Class | Accepted | Server does | Served as |
|---|---|---|---|
| `Image` (today) | JPEG, PNG, WebP, AVIF | sniff, strip, normalise, derive variants | the variant for the role, `inline` |
| `File` (new) | MP4, WebM, Ogg/Opus, M4A, PDF, and nothing else this phase | sniff against the closed list; **no decode, no transcode** | audio and video `inline` with nginx handling `Range`; everything else `attachment` |
| `Sealed` (built) | any bytes | count, hash during the stream, compare with the client's declared SHA-256 | `application/octet-stream`, `attachment`, always, under `default-src 'none'; sandbox` with no `media-src`: ciphertext is never played |

**`File` does not transcode, and that is a refusal, not a gap.** Transcoding video is an
ffmpeg dependency, minutes of CPU per upload and a parser for every container format on the
request path. That is a much larger attack surface than the four image decoders 07 §5 already
weighs. The client records in a format the class accepts. A server-side transcoder would be a
worker of its own, not a stage of this pipeline.

A PDF is never served `inline`. A PDF viewer is a document engine with script in it, and the
`nosniff`, `attachment` and content-origin split (00 §7, invariant 8) is what keeps that engine out of
the site's origin.

The per-account **byte** budget for `Sealed` uploads is enforced by the chat upload route,
before it opens the sink: the sink does not know the account, and refusing after the bytes
arrived is refusing too late.

`Sealed` bytes are attacker-chosen by definition: the server cannot tell ciphertext from
anything else of the same length. So a `Sealed` object is never `inline`, never on the site
origin, never sniffed (sniffing random bytes finds a "type" one time in a few hundred and
acts on it), and is counted against a per-account **byte** budget as well as a per-request
cap. Without the budget, an encrypted namespace is free file hosting.

`Mime` grows past eight values for `File`, and `MimeMask` is a byte whose `static_assert` says
"a ninth Mime needs a wider mask, not a wrap". It widens to `u16`. That is an ABI change, and
it lands as its own commit (CLAUDE.md §9.1).

### 6.5 References

An attachment is attached (`$inc`) in the send transaction and released in the revoke
transaction, in the expiry sweeper's transaction (§4.7), and when the last member leaves and
the conversation is deleted. This is 07 §7's rule unchanged: the count moves inside the
transaction that caused it, or it leaks.

---

## 7. End-to-end encryption

Opt-in, per kind (`E2ee::Optional`), or mandatory (`Required`). When a conversation is
encrypted, **the server never holds a key that decrypts a message or a sealed blob, and never
has.** This section is the server's half. The ratchet, the key store and every encryption and
decryption happen in the client, which is hammer's to design. What that design has to match
on the wire is fixed here.

### 7.1 Threat model

| Adversary | Gets | Does not get |
|---|---|---|
| A database dump, or a stolen backup | who talked to whom and when, sizes, membership, ciphertext | any message or media content |
| A stolen session cookie or access token | everything the account's **plaintext** conversations hold; the ability to send as the account | encrypted content: linking a new device needs a signature from an existing device's identity key (§7.3), which no cookie carries |
| The operator, honestly running the server | the metadata above | content |
| The operator, maliciously running the server | the ability to **add a ghost device** to an account it controls the first device of, or to withhold messages | content, without the users noticing that a key changed: clients verify the device-link chain and show "security code changed" (§7.4) |
| A member removed from a group | everything sent while they were a member | anything after: the server stops serving them at `ls`, and the remaining clients rotate their sender keys on the membership system message |

**What it does not hide:** metadata. Who messages whom, when, how often and how large is
visible to the server. Hiding it is sealed sender and much more, and it is deferred (§10).

**What it costs, stated so nobody discovers it later.** In an encrypted conversation the
server cannot search, cannot moderate content, cannot generate a link preview, cannot write a
push notification with the message in it, cannot give a new device the history, and cannot
give a new member anything sent before they joined. Each of those is a feature the
application gives up when it picks `Optional` or `Required` for a kind. That is why the
choice is per kind and visible in the table.

### 7.2 The protocol is ours, in the client; the server is shaped for one family

Version 1 is shaped for the **Signal family**, as WhatsApp uses it: an identity per device
(an X25519 agreement key and an Ed25519 signing key), a signed prekey, single-use one-time
prekeys, pairwise Double Ratchet sessions per device, and **sender keys** for groups,
distributed pairwise. The suite
is a stored byte on the device row, so a second suite is an addition and not a migration.

**MLS (RFC 9420) is the anticipated second suite, and the server design keeps room for it.**
MLS key packages are single-use with a last-resort fallback, exactly like one-time prekeys,
so the directory in §7.5 serves both unchanged. MLS also needs exactly one commit accepted per
group epoch. That is a conditional `$inc` on the conversation, the same shape as the `dsv`
fence in §7.6. The server delta is one fenced message kind, not a redesign.

**The implementation is our own, in hammer, with libsignal as the reference.** libsignal is
AGPL-3.0, and a client library that applications ship inside proprietary products cannot
carry that licence into every one of them. So:

- **It is written from Signal's published specifications** (X3DH, the Double Ratchet, Sesame
  for multi-device), plus libsignal's observable behaviour for sender keys, which have no
  standalone specification. It is **not a translation of libsignal's source**. Under most
  readings a line-by-line port is a derivative work, and it carries the licence with it,
  which is the thing this decision exists to avoid. libsignal is read to understand the
  protocol and is never copied.
- **libsignal is the test oracle.** A harness that runs libsignal produces interop vectors
  (key agreement outputs, ratchet chains, sender-key messages), and hammer's suite asserts its
  own output against them. The harness is a test tool and is never shipped. A protocol
  implementation checked only against itself shares 11 §8's failure: wrong in three places
  that agree with each other.
- **No primitive is hand-written.** X25519, Ed25519, HKDF-SHA-256, AES-256-CBC and
  HMAC-SHA-256 all come from WebCrypto. The code we write is the protocol state machine,
  which is still the riskiest code in either repository.
- **Two keys per device, not XEdDSA.** Signal signs with its Curve25519 identity key through
  XEdDSA, which WebCrypto does not provide, and hand-writing it would break the rule above.
  Each device therefore holds an X25519 key for agreement and an Ed25519 key for signatures.
  The cost is that this suite does not interoperate with Signal's network, and nothing here
  needs it to.
- **Not post-quantum in version 1.** PQXDH needs ML-KEM, which WebCrypto does not provide.
  It becomes a second suite byte when a vetted ML-KEM is available to the client, and it is
  an addition, not a migration.

**Phase 20 does not ship encrypted conversations to an application before an external
review of both halves**, and that gate is a row of its own. It matters more for a protocol we
wrote than it would for a vendored one.

### 7.3 Devices, and the signature that makes a cookie insufficient

A **device** is one key store: a browser profile, not a tab (hammer shares one across tabs).
`chat_identities` holds one document per account: a device-set version `dv` and at most
`max_devices` devices (a deployment knob, default 5), each with its id, suite, both identity
public keys, signed prekey and signature, last-resort prekey, `linked_at`, `last_seen`, and the
signature that admitted it.

- **The first device** is trust-on-first-use. Registering it requires a fresh primary
  authentication, a password or passkey inside the last five minutes, and not merely a
  session. If the session alone were enough, a stolen cookie on an account that has never used
  encryption could register the first device.
- **Every later device** is admitted by an existing device, which signs
  `("anvil-chat-link" ‖ account ‖ new device id ‖ both of its public keys ‖ timestamp)` with
  its Ed25519 signing key. This is the QR-code step. The server verifies the signature against
  the approver's stored key before writing anything. The client verifies the whole chain too,
  and that is what makes a ghost device visible; the server's own check is what stops a
  stolen session adding one at all.
- **Losing every device** is a reset: fresh primary authentication, a new first device, and
  a "security code changed" system message in every encrypted conversation the account is in.
  It cannot be silent, because silent resets are exactly what the malicious-server row in
  §7.1 would use.
- A device unseen for `device_idle_days` (default 30) is unlinked by a sweeper, which is the
  same as an explicit unlink. Unlinking a device is also what revoking the session that
  registered it does, so a "sign out everywhere" really is everywhere.

Keys are checked as bytes before they are stored: an X25519 key is 32 bytes and not a
low-order point, an Ed25519 key is 32 bytes and decodes, and a signature is 64 bytes and
verifies. The server cannot judge whether a key is *good*. It can refuse one that is malformed,
and a malformed key accepted here is a key some other client chokes on later. The primitives
are OpenSSL's `EVP_PKEY_X25519` and `EVP_PKEY_ED25519`, in `anvil/crypto`, in the foundation.

### 7.3.1 As built

`anvil/chat/devices.h`. One document per account, so every rule is a predicate in one write: the
first device is an upsert filtered on the account having no device, and a link is filtered on
the approver still holding the very key that verified, the new id being absent, and the account
being below `max_devices`. The link reads first only to fetch the approver's key and to give a
precise refusal; the write's filter is the authority.

The signed messages, byte for byte (Ed25519, pure):

| Message | Signed by | Bytes |
|---|---|---|
| signed prekey | the device's own key | `"anvil-chat-spk"`(14) ‖ device id(16) ‖ spk(32) |
| last-resort prekey | the device's own key | `"anvil-chat-lrk"`(14) ‖ device id(16) ‖ lrk(32) |
| link | the APPROVER's key | `"anvil-chat-link"`(15) ‖ account(16) ‖ new device id(16) ‖ agreement key(32) ‖ signing key(32) ‖ unix seconds, u64 BE(8) |

- **Golden vectors** for every row of the table above are `tests/testapp/chat_vectors.h`,
  produced by a third implementation from this table alone; `chat_vectors_test.cc` holds anvil
  to them and `testapp_emit_chat_vectors` prints them as hammer's fixture.
- **The client proposes the device id**, because the approver signs over it before the server
  has seen anything; nil is refused, and a unique partial index on the devices' ids makes one
  id name one device across every account.
- **The link's timestamp must be within ±5 minutes** of the server's clock, bounding how long a
  captured link is replayable, and **`gone` remembers the last sixteen unlinked ids**: without
  it a stolen device its owner unlinked could replay its own link inside that window and come
  straight back.
- **The fresh authentication is a window**, `[now − 5 min, now + 1 min]`, the minute into the
  future being clock skew between processes. Outside it, or with no answer from the hook, the
  registration is refused **`428 CAPABILITY_REQUIRED`, never `401`**, with the reason beside
  the error: `{"error":{"code":"CAPABILITY_REQUIRED","request_id":"…"},"reason":
  "chat.fresh_authentication","field":"authenticated_at"}`. Phase 20 answered `401` with no
  field, and a client reads a `401` as an expired access token: it refreshes, replays, is
  refused again, and a second `401` after a good refresh is a rejection that signs the person
  out of every tab. The session is valid; what is missing is a second, deliberate act, a
  password or a passkey, which is what `428` already means in anvil's vocabulary (a capability
  token is the other such act). A client asks for one and retries.
- **Each device records the session that registered it** (`sid`), and `unlink_session` unlinks
  it, so revoking a session ends the device it made. `ChatService::session_ended` is the call,
  with the keys and the queue discarded and the change propagated. anvil's session services
  name every session they revoke to `identity::SessionsRevoked` (01 §16), and the
  application forwards each one here. The hook is best effort. A device whose session died
  without the hook being asked can no longer be used, and the idle sweeper ends it.
- **The owner's listing marks the device the calling session registered** (phase 21):
  `"mine": true` on that one and `false` on the rest, and never the session's id. A browser
  may evict a key store, and the device it held stays current until the idle sweeper takes
  it thirty days later, with every sender encrypting to it meanwhile; this is how its client
  finds it to unlink it.
- **The approval travels back through a relay** (phase 21). The new device must post its own
  link, since a device belongs to the session that posts it, so the approver's signature has
  to reach it, and a desktop browser usually cannot scan a code. A short-lived mailbox, one
  row per requesting session in the application's `links` collection:
  1. the new device posts `POST /chat/link-requests {device_id, agreement_key, signing_key}`
     and is answered `201 {"token","expires_in_s":600}`, the token 256 CSPRNG bits of which
     only `SHA-256(token ‖ pepper)` is stored, as for invites; a second request from the same
     session replaces the first;
  2. the approver, any session of the SAME account, reads `POST /chat/link-requests/read
     {token}` → `{device_id, agreement_key, signing_key, expires_in_s}` and posts `POST
     /chat/link-requests/approve {token, approver, timestamp, link_signature}` → `204`. The
     signature is verified there, under the approver's stored key and over the 119-byte link
     message, exactly as the link will be, so a bad one is refused to the approver (`403`
     naming `approver` or `link_signature`) rather than discovered later; a second approval is
     `409`; a timestamp past ±5 minutes `400 timestamp`;
  3. the new device polls `POST /chat/link-requests/collect {token}`: `{"approval":null}`
     while pending, then once `{"approval":{approver, timestamp, link_signature}}`, removed in
     the same `find_one_and_delete`, then `404`; and posts the link as before.

  Every token is in a body, never a path, so the read and the collect are POSTs. Anything but
  a live request of the caller's own account, and a collect from any session but the one that
  asked, is the stealth `404`. The row expires in ten minutes, filtered on every read, with a
  TTL index as its collector. Only public material is held: a dump of the mailbox is keys
  and signatures any member of a conversation with the account is shown anyway.
- **A signed prekey and a last-resort key are rotated** (phase 21) by `PUT
  /chat/devices/{d}/keys` with `{signed_prekey, signed_prekey_signature}`,
  `{last_resort_key, last_resort_signature}` or both, each signature verified under the signing
  key stored for that device over the same messages as the table above, then written by one
  update whose filter restates that key. Phase 20 could not rotate either. **No previous key
  is kept on the server**: a sender who claimed the old bundle has already been handed it, and
  finishing a session started from it needs the old PRIVATE half, which only the device's own
  store holds and keeps for its overlap. Neither `dv` nor any `dsv` moves: the set of devices
  is unchanged, and raising every fence would refuse each sender in every conversation of the
  account once for a key no message is encrypted to.

### 7.4 The device set is a fence, and its changes are pushed to the conversations

Encrypting a message means encrypting it **for every device of every member**, so the sender
has to know that set exactly. If the set is stale in one direction, a new device cannot read
the message; in the other, a removed device can.

Each conversation carries `dsv`, the **device-set version**. Linking or unlinking a device:

1. writes the identity document and bumps its `dv`, then
2. `$max`es `dsv` to the change's millisecond on every conversation the account belongs to,
   up to `kMaxMembershipsPerUser`, and inserts a "devices changed" system message into each
   encrypted direct conversation.

Step 2 is not in step 1's transaction, because it writes the hot conversation documents
(§4.1). The identity document records the intent (`pend`, a date set with `$max` in the same
write as every link and unlink) and the propagation clears it when step 2 finishes; a sweeper
finishes any intent older than a minute. This is the outbox
pattern from 11 §3, applied unchanged. `$max` of a timestamp is idempotent and
order-independent, so a re-run or a race between two device changes cannot move `dsv`
backwards.

Device changes are rare and messages are not, which is why the cost goes on the rare side. A
send compares one integer it is already reading.

As built (`ChatService::propagate_devices`, `sweep_device_changes`). Three departures, each for a
reason the design did not see:

- **`dsv` moves to `max(dsv + 1, change)`, not `$max` of the change.** The change's millisecond is
  one process's clock. A device change stamped by a process running behind a version another
  process already wrote would leave `dsv` where it was, and a sender who read the device set
  before the change would pass the fence with it. Strictly rising closes that, and its cost is
  that a re-run raises `dsv` again: one spurious 409, never a stale sender let through. The
  update is a pipeline, `$max: [{$add: ["$dsv", 1]}, change]`, so it is still one write that
  reads nothing back.
- **A membership change raises `dsv` too**, in the transaction that already writes the
  conversation for `seq` and `mv`. A joiner or a leaver changes the set of devices a message has
  to be encrypted for exactly as a linked device does, and a sender whose set predates either
  would reach a leaver or miss a joiner.
- **The walk pages by conversation id**, over a new `{u, c}` index, rather than over the chat
  list's activity order, which moves while the walk runs: a conversation bumped mid-walk would
  slide behind the cursor and keep its old fence.

The "devices changed" message goes into each encrypted **direct** conversation, as the design
says, with the account as its sender and subject. Its client id is derived from the change
under the invite pepper, so a re-run or a racing run finds the first one's message through the
`{c, u, cid}` index and writes no second. The pepper is there because the sender is the account
itself: an id the account could compute is one it could spend first on a send of its own, and so
suppress the notice of its own change. The pending mark is cleared only while it still holds the
change that was pushed, so a change made during the walk stays pending for its own. The sweeper
is the application's to schedule, like the expiry sweeper: it finishes any change pending for
longer than `kDeviceChangeGrace` (a minute), oldest first, over a partial index on `pend`.

### 7.5 The key directory

`chat_prekeys` holds one-time prekeys: one row per key, uploaded in batches of up to 100, at
most 1 000 stored per device. **Claiming one is a single `find_one_and_delete`**, so a prekey
is handed out at most once. Check-then-act here gives two senders the same one-time key, and
the forward-secrecy guarantee that key exists for is gone.

When a device's pool is empty, the claim returns its **last-resort** prekey, which is
reusable, and flags the device so it uploads more on its next connection.

**Prekey exhaustion is an attack, so claims are budgeted twice.** One claimer draining a
victim's pool pushes every new session with that victim onto the last-resort key. Claims
count against a per-claimer rule and a per-target rule, both declared by the application
like every other rule (01 §7). The per-target rule is what an attacker with many accounts
runs into.

**As built** (`anvil/chat/prekeys.h`), the per-device bound is **exact**: each device carries a
`pk` counter reserved with `$inc` under the filter `pk ≤ 1000 − n`, in one transaction with the
`insert_many`, so a batch is all or nothing and two racing uploads cannot overfill a device. A
claim hands its slot back. A claim that finds a pool empty resets `pk` to zero and sets `low`,
which also heals any upward drift a crash left. A claim walks only the account's CURRENT
devices from the identity document and filters `{d, u}`, so an unlinked device's keys are
unreachable the moment the unlink commits and deleting them is an idempotent follow-up.

### 7.6 Sending an encrypted message

The body is a **common ciphertext** (one blob for everyone: a sender-key group message), a
**per-device map** `{device id → ciphertext}` (pairwise: every direct message, and every
sender-key distribution), or both. Both are opaque to the server, which checks:

- the sender's `dsv` equals the conversation's, inside step 3's filter, so a stale sender is
  refused with no extra round trip. The refusal is `409` with `chat.devices_stale` and the
  current device lists of the members, so the client re-encrypts and retries with the same
  `cid`;
- a per-device map's key set is **exactly** the current devices of the current members, minus
  the sending device. A missing device is the stale case. An extra one is refused outright,
  because it is either a bug or an attempt to deliver to a device that is not in the
  conversation;
- at most 64 entries per request. A pairwise fan-out to a large group exceeds the 256 KiB
  request bound (CLAUDE.md §2.4), which is why groups use sender keys and send their
  distribution in pages.

The common ciphertext is stored on the message row and kept like any message (retention,
timer, revocation). The per-device ciphertexts go to `chat_device_queue`, one row per device,
deleted when the device acknowledges them, and with a TTL index at `undelivered_days`
(default 30) as the garbage collector. Every read of the queue filters the expiry as well
(09 §6). Queue rows carry no media references, which is why a TTL index is allowed here and
not on messages (§4.7).

**As built (`ChatService::send_encrypted`, `anvil/chat/device_queue.h`). The send route takes
either body: one that names `dsv` is encrypted, `{cid, device, dsv, ciphertext?, devices?:
[{device, ciphertext}], page?, attachments?}`, every ciphertext unpadded base64url and bounded
before it is decoded. What the build settled that this section did not:

- **The 409 carries the first page of device lists**, `{"error":…, "reason":
  "chat.devices_stale", "devices": {dsv, members: [{user, devices}], next}}`, the same bytes the
  device listing writes, so a direct conversation or a small group re-encrypts without asking
  again. The page reads the conversation's `dsv` BEFORE the lists, so the version a client holds
  never claims a set newer than the lists it came with. Every conversation the routes answer
  carries its `dsv` for the same reason, and `create` and `open_direct` answer the version their
  own first membership transaction left, not the one before it.
- **Exactness and paging, reconciled with a flag.** The map must be exactly the current devices
  of the current members other than the sending device, as this section says. A group whose
  set does not fit one request sends its sender-key distribution with `page: true`: the map is
  then a subset, still with nothing extra, per-device only, and allowed in groups only. The
  fence is what catches a stale page.
- **The sending device is named in the body** and must be one of the actor's current devices.
  A session can name any device of its account, the line `prekeys.h` already draws: a stolen
  session can send ciphertext no recipient will decrypt, and nothing more.
- **The member read is paid only by a send with a map.** A group message under a sender key
  reads no member list at all; the fence alone guards it. A send with a map reads the current
  members' device ids, projected to `dev.i`, in one `$in`.
- **A retry is the first message even after the fence moved.** The client id is checked before
  the fence, so a lost response retried against an old version is answered with the stored
  message rather than refused, because re-encrypting it would be a second one.
- **Past a block (§3.6), the peer's queue rows are never written**; the sender's other devices
  still get theirs.
- **A per-device row expires with its message**, at the earlier of the message's expiry and
  `undelivered_days`, so a disappearing message does not outlast its timer in a queue. Revoking
  a message deletes its queue rows in the revoke's transaction, and so does the expiry sweeper.
- **An encrypted message cannot be edited or reacted to through the plaintext routes.** Both
  filter on a text message. An edit or a reaction in an encrypted conversation is a message
  with its `ref` inside the ciphertext (§4.5, §4.6); the cleartext `ref` check the table in
  §4.5 describes waits for a client that needs it.

Store-and-forward, but not delete-on-delivery.** WhatsApp deletes a message once every
device has it. That needs a per-device acknowledgement record for every message, which is a
write per device per message, the cost 11 §7 refuses for delivery receipts. The common
ciphertext is therefore kept under the kind's retention. That is safe for confidentiality,
since it is ciphertext under keys the server never had, and it lets a device that already
holds the sender key catch up after a week offline.

### 7.7 Encrypted media

The client generates a fresh 256-bit key per file, encrypts, and uploads the ciphertext into
the kind's `sealed_ns` with its declared SHA-256. The upload stream already hashes as it goes
(07 §4 step 2), so the check costs a comparison. The upload answers with a handle (§6.2), the
same as a plaintext upload. The **message** carries, inside its ciphertext, the file key, the
plaintext hash and the ciphertext hash. A recipient fetches the blob through the grant beside
the message (§6.1) and checks the ciphertext hash before decrypting. The server knows which
object a message references, because it has to hold the reference and mint the grant, but
the client is never told that id. A thumbnail is the client's: a few kilobytes of JPEG inside
the message ciphertext, or a second sealed blob.

Forwarding encrypted media re-sends the file key inside the new ciphertext and names the
source message (§6.2), so nothing is re-uploaded. The server sees a second reference on an
existing blob, which tells it that two messages carry the same file, and nothing else. Prior
art makes exactly the same trade.

### 7.8 Push for encrypted conversations carries nothing

The push payload is `{c, seq}`. The service worker wakes, fetches and decrypts, and writes
the notification text itself. Web Push encrypts the payload to the browser (11 §8), so the
push service could not read more anyway. Putting content in it would mean the server had the
content, which is the one thing this section exists to rule out.

As built (`chat::encrypted_push_payload`). The delivery `ChatPush` hands the transport has an
empty title and, as its body, exactly `{"c":"<uuid>","seq":<n>}`, the newest message waiting
for that recipient; the transport sends the body verbatim. Not the sender's name, the title or
the count either, although the server knows them: the notification a recipient sees should
come from what their own device decrypted, and from nowhere else. Neither wording hook is asked.
Who is pushed is the plaintext rule, except that a mention is inside the ciphertext, so
nothing breaks a mute.

### 7.9 Why channels are never encrypted

A channel's audience is unbounded and anyone may follow, so the key is effectively public. An
encrypted channel gives followers nothing an unencrypted one does not, costs a sender-key
redistribution to six figures of devices on every follow, and tells the application's users
something untrue. `kinds_are_well_formed` refuses it.

---

## 8. Live: the socket, typing, presence, push

### 8.1 Every write is an HTTP request; the socket only pushes

Sending, editing, reacting, receipts, joining: all of them are routes. The socket carries
wakes downstream and typing upstream, and nothing that changes durable state. The reason is
04 §8.4: **frames are not requests.** The rate-limit table, idempotency, `Origin`, the
stealth filter and the audit hooks all see requests and none of them sees frames. Sending
messages over the socket would mean re-implementing every one of them a second time, for a
transport whose only advantage over an HTTP/2 POST through the same connection pool is a few
hundred bytes of headers.

This also means a client behind a proxy that breaks WebSockets still works: the same routes
plus an SSE stream for wakes (§8.3).

As built (`ChatLive::follow_on_stream`). The fallback is the notification stream the client
already holds (`notifications/sse.h`), not a stream of its own: a reader's stream that holds a
lease receives `ChatWake` naming a conversation, and `ChatSync` when the reader's wake channel
is (re)subscribed. The conversation is all it carries. The client fetches from its cursor,
because a stream's event is 32 bytes and growing it for chat would grow every notification
stream in the process. A lease subscribes the reader's wake channel exactly as a socket does,
so a reader with a socket and a stream on one process is one subscription. Typing is not
carried. A client on the fallback gets fewer live features, and nothing breaks.

### 8.2 The socket

`register_websocket_route` (04 §8.2), so it inherits the gate, the `Origin` check, the
re-check period and the frame budget, with nothing new to wire. Frames are binary and
versioned, and the grammar is small enough to be a hand-written decoder in the foundation
with a fuzz target, as the edit recipe was (21 §2.2).

As built (`anvil/chat/frames.h`), downstream: `Wake`, `Typing`, `Presence`, `Receipt`,
`Membership`, `Ping`, `Pong`, `Sync` and `Mutation` (phase 21, `0x09`: the conversation and
its mutation counter, §4.5); upstream: `ClientTyping`, `ClientPing` and
`ClientPong`. `Pong` answers a `ClientPing`, because a browser cannot send a WebSocket
control ping and a client that asks whether its socket is alive needs an answer. `Sync`
carries nothing and means "you may have missed wakes": it is sent when the process's
subscription to the reader's wake channel is confirmed, the first time and after every
reconnect to Redis, because a wake published before that moment was never delivered and
nothing else would say so. A client syncs from its cursors on opening the socket and on
every `Sync`.

The grammar's golden bytes and every refusal, with its fault name and error code, are
`tests/testapp/chat_frame_vectors.h`, held by `chat_frames_test.cc` and printed by
`testapp_emit_chat_frames` as the fixture a client's codec is held to (phase 21): fourteen
goldens with what each decodes to, and some three hundred refusals, including every short
prefix and a trailing byte of each golden. The text validators' cases (§4.3) are likewise
`chat_text_vectors.h`, printed by `testapp_emit_chat_text`, every text as hex because some are
not UTF-8.

This **reverses 04 §8.5 for chat specifically**, and the reason is the one 04 §8.5 gave for
refusing: anvil had no producer. It has one now, so it ships a chat hub with a registry and
a per-connection ring of wakes, under SSE's policy that a full ring **drops the
connection** (11 §6). The generic WebSocket refusal stands. An application's own socket still
gets the budget and the re-check and nothing else.

The hub's slots come out of `kUpgradeShare` (`core/descriptor_budget.h`). Per account, at most
`max_devices` sockets, one per device. A second socket from the same device closes the first,
which is also how a stuck connection is replaced.

As built (`anvil/chat/hub.h`). The ring is **bytes, not slots**: 8 KiB per socket, every frame
behind a two-byte length, which holds three of the largest frames (a wake carrying 2 KiB) or
about two hundred and ninety that say "fetch it"; ten thousand sockets is 80 MiB of ring at
worst. A push into an empty ring wakes the socket's writer once, and its drain takes
everything; a frame that does not fit closes the socket as `Overflow`. A device is the
**session** the handshake's token names. A replacement takes the slot it frees, so neither the
process ceiling nor the account's refuses it. The subscriber's subscribe and unsubscribe are
called *under* the registry lock, deliberately: they only record a count in the subscriber's
own map, and called after the unlock two threads could deliver a user's first-socket subscribe
and last-socket unsubscribe reversed, leaving a subscription with no socket or a socket with no
subscription. What waits beyond the ring, in Trantor's output buffer and the kernel's, is
invisible to the hub because Drogon exposes no write-buffer level for a WebSocket; the socket
bounds it with a liveness deadline instead.

As built (`anvil/chat/socket.h`, `anvil/chat/live.h`). `ChatLive` is the one object a process
builds to push: it owns the member cache, the publisher, the hub and the subscriber, in the one
order in which the subscriber's thread never reaches a hub that is not there. A send calls it
after the commit, on the `db_pool` thread it already holds, and **before** the activity bump,
because the wake is what a waiting reader sees and the bump can be an `update_many` over a
thousand rows. The wake goes through Redis even to a socket on the sending process, so there
is one path to test and not two. The inline message is written by the history route's own
writer, so a message from a wake and the same message from a fetch are the same bytes and
carry grants, never ids; a revoked message never rides a wake. A message hidden past a block
(§3.6) wakes its sender's devices and nobody else's. The socket pings every 20 seconds and
closes one that says nothing for 45, on one timer per socket at the re-check period of 10
seconds. Every refusal is a close with an application code, from 4001 (replaced: do not
reconnect) to 4006 (a frame no correct client sends); `docs/01-seams.md` §18 lists them.
Typing travels up the same socket (§8.3).

### 8.3 Typing and presence are ephemeral and never stored

**Typing** is a frame upstream ("typing in c") that the hub republishes as a wake to the
conversation's other members, at most once every three seconds per connection per
conversation. It is never written anywhere and never reaches MongoDB. Dropping it costs
nothing. Channels have none, because a follower typing is not an event.

As built (`ChatLive::typing`). The socket admits one `ClientTyping` per conversation per three
seconds. It remembers the last eight conversations it relayed for and replaces the oldest,
because a person types in one at a time. A frame inside the interval is dropped, not refused.
The relay runs on `db_pool`, because whether the typist may be heard is a read: a current
member, not a channel, and in a direct conversation no block in either direction. Membership
is asked first, so every refusal is the same silence at the same cost and cannot say who
blocked whom. When `db_pool` is full the frame is dropped. The `Typing` frame goes through the
other members' wake channels, so it reaches whichever process holds their sockets, and never
to the typist's own devices.

**Presence** ("online", "last seen") is the feature most often turned into a stalking tool,
so it is **off unless the application turns it on**, and even then it is answered per viewer
through `hooks.may_see_presence(viewer, subject)`. Online is a Redis key per account with a
TTL refreshed by the socket. Last seen is the same key's last value, written to MongoDB at
most once every five minutes and only when the hook allows anyone to read it. If a value is
never shown, it is never written.

As built (`anvil/chat/presence.h`, `PresenceTracker`). One Redis key per account,
`anvil:chat:seen:<32 hex>`, holding a signed millisecond timestamp: positive is online and
heard from at, negative is offline since. It is kept for an hour after the last socket closes.
A user's first socket on a process and their last are the hub's subscribe and unsubscribe
callbacks, which record the change and return, so they are safe under its lock. The tracker's
own thread writes every change and refreshes every account it holds in **one pipelined round
trip per pass**, not one per socket. Online is a fresh positive value, so an account whose
process died reads offline within the online window. Last seen goes to MongoDB as a `$max` at
most once every 300 seconds per account, and only when `may_see(nil viewer, subject)` is true.
The read falls back to that row once the key has gone. The hook is
`ChatLiveConfig::presence.may_see` rather than a `ChatHooks` member, because presence belongs
to live delivery and a service without it has nothing to answer. The `Presence` downstream
frame stays in the grammar and is not sent. Pushing changes to everyone allowed to see them
needs a list of watchers per account, which is a presence registry by another name, so a client
polls the route for the accounts on screen. As built (phase 21), it polls them in one request:
`GET /chat/presence?users=<id>,<id>,…`, at most `kMaxPresenceBatch` (100, a chat-list page),
answers `{"presence":[{"user","online","last_seen"}]}` in the order asked with a duplicate
once. Every account is still asked of `may_see`, and only those it allows are read, in one
`MGET` and at most one `$in` for the ones whose key has gone, so a withheld entry costs and
reads exactly as one never seen. An account with sockets on two processes reads
offline when either loses its last one, until the other's next refresh. That is wrong by at
most one refresh interval, and nothing in chat decides anything by it.

### 8.4 Push nudges

A message for a member with no live socket produces a Web Push through the existing transport
(11 §7–8), to the `WebPush` clients the account already registered with notifications.
**One job per message, never one per recipient**, the same rule as 11 §7, paging the
recipients inside it.

Chat does **not** write notification rows. The chat list is already the inbox for chat, and a
second copy of every message in `notification_inbox` would be two read states for one fact,
and they would disagree.

- Coalesced per (account, conversation): a burst of forty messages is one push saying forty,
  not forty pushes. The job's idempotency key is the coalescing bucket, as in 11 §3.
- Muted members get nothing, unless they were mentioned and the kind allows mentions to
  break a mute (plaintext only: an encrypted mention is inside the ciphertext).
- Plaintext wording is the application's template (01 §7b), rendered per recipient in their
  current locale, with the preview off if the recipient turned previews off. Encrypted
  payloads carry `{c, seq}` and nothing else (§7.8).

As built (`anvil/chat/push.h`, `ChatPush`). The bucket is the conversation and a fixed window
of time (five seconds by default), so every send in one window asks the application's queue
for the same job and the queue keeps one. The job runs a grace period after the window closes
(three seconds by default), reads the conversation's newest hundred messages in one projected
query, pages the current members, and reads the `WebPush` endpoints of each page of a hundred
and twenty-eight in one `$in`. A recipient's count is every message in that read that they can
see, from somebody else, not revoked, not a system message, past their **delivered**
watermark; they are pushed when the count is not zero and at least one of those messages was
sent in this job's window, so an earlier window's messages never push twice.

**Who is pushed is decided by the delivered watermark, and not by the hub.** `has_socket()` is
one process's answer, and a recipient's socket may be on another. A device that holds a
message posts a receipt saying so, through whichever process it reached, and that durable
watermark is the one answer every process shares. The grace is the time it has to do so. The
cost is a duplicate, never a miss: a device holding a message that has not said so yet is
pushed anyway, and nothing makes a device that does not hold one look as though it does.

**The job is asked for before the message commits.** A process killed after the commit has
already asked. A process killed before it asked for a job that finds nothing new and pushes
nothing. A commit that lands within a second of its job's due time, a transaction slower than
the window and the grace, asks once more under a key of its own, so a slow commit is not a
miss either. Every queue is at-least-once, and a redelivered job pushes again, which the
device's notification tag absorbs.

Channels are not pushed, as they are not woken: a post would be a push to an unbounded
audience, and the application has notification topics for a broadcast. A message hidden past
a block asks for nothing. An encrypted conversation is pushed `{c, seq}` and nothing else
(§7.8), through the same job kind: its arguments name the conversation, and the job reads
whether it is encrypted. A verdict costs the endpoint exactly what a notification's would
(`notifications::record_verdict`), because it is the same endpoint.

---

## 9. Routes, hooks and the descriptor

anvil ships the handlers and an installer (`install_chat_routes`), and the application
declares the routes, their permissions and their budgets, as with image edits (01 §17). The
reference application's:

```
POST   /chat/conversations                         create a group or channel {cid, kind, …}
PUT    /chat/direct/{user}                         find-or-create (§3.2)
GET    /chat/conversations                         the chat list (§5.3)
GET    /chat/conversations/{c}                     the conversation and my membership
PATCH  /chat/conversations/{c}                     title, description
PUT    /chat/conversations/{c}/timer               the disappearing timer (§4.7)
GET    /chat/conversations/{c}/members             the member page, at most 100 (phase 21: was 200)
POST   /chat/conversations/{c}/members             add
PATCH  /chat/conversations/{c}/members/{u}         role
DELETE /chat/conversations/{c}/members/{u}         remove, or leave when {u} is me
POST   /chat/conversations/{c}/messages            send (§4.1, §7.6)
GET    /chat/conversations/{c}/messages            history: before= / after= a seq, changed_after= a mutation
PATCH  /chat/conversations/{c}/messages/{seq}      edit
DELETE /chat/conversations/{c}/messages/{seq}      revoke
PUT    /chat/conversations/{c}/messages/{seq}/reaction
GET    /chat/conversations/{c}/messages/{seq}/readers   {read_by, delivered_to} (§5.1)
POST   /chat/conversations/{c}/receipts            {delivered, read}
PATCH  /chat/conversations/{c}/preferences         mute, pin, archive, clear, private reads
POST   /chat/conversations/{c}/invites             create a link
DELETE /chat/conversations/{c}/invites             revoke a link {token}
POST   /chat/join                                  {token}
POST   /chat/conversations/{c}/follow              follow a channel
PUT    /chat/blocks/{u}, DELETE /chat/blocks/{u}   block, unblock (§3.6)
POST   /chat/uploads/{kind}                        the application's upload; answers a handle
GET    MEDIA_ORIGIN/m/{grant}/{role}               the grant route (§6.1)
GET    /chat/socket                                the upgrade (§8.2)
GET    /chat/presence/{u}                          online and last seen, per viewer (§8.3)
GET    /chat/presence?users={u},{u},…              the same for up to 100 accounts (phase 21)
-- phase 20
GET/PUT /chat/devices, POST /chat/devices/link, DELETE /chat/devices/{d}
PUT    /chat/devices/{d}/keys                      rotate the signed prekey, the last-resort key (phase 21)
POST   /chat/link-requests[/read|/approve|/collect] the link relay (§7.3.1, phase 21)
POST   /chat/keys                                  upload prekeys
POST   /chat/conversations/{c}/keys/claim          {users: [≤ 32]} → per account, one bundle per device (§7.5)
GET    /chat/conversations/{c}/devices             the members' devices and dsv, paged
GET    /chat/device-queue?device={d}, DELETE /chat/device-queue?device={d}&through={id}
```

As built (phase 21), a claim names up to `kMaxClaimBatch` accounts, `{"users":[…]}`, and is
answered per account in the order asked, `{"claims":[{"user","bundles":[…]|null,"refused":
null|"NOT_FOUND"|"RATE_LIMITED","retry_after":null|s}]}`: phase 20's one account per request
made a thousand-member group's first send a thousand requests, eight minutes at the reference
budget. Thirty-two is the response bound: a bundle is about 630 bytes, so at the reference five
devices an answer is about 100 KiB and at anvil's sixteen about 320 KiB. The caller's
membership refuses the whole request; every target's membership is then read in one `$in`
**before any budget is spent**, and each target's per-target budget is spent only for a target
who is a member. A refusal is per account rather than a 429 for the request, because the keys
already claimed for the others are spent and must reach the claimer. The per-claimer budget is
spent once per request; the per-target one is what protects a victim's pool, and it still
counts every claim against them. The single `{user}` form is retired rather than kept beside
it: encryption is not for production, no client was written against it, and one shape is one
decoder.

As built, the claim is scoped to a conversation, which the design's `/chat/keys/claim` was not:
a bundle is claimed to encrypt to somebody the claimer shares an encrypted conversation with,
and anybody else is the stealth 404. The per-target budget is spent only after both
memberships hold, so a stranger asking about an account spends none of it; otherwise anybody
could lock everybody out of claiming against an account by asking. Every device route names
the device it acts for, and the service refuses one that is not the caller's current device.
A first device needs `ChatHooks::authenticated_at`, the application's answer to when the
session last proved itself with a primary credential; unset refuses every first device.
Reading the queue, sending and uploading keys each record the device as seen, coalesced to a
write an hour, which is what keeps a device in use from the idle sweeper.

- **Every conversation-scoped refusal is the stealth `404`.** Not a member, never a member, no
  such conversation, and a conversation that has been deleted all answer with the same bytes,
  because each of them is the same filter: a member row that is not there.
- **Every write checks `Origin`** before reading the body, as the edit and account routes do.
- **The budget is spent on `db_pool`**, ahead of the body parse and the work: it is a Redis
  round trip, and the loop thread that accepted the request may not block on one.
- Hooks: `may_reach`, `may_see_presence`, and `on_message`, `on_membership`, `on_device`. The
  last three run after the commit, on the `db_pool` thread that made the change and before
  the response is written, so they must not block — `AuditService::write_async` is the call —
  and one that throws is logged and never fails the request. Messages are not audited by
  anvil. The volume is too high and the content is private, but an application with a
  regulatory duty to retain has `on_message` and can do so knowingly.
  As built, `may_see_presence` is `ChatLiveConfig::presence.may_see`. Asked with a nil viewer,
  it means "may anybody", which decides whether last seen is stored at all.
- The descriptor's `limits` gains `chat`: the bounds of §2.4, per kind, plus each kind's key,
  shape, encryption mode and rights. The client's composer is built from the same table the
  server enforces, so hammer never carries a second copy of a server rule (07 §5's argument,
  once more).

### 9.2 Staff review and reports

As built (phase 21). An application may need somebody outside a conversation to read it:
staff reviewing a reported dispute between a buyer and a seller. The plaintext is already
stored for the kind's retention; what was missing is the way in, and it is narrow on purpose.

- **A kind says whether it is reviewable** (`ConversationKindSpec::reviewable`, default false),
  and `kinds_are_well_formed` refuses it on an `E2ee::Required` kind. Per kind and not per
  permission, so a kind the product promised was private cannot be read by any permission at
  all, and the choice sits in the table where everybody reviewing the product can see it. The
  descriptor publishes it, so a client can tell its person. An `Optional` kind's encrypted
  conversations are refused one by one: the server holds nothing to show.
- **Staff read through two routes behind a permission the application names** in its route
  table, `Stealth`, so the paths reach only a holder's table: `GET /chat/review/{c}` (the
  conversation and a page of its members, `{conversation, members:[{user, role}], next}`) and
  `GET /chat/review/{c}/messages?before=` (the history shape over the whole retained log,
  hidden messages included, attachments as grants, `mine` false on every tally). anvil checks
  no permission itself and names none; the access filter does it from the application's table.
- **Every read is audited before anything is shown**, synchronously, through
  `ChatServiceDeps::review = {AuditService*, AuditAction}` with the reader as actor, the
  conversation as subject and the request's address; a read whose row cannot be written is
  `503`. A reviewable kind refused is `403` with `"reason":"chat.not_reviewable"`.
- **A staff read is not a membership.** It moves no watermark and appears in no member's
  receipts, list or wakes.
- **A member files a report** with `POST /chat/conversations/{c}/reports {from, to?, note?}`:
  a range of at most 500 messages inside what they can see, a note of at most 1 000 code points
  of their own prose, `201 {"id"}`, and `200` with the first report for the same range again
  (a unique `{c, by, f, to}` index). `ChatHooks::on_report` is told after the commit; what a
  report means is the product's. Staff list them with `GET /chat/reports?after=`,
  newest first, `{reports:[{id, conversation, reporter, from, to, note, at}], next}`.
- The four route ids are installed all together or not at all, so an application that
  reviews nothing declares none of them.

### 9.1 Indexes, all declared by the application, all explained

| Collection | Index | Serves |
|---|---|---|
| conversations | `{dpk}` unique, partial on `dpk` existing | §3.2 |
| conversations | `{by, cid}` unique, partial on `cid` existing | an idempotent create (§3.1, phase 21) |
| members | `{c, u}` unique | every membership check, the member listing |
| members | `{u, ar, act, _id}` | the chat list, with its compound cursor |
| members | `{u, pin}` partial on `pin` existing | the pinned rows |
| members | `{c, r, js}` | owner succession |
| members | `{c, rd}` | "read by" |
| members | `{c, dlv}` | "delivered to" (§5.1, phase 21) |
| members | `{u, c}` | a device change's walk over one person's conversations (§7.4) |
| messages | `{c, s}` unique | history, sync, a message by seq |
| messages | `{c, u, cid}` unique | idempotency |
| messages | `{exp, _id}` partial on `exp` existing | the sweeper (§4.7) |
| messages | `{c, mu}` partial on `mu` existing | the mutation catch-up (§4.5, phase 21) |
| reactions | `{c, s, u}` unique | §4.6 |
| invites | `{exp}` TTL | a garbage collector; the redeem filter refuses an expired link |
| blocks | `{u, b}` unique | both directions, as two seeks in one `$or` |
| reports | `{c, by, f, to}` unique | a retried report is the first (§9.2, phase 21) |
| identities | `{dev.i}` unique, partial on `dev.i` existing | one device id names one device (partial, because an account whose devices were all unlinked has an empty array, which a unique multikey index reads as one key) |
| identities | `{dev.seen}` | the idle sweeper |
| identities | `{pend}` partial on `pend` existing | the device-change sweeper (§7.4) |
| prekeys | `{d, kid}` unique | a claim's `{d, u}` rides the prefix; a duplicate key id is refused |
| device queue | `{d, _id}` | a device's rows in send order, and the acknowledgement's range delete |
| device queue | `{c, s}` | a revoked or expired message's rows |
| device queue | `{exp}` TTL | a garbage collector; the read filters `exp` too |
| link requests | `{u, sid}` | a session's earlier request, replaced (§7.3.1, phase 21) |
| link requests | `{exp}` TTL | a garbage collector; every read filters `exp` too |

As built. The design had `{c, att}` for "a message references this object" on the serving
path; with grants (§6.1) the serving path never asks that question, and forwarding and revoking
read the message row they already name, so the index had no query and was not built.

`chat_messages` is the one collection in this library expected to need sharding. Its shard
key is `{c: 1, s: 1}`: every read is one conversation's range, so it targets one shard, and
`c` is a UUIDv4, so new conversations spread across shards rather than all landing at one end
of a monotonic key.

---

## 10. Deferred, deliberately

Recorded so that each one is a decision and not an omission. Each waits for an application
that needs it.

- **Calls.** Signalling could ride the socket, but a call is TURN servers, a media relay and a
  bandwidth bill, none of which is a library concern.
- **History transfer to a new device.** Device to device, encrypted, in the client (prior art
  does the same). The server would carry an opaque blob and nothing more.
- **Server-side search.** Impossible for encrypted conversations. For plaintext ones it needs
  a text index with per-locale analysers that MongoDB's `$text` does not provide for Arabic,
  and that is a project of its own.
- **Verifiable abuse reports (message franking).** The sender commits to the plaintext inside
  the ciphertext and the server binds the commitment to sender, conversation and time, so a
  report can be verified without the server ever reading unreported messages. It is the right
  answer for moderation under encryption and needs a client protocol first. Until then a
  report carries whatever the reporter's client submits, and is labelled unverified.
- **Forward limits** ("forwarded many times"). A counter on plaintext messages, and a
  client-asserted one in encrypted messages, which is only worth building once a product
  wants the label.
- **View-once media**, **sealed sender**, **an MLS suite**, **resumable uploads** above the
  current cap, and **video transcoding** (§6.4).
- **Stories / status.** A different retention and audience model, not a conversation kind.
