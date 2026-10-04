# 12 — Sections CMS

Editable page content, split along ownership.

| | Owner | Lives in | Changes via |
|---|---|---|---|
| **Shape** — which sections exist, which keys each has, what each key accepts | Developers | a `constexpr` table in `.rodata` | a deploy |
| **Content** — the strings and the image ids | Staff | MongoDB | an authenticated write |

Two requirements only conflict if "section" means one thing: *nothing on the site should be
hardcoded*, and *the section table is a developer's concern*. The split above resolves it, and
everything in this document is a consequence.

anvil ships the mechanism. **The table is yours** (docs/01-seams.md §6) — anvil holds no
application's section keys, field names, bounds or copy, and a `constexpr` table anvil
populated would be exactly the bug CLAUDE.md §1 names.

---

## 1. The registry is an allow-list

That is the single most important security property of this subsystem, and it is worth
stating before anything about performance.

A key that is not in the compile-time table **cannot be written**. There is no mass-assignment
surface, and no way to inject an unexpected field into a document a renderer will later trust.
Binding walks the request's keys and looks each one up; an unknown key is a validation error,
never a silent drop.

> **Never add a passthrough, an "extra" map, or a wildcard field.** The allow-list is not a
> check applied to the codec — it *is* the codec, in both directions (§5).

Silence is the wrong answer for a second reason: dropping an unknown key hides client bugs and
hides probing, and it is how a client discovers that some *other* endpoint accepts the field it
just tried. The error names no field at all — an unknown key is reported with an **empty**
name, so a client cannot choose what appears in a response or in a log line.

Section keys never leave the table either. A request-supplied key is compared against the
registry and either matches an entry or is rejected; it never contributes a character to a
collection name, a Redis key, or a filesystem path. `is_wellformed_key` — lowercase, digits,
underscore, dotted, ≤ 48 bytes — is the second lock on that door.

---

## 2. Why `constexpr` and not a parsed blob

A `static const std::string kJson` of the same content would allocate at start-up, parse per
process, and move every error from compile time to first use. A `constexpr` table costs
nothing at all:

| | parsed blob | `constexpr` table |
|---|---|---|
| start-up | one heap allocation and a JSON parse **per process** | nothing |
| memory | private per process | `.rodata`, shared across threads *and* across forked processes |
| errors | a malformed default is a first-boot failure in production | a **build failure** |

Lookup is a binary search over a sorted array of `string_view` — no map, no hash, no runtime
construction. A miss costs a handful of compares and touches two cache lines.

Keys are dotted identifiers and the table is sorted by the **whole** key, so every `home.*`
entry is one contiguous run. A request for a whole page is therefore one scan of a range
rather than a walk of the table.

---

## 3. The rules a value must satisfy

Field types, and what each one actually enforces:

| Type | Enforced |
|---|---|
| `Text` | code-point bound, text class, NFC normalisation |
| `RichText` | sanitised **on write** against `input::HtmlPolicy`; hostile or over-deep markup is *rejected*, not cleaned |
| `Number` | an integer, never a float — the two things a section carries a number for are counts and prices, and neither survives binary floating point |
| `Bool` | a JSON boolean, never the string `"true"` |
| `Url` | `input::check_url(UrlUse::Link)` — site-relative or an allow-listed scheme |
| `Color` | `#rrggbb`, lowercase |
| `Choice` | membership in the field's **own** allow-list, binary-searched |
| `Image` | **a build error.** Images are declared in `SectionSpec::images`, never as a data field |

Eight rules govern binding, and each exists because its absence was a defect:

1. **Type before value.** Which member of `SectionValue` is live is decided by the registry's
   `FieldType`, never by inspecting what arrived. This is the same rule the JSON binder
   enforces at the edge, applied one layer in.
2. **An unknown key is an error**, reported with an empty field name (§1).
3. **Bounds are in code points, never bytes.** A byte limit silently halves the allowance for
   any non-Latin script.
4. **A localised field needs every declared locale.** A missing one is a validation error,
   never a fallback to another locale — a fallback shows the wrong language to a reader who
   cannot tell it is wrong. An *extra* member is refused too, for the reason in §1.
5. **An unvalidated URL is a stored open redirect**, and with `javascript:` it is stored XSS on
   every page that renders the section. `Url` fields go through the same check a link in a
   request body does.
6. **Images are verified against their slot** by the service, which is the layer that can ask
   the database: the media must exist, be in the configured namespace, and meet the
   `ImageSpec`'s minimum dimensions and aspect ratio.
