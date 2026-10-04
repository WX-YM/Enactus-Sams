# 04 — Image edits

The client half of crop, rotate, flip, resize and freehand drawing. anvil's half, including
the recipe's byte layout and why an edit is a new object, is anvil `docs/21-image-edits.md`.
This document assumes it. The rows that built both are [`15-tasks.md`](15-tasks.md) §Phase 9
and anvil's §Phase 17.

## 1. What the client does and does not do

The client **builds a recipe and previews it**. It never produces the edited pixels.

That rules out the obvious design: decode the photograph into a `<canvas>`, crop and paint
on it, `toBlob()` the result and upload that. It would work, and it is refused for three
reasons, in `CLAUDE.md`'s order:

- **It makes two renderers.** The canvas result would be what the person saw, the server's
  variants would be derived from it, and a re-edit would start from a re-encode of a
  re-encode. With a recipe, the server's render is the only render there is.
- **It puts the photograph in the tab.** A 12 MP canvas is 48 MB of backing store, and
  `imagePool` exists because two of those is the ceiling on a mid-range phone (`00` §3).
  An editor holding one for as long as a person fiddles with a crop is that ceiling spent
  on waiting.
- **It is destructive.** Re-editing a baked result cannot recover the part that was cropped
  away.

What it costs: an image is uploaded **before** it can be edited, so the uncropped original
crosses the network once. The existing `downscale` on `uploadImage` still applies. Editing
a local `File` before upload is deferred (§8).

## 2. The entry point

`hammer/edit`, its own entry point with its own gzip ceiling, for the reason `hammer/chart`
is one: most applications never show an editor, and they should pay nothing for one
(`CLAUDE.md` §2.1). The `edit` layer may import `core`, `wire` and `dom`.

| Module | Holds |
|---|---|
| `edit/recipe.ts` | The `Recipe` type, the canonical codec, `planEdit`, and the validator against `kEditLimits` |
| `edit/geometry.ts` | The orientation matrix, pointer → fixed-point, rotate and mirror that carry what is drawn, keyboard crop moves, decimation |
| `edit/history.ts` | Undo and redo over immutable recipes, bounded |
| `edit/submit.ts` | `submitEdit` and `reopenEdit`, and `editCall` over a client |
| `edit/editor.ts` | `renderImageEditor`: the SVG surface, the crop box, the draw layer, the stroke list |

### 2.1 The recipe type

```ts sketch: the shape of src/edit/recipe.ts, abridged
type QuarterTurns = 0 | 1 | 2 | 3;

type Stroke = {
    readonly rgba: number;                  // 0xRRGGBBAA, sRGB, straight alpha, A ≥ 1
    readonly width: number;                 // u16 fraction of the oriented short edge
    readonly points: Uint16Array;           // x0 y0 x1 y1 …, u16 fractions of the oriented frame
};

type Recipe = {
    readonly turns: QuarterTurns;
    readonly flip: boolean;
    readonly crop: FixedRect | null;
    readonly longEdgePx: number | null;
    readonly strokes: readonly Stroke[];
};

function encodeRecipe(recipe: Recipe, limits: EditLimits, source: SourceSize): Result<EncodedRecipe, RecipeError>;
function decodeRecipe(text: string, limits: EditLimits): Result<Recipe, RecipeError>;
function planEdit(recipe: Recipe, source: SourceSize, limits: EditLimits): Result<EditPlan, RecipeError>;
```

The decisions in that shape:

- **Points are a `Uint16Array`, not an array of `{x, y}`.** Four thousand points as objects
  is four thousand allocations, each with a hidden class, held for as long as the editor is
  open. As one typed array it is 16 KB, and it is already the bytes the codec writes
  (`CLAUDE.md` §2.3).
- **`EncodedRecipe` is branded, and only `encodeRecipe` makes one.** `submitEdit` takes only
  that, so a recipe that skipped validation cannot be sent. It is the `SanitizedHtml` trick
  applied to a request body. The coordinates themselves are plain numbers rather than a
  second brand: the encoder refuses any value that is not an integer in 0–65535, so a float
  cannot reach the wire whichever way it was made. The design called for branding them too,
  and one brand at the boundary turned out to be the one that pays.
