# 20 — Entries: sections that repeat

The sections CMS ([`12-sections-cms.md`](12-sections-cms.md)) splits a page along ownership:
developers own the **shape** in a `constexpr` table, staff own the **content** in MongoDB. That
is exactly right for a page's hero band, and wrong for a portfolio, a news feed, a forum or a
comment thread, where **how many** documents exist is content too. A project is added by a staff
member and a post by a member, and neither of those is a deploy.

An **entry kind** keeps the half of the split that matters and moves the other. The shape is
still `constexpr`, and it is literally a `sections::SectionSpec`, so a field is bound, checked,
normalised, sanitised, hashed, stored and serialised by the code a section field is. What becomes
data is only the **instance**: its id, which the server mints; its slug, which is checked; and
where it sits among its siblings.

anvil ships the mechanism. **The kind table is yours** ([`01-seams.md`](01-seams.md) §6a).

---

## 1. The allow-list survives

A section key is an allow-list entry, and that is the most important security property of the
sections CMS (doc 12 §1). An entry kind keeps every part of it:

- A request names a kind that is in the table or is refused. A kind key never contributes a
  character to a collection name, a Redis key or a path; it is compared against the table and
  the table's own `.rodata` spelling is what travels onward.
- The content a request carries is bound against **that kind's fields and nothing else**, by
  `sections::bind_data` and `sections::bind_images`. There is no "extra" map.
- An entry id is a UUIDv7 **the server mints**. It is never a key, a name or a path, and a read
  by id filters on the kind as well, so an id is never a way to read across kinds.
- A slug is a URL segment that is **compared, never interpolated**: lowercase ASCII letters and
  digits joined by single hyphens, at most 64 bytes. Nothing in it needs escaping in a URL, an
  HTML attribute or a log line, and it is ASCII on purpose. A title is localised; a slug is the
  one spelling both editions of a URL share.

"A section with a wildcard key" would have given the same flexibility with none of that, and it
is the design this module exists to avoid.

---

## 2. The knobs

One mechanism serves a portfolio and a forum because the differences between them come down to
a handful of independent choices, each stated in the table:

| Knob | Values | For |
|---|---|---|
| `workflow` | `Editorial`: writes land on a draft, and `publish()` makes it what readers see.<br>`Immediate`: a write is what readers see. | a project or an article<br>a post, a reply, a comment |
| `ordering` | `Manual`: a position staff set with `reorder()`.<br>`Newest` / `Oldest`: creation order. | a portfolio's running order<br>a feed; a thread read top to bottom |
| `slug` | `Unique`: every entry has a URL segment, unique within the kind.<br>`None`: addressed by id. | pages with URLs<br>replies |
| `flags` | Up to sixteen application-named booleans | "pinned", "featured", "locked", "hidden" |
| `parent` | The kind this one hangs under | a reply under a thread |
| `capacity` | The most entries one scope holds, per parent for a child kind and per kind otherwise | a bound on what one member can fill |

`kinds_are_well_formed` checks the whole table at compile time. It checks that kinds are sorted
and keyed like sections, that each shape passes the three section checks with labels complete in
every locale, and that flags are named without dots, labelled, unique and at most sixteen. A
parent must be declared, must not be the kind itself, and must not sit in a loop. Capacity must
be non-zero, and a `Manual` kind must be small enough (`kMaxManualCapacity`, 1000) to reorder in
one transaction.

**Creation order is the id.** A UUIDv7's leading 48 bits are the creation time in milliseconds,
so for `Newest` and `Oldest` the primary key already is the order and no second field has to
agree with it.

---

## 3. Storage: one document per entry

```
{ _id: <UUIDv7>, k: "blog.post", sc: "blog.post" | "forum.reply/<parent id>",
  p: <parent> | null, slug: "hello-world", o: <position>, f: <flag bits>, n: <children>,
  live: <bool>,
  pub: { data, media, etag, at, by } | null,
  drf: { data, media, etag, at, by } | null,      Editorial only
  created_at, created_by, updated_at, v }
```

A section is **two** documents per key, so that a public read cannot leak an unpublished string
through a forgotten projection. An entry is **one**, and the reason is what an entry has that a
section does not: **placement**. Its slug, flags, position and child count belong to the entry
rather than to either copy. With two documents, each of them would have to be written twice, in
a transaction, to stay in agreement. That is the multi-document invariant anvil's CLAUDE.md §6
asks to avoid where a single document will do.