7. **`required` is enforced against the MERGED document**, never against the patch — otherwise
   the first partial update makes the section invalid.
8. **Blanking every locale clears an optional field.** Without it a staff member can fill a box
   but never empty one: the text check's minimum is one code point, so `""` comes back as
   `TooShort` and the only way to take a line off the site is a deploy. A clear lands in the
   patch as an explicit empty value — dropping the key would leave the merge holding the old
   one and the clear would do nothing. Blanking *some* locales is not a clear; it is a
   half-translated field, and it is refused.

### `Choice` is the generalisation of an icon name

A free-text icon name is a field whose only failure mode is silent: the site renders nothing,
the editor shows a saved value, and no error is raised anywhere. The allow-list makes a wrong
value a validation error at the moment it is typed.

The list is **per field and is yours**. A vocabulary of icon names, badge styles or layout
variants is one product's, so anvil ships `FieldType::Choice`, the binary search and the
sortedness check, and never a list. The choices travel with the field on the registry endpoint
(§7), so an editor renders a picker without holding a second copy that drifts the first time a
value is added.

The empty string is never a member, and is always accepted: "none" is a choice a staff member
makes, and whether a field may be blank is `required`.

### The compile-time half

`ct::` holds constant-evaluation twins of the runtime validators — `count_code_points`,
`is_valid_utf8` (rejecting overlongs, surrogates, embedded NULs and anything above U+10FFFF),
`is_integer_literal`, `is_bool_literal`, `is_hex_color`, `is_safe_default_url`. They exist
because anvil's runtime UTF-8 validator is simdutf-backed and therefore not `constexpr`, and
because a default that violates its own `FieldSpec` must be a build failure rather than a
first-boot failure in production. None of it exists in the binary.

`is_safe_default_url` is deliberately **stricter** than the runtime check: a compiled-in
default has no excuse to be anything but a fragment reference, a site-relative path, or an
`https` URL.

---

## 4. Defaults, and `defaults_match_registry`

`kDefaults` is asserted **positionally identical** to `kSections`, so bootstrap walks both by
index and looks nothing up.

`defaults_match_registry(kSections, kDefaults)` is evaluated in a `static_assert`, and it
catches, at build time:

- a default longer than its own `max_cp` bound;
- a localised field missing a locale, or carrying one that is not valid UTF-8;
- a non-localised field carrying a second value — a value nothing will read, and a pair free to
  disagree;
- a `Number`, `Bool`, `Url`, `Color` or `Choice` default that its own type would reject;
- a section with no defaults at all, or an image slot with no file.

**Every** field needs a default, not only the required ones: a fresh deployment must produce a
complete, renderable section, and a section missing an optional string renders a gap rather
than an error.

A default is not a stand-in. It is what a fresh deployment publishes and what any restore
restores, so it has to be the real copy.

---

## 5. Storage

**Two documents per key**, under a compound `_id`:

```
{ _id: { k: "home.hero", s: 0 }, data: {…}, media: {…}, etag, updated_at, updated_by, v }
{ _id: { k: "home.hero", s: 1 }, … }          s: 0 published, 1 draft
```

Two documents rather than one document holding both subtrees. A public read then cannot leak
an unpublished string through a forgotten projection, because the unpublished string **is not
in the document it read**.

The compound `_id` gives the uniqueness constraint and the primary-key index for free: the
collection carries no secondary index at all, and no extra 16-byte `_id` per document. Field
order inside an embedded-document `_id` is significant to MongoDB — `{k, s}` and `{s, k}` are
different keys — so the identity is built in exactly one place.

**`data` and `media` are written as whole subdocuments**, never as dotted `$set` paths. A
section is a few hundred bytes and is written a handful of times a day, so a partial update
saves nothing — while a whole-subdocument write is what makes a field *removed from the
registry* actually disappear from storage instead of lingering forever behind an update that
never mentions it.

Localised values are keyed by **locale tag**, the same string the wire and the `?lang=` query
use. Three spellings of one locale is three places for them to disagree.

### The codec reads shape, the registry reads content

`db::codec::read_localized` requires every locale to be non-empty, and that is right for the
types it was written for. A section field is different: whether it may be blank is `required`,
and `check_required` enforces that against the merged document. A codec with a second opinion
is a second rule, and the two disagree the moment clearing an optional field becomes possible
— the writer stores empty strings happily and the reader then refuses the whole document,
taking every section read as one prefix off the site at once.

So the section codec checks **shape** — a subdocument carrying one string per declared locale —
and the registry checks **content**. A field missing a locale entirely is still corruption and
still fails, because that is a shape error rather than an empty value.

