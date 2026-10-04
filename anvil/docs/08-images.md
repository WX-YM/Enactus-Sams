# 08 — Images

libvips, confined to one directory. `src/images/detail.h` is the only place
`<vips/vips.h>` is included, which keeps glib, gobject and the decoder headers out
of every consumer of the library. Guarded by `ANVIL_WITH_VIPS`, so a build without
system libvips simply omits the subsystem.

libvips is chosen for one specific property: its demand-driven, streaming pipeline
is what makes the decompression-bomb defence below work at all.

## 1. Probe before decode

> A 40 KB PNG can decode to 50 000 × 50 000 pixels — about 10 GB of RAM,
> allocated **inside a C decoder** before any of this code runs.

So nothing is decoded until the header has been inspected and the dimensions
checked. Every cap is a refusal, not a resize:

| Cap | Default | Why |
|---|---|---|
| Dimension | 12 000 px | Past this is not a photograph anybody took |
| Pixels | 50 M | The product of the two dimensions, which is what allocates |
| Bytes | 25 MiB | Counted as they arrive, never from `Content-Length` |
| Pages | 1 | Multi-frame rejected outright — an animation is N decodes |
| Compression ratio | 1000:1 | The bomb signature: tiny file, enormous output |
| Operation timeout | 20 s | A wall clock armed on eval and checked between scanlines |

The ratio cap is the one that catches a bomb the dimension cap misses: a file
whose dimensions are plausible but whose content is a single repeated colour
compresses to almost nothing and decodes to everything.

`init()` and `shutdown()` must be called from the **main thread**, once, before
any worker touches the library — libvips has the same one-time
static-initialisation constraint the database driver does. `shutdown()` runs after
the pools drain: a worker still inside a libvips operation when `vips_shutdown`
runs is a use-after-free in a C library.

## 2. Normalising the master

`normalise_master` produces the file that is actually stored. Three operations, in
an order that matters:

**EXIF orientation is applied to the pixels, then the tag is dropped.** Dropping
the tag first leaves a sideways image; keeping it leaves every downstream consumer
to remember. This is the classic bug in image handling, and it is a bug precisely
because both halves look correct in isolation.

**The ICC profile is converted to sRGB, then dropped.** Stripping it without
converting renders an Adobe RGB photograph visibly desaturated — which looks like
a bad photograph rather than a bad pipeline, so it goes unreported.

**The embedded thumbnail is dropped.** A cropped image can otherwise retain a
thumbnail of the *uncropped* original, which is a disclosure that survives every
crop the interface offers.

Because the stored master is the normalised file, no code path — and no future
consumer — can ever be handed the original EXIF.

## 3. Variants

Generated **once, at upload**, from the master.

Never per request: re-encoding on demand puts image work on the read path, makes
latency unpredictable, and lets an attacker force arbitrary CPU by asking for many
size and format combinations.

Each variant is derived **from the master**, never by chaining through a smaller
one — chaining lossy re-encodes compounds artefacts.

**Never upscaled.** An 800 px master produces the 320 and the 640 and nothing
else: upscaling wastes bytes and looks worse than letting the browser do it.

Two failure modes, handled differently on purpose:

- A format this build **cannot encode** is skipped. A missing AVIF delegate should
  cost the AVIF variants, not the upload.
- A format that can be encoded but **fails mid-encode** aborts the whole set. A
  half-populated variant list is never recorded, because a variant list is read as
  a statement of what exists.

`unlink_variants` enumerates from the stored variant list, never from a directory
glob. A glob is racy, slow, and would delete a file belonging to a different id
that happens to share a prefix.

## 4. Roles, not widths

The public grammar is `GET /media/{ns}/{id}/{role}`. A role is what the image is
**for** — thumb, card, hero, full.

It is **declared in the route table** like any other route, with a description
beside it ([`01-seams.md`](01-seams.md) §14). It was settled here and declared
nowhere for a phase, which meant every application wrote the pattern by hand —
one address in an application and another in a route table, with nothing keeping
the two in agreement. It does not become a field in the media object below: that
table says what a role *is*, and the route table says where a route *lives*.

Making the client assemble `w640.avif` would require it to know the width ladder
and the format set, which is exactly the metadata that should stay server-side. A
client that knows the ladder is a client that will start building paths from it.

The role **vocabulary** is anvil's, because the grammar is. Which width each role
resolves to is the application's, because that is a fact about its design — and it
may differ per namespace: a thumbnail in a list and a hero on a landing page want
different ladders, and changing one needs no client release.

Every role width must **be** a rung of the ladder, and
`anvil/fs/namespace.h` static_asserts exactly that. A role pointing at a width
nothing writes resolves to the next rung down for every object in the system, and
the table looks entirely correct while doing it.

A role segment that matches no entry is a **miss**, never a fallback to the
master. A typo that silently serves four megabytes to a phone is the failure this
grammar exists to prevent.

The default role — what `GET /media/{ns}/{id}` with no segment means — is the
middle of the ladder rather than the top, because it is also what a caller that
*forgets* the segment gets. That mistake then costs resolution rather than
bandwidth.

The table lives beside the path grammar rather than in `images/`, because a URL
segment is accepted only if it names an entry: the serving path must be able to
validate one **without linking libvips**.

