# 05 — Chat

The client half of direct conversations, groups and channels: the calls, the log a device
keeps in step with the server, live delivery, the rendered surface, and end-to-end encryption.
anvil's half is anvil `docs/22-chat.md`, and this document assumes it: the conversation kinds,
the sequence number, the routes, the socket's frames and the server's half of the protocol are
all fixed there. The rows that build this are [`15-tasks.md`](15-tasks.md) §Phases 10–15;
anvil's are its §Phases 18–20, and that repository's phase 20 names this document as its
precondition.

**Status: design.** Nothing here is built. The plaintext half (§§3–8, §10) can start against
anvil as it stands. The encrypted half (§9) can be built and tested, and **is not for
production until the external review of both halves is checked** (§9.13). That is a sentence
in anvil's documentation and a type in this library's.

anvil's two governing sentences hold here unchanged:

> **A conversation is an ordered log with one writer of order: the server's sequence number.**

> **Encryption is decided when a conversation is created and never changes afterwards.**

The client adds two of its own:

> **The server's log is the record, and everything the client holds is a cache of it — except
> under encryption, where the device's archive is the only copy of what it decrypted.**

> **A key never leaves the device that made it, and nothing in this library can ask for one.**

The first sentence is why plaintext chat persists nothing and encrypted chat must. The second
is why every private key is a non-extractable `CryptoKey`, and why no API here takes or returns
key bytes.

---

## 1. What hammer owns, and what the application does

