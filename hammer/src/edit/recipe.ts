// An image edit, as the canonical recipe anvil renders (anvil
// `docs/21-image-edits.md` §2, and [`docs/04-image-edits.md`](../../docs/04-image-edits.md)).
//
// The recipe is a byte-exact contract between two codecs in two languages —
// this one and anvil's `images/recipe.cc` — and the vectors in
// `tests/edit/recipe_vectors.json` are the contract between them. Those vectors
// were produced by a third implementation written from the document, so neither
// codec is checked against its own belief.
//
// --- integers, all the way down ----------------------------------------------
//
// A coordinate is a u16 fraction of its frame, and every size is computed by
// integer round-half-up of a rational, exactly as the server computes it. A JS
// `number` holds every intermediate here exactly — the largest is 65535 × 12000,
// well inside 2^53 — so the arithmetic below is the server's, not an
// approximation of it. Floats would make 0.1 three different values in three
// places, and three values are three hashes of one edit.
//
// --- the recipe is validated where the person can be told --------------------
//
// Every rule anvil applies is applied here too, with the SAME fault name and the
// same wire field and reason, and against the bounds the descriptor published
// (`kEditLimits`). It is a round-trip saver and not the control: anvil decodes
// and re-checks every recipe, because a client-side check is one an attacker
// skips (`CLAUDE.md` §5).

import { decodeBase64Url, encodeBase64Url } from "../core/base64url.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

// The bounds the descriptor publishes as `limits.edit`, which the generated
// module emits as `kEditLimits`.
export type EditLimits = {
    readonly maxStrokes: number;
    readonly maxPoints: number;
    readonly maxEdgePx: number;
    readonly minEdgePx: number;
};

// The source's pixel size, from the edit state route. The rules that depend on
// it — a crop that rounds to nothing, a resize that would upscale, a result too
// narrow to serve — are statements about pixels, and a validator that did not
// know them could only guess.
export type SourceSize = {
    readonly widthPx: number;
    readonly heightPx: number;
};

export const kFixedOne = 65535;
export const kMaxStrokeWidth = 8192;
const kVersion = 1;

export type QuarterTurns = 0 | 1 | 2 | 3;

// Fractions of the oriented source, 0..65535.
export type FixedRect = {
    readonly x: number;
    readonly y: number;
    readonly w: number;
    readonly h: number;
};

export type Stroke = {
    // 0xRRGGBBAA: sRGB, straight alpha, alpha ≥ 1.
    readonly rgba: number;
    // A fraction of the ORIENTED source's short edge.
    readonly width: number;
    // x0 y0 x1 y1 …, fractions of the oriented source. One typed array rather
    // than an array of points: four thousand `{x, y}` objects are four thousand
    // allocations held for as long as the editor is open, and this is already
    // the shape the codec writes (`CLAUDE.md` §2.3).
    readonly points: Uint16Array;
};

// Every field declared, always: a no-op is `null`, never an absent key, so every
// recipe has one object shape (`CLAUDE.md` §2.3).
export type Recipe = {
    // Quarter turns clockwise, then an optional horizontal flip. A vertical flip
    // is two turns and a flip, so it has no field of its own — one picture, one
    // recipe.
    readonly turns: QuarterTurns;
    readonly flip: boolean;
    readonly crop: FixedRect | null;
    readonly longEdgePx: number | null;
    readonly strokes: readonly Stroke[];
};

export const kEmptyRecipe: Recipe = { turns: 0, flip: false, crop: null, longEdgePx: null, strokes: [] };

// The server's fault names, one for one (anvil `images/recipe.h`).
export type RecipeFault =
    | "edit.format"
    | "edit.canonical"
    | "edit.empty"
    | "edit.crop"
    | "edit.too_small"
    | "edit.upscale"
    | "edit.stroke"
    | "edit.bounds";

export type RecipeError = {
    readonly kind: "recipe";
    readonly fault: RecipeFault;
};