The leak is closed a different way. The published projection is built in **one** place
(`published_projection` in `repository.cc`) and excludes `drf` by name. The decoder does not
read `drf` on a published read even if a projection let it through. `APublishedReadNeverCarriesTheDraft`
also asserts that a draft-only string appears nowhere in the reader's decoded document or its
serialised bytes.

`data` and `media` inside each copy are the section codec's own encoding (doc 12 §5), so a
field type is stored the same way wherever it appears.

### Why `sc` exists

An index holds four keys (`db/migrations.h`), and a listing needs a scope, a stage, an order and
a tiebreak. Folding the kind and the parent into one string, `sc`, is what lets **one** index
answer every listing this module issues, forward and backward, without an in-memory sort:

| Index | Serves |
|---|---|
| `{sc: 1, live: 1, o: 1, _id: 1}` | every listing, both stages, both directions; the capacity count and the reorder snapshot (a prefix) |
| `{k: 1, slug: 1}` **unique**, partial on `slug` existing | lookup by slug, and slug uniqueness |

An editor's listing wants both stages, so `live` is an explicit two-value `$in`. With `live`
unconstrained the planner could not use `o, _id` for the sort; with the `$in` it merges two
ordered scans (`SORT_MERGE`), and the query catalogue check asserts there is no blocking `SORT`.
A time-ordered kind stores `o: 0` and lists with `o: 0` as an equality, so a cursor's range on
`_id` continues the same index.

A flag filter is `$bitsAllSet` over the scope's range: a residual, evaluated per key, never a
scan of the collection. For a feed of millions with a rare flag, that residual walks the scope.
If that becomes a real cost it is a second index, not a second design.

---

## 4. The operations, and what each guarantees

| Operation | Guarantee |
|---|---|
| `create` | Content canonicalised and checked for `required` fields against the shape; images verified with `sections::verify_images`; slug, flags and parent checked against the kind. Insert, media references and the parent's child count commit in **one** transaction |
| `write` | The patch is merged onto the working copy, exactly as a section patch is. The version is checked. The slug may move. Draft for `Editorial`, live for `Immediate` |
| `publish` / `unpublish` | `Editorial` only. The draft becomes the published copy (and stays), or the published copy goes and the draft stays |
| `set_flags` | Bits set and cleared in one `$bit` update. **Unversioned** (see below) |
| `reorder` | `Manual` only. The caller names every entry in the scope exactly once. Anything else is `Conflict` |
| `remove` | Versioned, **and** filtered on zero children. Media released and the parent's count decremented in the same transaction |

**Failures** use the vocabulary every anvil write uses. `ValidationFailed` names the field,
`"slug"`, `"flags"` or `"parent"`. `NotFound` covers no such entry under this kind, a parent that
is gone, an image outside the namespace, or a `WriteGuard` that does not match (§5).
`VersionMismatch` means the caller's read is stale. `Conflict` means the slug is taken, the
scope or parent is full, the entry has children, or a reorder named a set the scope does not
hold.

### Flags and positions are placement, and placement is not versioned

A content edit carries the version its editor read, and a stale one is refused. A **flag
toggle** does not. Two toggles of two flags commute: one `$bit` with disjoint `and` and `or`
masks is atomic on the server, and the service refuses an overlap. If pinning a project bumped
its version, pinning would race whoever has its editor open. **Positions** are rewritten by
`reorder()` inside one transaction, after the transaction's own snapshot has shown that the
caller named exactly the scope, so a stale list is `Conflict` rather than a silent reshuffle.

### Capacity

A child's bound is **exact**. The parent's count is incremented with `n < capacity` in the
filter, inside the create transaction, so two replies racing for the last slot are two
conditional updates of one document, and the server serialises them. A root kind's bound is
checked by **counting**, which two concurrent creates can both pass: it can be exceeded by the
number of creators racing for the last slot. That is stated rather than hidden, per anvil's
CLAUDE.md §6. The bound exists to stop a scope from growing without limit, not to keep an exact
figure. A deployment that needs it exact makes the kind a child of a singleton parent.

A parent with children is not removable. The delete filters on `n: 0`, so a reply counted
between the caller's read and the delete makes the delete miss rather than orphan the reply.

### Media

An entry holds **one** reference per distinct media id across both copies. Counting per copy
would make `publish`, which makes two copies name the same id, leak one reference per image per
publish. Every write computes the set before and after and attaches or releases the difference
in its own transaction, the rule doc 12 §5 states for sections.