| hammer ships (machinery) | The application supplies |
|---|---|
| typed calls over anvil's chat routes, and the one decoder of their hand-written answers | the **route values**, from its generated client (§3.2) |
| the composer's validator, which restates anvil's text rules | every word: labels, placeholders, the sentence for every refusal, in every locale |
| the per-conversation log, its cursors and catch-up, the chat list, unread counts | which kinds exist and their bounds (the descriptor carries them) |
| the outbox: client ids, ordering, retries, the pending-to-stored reconciliation | what "sent", "delivered" and "read" look like, if anything |
| the socket: frame codec, one per device, the close-code policy, the fallback | the stream route's event names, if it carries chat on SSE (§7.6) |
| typing and presence, as state | whether to show either, and to whom (anvil asks the server's hook) |
| grants as `<img>`/`<video>`/link addresses; upload handles | the upload route for chat media (§8.1) |
| the device key store, the link ceremony as bytes, chain verification, security codes | how the link bytes travel between two screens (§9.4.3) |
| the protocol: X3DH, the Double Ratchet, sender keys, the content envelope | the trust policy for a changed security code — required, never defaulted (§9.5) |
| sealed media, encrypted in a worker | the worker entry, two lines (§9.11) |
| the decryption a service worker needs to word an encrypted push | the service worker, the notification's words and its icon (§9.12) |
| unstyled components: list, conversation, composer, members, devices | every class name, colour, icon and font |

Nothing in the first column names a product. "Groups of 1 024", "admins only", "24 hours" and
"Message deleted" are all values or words an application supplies. `tests/testapp/` is where
the reference application's live, as for every other seam.

---

## 2. Entry points

Four, and the split is decided by who imports them rather than by tidiness:

| Entry point | Holds | May import | Needs |
|---|---|---|---|
| `hammer/chat` | calls, answers, the composer validator, the frame codec, the socket client, sync, the outbox, the stores | `hammer`, `hammer/wire`, `hammer/state` | **no DOM** |
| `hammer/chat-dom` | the components (§10) | `hammer`, `hammer/state`, `hammer/dom`, `hammer/chat` | a `document`, injected |
| `hammer/chat-e2ee` | the vault, devices, verification, the protocol, the envelope, sealed media's page half, the push handler | `hammer`, `hammer/crypto`, `hammer/wire`, `hammer/state`, `hammer/chat` | **no DOM** |
| `hammer/chat-seal-worker` | `serveSealPool`: chunked encryption, decryption and hashing of sealed media | `hammer`, `hammer/crypto` | a dedicated worker |

- **`hammer/chat` and `hammer/chat-e2ee` have no DOM because a service worker imports them.**
  An encrypted push is worded by the service worker after it decrypts the message (§9.12), and
  a service worker has no `document`. A store that reached for one would be a store the push
  path could not load. `tools/check-layering.sh` refuses a bare `document.` or `window.` in
  both, as it does in `dom`.
- **`hammer/chat-dom` does not import `hammer/chat-e2ee`.** An application with plaintext chat
  only pays nothing for a ratchet. The components read decrypted content through the store,
  and the store takes encryption through an interface `hammer/chat` declares and
  `hammer/chat-e2ee` implements (`ConversationSealer`, §9.8). A plaintext application never
  constructs one.
- **The worker half is its own entry point**, as `hammer/prehash-worker` is, because a worker
  bundle that imported the page half would carry the page half into every worker.
- **No chat-specific React hook.** Every store here is a `Readable`, and `useStore` already
  binds one. A hook is written when a consumer shows a lifecycle `useStore` cannot express.

Each entry point gets a gzip ceiling in `tools/bundle-budget.json`, set from its first measured
build as `hammer/edit`'s was. The targets the design is held to are in §11.

---

## 3. The descriptor and the routes

### 3.1 `limits.chat`

anvil's descriptor format 4 adds `limits.chat`: anvil's bounds, and per kind its key, shape,
encryption, history, receipts, windows, timers, the permissions that create one and each role's
rights **by name**. The generator emits it as `kChatLimits`, `as const`, so a kind key, a shape,
a right and an encryption mode are each a union and not a `string`.

- **A descriptor with `"chat": null` emits no `kChatLimits`**, and `createChat` requires one, so
  an application whose server has no chat fails to type-check rather than validating against
  numbers nobody sent. This is `kEditLimits`' arrangement (04 §6), and the reason is the same.
- **The generator refuses a kind that cannot be true**: a `direct` kind whose `max_members` is
  not 2, an encrypted `channel`, an encrypted kind with `history: "full"`, a right name it does
  not know. anvil's `kinds_are_well_formed` already refused each of these at compile time, so
  reaching the generator means the descriptor was edited by hand or the server is a different
  one, and both deserve a build failure.
- **Rights are affordances only** (00 §4.1). `mayI(kind, role, right)` decides whether a button
  is drawn. It never refuses a call the person managed to make. The server checks the right
  against the kind at every request, and a role read off a stale membership is a stale copy.
- **The format moves from 3 to 4**, and the fixture is regenerated from anvil's
  `testapp_emit_descriptor` in the same commit as the reader change. `check-descriptor.sh`
  regenerates the client from the committed descriptor and is green by construction, so a
  stale fixture is invisible until a person runs the emitter.

### 3.2 The routes are the application's values

anvil's 34 chat routes, the socket and the grant route are declared by the application, and
their ids are the application's (anvil 01 §18). hammer therefore takes them as values from the
generated client, in one object:

```ts sketch: the shape of the route table createChat takes, abridged; no file exists yet
type ChatRoutes = {
    readonly list: CallableRoute;           // GET    /chat/conversations
    readonly get: CallableRoute;            // GET    …/{c}
    readonly send: CallableRoute;           // POST   …/{c}/messages
    readonly history: CallableRoute;        // GET    …/{c}/messages
    readonly receipts: CallableRoute;       // POST   …/{c}/receipts
    // … one member per route anvil installs, 34 in all
    readonly socket: CallableRoute;         // GET    /chat/socket
    readonly grant: CallableRoute;          // GET    MEDIA_ORIGIN/m/{grant}/{role}
};
```

Each member's type names the placeholders the handler reads, in order, so a route whose
pattern has the wrong placeholders fails to type-check where anvil would refuse it at boot.
The device, key and queue routes are a second object, `E2eeRoutes`, which only
`hammer/chat-e2ee` takes.

### 3.3 The answers are hand-written, so they are decoded in one place

anvil's chat responses are **undescribed** in the descriptor: a message nests attachments,
mentions, a card and a system event, which the response grammar cannot express, and anvil
declined to publish an approximation a generated client would trust. So there is no generated
`ResponseOf` for any chat route, and `chat/answers.ts` is the one decoder for every shape
anvil's `routes.cc` and `render.cc` write: message, conversation, membership, chat list item,
history page, tallies, device, device page, bundle and queue row.

- **Each is narrowed once, at the boundary, from `unknown`** (`CLAUDE.md` §3.1). A 2xx body that
  is not the shape is `chat-answer`, never a guess, as `edit-answer` is for edits.
- **The decoder is written against recorded bytes**, not against this document. The live suite
  records each answer the reference server writes, and the unit suite decodes the recording.
  A decoder checked against a fixture written beside it is two copies of one belief.
- **Every attachment is a grant**, and the decoder makes it a branded `MediaGrant`. Nothing can
  construct one from a string a caller holds, so an `<img>` built from anything but a decoded
  answer does not type-check.
- **Every time is a `ServerInstant`** (`core/time.ts`), so `sent_at`, `edited_at`, `expires_at`,
  `linked_at` and `muted_until` cannot be subtracted from `Date.now()`.
- **Unknown values decode to a member hammer owns.** A message kind, a system event or a role
  anvil appends decodes to `unknown`, rendered by the application as it likes. A client that
  crashed on a new event would turn a deploy into an outage (00 §6).

### 3.4 What each route may retry

Retry policy belongs to the library (`CLAUDE.md` §6), and for chat it is decided by what anvil
does with a repeat, not by the verb:

| Route | Retried by hammer | Why |
|---|---|---|
| `send` | yes, with the same `cid` | `{c, u, cid}` is unique, so a repeat is answered with the first message. `200` and `201` are one success |
| `receipts`, `preferences`, `react`, `edit`, `open_direct`, `follow`, `join`, `block`, `unblock` | yes | each is a `$max`, an upsert or a set, so the second write is the first |
| `create` | **no** | there is no client id, so a repeat makes a second group. A lost response is surfaced as unknown. The cross-repo row asks for a `cid` |
| `add_members`, `remove_member`, `revoke`, `create_invite` | **no** | described as not idempotent, so a lost answer is surfaced. A person sees the outcome on the next read |
| `register_device`, `link_device` | **no**, then confirmed | a repeat is refused because the first one landed. On an ambiguous failure, the client reads `my_devices` and treats its own device id, with its own keys, as success |
| `upload_prekeys` | **no**, then confirmed | a repeat of a stored batch is refused on the `{d, kid}` index. Key ids are allocated by this device alone, so a collision can only be its own earlier upload, and is success |
| `claim_prekeys` | **no** | a claim consumes a key. A lost answer wastes a one-time prekey, and the next claim takes another |

---

## 4. Text, and the composer's validator

anvil validates plaintext message text by fixed rules (anvil 22 §4.3), and under encryption it
validates nothing: the rules are the receiving client's to enforce. So hammer restates them,
once, in `chat/text.ts`, and uses the same function in both places:

- **Before a send**, to save the round trip: the composer learns that a message is too long, or
  holds a character the server refuses, while the person is typing.
- **After a decrypt**, as the only enforcement there is. A message a sender's modified client
  wrote with a U+202E in it is refused at the recipient exactly as the server would have
  refused it in plaintext, and rendered as the application's "could not display" state.

The rules, restated from anvil and not reinterpreted:

- Bounds are **code points**, from the kind's `text_max_code_points`, never `String.length`
  (`CLAUDE.md` §8).
- `\n` is the only line break. `\r`, CRLF, U+2028 and U+2029 are refused.
- C0 and C1 controls are refused except `\n` and `\t`. Bidi overrides and embeddings
  (U+202A–U+202E) are refused. Isolates (U+2066–U+2069) are allowed.
- **The text must already be NFC, and is refused if it is not.** That is the server's rule, and it
  puts a duty on the composer: it normalises the text to NFC **before** it computes any mention
  offset, and recomputes the offsets after any edit. The server refuses rather than normalises
  because normalising would move the offsets. A composer that normalised after placing them
  would send spans pointing at the wrong names.
- A message that renders as nothing is refused as empty. White space and default-ignorable code
  points are not content.
- **Mentions are `{offset, length, user}` in code points, at most 32, each inside the text.** The
  DOM layer works in UTF-16 indices, so the one conversion between the two lives in
  `chat/text.ts` and nowhere else. A mention is never parsed out of `@name`.
- A reaction is one grapheme cluster of at most 8 code points (`Intl.Segmenter`).
- A link preview's URL passes the same scheme rule as anvil's HTML writer.

**The contract is a vector file, not this list.** anvil's `chat/text` has a fuzz target and a
case table. The cross-repo row asks for them printed as a fixture, as the edit recipe's were,
so that both validators are held to one set of inputs.

**Link previews are never fetched by hammer.** Fetching a URL a person typed discloses it, and
in an encrypted conversation that is the disclosure the encryption exists to prevent. A browser
cannot fetch an arbitrary page anyway. A preview's title, description and URL are the
application's to supply, from its own unfurler or from nothing, and hammer validates them as
untrusted input.

---

## 5. The log, the cursors, and sync

### 5.1 Three numbers per conversation

| Name | Meaning | Moved by |
|---|---|---|
| `head` | the conversation's newest seq, as the server last said | the chat list, `get`, a wake |
| `cursor` | every message this device should hold with `seq ≤ cursor` has been read from the server | a catch-up, and nothing else |
| `shown` | the highest seq this device has put on a screen | the DOM layer, through the store |

**Sequence numbers are monotonic, not dense** (anvil 22 §4.1). A crash between the allocate and
the insert burns a number. A hole is therefore not loss, and a client that refetched around one
would refetch forever around a seq that will never exist. **hammer never looks for a hole.** It
asks `after=cursor` and believes the answer.

That is why a wake does not move `cursor`. A wake carrying message 41 says that 41 exists. It
says nothing about 40, which may be burnt, still committing in another process, or carried by a
wake that Redis dropped. Moving the cursor to 41 on the wake's word would skip 40 forever if it
was the dropped one. So:

- **A wake is a display hint.** Its inline message is shown at once, deduplicated by seq and by
  `cid`. The cursor stays where it was.
- **A catch-up is the record.** It asks `after=cursor`, pages until a page comes back shorter
  than its limit, and then moves the cursor to the last seq it read.
- **Catch-up is coalesced, not per wake.** A busy group delivers wakes faster than anyone reads.
  One catch-up per conversation per window (2 s, while the conversation is open) is enough, and
  it is what anvil's inline wake was built to make rare. A conversation that is not open is
  caught up when it is opened, and only its chat-list row is updated from wakes.

The triggers for a catch-up of every held conversation are a socket opening, every `Sync` frame,
a `ChatSync` stream event, the tab becoming visible after being hidden, and a page resuming from
the back/forward cache or from being frozen. Each is a moment at which wakes may have been lost,
and anvil sends nothing else to say so.

### 5.2 What a catch-up cannot see, and the fallback until it can

**An edit, a revoke or a reaction on a plaintext message changes a row without allocating a
seq, and anvil sends no wake for any of them.** A device that already holds message 10 asks
`after=cursor`. Message 10 is not after the cursor, so the device never learns that it was
edited, revoked or reacted to. anvil 22 §4.5 says a revoked row keeps its seq "so the next sync
can tell them it had been revoked". A cursor sync cannot. This is a cross-repo row, and the
change that closes it is a mutation counter the server stamps on the row, plus a catch-up by
that counter.

Until then, the fallback is deliberately bounded rather than complete. **The window on screen is
re-read** (`before=` the newest held seq, one page) on the same triggers as a catch-up, and on
opening the conversation. Edits, revokes and tallies inside the visible page are then at most
one trigger stale. Outside it, they are corrected when the person scrolls there, because a page
read from the server replaces the page held.

The encrypted half does not have this gap. There an edit, a reaction and a revoke are messages
with a `ref` inside the ciphertext (§9.10), so each has its own seq and arrives through the log.

### 5.3 The window a conversation holds

An open conversation holds a **bounded window**: 300 messages by default, newest first, and the
rest are pages the person scrolls into. Scrolling back reads `before=` the oldest held seq and
evicts from the far end. A tab stays open for days, and a group that talks all day would
otherwise be a list that only grows (`CLAUDE.md` §2.3).

- **Only the open conversation holds messages.** The chat list holds rows, and a row holds the
  conversation, the membership and the approximate unread count (anvil 22 §5.2), and no
  messages.
- **Everything is keyed by the identity allowed to read it and dropped on identity change**
  (00 §7). Plaintext chat persists nothing: no IndexedDB, no `localStorage`, no service-worker
  cache of a history page. anvil answers every chat route `private, no-store`, and hammer keeps
  that promise.
- **Expiry is compared with the server's clock.** A held message with `expires_at` is dropped once
  the newest server instant this tab has seen is past it, and that instant comes from the
  newest `sent_at` in any answer or wake. Two server instants are two points on one line. The
  device clock is never asked (`CLAUDE.md` §6), so the server's filter on its next read is
  the authority, and the comparison only stops a message outliving its timer on a screen
  between reads.

### 5.4 The chat list and the unread count

The list is anvil's range scan, paged by its `{after_at, after_id}` cursor, with pinned rows on
the first page. It is a `Resource` with the existing cache rules: bounded, keyed by identity,
and invalidated by the stream rather than by a timer.

- **The unread count is the server's guess, and the conversation corrects it.** anvil sends
  `head − read`, capped at 999, which counts the reader's own messages and system messages
  (anvil 22 §5.2). When a conversation is opened, hammer counts the held messages above `read`
  that are from somebody else and are not system messages, and that number replaces the guess
  for that row until the next list read. What a badge says above some number is the
  application's call.
- **A wake moves its row to the top locally**, and the next list read replaces the local order.
  The server's activity bump is coalesced to two seconds, so the server's order can briefly
  disagree with the wake's. The rule `state/inbox.ts` follows applies here: a local adjustment
  is a guess between server answers, and the next answer wins outright.

### 5.5 Past members, blocks, channels

- **A past member's conversation is read-only and frozen.** anvil shows a past member the head at
  `ls − 1` and refuses them the member list. The store exposes `current: false` from the
  membership, and the composer is not drawn. A send that comes back `404` turns a conversation
  that believed itself current into a past one, because a removal is the common reason.
- **A message hidden past a block is shown to its sender as sent, forever** (anvil 22 §3.6). Nothing
  in the client can tell, and nothing tries to.
- **A channel follower is never woken** (anvil 22 §5.4). A channel's row updates from chat list
  reads, and an open channel catches up on the triggers above. A follower has no receipts, no
  typing and no member list, so the store exposes none.

### 5.6 What is stored, and who else may read it

An application may need somebody outside a conversation to read it: staff reviewing a reported
dispute between a buyer and a seller, say. **That storage is the server's, and it already
exists.** anvil keeps every plaintext message for its kind's `retention_days` (0 means until it
is revoked or expires), so whether messages are kept, and for how long, is a per-kind decision
the application already makes in its kind table. A kind for marketplace disputes keeps a year;
a kind for casual chat keeps thirty days.