- **A no-op is spelled `null`**, never as a full-frame crop or a resize to the natural size.
  anvil refuses both as non-canonical. The encoder turns a crop dragged out to the whole
  frame back into `null`, so a person who undoes a crop by hand sends a recipe with no crop
  rather than being refused.
- **The arithmetic is the server's.** `planEdit` is anvil's `plan_edit`, integer
  round-half-up for integer round-half-up. The largest intermediate, 65535 × 12000, is far
  inside 2^53, so a JS `number` holds it exactly.

### 2.2 The validator is the server's, restated

`encodeRecipe` checks every rule in anvil 21 §2.2 and returns the **same fault** the server
would: `edit.bounds`, `edit.too_small`, `edit.upscale`, `edit.empty`, `edit.stroke`, and the
rest. `kFaultWire` says which field and reason the server reports each one under in a
`VALIDATION_FAILED` body, so a screen can place the server's refusal where it placed its own.
The numbers come from the generated `kEditLimits` and nowhere else. It is a round-trip saver
rather than enforcement (`CLAUDE.md` §5). What it buys is that a person hitting the stroke cap
learns it while drawing, not after pressing save.

`SourceSize` is the source's pixel dimensions, from the edit state route. `edit.too_small`
and `edit.upscale` are statements about pixels, so a validator that did not know the
source's size could only guess them.

**The contract is `tests/edit/recipe_vectors.json`**, printed by anvil's
`testapp_emit_edit_vectors`. Its thirty vectors came from a third implementation written from
anvil's document, not from either codec. Both suites assert every one, and this suite checks
the decoded fields against what anvil's decoder read.

## 3. Geometry

The editor draws in the **oriented** frame at the oriented source's own pixel size, and
pointer input comes back through the SVG's own screen matrix:

```
source px ──orient──▶ oriented frame (fixed-point, what the recipe stores)
          ──crop────▶ crop frame
          ──fit─────▶ the viewBox, fitted by the browser
```

- **The image is physical, the chrome is logical.** A photograph's left edge is its left
  edge in Arabic too, so arrow keys on a crop handle move it physically, and the geometry
  never reads `dir`. The toolbar, the stroke list and the labels are laid out logically,
  like every other hammer surface (`CLAUDE.md` §8).
- **Rotating and mirroring carry what was drawn.** Each is an exact integer map on the
  fixed-point fractions, so a circle drawn round a face stays round the face when the
  picture is turned upright, and a rotation and its inverse return the identical recipe.
  A clockwise turn of a mirrored picture is one turn *fewer* of the source, because a
  rotation and a reflection do not commute. A test pulls a stroke back to source pixels
  through every orientation and checks it never moves.
- **A crop can hold a shape.** A picture bound for a place with one — a 16:9 slot, a square
  avatar — is refused there unless it has that shape, and a box dragged by hand lands within
  a server's tolerance of a ratio only by luck. `aspect: { num, den }` holds it: the editor
  opens on the largest centred box of that shape (no crop, when the whole frame already has
  it, since a whole-frame crop is the absent one spelled twice), every handle and key keeps
  it in oriented **pixels** — the recipe's fractions are per axis, so one fraction is two
  lengths — and a quarter turn, which stands a wide box up, refits it. A corner keeps its
  opposite corner still and follows whichever axis the pointer went further along; an edge
  grows the other axis about the centre. The fitted opening box is the base of the history,
  not a change to undo. The server stays the judge of whether the shape is close enough.
- **Decimation happens once, at pointer-up.** A stroke is recorded at the rate
  `pointermove` delivers, including `getCoalescedEvents()` so a fast stroke is not
  polygonal. When the pointer lifts, it is simplified with iterative Ramer–Douglas–Peucker at
  half an **output** pixel. A screen-pixel tolerance would keep detail the render cannot
  show, or drop detail it can, depending on the zoom.
- **A stroke past the count is refused before it starts; one past the point budget is
  refused whole.** Point budget cannot be known until the stroke is decimated, so the check
  is at the end of the gesture, and a stroke over it is dropped and reported as
  `stroke-limit` rather than truncated. A signature missing its last letters is worse than
  one the person is told to redraw.