---

## 5. Policy stays with the application

The service owns every **invariant** and no **policy**. Who may create a post, and whether a
member may edit only their own, are permission decisions. They belong in the application's
handlers against its own permission table, and anvil takes no view on them.

The one policy hook is `WriteGuard{author}`: "only if this user wrote it", enforced against the
stored row. An entry's author never changes, so comparing against a row just read has no race.
A mismatch answers **`NotFound`**, never `Forbidden`, so a member probing other members' posts
learns nothing an absent id would not tell them. A moderator's handler simply does not pass the
guard.

---

## 6. No cache, one signal

`SectionService` keeps three cache tiers because a section table is a few hundred kilobytes
whatever happens. An entry kind can be a forum with a million replies, so holding it in every
process is not free, and the service keeps **no cache**. What an application needs is the
signal:

`EntryServiceConfig::on_invalidated(kind)` is called once per changed kind, with the table's
own spelling. It fires on the writing instance and, through a Redis channel prefixed by
`channel_prefix`, on every other one. It fires only when **readers** would see a difference: a
published copy appearing, changing or going, a slug moving on a live entry, a flag, a reorder,
or the removal of a live entry. A draft save wakes nobody. The hook must not block, because on
the subscriber's path it runs on the one thread draining the channel; post the work. Exceptions
are caught and logged. A message naming a kind the table does not declare is dropped, for the
reason a request naming one is refused.

An application that renders pages builds its own cache above this and refills it on the hook,
the way doc 12 §6 describes for sections.

---

## 7. The fresh database: seeded once, ever

Sections need a default for every key, because a page reads every key. A kind needs none,
because an empty portfolio is a legitimate state. Where an application does want entries on day
one, it declares `KindSeeds`, which `seeds_match_kinds` checks at compile time. A seeded kind
must be a declared root kind, and its seeds must fit its capacity. Each seed's slug must obey
the rule and be unique, its flags must be declared, and its content must pass
`defaults_match_registry` against the shape.

`EntryService::bootstrap` seeds each kind **once, ever**, across every boot of every instance.
The rule for sections is insert-if-absent, and it is wrong here. An entry that was deleted and
an entry that was never created look the same, so insert-if-absent would put back, on the next
deploy, every seed a staff member removed. Each kind's seeding is instead **claimed**: a marker
document `{_id: "seed:<kind>"}` is inserted in the same transaction as the entries. Its string
`_id` cannot collide with an entry's binary one, and it has no `sc`, so no listing sees it.
Concurrent boots produce one seeding. A kind staff later empty stays empty.

The claim's duplicate-key error aborts the transaction on the server, so "already seeded" must
**unwind** the callback rather than return from it. Returning asks the driver to commit an
aborted transaction, which it retries for two minutes. The test that found this is
`SeedsLandOnceAndAnEmptiedKindStaysEmpty`.

Images are resolved first, outside the transaction, through the same `DefaultImageResolver`
section bootstrap uses. A missing one is counted in `images_missing`, not fatal.

Seeds are published copies, as a section default is: an `Editorial` seed starts live, with a
draft equal to it.

---

## 8. On the wire

Content is bound with the section binder. What an entry adds is in `entries/payload.h`:

- `bind_slug`, `bind_flags` (`{"pinned": true}` into bits to set and clear; an undeclared name
  is refused with an **empty** field name, per doc 12 §1) and `bind_order` (a bounded array of
  ids).
- `serialize_kinds`: each kind's configuration, its flags with labels, and its shape written by
  `sections::append_shape_json`, byte for byte the element the section registry endpoint writes.
  One editor control serves both.
- `serialize_entry` / `serialize_page`: placement, version, and each copy present with **every
  locale** side by side. A payload per locale would be two reads that could come from two
  versions. A field a copy does not hold is present as `null`, so an editor renders one control
  per field. `ImageLinks{base, suffix}` builds each `src`.

---

## 9. What is not here

- **A public JSON API per locale.** The editor serialisers carry every locale; a reader-facing
  JSON payload of one locale, like `sections::serialize`, is not shipped until an application
  needs one.
- **Listing by author.** "Every post by this member" is a query this module does not issue,
  because no index behind it is declared. It lands with its index when an application asks.
- **Cascading deletes.** A parent with children is refused. Deleting a thread with its replies
  is a decision the application makes one reply at a time, or not at all.
- **Seeding child kinds.** A child names a parent id, which no compile-time table can know.