Persisting messages **in the client** does not serve this need, and hammer does not do it for
plaintext. A reviewer is on their own device, so a copy in the buyer's browser is a copy nobody
reviewing can read. It is also one more copy of a person's words, outliving the session on a
device that may be shared (§5.3).

What is missing is the reviewer's way in, and it is anvil's to build (§13, row 13):

- **A staff read of a conversation** the reader is not a member of, behind a permission the
  application names. Every read is audited, because reading somebody's private conversation is
  exactly the act an audit log exists to record. The route is holder-scoped, so its path never
  reaches a bundle that is not staff's (01 §4.1).
- **A report**: a member files one against a message, which records the reported range, so a
  reviewer is pointed at what was reported rather than handed the whole history.
- **Neither exists for an encrypted conversation**, and that is a property and not a gap. The
  server holds nothing it can show. A report there carries what the reporter's client submits
  and is unverified until message franking exists (anvil 22 §10). A kind the application needs
  to inspect is a kind with `encryption: "never"`, and choosing that is the application's
  decision, made in the same table.

hammer's half is a read-only conversation view over the staff route, built from the same
decoder, store and components as a member's, with no composer, no receipts and no typing. A
staff reader must not move a member's read watermark or appear in "read by".

---

## 6. Sending

### 6.1 The outbox

Every send goes through one outbox per chat, and the outbox owns four things:

1. **The client id.** Sixteen bytes from `crypto.getRandomValues`, minted when the person presses
   send and never re-minted. It is anvil's idempotency key (anvil 22 §4.2), and it is durable on
   the server for as long as the message is.
2. **Order within a conversation.** Sends to one conversation are made one at a time, so the
   server's seq order is the order the person pressed send in. Two concurrent sends would be
   ordered by whichever reached the server first, and a person who sends "no" and then "wait"
   would be shown them reversed. Sends to different conversations run concurrently, under the
   request queue's per-origin bound.
3. **Retries**, per the library's policy (§3.4). A network failure, a `503` and a `500` retry
   with the same body. A `429` waits exactly `Retry-After` and holds the whole conversation's
   queue, because the budget is per account. A `400` is terminal, and the message stays pending
   with the server's field and reason so the person can correct it. A `404` is terminal and
   makes the conversation past (§5.5).
4. **Reconciliation.** A pending message is shown at once, keyed by its `cid`. It becomes the
   stored message when the send answers with its seq, **or** when a wake or a catch-up delivers a
   message with the same `cid` from this account, whichever comes first. anvil renders `cid` on
   every message, so the three paths converge on one row without counting.

**A pending message is never the input to another request** (`CLAUDE.md` §6). A reply to a
pending message, an edit of one or a reaction to one waits in the outbox until the target has
its seq.

**Plaintext pending messages live in memory.** A reload loses an unsent plaintext message, and
that is a stated decision rather than an oversight. A persisted outbox is a persisted API request
holding a person's words, which outlives the session that wrote it unless something wipes it.
That is the opt-in 00 §7 describes, not a default. Under encryption the outbox persists, because
there it must (§9.8), in the vault that the same wipe destroys.

### 6.2 Receipts

`dlv` and `rd` are watermarks, advanced with `$max` (anvil 22 §5.1), so hammer sends
watermarks, never per-message receipts.

- **Delivered is posted when the device holds the messages**: after a catch-up, or after an inline
  wake was put in the store. anvil decides whom to push by the delivered watermark and not by
  sockets (anvil 22 §8.4), so a device that holds a message and does not say so is a device
  whose account is pushed about it anyway. Delivered is posted promptly, within a second of
  holding, coalesced per conversation.
- **Read is the highest seq actually shown**, while the conversation is open, the tab is visible and
  the window has focus. The DOM layer reports what crossed the viewport, and the store never
  reports read for a hidden tab. Read is posted when it moves, coalesced to one post per
  conversation per two seconds, and flushed with `fetch(…, { keepalive: true })` on
  `visibilitychange` to hidden. A read receipt sent from the server's head rather than from the
  screen tells the sender a lie (anvil 11 §4).
- Both values are clamped by the server to its head, and both are sent from a watermark the store
  holds, so a duplicate post is harmless.

**What a sender's ticks can show.** anvil sends no `Receipt` frame, although the grammar has one,
and offers no route for another member's delivered watermark. Read is answerable:
`read_by(seq)` returns the members whose `rd ≥ seq`, and because `rd` is a watermark, **one call
for the newest of the sender's own messages answers every tick on the screen.** Everything older
was read by at least the same people. That is one request per open conversation on each sync
trigger, never one per message. Delivered is not answerable for anyone but oneself, and the
cross-repo row asks for it. Until then, the store exposes sent and read, and an application
that wants a delivered state has no honest source for one.

### 6.3 Preferences

Mute, pin, archive, clear and private reads are one `PATCH`. Two of them need care:

- **`muted_until` is an absolute instant, and the client has no clock to compute one from.**
  "Mute for eight hours" is `now + 8 h`, and `Date.now()` is the device's clock, which is wrong
  by an amount nothing on the device can measure. The cross-repo row asks anvil for a duration.
  Until then, hammer computes the instant from the newest server instant it has seen, the same
  instant §5.3 compares expiry against, and documents the bound: the mute ends late by however
  long ago that instant was seen.
- **Clear** sets `hide_before` on the server. The held window is emptied locally in the same step,
  and, under encryption, the archive's rows below it are deleted in the vault (§9.3).

### 6.4 Invites

An invite token is a bearer secret in a link shared somewhere the inviter does not control.
hammer's rule about secrets in URLs (`CLAUDE.md` §5) is satisfied the way the preview capability
satisfies it. **The application builds the join link with the token in the fragment**, which is
never sent to a server or in a `Referer`. `chat.join` reads it once, strips it from the address
bar with `history.replaceState`, and posts it in the body, and anvil already reads it from the
body rather than the path for the same reason. A token that does not work is anvil's one `404`,
whatever the cause, and the copy for it must not guess which cause it was (anvil 22 §3.5).

---

## 7. Live delivery

### 7.1 One socket per device, owned by one tab

anvil keys the socket by **device**, and for the socket a device is the session. A second socket
from the same session closes the first with `4001`, and the client must not reconnect after a
`4001`, because reconnecting takes the socket back and the two tabs would then trade it for ever.

N tabs of one profile are one session. So **exactly one tab holds the socket**: the leader,
elected by `navigator.locks` under `hammer.chat.socket`, the arrangement `wire/leader.ts` already
uses for the refresh and the stream. The leader fans every frame out over `BroadcastChannel`.
Followers handle the same frames through the same dedupe, so the code path is the same whether a
tab holds the socket or not.

- **Without `navigator.locks` there is no socket.** The degraded path that refresh accepts (one per
  tab, race left in) is not acceptable here: two tabs, two sockets, and each closes the other
  with `4001` on every reconnect. The store reports `degraded` and runs on polling or the stream
  fallback, which work in every tab.
- **The leader gives the socket up when it is frozen or hidden for long enough to be throttled.**
  On the `freeze` lifecycle event it closes the socket and releases the lock, and another tab
  takes over. A frozen tab answers no ping, so anvil would close it as silent (`4003`) after 45
  seconds anyway, with wakes lost in between.

### 7.2 The frame codec

`chat/frames.ts` is anvil's `chat/frames.h`, byte for byte: `u8 version ‖ u8 type ‖ body`,
big-endian, **canonical**, so one frame has exactly one encoding and the decoder refuses anything
that would not re-encode identically. Downstream types are `0x01–0x3F`, upstream `0x40–0x7F`.

| Frame | Bytes | hammer does |
|---|---|---|
| `Wake` | `c(16) ‖ seq(i64) ‖ len(u16) ‖ inline(len ≤ 2048)` | shows the inline message, schedules a catch-up (§5.1) |
| `Typing` | `c(16) ‖ user(16)` | marks the user typing in the conversation (§7.4) |
| `Mutation` | `c(16) ‖ mutation(i64 ≥ 1)` | a message the reader may hold changed; catches the conversation's mutations up (§5.2) |
| `Presence`, `Receipt`, `Membership` | — | decoded and ignored: anvil sends none of them today |
| `Ping` | header only | answers `ClientPong` |
| `Pong` | header only | settles an outstanding `ClientPing` |
| `Sync` | header only | catches every held conversation up |
| `ClientTyping` | `c(16)` | sent at most once per three seconds per conversation |
| `ClientPing`, `ClientPong` | header only | liveness, both ways |