### A client still needs a number, and gets one

A responsive `srcset` needs width descriptors — `320w`, `1024w` — or the browser
cannot choose between the sources it is given. That reads at first like a
contradiction of everything above, and it is not: **the objection is to a client
constructing a path, not to it knowing a number.**

So the client descriptor ([`01-seams.md`](01-seams.md) §14) emits a width
*attached to the role it belongs to*, per namespace, and emits nothing else — no
bare ladder, no format list, no file extension. A client writes

```html
<img srcset="/media/content/{id}/thumb 320w, /media/content/{id}/card 1024w">
```

where every URL is still a role and every number is still the server's, and it
still cannot assemble `w640.avif` because it has never been told either half.
"Changing one needs no client release" survives intact: a client re-fetching the
descriptor gets new numbers against the same role names, and the paths it already
holds keep working while it does.

## 5. Cropping, and every other edit

An edit is a canonical recipe rendered once into a **new object** under a new id, holding a
reference on its source. That is [`21-image-edits.md`](21-image-edits.md).

This section used to say a crop was stored as parameters on the binding and re-derived the
variants in place. Nothing ever stored the parameters, and the re-derivation wrote its result
over the source's own variant files: a deduplicated upload shares those files with every
document holding the same bytes, so cropping one cropped all of them. `apply_crop` is gone,
and the property worth keeping, that no edit ever writes to a master, is now pinned against
the edit path.

## 6. Serving

The handler authorises, sets headers, and returns an **empty body** carrying
`X-Accel-Redirect`. The reverse proxy reads the file itself.

Reading a 4 MB image into a `std::string` costs two copies, 4 MB of heap per
concurrent request, and a worker held for the duration of a slow client's
download. That is the exact cost the architecture exists to avoid.

**The entire security boundary is `internal;` on the proxy's location block.**
Without that keyword, protected storage is world-readable and nothing errors.

Details that are decisions:

- The accel path is **percent-encoded unconditionally**, even though every
  component is server-generated hex. A CR or LF reaching a response header is
  response splitting, and "this value can never contain a newline" is exactly the
  assumption that breaks during a refactor.
- The content type comes from the **stored** enum, never from the request.
- `X-Content-Type-Options: nosniff` is always present: uploaded bytes served with
  a content type is a script-execution vector without it.
- Format negotiation reads `Accept` — AVIF when advertised, WebP otherwise — and
  the response carries `Vary: Accept`. Without the `Vary`, one cached response
  serves both.
- Role resolution serves, in order: the widest variant of the negotiated format
  at or below the role's width; the widest **decodable** variant at or below it;
  the narrowest above it; and the master **only** when the object has no variant
  this client can be served at all. A legitimate id must not 404 because a
  variant was skipped.

### The master is the last answer, not the second

This searched one format and fell straight to the master, and both halves were
right on their own terms. Together, on a build whose libvips has no libheif, they
were severe: `negotiate_format` answers `Avif` for any client that advertises it,
which is essentially every current browser; no object has an AVIF variant, because
`generate_variants` correctly **skips** a format it cannot encode (§3); so the
fallback fired and the client was served the normalised master — a multi-megabyte
PNG, to a phone, in place of a 90 KB WebP.

Nothing reported it. The upload succeeded, the variants that could be written
were written, the role resolved, the response was a 200 with a correct
`Content-Type`, and the only symptom was page weight.

The master is the widest and least compressed object on disk, so reaching it is
the **worst** outcome available rather than a neutral default — which is also why
the old "never fails upward" phrasing was self-defeating: falling to the master
was already a failure upward, and a larger one than any variant. A role narrower
than every written variant now takes the narrowest variant.

The question is **object-local rather than build-local** on purpose. It asks what
this object *has*, not what this binary can encode, so the answer stays correct
for an object stored while a delegate was available and read back by a build
without one, and does not change meaning the day the delegate is installed.

**Degradation is directional: AVIF may be answered with WebP, never the reverse.**
Every client that advertises AVIF decodes WebP, and `negotiate_format` answers
`Webp` precisely for the clients that did *not* say they decode AVIF — so serving
one an AVIF is a broken image. A symmetric "try the other format" is the obvious
spelling and is wrong in exactly the direction that reaches the oldest clients.
`accel_redirect_response` derives the `Content-Type` from the key, so a degraded
key carries a degraded type and the two cannot disagree.

The report that found this left one decision to the library rather than taking it:
whether the second question belongs in `resolve_role` or in each consumer's
handler. It belongs here. The old fallback was not a contract anybody wanted — no
caller ever asked to be handed a multi-megabyte master where a 90 KB variant
existed — and leaving it would have meant every application writing the same
second question after every call, which is the duplication [§1 of
`01-seams.md`](01-seams.md) exists to prevent. It does change the behaviour of a
published function, so it is a **behaviour change under CLAUDE.md §9.3** and
carries a changelog line.

Found by the first application built on this library, on a machine whose libvips
had no libheif.

File modes are published as constants rather than left as literals: `0750` on
directories, `0640` on files. The proxy reads these files as its own user in the
application's group, so the group bit is load-bearing — and getting it wrong fails
at the *last* step of a request the application has already authorised, with
nothing in the application log at all.