// Where the server reports each fault in a VALIDATION_FAILED body: the field and
// the reason, so a screen can place the server's refusal where it placed its own.
export const kFaultWire: Readonly<Record<RecipeFault, { readonly field: string; readonly reason: string }>> = {
    "edit.format": { field: "recipe", reason: "BAD_FORMAT" },
    "edit.canonical": { field: "recipe", reason: "BAD_FORMAT" },
    "edit.empty": { field: "recipe", reason: "REQUIRED" },
    "edit.crop": { field: "recipe.crop", reason: "OUT_OF_RANGE" },
    "edit.too_small": { field: "recipe", reason: "TOO_SHORT" },
    "edit.upscale": { field: "recipe.resize", reason: "OUT_OF_RANGE" },
    "edit.stroke": { field: "recipe.strokes", reason: "BAD_FORMAT" },
    "edit.bounds": { field: "recipe.strokes", reason: "TOO_LONG" },
};

function refuse(fault: RecipeFault): Result<never, RecipeError> {
    return fail({ kind: "recipe", fault });
}

// round(numerator / denominator), half up, in integers — the server's
// `round_div`, and the only rounding in the contract.
export function roundDiv(numerator: number, denominator: number): number {
    return Math.floor((2 * numerator + denominator) / (2 * denominator));
}

// --- the plan: what a recipe renders to, in pixels ---------------------------

export type PixelBox = {
    readonly left: number;
    readonly top: number;
    readonly width: number;
    readonly height: number;
};

export type EditPlan = {
    readonly orientedWidthPx: number;
    readonly orientedHeightPx: number;
    readonly crop: PixelBox;
    readonly outWidthPx: number;
    readonly outHeightPx: number;
};

export function orientedSize(source: SourceSize, turns: QuarterTurns): { readonly widthPx: number; readonly heightPx: number } {
    return turns % 2 === 1
        ? { widthPx: source.heightPx, heightPx: source.widthPx }
        : { widthPx: source.widthPx, heightPx: source.heightPx };
}

function scale(fraction: number, extent: number): number {
    return roundDiv(fraction * extent, kFixedOne);
}

// anvil's `plan_edit`, rule for rule and rounding for rounding.
export function planEdit(recipe: Recipe, source: SourceSize, limits: EditLimits): Result<EditPlan, RecipeError> {
    const oriented = orientedSize(source, recipe.turns);
    let crop: PixelBox = { left: 0, top: 0, width: oriented.widthPx, height: oriented.heightPx };
    if (recipe.crop !== null) {
        // Both edges rounded, never an edge and an extent, so the far edge can
        // never land a pixel outside the image.
        const left = scale(recipe.crop.x, oriented.widthPx);
        const top = scale(recipe.crop.y, oriented.heightPx);
        const right = scale(recipe.crop.x + recipe.crop.w, oriented.widthPx);
        const bottom = scale(recipe.crop.y + recipe.crop.h, oriented.heightPx);
        if (right <= left || bottom <= top) {
            return refuse("edit.crop");
        }
        crop = { left, top, width: right - left, height: bottom - top };
    }

    const croppedLong = Math.max(crop.width, crop.height);
    let target = croppedLong;
    if (recipe.longEdgePx !== null) {
        if (recipe.longEdgePx >= croppedLong) {
            return refuse("edit.upscale");
        }
        target = recipe.longEdgePx;
    }
    target = Math.min(target, limits.maxEdgePx);

    let outWidthPx = crop.width;
    let outHeightPx = crop.height;
    if (target !== croppedLong) {
        outWidthPx = Math.max(1, roundDiv(crop.width * target, croppedLong));
        outHeightPx = Math.max(1, roundDiv(crop.height * target, croppedLong));
    }
    if (outWidthPx < limits.minEdgePx) {
        return refuse("edit.too_small");
    }
    return ok({
        orientedWidthPx: oriented.widthPx,
        orientedHeightPx: oriented.heightPx,
        crop,
        outWidthPx,
        outHeightPx,
    });
}

// --- the codec ---------------------------------------------------------------

declare const kEncodedBrand: unique symbol;

// A recipe that passed every rule and the plan, as the base64url text the edit
// route takes. Only `encodeRecipe` makes one, so a recipe that skipped
// validation cannot be submitted — the `SanitizedHtml` trick, applied to a body.
export type EncodedRecipe = {
    readonly text: string;
    readonly plan: EditPlan;
    readonly [kEncodedBrand]: true;
};