- **A malformed downstream frame closes the socket from the client side and is counted.** It cannot
  happen with a correct server, so it is a server defect or an intermediary rewriting bytes, and
  reading on would be reading frames whose boundaries are now a guess. The connection is
  reopened and the device syncs.
- **The inline message is the history route's own bytes**, so it is decoded by `chat/answers.ts`'s
  message decoder and by nothing else. A wake and a fetch are the same message by two paths.
- **The contract is golden bytes, not the header.** anvil's `chat_frames_test.cc` says its golden
  bytes are what a client's decoder is written against. The cross-repo row asks for them as a
  printed fixture, like the edit vectors, so the two codecs are held to one file.
- A decoded `Wake` holds a copy of its inline bytes, never a view into the socket's buffer.
- **A counter past 2^53 is refused as `frame.value`**, which anvil would accept. A JavaScript
  number holds integers exactly only that far, and reading one past it would hand the store a
  seq that is not the server's. No conversation reaches it.

### 7.3 Close codes, reconnecting, and liveness

anvil's close codes are its contract with the client (anvil 01 §18), and each is a decision:

| Code | Meaning | hammer does |
|---|---|---|
| `4001` | replaced by another socket of this device | **does not reconnect.** Reports `replaced`. With the leader lock in place this means the lock failed or a tab resumed into a lost election, and both are worth counting |
| `4002` | overflow: the client fell behind its ring | reconnects, then syncs |
| `4003` | silent for 45 s | reconnects, then syncs |
| `4004` | the process is full | backs off and runs on the fallback. Polling and the routes still work |
| `4005` | reauthenticate | runs the leader's credential refresh (00 §5), then reconnects and syncs. A refresh that fails is a logout, as it is for any `401` |
| `4006` | a frame no correct client sends | **does not reconnect.** This client is broken, and a loop would hammer the server with the same broken frame. Reports `broken`, counts it, and runs on polling |
| anything else | a network drop, a deploy, a proxy | reconnects, then syncs |

Reconnection backs off exponentially from one second to sixty, **with jitter drawn from
`crypto.getRandomValues`**, never `Math.random` (`CLAUDE.md` §5). A deploy restarts every process
at once, and a jitter that synchronised would bring every client back in the same second. The
circuit breaker for the site origin (`wire/breaker.ts`) is consulted before each attempt, so a
socket does not hammer a server every HTTP call has already given up on.

anvil pings every 20 seconds and closes a socket that says nothing for 45. hammer answers every
`Ping` at once. It also sends a `ClientPing` when the browser reports `online` again and when the
tab becomes visible, and reconnects if no `Pong` arrives within ten seconds. A socket the
operating system silently dropped while the laptop slept is otherwise only discovered by the
server.

### 7.4 Typing

Upstream, the composer reports input, and the store sends `ClientTyping` at most once every three
seconds per conversation. anvil drops anything faster, so sending more is waste. Nothing is sent
in a channel.

Downstream, a `Typing` frame marks `(conversation, user)` as typing. The mark clears six seconds
after the last frame for that pair, a duration in the client's own clock. That is a UI hint and
nothing depends on it. The mark also clears when a message from that user arrives. Typing is
never announced to a screen reader: in a group it would be a stream of interruptions saying
nothing anyone asked to hear.

### 7.5 Presence

anvil sends no `Presence` frame. A client asks `GET /chat/presence/{user}`, one account per call,
and "withheld" and "never seen" are the same answer, `{online: false, last_seen: null}`. So:

- **hammer asks only for the peer of the open direct conversation**, at an interval the application
  supplies, and stops while the tab is hidden. Asking for every account on a chat-list screen is
  one request per row, an N+1 over the network (`CLAUDE.md` §7), and the cross-repo row asks
  for a batch.
- **`last_seen` is a `ServerInstant`.** "Last seen 5 minutes ago" needs a server "now", and §5.3's
  newest server instant is what the store offers for it. Formatting is the application's.
- **The two answers are one state.** The store exposes `{online, lastSeen}` and gives no way to tell
  withheld from never seen, because the server gives none.

### 7.6 The fallback: the stream, then polling

Where WebSockets do not survive a proxy, anvil carries wakes on the reader's existing
notification stream: `ChatWake`, naming one conversation, and `ChatSync`. Neither carries a seq
or a message. **The event names on that stream are the application's**, because the application
writes its stream route, so `createChat` takes a decoder from the application's stream event to
`{wake: conversation} | {sync: true} | null`. hammer's existing stream client (`wire/sse.ts`)
already elects one leader per session for the stream. Chat adds no second stream.

The order is: the socket when it is available; the stream when the socket fails three times in a
row or answers `4004`, and the application wired chat onto its stream; polling otherwise. Polling
runs a catch-up of the open conversation and a read of the chat list's first page every 15
seconds while the tab is visible, and stops while it is hidden. A hidden tab learns what it
missed when it is next shown, and §5.1 already syncs on that.

The socket is retried in the background at the backoff's ceiling while a fallback runs, and wins
back when it connects.

---

## 8. Media in plaintext conversations

### 8.1 Uploading

anvil ships no chat upload route. Streaming bytes into storage is the application's upload
handler, which answers with an upload handle rather than an id (anvil 22 §6.2), and the
reference application does not have one yet. That is a cross-repo row, and until it lands
nothing in this section can be run end to end.

- **The body is the `File`, streamed** (`wire/upload.ts`), and the size and type are checked against
  the namespace's `accepts` and the descriptor's cap before a byte is sent. Nothing here reads a
  file into the heap.
- **The answer is a handle**: 72 opaque characters, branded `UploadHandle`, never parsed, and
  redeemable only in a send by the same account within the hour. A handle the outbox has not
  spent within that hour is refused by the send, and the attachment must be uploaded again. The
  store reports that as `upload-expired` rather than as an unknown failure.
- **What the composer sends beside the handle is content, not storage metadata**: the file name,
  and the width, height and duration the sending device already knows. Image dimensions come from
  the downscale `imagePool` already performs (`state/media.ts`). A video's dimensions and duration
  come from a `<video>` element pointed at an object URL over the **local** `File`, revoked the
  moment its metadata loads. That is the one legitimate `createObjectURL` in plaintext chat, and
  it carries a `ban-exempt` reason naming it. It reads a handle the person already gave the page.
  It never wraps a fetched body.

### 8.2 Showing

Every attachment arrives as a grant. Its address is the grant route's, built by the route
builder: `MEDIA_ORIGIN/m/{grant}/{role}`.

- **An image is an `<img src>` with `referrerpolicy="no-referrer"`**, a `srcset` over the
  namespace's roles where the image class has variants, and `decoding="async"`. Never a fetch,
  never a blob (`CLAUDE.md` §2.2). The bytes go through the CDN and the browser's cache, which the
  grant's deterministic encryption exists to let work.
- **Audio and video are `<video>`/`<audio>` elements on the grant address.** nginx answers `Range`
  behind `X-Accel-Redirect`, so seeking never downloads the file.
- **Everything else is a link that downloads**, `rel="noopener noreferrer"`, because anvil serves
  it as an attachment. A PDF is never framed or opened inline: the media origin serves it under
  a sandbox and that is where it stays.
- **A grant expires in ten to twenty minutes, and the client is not told when.** It is opaque, and it
  must stay opaque. Comparing its expiry with the device clock would be wrong anyway. So an
  element whose load fails is the signal: the store re-reads the page that holds the message
  (one request, coalesced per page per minute), which mints fresh grants, and the element is
  re-pointed. A conversation left open for an hour costs one page read when the person
  scrolls up, not a refresh of every grant on a timer.
- **A grant is a URL that is a capability.** It is never logged (a request record carries a
  route id and never a URL, 00 §9), never put in an analytics payload, and never stored past the
  held window.

### 8.3 Forwarding

A forward names `(conversation, seq, index)`, a message the forwarder can see, and never an
object. Nothing is uploaded again, and nothing in the client ever held the object id that would
let it do otherwise.

---

## 9. End-to-end encryption

This is the half anvil's phase 20 was built to receive. The server never holds a key that
decrypts a message, so every encryption, decryption, key and ratchet step is here. **It is also
the riskiest code in either repository**, and everything below is written to be reviewed, not
to be trusted.

### 9.1 What the client adds to the threat model

anvil 22 §7.1 covers the server's view. The client's view adds four adversaries:

| Adversary | Gets | Does not get |
|---|---|---|
| Script injected into the site origin, while it runs | everything the person can read while it runs, and the ability to send as them | the device's identity keys, which are non-extractable and cannot be cloned off the machine. It cannot become the device elsewhere after the page closes |
| A copy of the browser profile's storage | the vault's records, encrypted under the vault key, and the vault key as the browser stores a non-extractable key. Whether that is protected is the browser's and the operating system's business | nothing hammer can promise. The archive's encryption at rest is for wiping, not for this (§9.3) |
| The next person on a shared device, after a sign-out | nothing. The vault is destroyed on sign-out, because anvil unlinks the device when its session ends | — |
| A server that adds a ghost device | the ghost's decryption of everything sent to it, **if** the person's client encrypts to it | silence: the ghost's link does not verify against a key the peer's client has pinned, and the trust policy decides what happens next (§9.5) |

