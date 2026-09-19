# 07 — Filesystem and stored objects

Where uploaded bytes go, how they get there, how they come back out, and what
removes them.

One sentence governs all of it: **binary file bodies never enter the C++ heap.**
Reading a 4 MB image into a `std::string` to write it to a socket costs two
copies, 4 MB of heap per concurrent request, and a thread held for the duration
of a slow client's download — a hundred slow mobile clients would occupy a
hundred threads. Everything below is a consequence.

## 1. Layout

```
$STORAGE_ROOT/ns/<namespace>/<aa>/<bb>/<32 hex>            master
$STORAGE_ROOT/ns/<namespace>/<aa>/<bb>/<32 hex>.w640.avif  derived
$STORAGE_ROOT/tmp/<32 hex>.part                            in flight
```

**Every component is derived from a server-generated UUIDv4.** Nothing in this
subsystem ever accepts a filename, an extension or a subdirectory from a request.
That is the structural defence against traversal: there is nothing to traverse
with, so there is no filter to get wrong.

Two shard levels of 256 give 65 536 buckets, and sharding is not optional. One
directory holding a million entries makes every create and lookup slow on ext4
even with `dir_index`, and makes any `readdir`-based maintenance impractical.

A relative path is built in **automatic storage** — `RelPath` is a fixed char
array that fits in a cache line, NUL-terminated so it can be handed to `openat`
with no copy. Building one costs a memcpy of a handful of bytes and 34 hex
conversions: no allocation, no syscall.

`tmp/` and `ns/` must be on **one filesystem**, asserted at boot. A cross-device
rename degrades to a copy and stops being atomic, which would quietly remove the
publish guarantee in §4.

## 2. Namespaces

Which API owns a stored object, as a validated one-byte index.

The point is the TYPE. A function taking a namespace *string* from a caller can
be handed `"../section"`; a function taking an `Ns` cannot. The segmentation is
enforced by the type system at zero runtime cost.

**The namespace is in every media filter**, not checked by a caller. A handler
holding another API's object id gets nothing back, indistinguishable from an id
that never existed — and it is indistinguishable because the namespace is part of
the query rather than a condition somebody remembered to write.

**The order is persisted**: the index is stored as int32 in the media row, so the
application's table is append-only. Never reorder, never reuse a retired slot.
Which namespaces exist is the application's; see [`01-seams.md`](01-seams.md).

## 3. Why `openat` against boot-time descriptors

Resolving a path to a string and then opening it by name is a TOCTOU race: a
process that can create a symlink inside `STORAGE_ROOT` between the check and the
open wins it.

Instead the namespace directories are opened **once at boot**, and every
subsequent open is an `openat` walk relative to those descriptors, **one
component at a time, with `O_NOFOLLOW` on each**.

The per-component part is the half that is easy to get wrong. `O_NOFOLLOW`
applies only to the FINAL component of a path, so a single
`openat(ns_fd, "aa/bb/<hex>")` would leave both shard components unprotected —
which is exactly where a planted symlink would sit.

`path_is_within` exists for the sweeper and for the fuzz corpus, and it compares
**whole path components** rather than string prefixes. `/srv/storage-evil` has
`/srv/storage` as a string prefix and is a different directory entirely.

### File modes

`0750` on directories, `0640` on files, published as constants rather than
repeated as literals by each writer.

The group bit is load-bearing: Nginx reads these files itself once a handler has
issued a redirect, and it runs as its own user in the application's **group**. A
writer that uses `0600` produces a `403` from Nginx on a path the handler has
just authorised, with nothing whatever in the application log — which is why the
modes are one constant and not a thing each writer remembers.

## 4. The upload pipeline

Stages belong to different pools, and a single `store()` would have to pick one.
Holding a database client across a multi-second transcode starves every query in
the process; running libvips on a database thread makes login latency depend on
image sizes.

| | Stage | Pool |
|---|---|---|
| 1 | Refuse on declared length, free-space floor, and pool saturation | loop |
| 2 | Stream chunks into a temp file, hashing as they pass | loop |
| 3 | `fsync` and sniff | `cpu_pool` |
| 4 | Deduplication lookup | `db_pool` |
| 5 | Probe, normalise, publish the master, derive variants | `cpu_pool` |
| 6 | Insert the row | `db_pool` |

**Step 1 refuses before accepting bytes.** `Content-Length` is an
attacker-supplied hint, not a limit — the real cap is counted as bytes *arrive*,
and exceeding it aborts the stream rather than discovering the problem afterwards.
Free space is checked first because discovering a full disk halfway through an
upload is a partial file plus a failed request instead of a clean `507`.