### Versioned writes

Every staff write goes through `db/versioned.h`: the expected version in the filter, `$inc` in
the update. Two staff saving one section produce one winner and one `VersionMismatch`, never a
silent lost update.

The write and the media reference counts it changes are **one transaction**. A count that
commits while the section write aborts leaves live content pointing at files the collector will
delete; the reverse leaves a file nothing can reclaim. Re-writing the same id in the same slot
adjusts nothing, so a repeated save cannot inflate a count.

---

## 6. Reading: three tiers

A section read is the hottest path a CMS has — every page render reads one.

| Tier | Cost of a hit | Runs on |
|---|---|---|
| 1. process-local | one atomic load and a `write()` of bytes that already exist | **the event-loop thread** |
| 2. Redis, short TTL | one round trip, no BSON decode | `db_pool` (blocking) |
| 3. MongoDB | a primary-key lookup, a decode and a serialisation | `db_pool` (blocking) |

Tier 1 is one `std::atomic<std::shared_ptr<const SerializedSection>>` per (section, locale),
swapped atomically. Readers need no lock at all, and a swap during a read leaves the reader
holding the old payload alive rather than reading a half-updated one. The whole published set
is a few hundred kilobytes and is preloaded at boot, so tier 3 should essentially never be
reached in steady state.

**Serialisation happens once, at cache fill** — never per request. Text is emitted as raw UTF-8,
never `\uXXXX`, or every non-Latin character costs six bytes instead of two.

A payload carries **one locale**. Returning every declared locale multiplies the payload by the
locale count for content all but one of which is discarded on arrival.

The slot array is the **one** heap allocation this subsystem makes, taken once at construction.
The registry arrives as a `std::span` because it dimensions nothing anvil compiles, so its size
is a runtime value — and never a `std::vector`, whose reallocation would move atomics a reader
holds a reference to.

### The ETag

`canonicalise` reorders content into registry order and drops anything the registry no longer
declares, so two instances that received the same patch with its keys in a different order
produce **byte-identical** output. That is what makes the ETag a function of the *content*
rather than of the request that happened to write it.

Two digests, for two questions. `content_etag` covers **every locale** and is stored in the
document: it answers "did anything change", which a per-locale rendering cannot — a change
confined to one locale would not move it. `serialize` produces a per-locale ETag, which is what
a conditional GET compares.

### Invalidation

Deleting the Redis key is **not enough**. Every other process still holds its own tier-1 entry
and would serve stale content until that entry aged out, so staff see their change appear and
disappear depending on which instance answered — a confusing and nearly unreproducible bug.

A write therefore drops its local entry, deletes the Redis keys, and **publishes the section
key on a Redis channel**. Every instance subscribes on one dedicated thread — a subscribe loop
never returns, so it cannot live on a bounded pool without permanently consuming a worker — and
drops its local entry on receipt. A published key is data from another process and gets the
same allow-list treatment a request would: it is looked up in the registry and discarded if it
is not there.

**The order within a write is load-bearing and is the reverse of the obvious one.** Publishing
drops this instance's entry as its first act, so the write publishes *first* and installs its
freshly serialised bytes *second*. Storing them before publishing means storing them and then
immediately nulling them — the write path serialises twice as much as it needs to and still
leaves the next reader going to MongoDB.

Redis being down degrades this and nothing else: reads fall back to MongoDB, writes still
succeed, and only cross-instance freshness stops working. The degradation is **logged rather
than silent**, because a Redis outage nobody notices is one that stays.

The Redis keys and the channel carry a **configurable prefix**. anvil is a library: two
applications sharing one Redis must not share a cache key, and a constant would make that a
deployment hazard rather than a configuration choice.

#### A cache above this one is told too

`SectionServiceConfig::on_invalidated` is a `std::function<void(std::string_view key)>` called
once per invalidated key. It exists because a derived cache *above* this library's cache had no
way to hear that a key changed: `invalidate_local` and the Redis subscriber are both internal
to `SectionService`, so a consumer holding typed values for a renderer, or a pre-rendered
fragment, had nothing to subscribe to.

Both workarounds are worse than a hook, which is why this is the one place it is offered:

- **Dropping from the application's own write handler** misses every write that happened on
  another instance. That is the bug the Redis channel exists to fix, reintroduced one layer up
  — and it has the same symptom, a change that appears and disappears depending on which
  instance answered.
- **Subscribing to the same channel separately** is a second listener thread doing what one
  already does, with a second reconnect policy to keep in agreement.

Three properties, each of which is the difference between a hook and one of those workarounds:

- It is called from **`invalidate_local`**, so it fires on the SUBSCRIBER's path as well as on
  the local write. A hook that only fired where the write happened would be the first
  workaround with anvil's name on it.
- It is called **after** the local entry is dropped. A consumer that re-reads on the callback
  therefore cannot read back the stale value it was just told about.
- It **must not block**. On the subscriber's path it runs on the one thread draining the
  subscription, so anything slow there stops every other instance's invalidations from being
  noticed. Post the work; do not do it in the callback, and do not re-enter the service from
  inside it.

The key handed to it is always one the registry declares — the allow-list applies to the hook
as well as to the cache — and it is the registry's own `string_view`, which lives in `.rodata`,
rather than a view into a Redis message buffer that dies with the callback. An exception
escaping the callback is caught and logged: `invalidate_local` is `noexcept` and the subscriber
thread must survive a consumer's bug.

---

## 7. What the endpoints expose

- **The content**, one locale per payload, from tier 1.
- **The image specs** for one section — minimum dimensions and aspect ratio, with a label per
  locale. Requirements that live only in documentation get ignored; requirements returned by
  the API get rendered.
- **The whole registry**, so a staff editor is *generated* rather than written twice. A
  hard-coded field list is a second copy that drifts the first time a field is added, and the
  failure lands on a staff member typing into a box the server rejects as unknown. It is
  serialised per call rather than cached: it is read once when an editor opens, not on the
  per-render path, and a cache here would be a copy of a `constexpr` table.

An aspect ratio of `0/0` means "no constraint" and serialises as **`null`**, never as
`{"num":0,"den":0}`. An object is always truthy, so a reader testing `im.aspect ? … : null`
prints "shaped 0:0" beside a slot that is asking nothing — which a staff member can neither
satisfy nor recognise as no requirement.

Aspect ratios are compared with integer cross multiplication and a tolerance of one part in a
thousand, **never as a double**. Cropping and resampling do not land on an exact ratio for
every source size, and rejecting a 1920×1081 hero would be a defect dressed as a control.

---

## 8. Bootstrap

A fresh database must render every page with no manual steps.

Bootstrap runs **at every boot, from every instance, concurrently**. It is therefore
insert-if-absent — `insert_one` plus tolerance of the duplicate-key error — and never a read
followed by a write, which between N booting instances is a race whose loser silently
overwrites the winner.

> **It must never replace existing content.** A boot path that resets sections turns every
> deploy into a content wipe.

Published documents only. A fresh database has no drafts, and creating an empty one would put a
document in the draft state that nobody authored.

### Default images arrive through a hook

Defaults cannot reference uploaded media ids — on a fresh database there are none — so they
ship as **files** and are registered through the ordinary media pipeline, which probes,
normalises and derives variants for them exactly as it would for an upload. A default image
therefore cannot be a format the serving path refuses.

But that needs an encoder, and **the section row lifecycle must not depend on whether this
build has one**. A deployment serving already-stored objects with no image subsystem still has
to create, read and write its sections. So the file-to-id step is a callable the caller
supplies: `sections/default_images.h` ships one built over `MediaService` and the upload sink,
in a translation unit compiled only when the image subsystem is on, and an application with no
images passes nothing. The `no-vips` configuration is part of the gate rather than a courtesy;
it is what proves this.

Idempotence comes from the **content hash**, not from a marker: re-running bootstrap against a
populated database finds the identical bytes already stored and reuses that row rather than
storing a second copy. There is no "have I run before" flag to get wrong.

The rows are then **pinned** — `media::kPinnedRefs` added once, so the count can never reach
zero and the collector can never reclaim them. A deployment left pointing at a nonexistent
image is a broken page, which is worse than the state it booted from.

A missing default image is **loud but not fatal**: the section is created with its text, the
slot is counted in `BootstrapReport::images_missing`, and the page renders with a gap until
somebody fixes the deployment. A missing decorative image must not stop a deployment booting
with correct copy.

### The report has two numbers about images, and they answer different questions

`BootstrapReport::images_missing` counts **this boot's resolver failures**.
`BootstrapReport::image_slots_unbound` counts **declared slots with nothing bound in storage**
once the boot is over. In a fresh database they agree. Everywhere else they can disagree, and
the second one is the one to read.