function isU16(value: number): boolean {
    return Number.isInteger(value) && value >= 0 && value <= kFixedOne;
}

// Every stroke rule and bound, shared by both directions.
function checkStrokes(strokes: readonly Stroke[], limits: EditLimits): RecipeFault | null {
    if (strokes.length > limits.maxStrokes || strokes.length > 255) {
        return "edit.bounds";
    }
    let total = 0;
    for (const stroke of strokes) {
        const alpha = stroke.rgba & 0xff;
        const pointCount = stroke.points.length / 2;
        if (
            !Number.isInteger(stroke.rgba) ||
            stroke.rgba < 0 ||
            stroke.rgba > 0xffffffff ||
            alpha === 0 ||
            !isU16(stroke.width) ||
            stroke.width === 0 ||
            stroke.width > kMaxStrokeWidth ||
            pointCount < 1 ||
            !Number.isInteger(pointCount)
        ) {
            return "edit.stroke";
        }
        total += pointCount;
        if (total > limits.maxPoints) {
            return "edit.bounds";
        }
    }
    return null;
}

// The canonical bytes. The recipe must already have passed the checks: this
// writes what it is given.
function writeBytes(recipe: Recipe, crop: FixedRect | null): Uint8Array {
    let size = 3 + (crop === null ? 0 : 8) + (recipe.longEdgePx === null ? 0 : 2);
    for (const stroke of recipe.strokes) {
        size += 9 + stroke.points.length * 2;
    }
    const out = new Uint8Array(size);
    const view = new DataView(out.buffer);
    let at = 0;
    out[at++] = kVersion;
    out[at++] =
        recipe.turns | (recipe.flip ? 0x04 : 0) | (crop === null ? 0 : 0x08) | (recipe.longEdgePx === null ? 0 : 0x10);
    if (crop !== null) {
        view.setUint16(at, crop.x);
        view.setUint16(at + 2, crop.y);
        view.setUint16(at + 4, crop.w);
        view.setUint16(at + 6, crop.h);
        at += 8;
    }
    if (recipe.longEdgePx !== null) {
        view.setUint16(at, recipe.longEdgePx);
        at += 2;
    }
    out[at++] = recipe.strokes.length;
    for (const stroke of recipe.strokes) {
        out[at++] = 0;
        view.setUint32(at, stroke.rgba >>> 0);
        at += 4;
        view.setUint16(at, stroke.width);
        view.setUint16(at + 2, stroke.points.length / 2);
        at += 4;
        for (const coordinate of stroke.points) {
            view.setUint16(at, coordinate);
            at += 2;
        }
    }
    return out;
}

// Validates, plans against the source, and encodes.
//
// One normalisation, and only one: a crop that reaches the whole frame is sent
// as no crop. A person who drags the box out to the edges has undone the crop,
// and the server refuses the full-frame spelling as a second name for "none"; a
// refusal for that would be a refusal for doing nothing wrong. A resize at or
// above the natural size is NOT normalised: the editor never offers one, so
// meeting it means a caller built it, and it is refused as the server would.
export function encodeRecipe(
    recipe: Recipe,
    limits: EditLimits,
    source: SourceSize,
): Result<EncodedRecipe, RecipeError> {
    let crop = recipe.crop;
    if (crop !== null) {
        if (!isU16(crop.x) || !isU16(crop.y) || !isU16(crop.w) || !isU16(crop.h)) {
            return refuse("edit.crop");
        }
        if (crop.w === 0 || crop.h === 0 || crop.x + crop.w > kFixedOne || crop.y + crop.h > kFixedOne) {
            return refuse("edit.crop");
        }
        if (crop.x === 0 && crop.y === 0 && crop.w === kFixedOne && crop.h === kFixedOne) {
            crop = null;
        }
    }
    if (recipe.longEdgePx !== null && (!isU16(recipe.longEdgePx) || recipe.longEdgePx === 0)) {
        return refuse("edit.upscale");
    }
    const strokeFault = checkStrokes(recipe.strokes, limits);
    if (strokeFault !== null) {
        return refuse(strokeFault);
    }
    const normalised: Recipe = crop === recipe.crop ? recipe : { ...recipe, crop };
    if (!normalised.flip && normalised.turns === 0 && crop === null && normalised.longEdgePx === null && normalised.strokes.length === 0) {
        return refuse("edit.empty");
    }
    const plan = planEdit(normalised, source, limits);
    if (!plan.ok) {
        return plan;
    }
    return ok({ text: encodeBase64Url(writeBytes(normalised, crop)), plan: plan.value } as EncodedRecipe);
}