**Step 2 hashes during the stream**, in one pass over a 64 KB buffer. The
alternatives are both worse: hashing the finished temp file is a second full read
of every byte just written, and holding the body in memory to hash it is the
exact copy this architecture exists to avoid.

**Step 4 is before step 5, deliberately.** Identical bytes have already been
probed, normalised and transcoded once; doing it again is seconds of CPU to
produce a file that already exists.

**Step 6 is last, and the ordering is a correctness property.** A row that
precedes its file is a 500 for whoever reads it first. A file that precedes its
row is an orphan the sweeper reaps. Those are not the same cost, which is why the
order is fixed rather than convenient.

Every intermediate output is written to `tmp/` and **published by rename**. A
failure mid-encode then leaves a temp file the sweeper collects, rather than a
zero-byte file in a namespace directory that would be served as a broken image.

## 5. Content type comes from the bytes

Sniffed from magic bytes against a **closed allow-list**. Never from the
`Content-Type` header, never from a filename extension, and never from anything
else the client supplied.

A claimed type that disagrees with the sniffed one is a rejection rather than a
correction: the disagreement is information, and the file is refused.

**SVG is rejected explicitly**, with its own reason code, rather than falling
through the allow-list by omission. An SVG is a document that can carry script,
so serving one from a media origin is stored XSS — and naming the rejection makes
it auditable. An SVG upload is a probe, not a mistake.

### The allow-list is anvil's; narrowing it is the application's

The closed list is what the pipeline can sniff, decode and re-encode, so no
application can widen it by declaring something. Which types a **particular
namespace** takes is a different question and a different owner:
`NamespaceSpec::accepts` is a mask over `Mime` defaulting to everything the
pipeline decodes, and `UploadSink` refuses a type its namespace does not accept
with its own reason, `upload.ns_type` — distinct from `upload.magic`, because an
unrecognised file is a probe or a mistake and this is a legitimate image in the
wrong place.

The sink takes its namespace at `open()` rather than at `publish()`, which is
also where it used to be passed for the second time. A sink that learned its
namespace only once the bytes were on disk could not refuse one, and a fact
passed twice is two things to keep in agreement.

**It is per namespace rather than one global list, and that is the whole point.**
A global list would close the copy and not the case that reopens it: the moment
one namespace takes no AVIF, the narrower list gets written into the client by
hand again. The descriptor emits the same mask as `accepts` beside each
namespace's roles ([`01-seams.md`](01-seams.md) §14), so the list a file picker
is built from and the list the upload path enforces are one table read twice.

The reference application narrows exactly one: `guest` takes JPEG and PNG,
because it is the only namespace fed by unauthenticated input and the AVIF and
WebP decoders are the newer and larger of the four attack surfaces.

The stored type is an **enum**, and the serving path reads that enum. A file
cannot be allowed to choose how a browser interprets it, and the one place that
could happen is the response header.

Every refusal on this path is audited with its true reason. The client is told
only `UNSUPPORTED_MEDIA`: a file crafted to discover which formats slip through
is a probe, and answering it precisely is answering the probe.

## 6. Serving

The handler authorises, sets headers, and returns an **empty body** carrying
`X-Accel-Redirect`. Nginx serves the file with `sendfile()`, copying disk to
socket inside the kernel. No byte of the object enters this process.

**The entire security boundary is `internal;` on the Nginx location.** Without
that keyword, every file under the storage root is reachable by direct URL, and
nothing inside this process can tell. A deployment smoke test that requests the
internal prefix directly and asserts a `404` is the only thing that can.

Four headers, each for a reason:

- `X-Accel-Redirect`, **percent-encoded unconditionally**. Every component is
  server-generated hex today; a CR or LF reaching a response header is response
  splitting, and "this value can never contain a newline" is exactly the
  assumption a refactor breaks. The path builder is three functions away and
  nothing links the two.
- `Content-Type`, from the stored enum or from the variant's format — never from
  the request.
- `X-Content-Type-Options: nosniff`. Without it a browser may sniff a crafted
  file as HTML and execute it on the origin serving media, which is the one
  origin most likely to be reachable without a session in front of it.
- `Vary: Accept`, because the body depends on the request's `Accept` and on
  nothing else a cache can see. Without it a shared cache serves one client's
  AVIF to a client that cannot decode one.

`Cache-Control` is `private`: these routes are access-controlled, so a shared
cache holding a response would serve it to the next requester.

### The public grammar names no width and no format

