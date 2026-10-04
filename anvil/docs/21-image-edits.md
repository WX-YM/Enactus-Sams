# 21 — Image edits

Crop, rotate, flip, resize, and freehand drawing, on an image that is already stored.

**Status: built.** The rows are [`15-tasks.md`](15-tasks.md) §Phase 17, and the client half is
hammer's `docs/04-image-edits.md`. Where the build departed from this design, the section says
so.

One sentence governs all of it: **an edit is a recipe, and the recipe is rendered once, by
the server, into a new object.** The master is never written, the recipe is never applied
twice to its own output, and the pixels a client previews and the pixels the server stores
come from the same numbers.

## 1. What is wrong with the crop that exists

[`08-images.md`](08-images.md) §5 says a crop is stored as parameters on the binding and
re-derives the variants from the master. `images::apply_crop` implements the second half and
nothing implements the first: no binding stores a `CropRect`, and nothing outside
`tests/images_test.cc` calls it. That would be a missing feature, except that the half that
exists is wrong in three ways, each found by reading it against
[`07-filesystem.md`](07-filesystem.md):

1. **It writes the cropped variants under the source's id.** `render_cropped` publishes
   `<id>.w640.avif` over the file the upload wrote. Uploads are **deduplicated per namespace
   by content hash** (07 §4, step 4), so two documents that uploaded the same photograph hold
   the same id — and cropping one crops both. The crop is a property of one binding, and it is
   being stored in a file every binding of those bytes shares.
2. **A reader sees a mix of old and new.** Variants are renamed into place one at a time, so
   a `srcset` resolved mid-job has a cropped `card` beside an uncropped `thumb`. The header's
   claim that "the slot keeps serving the previous variants until the new ones are ready" is
   true of no file.
3. **A failure deletes the files the row still lists.** On a mid-set encode failure it
   unlinks what it produced. What it produced was published over the upload's own names, so
   the rollback deletes variants the row still records, and every later request for that
   role takes the fallback in 08 §6 or a 404.

None of the three is fixable inside `apply_crop`, because all three come from where it
writes: a new result under an old name. The fix is structural, and it is the rest of this
document.

## 2. A recipe, not pixels

An edit is a small document naming what to do to the master. It is **canonical**: one
edit has exactly one encoding. So the recipe can be hashed, the hash can be a unique key,
and a retried request resolves to the object the first attempt made.

### 2.1 The operations, and their one order

| Operation | Parameter | Frame it is expressed in |
|---|---|---|
| Orient | quarter turns clockwise (0–3), then an optional horizontal flip | the master |
| Draw | up to `max_strokes` strokes | the **oriented** master, uncropped |
| Crop | a rectangle | the oriented master |
| Resize | the long edge of the result, in pixels | the cropped result |

The order is fixed and is not a parameter. Rotation and flip are the eight symmetries of a
rectangle, and every one is a quarter-turn count followed by at most one horizontal flip. A
vertical flip is two quarter turns and a horizontal flip, so it has no bit of its own —
otherwise one image would have two recipes, and two recipes are two hashes, two objects and
two sets of files for one picture.

**Strokes live in the uncropped frame, and that is a UX decision made in the wire format.**
A person who draws an arrow and then re-crops expects the arrow to stay on what it pointed
at. Coordinates relative to the crop would move every stroke on every change of the crop.
Coordinates relative to the oriented master stay put, and the crop clips them. (They are
oriented rather than raw because a person draws on the picture they see.)

What is **not** an operation, deliberately:

- **Text.** Rendering text is a font stack, shaping and bidi on the server, and a font is
  the application's look (`CLAUDE.md` §1). A caption is a field beside the image.
- **Free rotation (straighten).** Arbitrary angles need a resampling policy and a
  fill-or-crop decision, and the corners a person did not photograph have to be invented.
  Quarter turns are exact and lossless in geometry.
- **Filters, brightness, contrast.** Different machinery (colour transforms), not asked for,
  and each is a knob whose default is a product decision.