// A recipe as the server stored it. Refused on exactly the rules anvil refuses
// on, including the canonical one: whatever was read must re-encode to the
// identical text.
export function decodeRecipe(text: string, limits: EditLimits): Result<Recipe, RecipeError> {
    if (text.length % 4 === 1) {
        return refuse("edit.format");
    }
    const bytes = decodeBase64Url(text, Math.floor((text.length * 3) / 4));
    if (bytes === null || bytes.length < 3) {
        return refuse("edit.format");
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let at = 0;
    const need = (count: number): boolean => at + count <= bytes.length;

    const version = bytes[at++];
    const flags = bytes[at++] ?? 0;
    if (version !== kVersion || (flags & 0xe0) !== 0) {
        return refuse("edit.format");
    }

    let crop: FixedRect | null = null;
    if ((flags & 0x08) !== 0) {
        if (!need(8)) {
            return refuse("edit.format");
        }
        crop = { x: view.getUint16(at), y: view.getUint16(at + 2), w: view.getUint16(at + 4), h: view.getUint16(at + 6) };
        at += 8;
        if (crop.w === 0 || crop.h === 0 || crop.x + crop.w > kFixedOne || crop.y + crop.h > kFixedOne) {
            return refuse("edit.crop");
        }
        if (crop.x === 0 && crop.y === 0 && crop.w === kFixedOne && crop.h === kFixedOne) {
            return refuse("edit.canonical");
        }
    }

    let longEdgePx: number | null = null;
    if ((flags & 0x10) !== 0) {
        if (!need(2)) {
            return refuse("edit.format");
        }
        longEdgePx = view.getUint16(at);
        at += 2;
        if (longEdgePx === 0) {
            return refuse("edit.upscale");
        }
    }

    if (!need(1)) {
        return refuse("edit.format");
    }
    const count = bytes[at++] ?? 0;
    if (count > limits.maxStrokes) {
        return refuse("edit.bounds");
    }

    const strokes: Stroke[] = [];
    let total = 0;
    for (let s = 0; s < count; s += 1) {
        if (!need(9)) {
            return refuse("edit.format");
        }
        if (bytes[at] !== 0) {
            return refuse("edit.format");
        }
        const rgba = view.getUint32(at + 1);
        const width = view.getUint16(at + 5);
        const pointCount = view.getUint16(at + 7);
        at += 9;
        if ((rgba & 0xff) === 0 || width === 0 || width > kMaxStrokeWidth || pointCount === 0) {
            return refuse("edit.stroke");
        }
        total += pointCount;
        // Before the points are read, so a recipe claiming a million of them
        // costs a comparison and not an allocation.
        if (total > limits.maxPoints) {
            return refuse("edit.bounds");
        }
        if (!need(pointCount * 4)) {
            return refuse("edit.format");
        }
        const points = new Uint16Array(pointCount * 2);
        for (let p = 0; p < points.length; p += 1) {
            points[p] = view.getUint16(at);
            at += 2;
        }
        strokes.push({ rgba, width, points });
    }
    if (at !== bytes.length) {
        return refuse("edit.format");
    }

    const recipe: Recipe = { turns: (flags & 0x03) as QuarterTurns, flip: (flags & 0x04) !== 0, crop, longEdgePx, strokes };
    if (recipe.turns === 0 && !recipe.flip && crop === null && longEdgePx === null && strokes.length === 0) {
        return refuse("edit.empty");
    }
    // The canonical check, once: base64url has more than one spelling for a
    // tail, and a recipe with two spellings is two hashes.
    if (encodeBase64Url(writeBytes(recipe, crop)) !== text) {
        return refuse("edit.canonical");
    }
    return ok(recipe);
}