**What it does not hide:** metadata, as anvil says. And within the device: an injected script
reads plaintext the moment the page decrypts it. No design in a browser changes that. What the
non-extractable keys change is how long the compromise lasts. It ends with the page, as the
`HttpOnly` cookie's does, rather than with whoever copied a key.

### 9.2 The suite

Version 1 is suite byte `1` on the device row: **X25519 for agreement, Ed25519 for signatures,
HKDF-SHA-256, HMAC-SHA-256, AES-256-CBC for messages and AES-256-GCM for sealed media**, all from
WebCrypto. The decisions in it:

- **Two keys per device, not XEdDSA.** WebCrypto has no XEdDSA, and hand-writing it would put
  hand-written curve arithmetic in the riskiest module in the library. Each device holds an
  X25519 key for agreement and an Ed25519 key for signatures, as anvil's device row already
  stores. The cost is that this suite does not interoperate with Signal's network, and nothing
  here needs it to.
- **Signal's constructions everywhere the two keys do not force a difference.** X3DH's KDF, the
  Double Ratchet's root and chain KDFs, the message-key expansion, CBC with an HMAC over the
  message, and the sender-key chain are libsignal's, constant for constant, **so that libsignal
  can be the oracle** (§9.14). A construction of our own would need its own oracle, and a
  protocol checked only against itself is wrong in three places that agree with each other.
  The framing around those constructions is ours, because the wire is ours, and it has its own
  golden vectors.
- **AES-256-GCM in a chunked construction for sealed media, not Signal's attachment format.**
  Signal's is CBC with one HMAC over the whole file. WebCrypto has no incremental HMAC, so
  checking that MAC needs the whole file in memory, which `CLAUDE.md` §2.2 refuses for a 40 MB
  video. A chunked AEAD (§9.11) authenticates as it goes.
- **One primitive is hand-written: incremental SHA-256.** anvil compares a sealed upload's
  declared SHA-256 with the hash of what it received, and WebCrypto's `digest` is one-shot. So a
  streamed upload needs a streaming hash. `hammer/crypto` already holds BLAKE2b in plain
  JavaScript for the same reason, and SHA-256 joins it with NIST's CAVP vectors as its oracle.
  It is a hash. No secret passes through it, so a timing difference in it leaks nothing.
- **No post-quantum agreement in version 1.** PQXDH needs ML-KEM, which WebCrypto does not provide.
  It becomes suite byte `2` when it does.
- **A browser without X25519 or Ed25519 in WebCrypto reports `unsupported`**, and encrypted
  conversations are unavailable in it: listed, not openable, and not encrypted to. There is no
  polyfill, for the reason there is no XEdDSA.

### 9.3 The vault

**The one thing in hammer that persists.** 00 §7 says nothing is persisted, and the
deferred-work list says persistence returns only as an opt-in that names its own eviction and
threat model. This is that opt-in, and it exists because encryption cannot work without it:
a device's identity keys must outlive a reload, its ratchet state must, and **what it decrypted
is unrecoverable once the message keys are deleted**, which forward secrecy requires. A device
that forgot what it read would show an empty encrypted conversation after every reload.

**Where.** One IndexedDB database per account on the profile. `indexedDB` is banned in `src/`
(`tools/check-source-bans.sh`), and `chat-e2ee/vault.ts` is the one module exempt, with the
exemption's reason on the line. The check learns to allow it in that file only.

**What.** Identity keys, the signed and last-resort prekey private halves, one-time prekey
private halves by key id, pairwise sessions by peer device, sender keys by `(conversation, sender
device, distribution)`, the pinned roots of every peer account (§9.5), the outbox (§9.8), the
dedupe of processed queue rows, and the **archive**: every decrypted message by `(c, seq)`, with
an index on `(c, sender, cid)`.

**How it is protected, and what that is for.**

- **Every private key is a non-extractable `CryptoKey`**, generated by WebCrypto with
  `extractable: false` and stored as one. IndexedDB holds a `CryptoKey` by structured clone, and
  script can use it but never read it. This is the property §9.1's first row depends on.
- **Everything else is sealed under a vault key**: one non-extractable AES-256-GCM key per vault,
  with a fresh random nonce per record. Ratchet chain keys must be raw bytes to be chained, so
  they cannot be non-extractable keys, and they are sealed instead. **Sealing is for one
  purpose: destruction.** Deleting the vault key makes every record unreadable at once, even if
  the browser deletes the database lazily, or the tab closes halfway through the wipe. It is
  not protection against a copy of the profile, because the vault key is stored beside what it
  seals, and this document does not claim that it is.

**When it is destroyed.** On sign-out, on an identity change, when the server no longer lists this
device, and when the vault finds itself bound to a session other than the current one. anvil
ends a device when the session that registered it ends (anvil 22 §7.3.1), so a vault that
outlived its session holds keys nobody will encrypt to again. The wipe deletes the vault key
first and the database second, and the sign-out fan-out (00 §5) carries it to every tab.

**What that costs, stated so nobody discovers it later.** **Signing out of a browser destroys its
encrypted history**, and with no device-to-device history transfer (deferred, anvil 22 §10) it is
gone from that browser for good. That is how prior art behaves, for the same reason. The
person's other devices still hold their own copies.

**Concurrency.** N tabs and the service worker share one vault, and the ratchet is a state
machine that must never step twice from one state. Two writers that both encrypt from chain
index 7 reuse a message key, which breaks the confidentiality of both messages.

- **Every read-modify-write of protocol state runs under one Web Lock**, `hammer.chat.vault.<account>`,
  exclusive and waiting, in tabs and in the service worker alike. One lock rather than one per
  session, so there is no lock order to get wrong. The contention is between the tabs of one
  person.
- **IndexedDB transactions close when a task awaits anything that is not IndexedDB**, and every
  WebCrypto call is such an await. So a step is: read the state in one transaction, compute,
  then write the new state **and everything that depends on it** (the archive row, the outbox
  entry, the processed-row mark) in a second transaction, all under the lock. A crash between
  the two leaves the old state and nothing written, and the work is redone from the old state.
- **Without `navigator.locks` or IndexedDB, encryption is unavailable**, never degraded. There is no
  fallback that is safe, so there is none.

**Eviction by the browser.** A browser may evict a site's storage under pressure, and some evict
script-writable storage after a period without interaction. The vault asks for persistence with
`navigator.storage.persist()` when the application tells it to (a browser may prompt, so the
moment is the application's), and reports quota pressure from `navigator.storage.estimate()`. A
vault that vanished leaves a device the server still lists. The cross-repo row "which device is
mine" exists so the client can unlink the orphan rather than leave every sender encrypting to it
until the idle sweeper runs thirty days later.

### 9.4 Devices

#### 9.4.1 The states

| State | Means | The person can |
|---|---|---|
| `unsupported` | no X25519/Ed25519, no locks or no IndexedDB | use plaintext conversations only |
| `unregistered` | the account has no device at all | register this browser as the first device, after re-entering the password |
| `needs-link` | the account has devices, and this browser is not one of them | link it from an existing device (§9.4.3), or reset (§9.4.5) |
| `linked` | this browser is a current device of the account | read and send encrypted messages |
| `ended` | the server no longer lists this device | nothing until the vault is wiped, which happens at once, and the state becomes one of the two above |

#### 9.4.2 The first device

The first device is trust on first use, admitted by a **fresh primary authentication**: a
password or passkey within the last five minutes (anvil 22 §7.3). hammer's flow is to register
right after a sign-in, while the sign-in is fresh, and otherwise to ask the application to
re-authenticate first.

**This flow cannot ship until a cross-repo row lands, and it is blocking.** anvil refuses a stale
authentication with `401 UNAUTHENTICATED`, with no field in the body. To hammer that is an expired
access token. The leader refreshes and replays (00 §5), the replay is refused with `401` again,
and a second `401` after a successful refresh is a rejection: identity cleared, sign-out broadcast
to every tab. **Registering a device with a stale sign-in would sign the person out everywhere.**
The refusal needs a code that means "prove yourself again" and not "who are you". `428
CAPABILITY_REQUIRED` already means "a second, deliberate act is needed first" in hammer's error
table, and a distinct `reason` would do as well. hammer does not work around it by special-casing
a `401` on one route, because the same route can also legitimately expire.

#### 9.4.3 Linking another device

Every later device is admitted by a signature from an existing one, over
`"anvil-chat-link" ‖ account ‖ new device id ‖ agreement key ‖ signing key ‖ unix seconds`
(anvil 22 §7.3.1). The new device must post the link itself, because anvil records the
**posting session** as the one that owns the device, and ends the device when that session ends.

So the ceremony is four steps across two screens. anvil relays the request and the approval
through a short-lived mailbox (anvil 22, the link relay), and the one thing that does not go
through the server is what lets the approver tell that the server did not change the request:

1. **The new device** generates its keys in its vault, leaves its device id and both public keys
   in the relay, and gets a token. It shows a **link offer**: the token and a 128-bit digest of
   the request, `SHA-256("anvil-chat-offer" ‖ account ‖ device id ‖ agreement key ‖ signing
   key)` cut to 16 bytes, as 66 characters of base64url, small enough for a QR code.
2. **The approving device** reads the offer from that screen, fetches the request from the relay
   by the token, and **refuses it unless its digest equals the offer's**. Then it signs on the
   person's confirmation and leaves the approval in the relay.
3. **The new device** collects the approval once, posts the link, and reads `my_devices` to
   confirm.
4. Nothing else is trusted: the server verifies the signature before it relays it, and verifies
   it again when the link is posted.

- **The digest is why the offer must not go through the server.** The relay holds the request, so
  a malicious server could swap in keys of its own and have the person's own phone sign its
  ghost device. A short code compared by eye does not stop that: a six-digit code is 20 bits,
  and a server can generate keypairs until one matches in seconds. A 128-bit digest carried
  from one screen to the other cannot be ground. The design first had a six-digit code here;
  reading the relay's threat made it plain that the code was decoration.
- **When the offer cannot be scanned**, a desktop that cannot see the phone, the offer can be typed
  or pasted. It is 66 characters, which is tedious and is the honest cost: a shorter code would be
  one a malicious server can match.
- **The timestamp is the server's**, read from the HTTP `Date` header of the approver's most recent
  answer from the site origin, never from `Date.now()`. anvil refuses a timestamp more than five
  minutes from its own clock. A device whose clock is off by more than that would otherwise be
  unable to link anything, with no error that says why.
- **How the offer travels between the two screens is the application's.** A QR encoder is
  generic machinery and a camera is a permission, so hammer ships the offer as text and the
  application draws and reads it.

#### 9.4.4 Keeping a device alive

- **One-time prekeys are replenished when the server says `low`.** `my_devices` carries the flag, and
  the device uploads batches of 100, up to anvil's 1 000. Key ids are this device's, allocated
  monotonically and recorded in the vault before the upload, so a retried batch is
  recognisable (§3.4).
- **Reading the queue, sending and uploading keys mark the device seen.** A device in use is never
  taken by the idle sweeper.
- **A signed prekey that never rotates** is a weakness in the forward secrecy of every session's
  first message. anvil cannot rotate one yet ("nothing has asked for it"). hammer asks: the
  cross-repo row wants a rotation route, and the client will rotate the signed prekey and the
  last-resort key on a period once there is one. The period is library policy, thirty days,
  because it is a property of the protocol and not of a product.

#### 9.4.5 Losing every device

A person who has lost every device resets: they unlink the account's devices (any session of the
account may), which leaves the account with none, and then register this browser as a first
device with a fresh authentication. anvil writes a "devices changed" system message into every
encrypted direct conversation, so the reset cannot be silent. That is the property the
malicious-server row of anvil's threat model relies on. A "sign out everywhere" reaches the same
state by another road, because each revoked session ends the device it registered.