- **Pixel erasing.** Erasing is removing a stroke from the recipe. There is nothing to erase
  under a stroke because the master was never painted.
- **Shapes other than a freehand stroke.** A straight line is a two-point stroke, and a dot
  is a one-point stroke. Rectangles and arrows are a later extension of the stroke kind byte
  (§2.2), and they are not in version 1.

### 2.2 The canonical encoding

Fixed-point integers, big-endian, and no floats anywhere. A coordinate is a `u16` fraction of
its frame's extent, `0` to `65535`. At the 12 000 px dimension cap that is 0.18 px, finer
than any pointer. Floats are refused because `0.1` in a JSON body, a JS `number` and a C++
`float` are three different values, and three different values are three different hashes
of one edit.

```
u8   version            = 1
u8   flags              bits 0–1 quarter turns; bit 2 flip-h; bit 3 crop; bit 4 resize;
                        bits 5–7 zero
[crop]   u16 x, u16 y, u16 w, u16 h            fractions of the oriented master
[resize] u16 long_edge_px
u8   stroke_count
per stroke:
  u8  kind              = 0 (freehand); anything else is refused in version 1
  u8  r, g, b, a        sRGB, straight alpha, a ≥ 1
  u16 width             fraction of the oriented master's SHORT edge
  u16 point_count       ≥ 1
  point_count × (u16 x, u16 y)                 fractions of the oriented master
```

On the wire the recipe is one **base64url** string without padding: the same encoding the
permission set uses, one string field in a flat response shape (`FieldKind::String`, 01 §14)
and one in a request body. A JSON array of eight thousand integers would be two and a half
times the bytes and a parser allocation per number.

**The server re-encodes what it decoded and refuses anything that is not byte-identical.**
That single comparison enforces every canonical rule at once. The rules still get names, so
a refusal can say which one it was:

| Refused | Fault | On the wire | Why it is not merely normalised |
|---|---|---|---|
| a reserved flag bit, an unknown version or stroke kind, trailing bytes | `edit.format` | `recipe`: `BAD_FORMAT` | a bit this build ignores is a bit the next build reads; a recipe with a tail is two recipes |
| a crop that is the whole frame | `edit.canonical` | `recipe`: `BAD_FORMAT` | that is the absent crop, spelled a second way |
| a crop outside the frame, or one that rounds to no pixels | `edit.crop` | `recipe.crop`: `OUT_OF_RANGE` | there is nothing to render |
| a resize at or above the cropped long edge | `edit.upscale` | `recipe.resize`: `OUT_OF_RANGE` | never upscaled, same rule as 08 §3 |
| a result narrower than the smallest rung | `edit.too_small` | `recipe`: `TOO_SHORT` | every variant would be an upscale — the old `crop.too_small` |
| no operation at all | `edit.empty` | `recipe`: `REQUIRED` | the identity edit is the source; bind that |
| a stroke with `a = 0`, zero or over-wide width, or no points | `edit.stroke` | `recipe.strokes`: `BAD_FORMAT` | invisible content is still content that is hashed |
| more strokes or points than the caps | `edit.bounds` | `recipe.strokes`: `TOO_LONG` | see §5 |
| a source that is itself an edit | `edit.not_source` | `id`: `NOT_ALLOWED` | §3.1 |

The fault is the audit's; the client is told `VALIDATION_FAILED` with the field and reason
beside it, in the `fields` map every validation failure already uses, through
`images::field_error`. The design had the fault itself on the wire as a second vocabulary.
The fields map was already the one a generated client decodes, so the build used that.
Unlike the upload path's `UNSUPPORTED_MEDIA` (07 §5), a recipe is not a probe of which
decoders exist, so a precise answer gives nothing away. It is also exactly what the
client-side validator is supposed to agree with, and hammer's `kFaultWire` is this table.

## 3. The derived object