## 4. The surface

`renderImageEditor(mount, options): EditorHandle`. It is unstyled, and it names no word and
no colour (`CLAUDE.md` §9).

### 4.1 One SVG, and why

The preview is a single `<svg>`. `<image href>` points at the address the application hands
in: a role of the source from the route builder, wide enough for the space the editor has.
Orientation is a `matrix()` in a `transform` attribute, the crop's dimming is an even-odd
`<path>`, and each stroke is a `<path d>`.

- **No pixel is decoded into script.** The browser decodes the `<image>` exactly as it would
  an `<img>`, from the media origin, through the CDN cache. Nothing is `fetch`ed and nothing is
  drawn to a canvas, so the cross-origin tainting question never arises (`CLAUDE.md` §2.2).
  An `href` that is not a web address is refused at mount, as programmer error.
- **It is CSP-safe without trying.** `transform`, `d`, `x`, `width` and `stroke` are
  attributes, not inline style, and a test asserts no `[style]` exists after a stroke. The
  one thing the canvas needs that an attribute cannot say is `touch-action: none`, so touch
  does not scroll the page under a stroke. The application's stylesheet sets it on the canvas
  class.
- **It matches the server's render, and that is measured.** `stroke-linecap="round"`,
  `stroke-linejoin="round"` and `stroke-opacity` on one path paint exactly the union of
  capsules anvil rasterises, blended in sRGB (anvil 21 §4.2). `tests/edit/parity.test.ts`
  screenshots this preview at each output size against anvil's renders of the same recipes
  (`tests/edit/parity/`, written by anvil's `testapp_emit_edit_renders`). Turn, flip and crop
  agree to the byte, strokes to a mean of 0.02 levels, a resize to under 0.3, because
  libvips' resampling kernel is not the browser's. A control with the flip left out scores
  42. The outermost pixel of a resized picture is the one convention that differs, since the
  browser filters an image's edge against transparency where libvips extends it, so the
  comparison leaves that pixel out.
- **A stroke's colour is data.** The palette is the application's: each colour arrives with
  a label and a `className` for its swatch, because a swatch that showed its colour with an
  inline style would be a style this library set. The chosen value goes into the path's
  `stroke` attribute, the one colour this library ever writes, and it is the person's.

`pointermove` updates are coalesced to one `d` write per animation frame, never one per event.

### 4.2 Parts, copy and state

The pattern `renderUpload` uses. `EditorPart` names every element the application can class,
and `EditorCopy` supplies every label, one sentence per refusal, and a function for every
string built from a number: `cropSize(widthPx, heightPx)` for the live region, and
`strokeName(index, total)` for each entry in the stroke list.

`EditorHandle` adds `recipe()`, `encode()` and `setTool()` to the usual `element` and `close`.
Saving is the application's button calling `encode()` and then `submitEdit`, so whether a save
detaches is decided where the words for it are.

### 4.3 Keyboard and assistive technology

- **Crop.** The crop box and each of its eight handles are focusable, in document order, and
  labelled. Arrow keys move by 1% of the frame and Shift+arrow by 10%. `Home` and `End` take a
  handle's edge to the frame's. A held key is one history entry, committed on key-up, and the
  live region announces the resulting size through `cropSize`, debounced, so a held key
  announces the size it stopped at.
- **Orient.** Rotate and flip are `<button>`s, in a `role="toolbar"` with a label.
- **Draw.** Freehand drawing is path-dependent input, and WCAG 2.1.1 exempts it. Every
  **other** action a stroke can take is keyboard-reachable: the stroke list is a listbox,
  selecting an entry highlights that stroke, and `Delete` removes it.
- **Undo, redo** are buttons and `Ctrl`/`⌘`+`Z`, `Shift`+…+`Z` and `Ctrl`+`Y`, handled on the
  editor root, never on the document. A shortcut bound to the document would take `Ctrl+Z`
  from a text field elsewhere on the page.
- No focus trap. The editor is inline content, and `Escape` means nothing to it.

### 4.4 History

`EditHistory` keeps past and future recipes, **bounded at 100 entries** and evicting the
oldest (`CLAUDE.md` §2.3). A stroke's `Uint16Array` is shared, never copied, between
successive recipes that contain it, and a test checks it by identity. A drag replaces the
present on every frame and is one entry when it ends. An undo mid-drag abandons the drag.

## 5. The two calls

```ts sketch: the shape of src/edit/submit.ts, abridged
function submitEdit(call: EditCall, options: {
    readonly route: CallableRoute;          // the application's edit route, generated
    readonly source: MediaSubject;
    readonly recipe: EncodedRecipe;
    readonly detach: boolean;               // required, never defaulted
    readonly signal: AbortSignal;
}): Promise<Result<EditedMedia, HammerError | EditAnswerError>>;

function reopenEdit(call: EditCall, options: {
    readonly route: CallableRoute;          // the application's edit state route, generated
    readonly subject: MediaSubject;
    readonly limits: EditLimits;
    readonly signal: AbortSignal;
}): Promise<Result<ReopenedEdit, HammerError | EditAnswerError | RecipeError>>;
```

- **`submitEdit` is retried by the library, per its ordinary policy** (`CLAUDE.md` §6), and
  carries an idempotency key. anvil also resolves an identical edit of the same source to one
  object by the recipe's hash, so a retry after a lost response returns the **same** object,
  with a `200` rather than a `201`. Both are success, and `EditedMedia` does not say which.
- **`detach` is required, never optional.** A detached edit can never be reopened and holds
  nothing on its source, which is what lets the source be collected. It exists for redaction,
  and anvil 21 §7 says what that does and does not guarantee. A caller decides which it is
  making rather than reaching one by omission.
- **Neither call binds the result.** Swapping a document's reference from the old id to the
  new one is the application's own versioned write (`state/versioned.ts`), carrying the
  version it read, because the document is the application's.
- **`reopenEdit` on an edit answers with its source** and its recipe, so the editor always
  opens on the original and a re-edit is always source plus whole recipe. anvil refuses an
  edit of an edit. A stored recipe this client cannot decode is refused, not dropped: opening
  the source without it would look like an edit that lost its work.
- **Both answers are narrowed once**, against the shapes anvil's handlers write. A 2xx body
  that is not that shape is `edit-answer`, never a guess.

## 6. The descriptor

- `limits.edit`, emitted as `kEditLimits`. A descriptor from a server that predates it emits
  no `kEditLimits`, so an editor mounted against that server **fails to type-check** rather
  than validating against numbers nobody sent. The design had the generator refuse a
  descriptor with an edit route and no bounds; that needed the generator to know which route
  is the edit route, which is an application's route id, so the type-check does the job
  instead.
- The two routes are the application's. anvil's reference application declares them as
  `media.edit` and `media.edit_state` under `/media-edits/{ns}/{id}`. That prefix is its own
  because every GET under `/media/{ns}/{id}/…` is a role on the public object route. Their
  response shapes are anvil's, so the generated `ResponseOf` for each is what the handlers
  write.

Both are §12 of [`01-seams.md`](01-seams.md).

## 7. Budgets

| | Measured | Ceiling |
|---|---|---|
| `hammer/edit`, gzipped | 8.3 KB | 9 KB, in `tools/bundle-budget.json` |
| resident, while open | one recipe history (≤ 100 entries over shared points) and the browser's own decoded `<image>` | no canvas and no bitmap in script |
| main thread per `pointermove` | one `d` write per animation frame | — |

## 8. Deferred, deliberately

- **Editing before upload.** The preview would need `createObjectURL` over the local `File`,
  which is legitimate for a `File` and still a second path to own. The result would still
  have to be rendered by the server, so the upload saves nothing. It is worth doing when an
  application needs a crop before a person may upload at all (an avatar with a mandated
  aspect ratio), and that application will say so.
- **Aspect-locked crop.** An `aspect` option is a small constraint in `nudgeCrop` and
  `cropBetween`, and it is the first extension. It is out of version 1 only because nothing
  here has a caller for it yet.
- **Shapes, text, filters, free rotation.** anvil 21 §2.1 gives the reasons, and they are
  the server's reasons: the client cannot preview an operation the server does not render.