### 9.5 Verifying devices, and the security code

anvil verifies a link's signature before it writes anything. **The client verifies the chain
again**, because the server's check is what stops a stolen cookie adding a device, and the
client's check is what makes a ghost device the operator added visible.

- **Each peer account has a pinned root**: the signing key of the first device this client ever
  saw for that account, pinned in the vault the first time it saw it. That pin is trust on
  first use, as with every messenger.
- **A device is trusted if its link chain reaches a pinned key**: its `link` names an approver, the
  signature verifies over the link message under the approver's signing key, and the approver is
  trusted. The approver may since have been unlinked, so the vault keeps every signing key it
  has trusted for that account, not only the current ones. A device with `link: null` other
  than the pinned root is a **reset**.
- **The security code** is Signal's numeric fingerprint construction, version 2, with the account's
  pinned root signing key in place of Signal's identity key. Each half is 30 digits, from
  iterated SHA-512 over the key and the account id, and both people see the same 60 digits.
  Reusing the construction lets libsignal check it (§9.14), and the change of input is the one
  this suite forces.
- **A code changes only on a reset.** A device linked through the chain does not change it, which is
  what keeps the code meaningful in an account that adds a laptop. A code that changed every
  time anybody linked anything would teach people to ignore it.

**What happens on an untrusted device, or a reset, is a trust policy, and the application must
choose one.** `createE2ee` takes `trust: "verify-first" | "notify"`, and there is no default:

- `verify-first`: the client refuses to encrypt to the account until the person accepts the new
  code (`acceptIdentity(account)`). Sends to that conversation wait in the outbox, visibly.
- `notify`: the client accepts and pins the new root, and reports a `security-code-changed` event
  the application must show, in the conversation, at its position in the log.

The safer one is not chosen silently, because which one a product wants is a product decision.
It is not defaulted either, because a default here is a security property an application would
inherit without reading this paragraph. `detach` on an image edit is required for the same
reason (04 §5).

### 9.6 Pairwise sessions

One Double Ratchet session per pair of devices. A direct message is pairwise to every device of
both people other than the sending one. A sender key is distributed pairwise.

- **A session starts with X3DH** over a bundle claimed from anvil: the peer device's agreement key,
  its signed prekey and signature, a one-time prekey or `null` (the pool was empty, and the
  reusable last-resort key stands in), and the last-resort key and its signature. Both
  signatures are verified under the device's signing key before the bundle is used. The first
  message is a **prekey message** that names which prekeys were used, so the recipient can find
  the private halves and delete the one-time one.
- **Claims are budgeted twice by anvil**, per claimer and per target. They are scoped to an encrypted
  conversation both people are in, one account per call. A group's first message needs a session
  with every other device in it, which is one claim per member. At anvil's reference budget of
  120 claims a minute, a first send in a group of a thousand takes over eight minutes, and it
  is a thousand sequential requests, an N+1 over the network. The cross-repo row asks for a
  batch claim. Until then hammer establishes sessions in the background from the moment a
  conversation is opened, in pages, honouring every `Retry-After`, and shows the send as
  pending until the distribution it needs is complete.
- **The Double Ratchet is Signal's specification**, with its bounds. At most 2 000 skipped message
  keys stored per session, and a message more than 25 000 ahead of its chain is refused. Both
  numbers are libsignal's, and both exist because a peer can otherwise make a client derive and
  store keys without limit.
- **A message that fails to decrypt is recorded, acknowledged, and repaired.** It is stored in the
  archive as undecryptable at its position, so the log has no hole the person cannot see. Its
  queue row is acknowledged, so it cannot block the queue forever. The session is marked
  suspect, and the next send to that device starts a new session with a fresh claim. A peer
  whose vault was lost and restored is otherwise a conversation that fails silently in one
  direction for ever.

### 9.7 Groups: sender keys

A group message is encrypted once, under the sending device's **sender key** for that
conversation, and stored as the message's common ciphertext. Each other device needs the sender
key first, delivered pairwise as a **distribution**.

- **One sender key per (conversation, sending device)**, with its own Ed25519 signing key. Every
  group message is signed with it, because a sender key is shared, and anyone holding it could
  otherwise forge a message as its owner.
- **A distribution goes in the same send as the message when it fits**: the common ciphertext,
  plus a per-device map holding a distribution for each device that lacks the current key. A
  larger set goes first in pages of up to 64 with `page: true`, which anvil allows in groups
  only and checks against the fence like any send.
- **The key rotates when the set of devices shrinks**: a member leaves or is removed (the system
  message is the signal), or a device of any member is unlinked (the device page shrank since the
  last one). A removed device holds the old chain and can read everything sent under it, which
  is anvil's threat model row for a removed member. A device that joins gets the current key at
  its current iteration and can read nothing earlier, which matches anvil's `FromJoin` rule.

### 9.8 Sending an encrypted message

The conversation's `ConversationSealer`, which `hammer/chat` declares and `hammer/chat-e2ee`
implements, turns an outgoing message into anvil's encrypted body:
`{cid, device, dsv, ciphertext?, devices?, page?, attachments?}`.

**A ciphertext is made once, and a retry sends the same bytes.** Under the vault lock, the sealer
steps the ratchet, encrypts, and writes the new ratchet state **and the finished request body**
to the vault's outbox in one transaction. The outbox then sends and retries that body until the
server answers. Encrypting again on a retry would step the ratchet a second time for one
message. The peer would see a skipped key and a duplicate, and anvil would answer the second
body with the first message anyway, because the `cid` is checked before the fence.

**The fence.** Every encrypted send names the `dsv` it encrypted against. anvil refuses a stale one
with `409` and `reason: "chat.devices_stale"`, carrying the first page of the current device
lists. On that answer, the sealer:

1. reads the rest of the device pages if there is a `next`;
2. verifies every new device (§9.5) and applies the trust policy;
3. claims bundles and starts sessions for devices that are new;
4. if a device left a group, rotates the sender key, re-encrypts the common ciphertext and
   redistributes it;
5. rebuilds the per-device map, reusing the common ciphertext when nothing left;
6. writes the new body to the outbox, **with the same `cid`**, and sends it.

The `409` spent no seq, so the `cid` is still unused on the server. A loop that never converges is
refused after three rounds and reported as `devices-unsettled`. A conversation whose devices
change faster than a person can send to it is a problem worth a sentence on the screen.