A rendered edit is **a new media row with its own id, its own master and its own
variants**, in the **same namespace** as its source. It is served, negotiated and resolved by
role through exactly the path every other object takes (08 §6), so `resolve_role`,
`X-Accel-Redirect` and the `internal;` boundary learn nothing new.

The row gains three optional fields, omitted on a row that is not an edit (the same rule as
`uip`, 07 §8):

| Field | Holds | Why |
|---|---|---|
| `src` | the source's id, binary | the editor reopens the source, not the result |
| `edit` | the canonical recipe bytes | the editor reopens with the recipe loaded |
| `esha` | SHA-256 of the canonical recipe | the idempotency key the server derives for itself |

A **unique partial index on `{ns, src, esha}`** where `src` exists makes the same edit of the
same source one object. A retry, a double submit and two people making the same crop all
resolve to it, with no idempotency key in play. The client still sends a key (hammer
`CLAUDE.md` §6), and here it is belt and braces rather than the mechanism.

### 3.1 Never chained

The source of an edit must itself be a **source**, a row with no `src`. An edit of an edit
is refused with `edit.not_source`. Re-editing sends the whole new recipe against the
original. That is what makes a re-crop not compound:

- The pixels are at most **one** lossy generation from the master, however many times the
  image was re-edited.
- Two recipes never need composing. Composition is where a rotated crop of a flipped crop
  goes wrong by one axis.

### 3.2 References

A derived row holds **one reference on its source**. It is taken by `$inc` in the same
transaction that inserts the derived row, for the reason every count moves inside the
transaction that caused it (07 §7). A source cannot be collected while an edit of it exists,
because the edit is how the source is reopened.

Deleting a derived row releases that reference **in the same transaction** as the claim.
Released afterwards, a crash between the two leaks the source forever: a count no sweep can
tell from a live reference. `delete_if_unreferenced` and `claim_unreferenced` therefore
become transactional for a row that carries `src`, and stay single-document for one that
does not.

The binding swap is the application's document write, and it needs nothing new: `release`
the old id and `attach` the new one, in the document's own transaction, carrying the version
it read. The derived row starts at zero references like any upload and is protected by the
same grace period until that write lands.

### 3.3 Byte deduplication does not see derived rows

`find_by_hash` gains `src: {$exists: false}`. A derived master's bytes can equal an upload's
(a rotated photograph uploaded again, already rotated), and resolving that upload to the
derived row would hand a fresh upload a `src` it never had. Its `sha` still records its own
bytes, for the sweeper and for the purge tooling. The `{ns, sha}` index was checked before
this landed: it is an ordinary index, not a unique one, so this is a filter change and no
migration.

## 4. Rendering

One job on `cpu_pool`, in this order, all from the source master opened **read-only**:

1. **Orient**: `vips_rot` then `vips_flip`. Exact, no resampling.
2. **Crop**: `vips_extract_area` on the oriented image, with the same rounding and far-edge
   clamp as `to_pixels` today.
3. **Resize**: to the recipe's long edge, capped at `max_edge_px` (below).
4. **Draw**: rasterise each stroke at **output** resolution, having mapped its points and
   width through steps 1–3. Rendering order is orient → draw → crop → resize. Drawing last
   is the same picture, and it bounds the work to the output.
5. **Encode the derived master** in the source's `Mime`, so the content type still comes
   from the stored enum (07 §5). Then derive the variants from it with the existing
   `generate_variants`. It is a master in every sense the variant code cares about.

Every file goes to `tmp/` under the **derived** id and is published by rename. A failure
leaves temp files the sweeper collects and nothing under any id a row names. That one
change is what closes all three defects in §1.

### 4.1 The derived master is capped at the largest rung

`max_edge_px` is the widest rung of the ladder. Nothing ever serves an edit above it:
`resolve_role` reaches the master only when an object has no variant a client can be served
(08 §6). Capping it is also what makes drawing affordable. The first sketch drew at master
resolution, where one stroke across a 50 MP image is a 50 MB coverage mask. That is the
number the design moved to avoid.