> **`images_missing` counts RESOLVER FAILURES, not unbound slots — and bootstrap binds an
> image only into a section it CREATES.** Both facts are consequences of insert-if-absent
> and both are correct; together they produced a reading that was easy to get wrong.
>
> `bootstrap_sections` calls the resolver for every declared slot and increments
> `images_missing` when it answers `nullopt`. The id it returns is then set on the content it
> is about to insert — and if the section already exists, that content is discarded along with
> the binding, because insert-if-absent does not update.
>
> So a database bootstrapped BEFORE the defaults tree existed, or before the image subsystem
> was switched on, reported **`images_missing == 0` on every subsequent boot while every one of
> its slots stayed empty.** The resolver did its work; the files were registered, hashed,
> transcoded and pinned; nothing was wrong; and no section pointed at any of them. An operator
> reading the boot log for the number this document told them to read was told everything was
> fine.
>
> This must not be "fixed" by having bootstrap update an existing section. A boot path that
> overwrote stored content would make every deploy a content wipe, which is the whole reason
> insert-if-absent is the rule. Attaching defaults to an existing database stays a
> **deliberate, out-of-band act** — a migration that fills only still-empty slots, or a staff
> member doing it in the editor. What changed is that the boot log stopped saying it had
> already happened.
>
> Found by the first application built on this library, which added its defaults tree one
> phase after its sections and spent a while believing the number.

`image_slots_unbound` is taken from the **stored documents**, after the insert loop, so it is
the same answer whatever route the deployment took to reach the state it is in. It is one
`$in` over the `_id` index with a `media`-only projection: the whole registry is one round
trip and one index seek per section that declares a slot, and nothing comes back but the map
being counted. A section declaring no slot is never asked about. `SectionRepository::
count_unbound_image_slots` is the same call, available to anything else that wants to ask.

Three things it deliberately does:

- It counts **slots, not sections**. A section declaring three slots with one bound is two
  gaps on the page, and a count of sections whose `media` map is entirely empty calls that
  zero — the same shape of wrong answer the number exists to replace.
- A section whose document is **absent** counts all of its slots. Absent and empty are the
  same gap.
- A slot is bound only if it holds **binary** data. A slot carrying the 36-character string
  form of an id — what a migration written without reading §2.3 produces — is something no
  renderer resolves, so counting mere presence would call it bound.

A failure of that read **fails the whole bootstrap**. A boot that cannot see the state it just
wrote has not finished, and returning a report whose newest number silently reads zero is the
exact defect the number exists to close.

**Non-zero is not always wrong.** A slot a staff member deliberately left empty counts here
too, and there is no stored difference between that and one nobody ever filled. It is the
number to look at, not the number to alert on.

Files are opened `O_NOFOLLOW`. The defaults tree is read-only and operator-installed, and a
symlink out of it would be a way to make this process publish an arbitrary file as site
content.

---

## 9. Drafts

`{k, s: 1}` is a draft of the live page. Bootstrap seeds a published document per section and
**no draft of any of them**, so the first "save as draft" of every section in the registry has
to create one — before that branch existed, the state a fresh database is in was the state
where the feature did not work, which is every deployment.

The base is the **published content** and the patch lands on top of it, so anything the patch
does not mention reads as the site reads today rather than as empty.

Three properties of that path:

- Creation is **insert-if-absent**, not insert. Two staff pressing "save as draft" in the same
  second are two creates of the same document, and the loser must be told its version is stale
  rather than handed a duplicate-key error or, worse, overwriting the winner.
- It is reachable **only from version 0**. A caller that believes a draft already exists at some
  version is working from a stale read, and creating one underneath it would silently discard
  whatever it thought it was editing.
- **No reference counts move.** A draft references media the published document already
  references; taking a second count on the same id would leak a reference the moment the draft
  is discarded.

Reading a draft that has never been written still answers `NotFound`, deliberately: "I saved a
draft" and "I never saved one" must not render identically.

Drafts are not cached and publish no invalidation — tier 1 holds published sections only, and a
draft has no public reader to go stale.

---

## 10. What is not here

**Backup, restore and reset.** A snapshot taken before an overwrite, and the reset that
restores a section to its shipped defaults, are a separate collection with its own retention
policy, and they are not in Phase 4's scope (docs/15-tasks.md). The pieces they need are
already in place and were built with them in mind: `resolve_default_content` is exposed
separately from the bootstrap loop precisely so that "what a restore produces" and "what a
fresh database gets" are the same content by construction rather than by review.

Two things to carry forward when they land, both of which cost a real outage when they were
learned the first time:

- A restore **replaces** rather than delete-then-inserts. Delete-then-insert leaves a window in
  which the section has no content at all.
- The restored version is `previous + 1`, not 1, so an in-flight edit holding the pre-restore
  version **fails** instead of colliding with a reused number.