**The outbox persists here.** An encrypted message whose ratchet step was committed and whose
body was then lost would be a hole in the peer's chain. So the body lives in the vault from the
moment it exists, a reload resumes the send with the same bytes, and a sign-out wipes it with
everything else.

### 9.9 Receiving

Ciphertext reaches a device three ways: a wake's inline message, a history page, and the
device's own queue (per-device ciphertexts, which carry every direct message and every
distribution). The order matters. **The queue is drained first**, because a group message's
common ciphertext cannot be read until the distribution in the queue has been.

- **The queue is drained by one context at a time**, under the vault lock, on a socket opening, every
  `Sync`, any wake in an encrypted conversation whose message has no common ciphertext, and a
  push. Each row is decrypted, written to the archive with the new session state in one
  transaction, and marked processed. Then the queue is acknowledged `through` the last row
  processed. An acknowledgement lost in transit is harmless: the rows come back, and the
  processed mark skips them.
- **Every decrypt is idempotent by position.** A message already in the archive at `(c, seq)` is never
  decrypted again, which a second decrypt could not do anyway, because its key was deleted
  after the first. The archive is checked before the ratchet is touched.
- **A message the device cannot decrypt is classified, never dropped.**

| Class | Means | Shown as |
|---|---|---|
| `before-device` | sent before this device was linked (`sent_at < linked_at`, two server instants) | the application's "from before this device" state |
| `awaiting-key` | a group message whose distribution has not arrived yet | pending, retried after each drain, and `undecryptable` after a bound |
| `undecryptable` | anything else | the application's "could not decrypt" state, and the session is repaired (§9.6) |
| `invalid` | decrypted, but the content broke §4's rules or the envelope's | the application's "could not display" state |

### 9.10 The content envelope

What is inside the ciphertext is hammer's format and nobody else's. The server never parses it,
and a peer's client parses it as **untrusted input**.

- **It is UTF-8 JSON**, version 1, narrowed from `unknown` exactly once. JSON rather than a binary
  layout because no byte of it is compared or hashed by anyone, so it needs no canonical form.
  `JSON.parse` is native, so a binary codec would be code shipped to every device for nothing.
  The framing **around** it (the ratchet header, the prekey message, the sender-key message) is
  binary and canonical, because it is parsed before anything is authenticated and has to be
  bounded first.
- **It is padded before encryption**, to a multiple of 160 bytes after a `0x80` terminator, so the
  ciphertext's length says less about the message's. That is Signal's padding, for Signal's
  reason.
- **Types**: text (with mentions, a reply, and attachment descriptors), card, reaction, edit,
  revoke, sender-key distribution, and session repair. An unknown type is `invalid` rather than a
  crash, so a newer client's message is not an outage for an older one.
- **A reference is `(sender, cid)`, never a seq.** A reply, edit or reaction may name a message whose
  seq the referrer does not know yet, its own pending message included, and anvil's
  `{c, u, cid}` index makes the pair unique (anvil 22 §4.2). The archive indexes it.
- **Every field is bounded and validated with the plaintext rules**: the kind's code-point bound, §4's
  character rules, mention spans inside the text, at most ten attachments, a reaction as one
  grapheme cluster. A card's body is validated by the application's validator for that card kind,
  which `createE2ee` takes, because inside ciphertext anvil validates nothing.

**Edits, reactions and revokes are messages with a reference**, as anvil requires, so each syncs
through the log with its own seq. A revoke is two things: an encrypted revoke message, so every
device deletes its archived copy, and anvil's `DELETE`, so the server deletes the ciphertext and
the queue rows. The `DELETE` alone would leave the decrypted copy in every archive that already
held it, and that is the very thing a person revoking a message wants gone.

### 9.11 Sealed media

An attachment in an encrypted conversation is encrypted on the device, uploaded as opaque bytes
into the kind's sealed namespace, and referenced from the message by its upload handle. Its
file key, its plaintext hash, its ciphertext hash, its type, its name, its dimensions and a
thumbnail of a few kilobytes all travel inside the message's ciphertext (anvil 22 §7.7).

- **The format is chunked AES-256-GCM**: 64 KiB plaintext chunks, each sealed under the file key
  with a nonce built from the chunk's index and a final-chunk flag, so chunks cannot be
  reordered, dropped or truncated without failing authentication. It is the STREAM
  construction. It authenticates as it goes, which one MAC over the whole file cannot do
  without the whole file.
- **It runs in a worker pool, `sealPool`, of one.** Encrypting and hashing twenty-five megabytes
  is too long for the main thread (`CLAUDE.md` §4), and a second worker would double the memory
  while the request queue serialises the uploads anyway. The worker reads the `File` in slices,
  so the whole file is never in its heap. It seals each chunk, feeds the incremental SHA-256 of
  the ciphertext, and collects the ciphertext as a **Blob of Blobs**: every megabyte becomes a
  `Blob` part, so the heap holds at most a megabyte while the browser holds the rest where it
  keeps blobs. The upload is that `Blob`, with the declared SHA-256 beside it. The descriptor's
  upload cap bounds the total.
- **Receiving reverses it, with the hash checked first.** The ciphertext is fetched from the grant
  address with `credentials: "omit"`, because the media origin never gets one (00 §8,
  invariant 7). It is hashed and decrypted chunk by chunk in the worker. Nothing is handed to the
  page unless the hash equals the one inside the message and every chunk authenticated.
- **Showing it needs an object URL, and that is the second exemption.** `CLAUDE.md` §2.2 forbids
  `createObjectURL` over a fetched body, because the browser could have shown the bytes from the
  media origin itself. Sealed bytes cannot be shown that way, since the media origin holds only
  ciphertext and serves it as `application/octet-stream` under a sandbox. So
  `chat-e2ee/sealed.ts` creates one object URL per decrypted attachment on screen, revokes it
  when the element leaves the held window, and is the only module exempt. **The application's
  CSP must allow `blob:` in `img-src` and `media-src`** for encrypted media to show, and
  `03-deployment.md` gains that line when the code lands.
- **Forwarding** re-sends the file key inside the new message's ciphertext and names the source
  message, and nothing is re-uploaded (anvil 22 §7.7).

### 9.12 Push for encrypted conversations

anvil pushes an encrypted conversation's members `{"c":…,"seq":…}` and nothing else (anvil 22
§7.8). A notification that says anything has to be worded **by the device, after it decrypts**,
and on the web that means the service worker.

hammer ships no service worker. The deferred-work list gives the reason, and it stands: a
service worker is a second, longer-lived program with its own update lifecycle. What hammer ships
is a function **the application's** service worker calls from its `push` handler,
`chatPushNotification(event, options)`. It:

1. decodes the payload, and refuses anything that is not exactly `{c, seq}`;
2. under the vault lock, with a deadline, drains the device queue and catches the conversation
   up to `seq`, through `fetch` with the session's cookies, which a same-origin service worker
   sends;
3. decrypts what arrived into the archive, as a tab would;
4. asks the application's `wording(decrypted)` for the title and body, in the person's locale;
5. returns them, and the application shows the notification.

- **On any failure it returns the application's generic wording**, never nothing. A push the browser
  was told is user-visible must show a notification, and the failure (a deadline, a lock held by a
  frozen tab, a session that needs a refresh the service worker cannot win) is a reason to say
  less, never to say nothing.
- **A refresh from the service worker takes the same lock the tabs use** (`hammer.refresh`). Locks are
  shared across every context of an origin, and anvil rotates refresh tokens as a
  compare-and-swap, so a service worker that refreshed on its own would race the tabs and sign
  the person out.
- **The notification's tag is the conversation**, so a burst is one notification, replaced, and a
  redelivered push replaces rather than repeats (anvil 22 §8.4).

Plaintext pushes are already worded by the server's template, and showing them is the
application's notification code. hammer adds nothing there.

### 9.13 The review gate is a type

anvil will not ship an encrypted kind to an application before an external review of both halves.
hammer enforces the same rule where an application can see it:

```ts sketch: the shape of createE2ee's options, abridged; no file exists yet
function createE2ee(options: {
    readonly chat: Chat;
    readonly routes: E2eeRoutes;
    readonly trust: "verify-first" | "notify";
    readonly cards: CardValidators;
    // A literal type, and a required one. Removed by the release that
    // follows a completed external review of both halves, so that an
    // application written against an unreviewed protocol stops compiling
    // the day the protocol is reviewed and has to read the review notes.
    readonly unreviewedProtocol: "not for production";
}): Result<E2ee, E2eeUnavailable>;
```

An application has to type the words to turn encryption on, and it will have to delete them
deliberately later. A flag nobody reads is a flag that ships.

### 9.14 The oracle, and the vectors

**Written from Signal's published specifications**: X3DH, the Double Ratchet and Sesame. libsignal
is read to understand behaviour the specifications do not pin, sender keys above all. **It is not
translated.** libsignal is AGPL-3.0, a line-by-line port is a derivative work under most readings,
and hammer ships inside proprietary applications (anvil 22 §7.2).

**libsignal is the test oracle, and it lives outside this repository.** A harness, in a separate
repository that hammer never depends on, links libsignal with a seeded random source and prints
vectors: X3DH key agreement from fixed private keys, root and chain keys across a ratchet with
skipped and out-of-order messages, message keys and their CBC ciphertexts, sender-key chains and
their message keys, and the numeric fingerprint. hammer commits the **printed vectors** as a
fixture, which is data, and its suite asserts its own output against every one. The harness is
kept out of hammer's tree so that no AGPL source is in a repository that ships, and so that
`npm ci` never sees a Rust toolchain.