**Memory per concurrent edit**, at a 2560 px rung: the output frame, 2560² × 3 ≈ 20 MB, plus
**one** stroke's coverage mask at a time, sized to that stroke's bounding box and at most
2560² ≈ 6.5 MB. Strokes are composited one at a time and each mask is freed before the next
is built. `cpu_pool`'s bound is what caps how many of these exist at once, the same bargain
07 §9 makes for the upload's decode.

Upstream of that frame, the source is opened for **random** access, because a quarter turn
reads it in column order. libvips then decodes the source once rather than streaming it,
bounded by the upload caps (50 MP) and spilled to disk past `VIPS_DISC_THRESHOLD`. It is the
same cost `apply_crop` paid, and it is the reason an edit counts into the upload budget.

The strokes are drawn into that output frame in plain memory: `vips_image_write_to_memory`,
the rasteriser over the buffer, and `vips_image_new_from_memory` over the same buffer for the
encode. The loaded image keeps its own reference for as long as the deadline armed on it
does. The first build lost it when drawing replaced the pipeline, and the deadline's
destructor then disconnected a handler from a freed object.

### 4.2 The stroke rasteriser is arithmetic, not SVG

The obvious implementation writes the strokes out as an SVG document and has libvips load it
through librsvg. **It is refused.** 07 §5 rejects SVG by name because it is a document
format with a parser behind it, and generating one server-side from request data puts that
parser back on the request path, with the escaping of every number as the only thing between
a request and it.

The rasteriser is a few hundred lines of our own. A stroke is the union of capsules, one per
segment: round caps and round joins at constant width. Coverage is computed per pixel inside
the stroke's bounding box as the **maximum** over segments, so a stroke that crosses itself
does not darken where it overlaps. The mask is composited once per stroke with
source-over in **sRGB**, not linear light.

Each of those choices is there so the server's pixels match the browser's preview: an SVG
`<path>` with `stroke-linecap="round"`, `stroke-linejoin="round"` and `stroke-opacity`
paints exactly this shape, unions its own overlaps, and blends in sRGB. Edge anti-aliasing
may differ by a level. Geometry, colour and overlap may not, and a test pins each.

The claim is also measured whole. `testapp_emit_edit_renders` renders a fixed set of recipes
over a generated source into PNG masters, and hammer screenshots its real preview of each at
the output size and compares (hammer `tests/edit/parity.test.ts`). Orientation and crop agree
to the byte, strokes to a mean of 0.02 levels, and a resize to under 0.3, because libvips'
kernel is not the browser's. The one systematic difference is the outermost pixel of a
resized picture, which the browser filters against transparency and libvips extends.

## 5. Bounds

| Bound | Value | Why |
|---|---|---|
| `max_strokes` | 64 | a stroke count is a count of composites |
| `max_points` | 4096, over all strokes | 16 KiB of recipe; a hand does not need more once decimated (hammer 04 §3) |
| `max_edge_px` | the widest rung | §4.1 |
| `min_edge_px` | the narrowest rung | below it every variant is an upscale |
| recipe body | the ordinary `body_max_bytes` | 4096 points base64url is ~22 KB |
| deadline | the existing 20 s operation timeout | the render is one libvips pipeline |
| rate | the `media` bucket | an edit is one decode, like an upload |

The first four are **emitted in the descriptor's `limits`**, as `limits.edit`, and hammer's
validator reads them. A client that bounds a recipe by its own numbers refuses what the
server takes, or sends what it refuses. That is the code-point lesson (hammer `CLAUDE.md`
§8) in another unit. Publishing the rung widths as numbers is the distinction 08 §4 already
draws: a number is not an address, and nothing here names a path.

## 6. The route, and who owns it

anvil ships the service and both handlers (`install_media_edit_routes`, 01 §17), and the
**application declares the routes**, the permission and the budget. The reference
application's:

```
POST /media-edits/{ns}/{id}    {"recipe":"<b64url>","detach":false}    → 201 or 200 {id,width,height}
GET  /media-edits/{ns}/{id}                                              → 200 {source,width,height,recipe}
```

- **Under a prefix of their own.** The design put the edit at `/media/{ns}/{id}/edits` and
  widened the metadata route. Every GET under `/media/{ns}/{id}/…` is a role on the public
  object route, so an edit route there would be one role name from the wrong handler; and
  the metadata route of the reference application is a list route. The installer refuses a
  pattern that does not end `{ns}/{id}`.
- `201` for a new object, and `200` with the same shape for an edit that already existed
  (§3). Both are successes, and hammer treats them as one.
- **The state route answers for any object.** For an edit it names the SOURCE, the source's
  size and the recipe, so an editor always opens on the original. For anything else the
  object is its own source and the recipe is `null`.
- A source that does not exist, or exists in another namespace, is the **stealth 404**:
  byte-identical to a missing id, because it is the same filter.
- Both success shapes are the library's own (`anvil/media/edit_shapes.h`), so a route
  description that names them declares what the handlers write.
- **The edit route checks `Origin`** against the installed allow-list before it reads the
  body, and refuses with `FORBIDDEN`. The access filter decides who may edit, and knows
  nothing of where a request came from.
- **Every answered edit is reported to the application's `on_edit`**, after the response,
  with the actor, the source, the object handed back and the code. The handler is anvil's,
  so this is the only way an edit reaches an application's audit log
  ([`01-seams.md`](01-seams.md) §17).

The handler is four stages, each on its own pool, for the reason 07 §4 gives:

| | Stage | Pool |
|---|---|---|
| 1 | Parse the body and bound the recipe text | loop |
| 2 | Charge the per-account budget, a Redis round trip and so never on the loop; decode and canonicalise the recipe; find the source; refuse a derived source; plan; look up `{ns, src, esha}` → answer `200` if present | `db_pool` |
| 3 | Render and derive variants under a fresh id | `cpu_pool` |
| 4 | Transaction: insert the row, `$inc` the source. A duplicate-key loss unlinks this attempt's files (they are under its own fresh id, so nothing else can be touched) and answers `200` with the winner | `db_pool` |

## 7. Redaction is not what this does

A crop or a black stroke over a face **does not remove the face**. The source still exists,
it is still addressable by its id through `GET /media/{ns}/{id}/{role}`, and the whole point
of §3 is that it stays that way so the edit can be reopened. Ids are UUIDv4 and unguessable,
but an id reaches every client that can open the editor.

So the request takes a second, explicit mode: **`"detach": true`**. A detached edit is
rendered identically and stored **without `src`, `edit` or `esha`**. It holds no reference on
the source, cannot be reopened, and is deduplicated only by the idempotency key. After the
application's binding swap releases the source, the source is collected on the ordinary
schedule if nothing else references it.

The limit is worth a sentence in the docs, not a flag: if another document still references
those bytes (the dedupe of 07 §4 again), the source survives, correctly. anvil cannot
unpublish a picture that somebody else is also using. The application is told this in
`01-seams.md`, and hammer's `submitEdit` takes `detach` as a required boolean, so the
application decides which edit it is making rather than reaching one by omission.

## 8. What happens to `apply_crop`

It was **removed** rather than fixed, in the same commit that landed the render path.
`validate_crop`'s rules move into the recipe validator as `edit.canonical`, `edit.bounds`
and `edit.too_small`; `CropRect` and `crop.h` go. There is no caller to migrate, but both
are in `include/`, so it is a **removal from the published surface under CLAUDE.md §9.3** and
carries a changelog line. [`08-images.md`](08-images.md) §5 becomes a pointer to this
document, and `images_test.cc`'s "crop leaves the master byte-identical" case becomes the
same assertion over an edit, because that property survives and is still the one worth
pinning.