A client asks for a **role** — thumb, card, hero, full — resolved server-side.
Making the client assemble `w640.avif` would require it to know the width ladder
and the format set, which is exactly the metadata that is supposed to stay
server-side; and a client that knows the ladder is a client that will start
building paths from it.

Format is negotiated from `Accept` by a **substring scan**, not a q-value parser.
The header is attacker-controlled and unbounded, and the only question is "does
this client say it can decode AVIF" — a full RFC 7231 parse would be a
request-path parser written to answer a boolean.

**Role resolution never fails upward.** The pipeline does not upscale, so an
800 px master produces a 320 and a 640 and nothing else — and a `hero` request on
that object serves the 640, or the master. A legitimate id that returns `404`
because a variant was not written turns an editorial choice about source
resolution into a broken image on a public page.

A role segment that is not exactly a table entry is a `404` and never a fallback
to the master: the set is closed, and a typo that silently serves 4 MB to a phone
is the failure this grammar exists to prevent.

## 7. Deletion and the sweeper

**Row first, then files.** A crash between them leaves a sweepable orphan; the
reverse order leaves a row pointing at nothing, which is a 500 on the read path.

The row is claimed and removed in **one atomic operation**, conditional on the
reference count still being zero. A separate read followed by a delete lets an
attach land in between, and the files behind a live reference are then unlinked.
`nullopt` means "not claimed", never "deleted anyway" — the caller learns it must
not unlink.

Variants are enumerated from the row's own list, **never from a `readdir` glob**.
A glob is racy, slow, and will happily delete a neighbour that shares a prefix.

### Reference counting

Adjusted by `$inc` **inside the owning document's transaction**, never in a
separate write. Two reasons, and the second is the one that bites:

- A read-then-write loses one of two concurrent attaches.
- A count that commits while the document referencing it aborts is a leak no
  amount of sweeping can distinguish from a live reference.

So `attach` and `release` take the caller's session rather than opening their
own. Deletion is never driven by a handler unlinking a file; it is driven by the
count reaching zero and by the sweeper's grace period expiring.

A **grace period** is required. A row created moments ago and not yet attached is
an upload whose owning document is still being written, not an orphan.

The sweeper's claim is atomic for the same reason a delete is: two sweepers
racing produce one deletion and one "nothing to do".

When a sweeper decides a row is unreferenced by asking the owning collection,
zeroing the count carries the count it **expected**. An attach landing between
that question and that write would otherwise have its brand-new reference zeroed
— which is a live file collected a day later.

### The pin

Rows that are part of a deployment's own default content carry a large added
constant, so ordinary attach and release traffic can never walk them to zero.

It is **added** rather than substituted, so the ordinary count comes back out by
subtraction — a row rendering "used in 1000003 places" is the bug that shape
avoids. The pinned test is `> pin/2` and not `>= pin`: a release with no matching
attach walks a pinned row *below* the pin, and an `>=` test would call those
unpinned and then report a number near the pin as their ordinary count.

## 8. Abuse

An object whose uploader has no account records the **sending address**. It
cannot be derived from the owner: the owner is a UUID, so every account-less
upload's is the same nothing, and the address is the only handle abuse tooling
has on who sent one.

It is **omitted** on disk when absent, never written null: the index over it is
partial on the field existing, and a null on every account-backed upload would
put one entry per row into an index that exists for the minority that have one.
It is likewise **absent rather than all-zero** when decoded — the unspecified
address is a real address, and turning "we did not record this" into it makes
every such row look like one sender.

A by-address purge is **bounded**. A flood is thousands of small files, and
"delete all of them" with no bound is the attacker deciding how long a request
runs. One request, one pass, no loop: when a pass fills its bound the answer says
so and the operator asks again, which is safe because the operation is idempotent
by construction.

A row that is still referenced is **left alone and counted**. An operator told
"42 removed" has been told the wrong thing if eleven survived.

## 9. The memory budget

One 64 KB streaming buffer per concurrent upload, and nothing else.

That is the whole of it, and it is the number every other decision in this
document protects. Any payload that can exceed 256 KB is streamed or rejected;
there is no third option (ENGINEERING_RULES.md §2.4). A `std::string` holding a request body
is the one allocation that would make peak memory a function of what a client
chose to send rather than of how many clients there are.

The image stage is the exception that proves the rule: libvips genuinely needs
the pixels, which is why it runs on `cpu_pool` with its own bound, why the
decompression check happens **from the header before any pixel is decoded**, and
why the pool's size is what caps how much of that can be in flight at once.