One risk is recorded in the oracle's row rather than here: current libsignal releases speak PQXDH,
and the harness needs a version, or an internal entry point, that still exposes classic X3DH. The
row closes when the harness prints vectors for this suite's constructions, however it gets there.

**Three more fixtures**, each from a source that is not hammer:

- anvil's `testapp_emit_chat_vectors`, already printed: the device bundle encodings, both signed
  prekey messages, the 119-byte link message and its signature, five keys the server must refuse,
  an encrypted send body, and the push payload. hammer's device code is held to every byte.
- The framing (ratchet header, prekey message, sender-key message, distribution) from a **third
  implementation** written from §9.15's tables alone, as anvil's chat vectors came from a script
  over Python's `cryptography`.
- NIST's CAVP vectors for the incremental SHA-256.

### 9.15 The framing, byte for byte

Every multi-byte integer is big-endian. Every key is its raw 32 bytes. Every frame begins with the
suite byte, so a second suite is an addition.

| Message | Layout |
|---|---|
| Ratchet message | `suite(1) ‖ type=1(1) ‖ ratchet key(32) ‖ previous chain length(u32) ‖ counter(u32) ‖ ciphertext ‖ mac(8)` |
| Prekey message | `suite(1) ‖ type=2(1) ‖ sender device(16) ‖ sender agreement key(32) ‖ sender signing key(32) ‖ base key(32) ‖ signed prekey(32) ‖ one-time key id(u32, 0xFFFFFFFF = the last-resort key) ‖ ratchet message` |
| Sender-key message | `suite(1) ‖ type=3(1) ‖ distribution id(16) ‖ iteration(u32) ‖ ciphertext ‖ signature(64)` |
| Distribution (inside a pairwise envelope) | `distribution id(16) ‖ iteration(u32) ‖ chain key(32) ‖ signing key(32)` |

- **The MAC** is HMAC-SHA-256 over `sender agreement key ‖ sender signing key ‖ receiver agreement key ‖
  receiver signing key ‖ the message up to the MAC`, truncated to 8 bytes as Signal's is. Both
  of each device's keys are bound, because this suite has two.
- **The signed prekey is carried whole** rather than by an id. anvil holds one signed prekey per
  device today, and naming it by its bytes means a rotation (§9.4.4) changes nothing here.
- **The sender-key signature** is Ed25519 over everything before it.
- A frame that is not exactly its layout is refused before any key is touched.

---

## 10. The rendered surface

`hammer/chat-dom`, unstyled and wordless, by the component contract every hammer surface follows
(01 §19): a mount, a `Parts` type naming every element an application can class, a `Copy` type
supplying every word and a function for every sentence built from a value, and a handle with
`close()`.

| Component | Renders |
|---|---|
| `renderChatList` | the paged list, pinned rows, unread counts, typing markers, archived as a second list |
| `renderConversation` | the windowed log, day separators, system messages worded by the application, pending and failed sends, receipts, typing, attachments |
| `renderComposer` | the text field, mentions, reply, attachments, the live validator |
| `renderMembers` | the member page, roles, and the actions the kind's rights draw |
| `renderDevices` | this account's devices, the link ceremony's screens, the security code |

The decisions that are not the usual ones:

- **The conversation is a `role="feed"`, not a `role="log"`.** A log is a live region, and a busy group
  makes a live region read every message aloud, interrupting whatever the person was doing. A
  feed is the WAI-ARIA pattern for a scrolling list of articles. Each message is an `article`
  with a label the application words, `Page Up`/`Page Down` move between them, and it is not live.
  **Arrivals are announced by a separate polite live region**, only while the person is at the
  newest message, coalesced to one announcement per two seconds through `Copy.arrivals(count)`.
  The person who scrolled up to read is not interrupted.
- **Focus is never moved by an arrival.** A message arriving while the person types leaves the caret
  where it was. Opening a conversation focuses the composer. `Escape` in the composer clears a reply,
  and nothing else.
- **The DOM is windowed.** Only the messages near the viewport are elements, and the held window
  (§5.3) is the bound on what can be. Scroll position is anchored on the message under the
  viewport, so a page loaded above does not throw the reader.
- **Every user-authored string is isolated** (`CLAUDE.md` §8): the body with `dir="auto"`, a sender's
  name in an isolate wherever it is interpolated into the application's sentence. Mentions are
  rendered from their code-point spans, through §4's single conversion.
- **No markup from a message, ever.** Text is a text node. A link is recognised by the application's
  matcher, if it supplies one, and rendered as an anchor whose `href` passed the scheme rule, with
  `rel="noopener noreferrer"`. Nothing a sender typed reaches the one insertion site.
- **Read is reported from the viewport**, through `IntersectionObserver`, and only while the document is
  visible and focused (§6.2).
- **Typing is shown and never announced** (§7.4). Reduced motion stops the typing animation, which is
  the application's anyway, since animation is styling.

---

## 11. Budgets

| | Target | Notes |
|---|---|---|
| `hammer/chat`, gzipped | 12 KB | codec, calls, answers, sync, outbox, stores, socket |
| `hammer/chat-dom`, gzipped | 14 KB | five components |
| `hammer/chat-e2ee`, gzipped | 16 KB | the vault, the protocol, the envelope, the push handler; WebCrypto does the arithmetic |
| `hammer/chat-seal-worker`, gzipped | 4 KB | the SHA-256 and the chunk loop |
| resident, plaintext | one held window of 300 messages, one chat-list page | nothing persisted |
| resident, encrypted | the same, plus sessions loaded on use | the archive is in IndexedDB, not in memory |
| sealed media, peak heap | one megabyte of ciphertext and one chunk | Blob of Blobs (§9.11) |
| requests per wake | zero for an inline wake; one coalesced catch-up per open conversation per two seconds | §5.1 |
| sockets per profile | one | §7.1 |

Each ceiling is set from the first measured build, as every other entry point's was, and the
target is what that first measurement is argued against.

---

## 12. What the application supplies, in one place

This becomes a section of [`01-seams.md`](01-seams.md) in the same commit as the first code that
reads it (`CLAUDE.md` §1):

- `kChatLimits`, generated.
- `ChatRoutes` and, for encryption, `E2eeRoutes`: the generated route values.
- An upload function for chat media that answers a handle, over the application's upload route.
- The stream decoder, if chat rides the notification stream (§7.6).
- The presence interval, if presence is shown.
- `trust`, the card validators, and `unreviewedProtocol`, for encryption.
- The link transport (§9.4.3), until anvil relays it.
- The service worker's `wording`, for encrypted pushes.
- `Parts` and `Copy` for every component.
- The worker entry for `serveSealPool`, two lines.

---

## 13. What anvil has to ship

Each is a row in [`15-tasks.md`](15-tasks.md) §Cross-repo, with the fallback named, and none is
worked around in a way an application would have to copy.

1. **A fresh-authentication refusal that is not `401`** (§9.4.2). Blocking for the first device.
2. **A catch-up for mutations**: edits, revokes and reactions on plaintext messages are invisible to
   `seq > cursor`, and nothing wakes for them (§5.2).
3. **Another member's delivered watermark**, or the `Receipt` frame the grammar already has (§6.2).
4. **A chat upload route** in the reference application, answering a handle, and the sealed variant
   taking a declared SHA-256 and enforcing the per-account byte budget (§8.1, §9.11).
5. **A batch prekey claim** (§9.6).
6. **A batch presence read** (§7.5).
7. **A mute duration** in place of an absolute `muted_until` (§6.3).
8. **A client id on `create`** (§3.4).
9. **Which device is mine**: `my_devices` marking the device the calling session registered (§9.3).
10. **A link relay** for the approval's way back (§9.4.3).
11. **Signed-prekey and last-resort rotation** (§9.4.4).
12. **Golden vectors as printed fixtures**: the frame bytes and the text validator's cases (§4, §7.2).
13. **A staff read of a conversation, and reports**, audited and holder-scoped (§5.6).

---

## 14. Deferred, deliberately

- **History transfer to a new device.** Device to device, encrypted, as anvil's deferred list says.
  It needs a relay, and a design of its own.
- **Search.** Over plaintext it is a server index anvil has not built. Over the archive it is a
  client index over decrypted text in IndexedDB, which is more plaintext at rest and needs its
  own threat model.
- **A persisted plaintext outbox or cache.** §6.1 and §5.3 say why not by default.
- **Calls, sealed sender, MLS, post-quantum agreement, view-once, franking, forward limits, stories.**
  Each waits for anvil, for WebCrypto, or for an application that needs it.
- **A QR encoder and a scanner.** Generic machinery, and a camera permission. The link ceremony is
  bytes until a consumer shows that every application gets the QR step wrong.
- **Link-preview fetching.** Never by hammer (§4). An application with an unfurler supplies the fields.
- **Multiple accounts in one profile at once.** The vault is per account and the session is one,
  so there is nothing to design until the session model has a second account in it.
