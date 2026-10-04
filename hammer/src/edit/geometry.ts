// Where things are, in an image edit.
//
// Three frames, and every one of them is PHYSICAL — a photograph's left edge is
// its left edge in Arabic too, so nothing here reads `dir`:
//
//     source px ──orient──▶ oriented frame (fixed-point, what the recipe stores)
//               ──crop────▶ crop frame
//               ──fit─────▶ the SVG viewBox the editor draws in
//
// The editor draws in the ORIENTED frame at the oriented source's own pixel
// size, so a stroke's width in the preview and in anvil's render is the same
// fraction of the same short edge, and the browser's SVG does the fitting.

import type { FixedRect, QuarterTurns, Recipe, SourceSize, Stroke } from "./recipe.js";
import { kFixedOne, orientedSize } from "./recipe.js";

// The SVG `transform` that draws the SOURCE into the oriented frame: the quarter
// turns clockwise, then the horizontal flip — the one order the recipe names and
// the order anvil's `vips_rot` then `vips_flip` apply.
//
// A matrix rather than `rotate()` and `translate()`: one attribute, one parse,
// and no chance of composing the two in the other order.
export function orientTransform(source: SourceSize, turns: QuarterTurns, flip: boolean): string {
    const w = source.widthPx;
    const h = source.heightPx;
    // x' = a·x + c·y + e,  y' = b·x + d·y + f
    let [a, b, c, d, e, f] =
        turns === 1 ? [0, 1, -1, 0, h, 0]
        : turns === 2 ? [-1, 0, 0, -1, w, h]
        : turns === 3 ? [0, -1, 1, 0, 0, w]
        : [1, 0, 0, 1, 0, 0];
    if (flip) {
        const oriented = orientedSize(source, turns);
        a = -a;
        c = -c;
        e = oriented.widthPx - e;
    }
    return `matrix(${a} ${b} ${c} ${d} ${e} ${f})`;
}

// A position in the oriented frame, in pixels, as the fraction the recipe
// stores. Rounded to the u16 the wire carries, so the preview can only ever draw
// a point the server will render.
export function toFixed(px: number, extentPx: number): number {
    if (!(extentPx > 0)) {
        return 0;
    }
    return Math.min(kFixedOne, Math.max(0, Math.round((px * kFixedOne) / extentPx)));
}

export function fromFixed(fixed: number, extentPx: number): number {
    return (fixed * extentPx) / kFixedOne;
}

// A stroke's width in oriented pixels: a fraction of the SHORT edge, so a
// quarter turn leaves it unchanged.
export function strokeWidthPx(stroke: Stroke, source: SourceSize): number {
    return (stroke.width * Math.min(source.widthPx, source.heightPx)) / kFixedOne;
}

// --- rotate and flip, with everything drawn staying on what it was drawn on ----
//
// Rotating the picture after cropping or drawing must carry the crop and every
// stroke with it: a person who circled a face and then turned the photograph
// upright expects the circle still round the face. Each is an exact integer map
// on the fixed-point fractions, so a rotation and its inverse return the recipe
// they started from bit for bit.

function rotateRect(rect: FixedRect, clockwise: boolean): FixedRect {
    return clockwise
        ? { x: kFixedOne - (rect.y + rect.h), y: rect.x, w: rect.h, h: rect.w }
        : { x: rect.y, y: kFixedOne - (rect.x + rect.w), w: rect.h, h: rect.w };
}

function mapPoints(stroke: Stroke, map: (x: number, y: number) => readonly [number, number]): Stroke {
    const points = new Uint16Array(stroke.points.length);
    for (let i = 0; i + 1 < points.length; i += 2) {
        const [x, y] = map(stroke.points[i] ?? 0, stroke.points[i + 1] ?? 0);
        points[i] = x;
        points[i + 1] = y;
    }
    return { rgba: stroke.rgba, width: stroke.width, points };
}

// A quarter turn of the picture as the person sees it. With the picture already
// flipped, turning it clockwise is one turn FEWER of the source, because a
// rotation and a reflection do not commute.
export function rotate(recipe: Recipe, clockwise: boolean): Recipe {
    const step = clockwise !== recipe.flip ? 1 : 3;
    const turns = ((recipe.turns + step) % 4) as QuarterTurns;
    const map = clockwise
        ? (x: number, y: number) => [kFixedOne - y, x] as const
        : (x: number, y: number) => [y, kFixedOne - x] as const;
    return {
        turns,
        flip: recipe.flip,
        crop: recipe.crop === null ? null : rotateRect(recipe.crop, clockwise),
        longEdgePx: recipe.longEdgePx,
        strokes: recipe.strokes.map((stroke) => mapPoints(stroke, map)),
    };
}

// A left-to-right mirror of the picture as the person sees it.
export function mirror(recipe: Recipe): Recipe {
    return {
        turns: recipe.turns,
        flip: !recipe.flip,
        crop: recipe.crop === null ? null : { ...recipe.crop, x: kFixedOne - (recipe.crop.x + recipe.crop.w) },
        longEdgePx: recipe.longEdgePx,
        strokes: recipe.strokes.map((stroke) => mapPoints(stroke, (x, y) => [kFixedOne - x, y] as const)),
    };
}

// --- moving the crop by keyboard --------------------------------------------------

export type CropHandle = "move" | "n" | "s" | "e" | "w" | "ne" | "nw" | "se" | "sw";

// One percent of the frame, and ten with Shift.
export const kCropStep = 655;
export const kCropStepLarge = 6554;
// The smallest crop the box can be shrunk to, so a key held down never folds it
// to nothing. The server's own floor is in pixels and is checked on encode.
export const kCropMinimum = 655;

const kWholeFrame: FixedRect = { x: 0, y: 0, w: kFixedOne, h: kFixedOne };

function clamp(value: number, low: number, high: number): number {
    return Math.min(high, Math.max(low, value));
}

// Moves one edge or corner of the crop — or the whole box — by (dx, dy) in fixed
// units, keeping it inside the frame and no smaller than kCropMinimum.
export function nudgeCrop(crop: FixedRect | null, handle: CropHandle, dx: number, dy: number): FixedRect {
    const from = crop ?? kWholeFrame;
    let left = from.x;
    let top = from.y;
    let right = from.x + from.w;
    let bottom = from.y + from.h;

    if (handle === "move") {
        const moveX = clamp(dx, -left, kFixedOne - right);
        const moveY = clamp(dy, -top, kFixedOne - bottom);
        return { x: left + moveX, y: top + moveY, w: from.w, h: from.h };
    }
    if (handle.includes("w")) left = clamp(left + dx, 0, right - kCropMinimum);
    if (handle.includes("e")) right = clamp(right + dx, left + kCropMinimum, kFixedOne);
    if (handle.includes("n")) top = clamp(top + dy, 0, bottom - kCropMinimum);
    if (handle.includes("s")) bottom = clamp(bottom + dy, top + kCropMinimum, kFixedOne);
    return { x: left, y: top, w: right - left, h: bottom - top };
}

// A crop from two corners a pointer dragged between, in fixed units.
export function cropBetween(ax: number, ay: number, bx: number, by: number): FixedRect {
    const left = clamp(Math.min(ax, bx), 0, kFixedOne);
    const top = clamp(Math.min(ay, by), 0, kFixedOne);
    const right = clamp(Math.max(ax, bx), 0, kFixedOne);
    const bottom = clamp(Math.max(ay, by), 0, kFixedOne);
    return {
        x: left,
        y: top,
        w: Math.max(kCropMinimum, right - left),
        h: Math.max(kCropMinimum, bottom - top),
    };
}

// --- a crop of a fixed shape ---------------------------------------------------------
//
// A picture bound for a place with a shape — a 16:9 slot, a square avatar — is
// cropped to that shape or refused there, and a hand-dragged box lands within a
// server's tolerance of a ratio by luck. So the ratio can be held: every box
// the person makes is corrected to it, in PIXELS of the oriented frame, where
// the ratio is stated. The fractions a recipe stores are per axis, so the same
// fraction is a different length across and down.

export type CropAspect = {
    // Width to height, in the application's own integers: 16 and 9, not 1.777.
    readonly num: number;
    readonly den: number;
};

type Frame = { readonly widthPx: number; readonly heightPx: number };

function toRect(frame: Frame, leftPx: number, topPx: number, widthPx: number, heightPx: number): FixedRect {
    // Both edges rounded, never an edge and an extent, as anvil's plan does:
    // an extent rounded on its own can put the far edge past the frame.
    const x = toFixed(leftPx, frame.widthPx);
    const y = toFixed(topPx, frame.heightPx);
    return {
        x,
        y,
        w: Math.max(1, toFixed(leftPx + widthPx, frame.widthPx) - x),
        h: Math.max(1, toFixed(topPx + heightPx, frame.heightPx) - y),
    };
}

// The largest box of `aspect` the frame holds, centred. Null when that is the
// whole frame, because a crop of the whole frame is the absent crop spelled a
// second way and the server refuses it as one.
export function largestCrop(frame: Frame, aspect: CropAspect): FixedRect | null {
    const ratio = aspect.num / aspect.den;
    const widthPx = Math.min(frame.widthPx, frame.heightPx * ratio);
    const heightPx = widthPx / ratio;
    const rect = toRect(frame, (frame.widthPx - widthPx) / 2, (frame.heightPx - heightPx) / 2, widthPx, heightPx);
    return rect.x === 0 && rect.y === 0 && rect.w === kFixedOne && rect.h === kFixedOne ? null : rect;
}

// Whether `crop` already has the shape, to within a pixel of the frame — the
// most the rounding to fractions can move it.
export function holdsAspect(crop: FixedRect | null, frame: Frame, aspect: CropAspect): boolean {
    const box = crop ?? kWholeFrame;
    const widthPx = fromFixed(box.w, frame.widthPx);
    const heightPx = fromFixed(box.h, frame.heightPx);
    return Math.abs(widthPx - (heightPx * aspect.num) / aspect.den) <= 1;
}

// `proposed` — what nudgeCrop or cropBetween made of a gesture on `handle` —
// corrected to `aspect`. The edge or corner opposite the handle stays where it
// was; an edge handle grows the other axis about the box's centre, so dragging
// the right edge of a wide box does not also drag its top. The box never leaves
// the frame and never shrinks below kCropMinimum on either axis.
export function lockCrop(proposed: FixedRect, handle: CropHandle, frame: Frame, aspect: CropAspect): FixedRect {
    if (handle === "move") {
        return proposed;
    }
    const ratio = aspect.num / aspect.den;
    const left = fromFixed(proposed.x, frame.widthPx);
    const top = fromFixed(proposed.y, frame.heightPx);
    const right = fromFixed(proposed.x + proposed.w, frame.widthPx);
    const bottom = fromFixed(proposed.y + proposed.h, frame.heightPx);

    const dirX = handle.includes("e") ? 1 : handle.includes("w") ? -1 : 0;
    const dirY = handle.includes("s") ? 1 : handle.includes("n") ? -1 : 0;
    const anchorX = dirX === 1 ? left : dirX === -1 ? right : (left + right) / 2;
    const anchorY = dirY === 1 ? top : dirY === -1 ? bottom : (top + bottom) / 2;

    // A corner follows whichever axis the pointer went further along, so the
    // box always reaches it; an edge follows its own axis.
    const wanted =
        dirX !== 0 && dirY !== 0 ? Math.max(right - left, (bottom - top) * ratio)
        : dirX !== 0 ? right - left
        : (bottom - top) * ratio;
    const roomX = dirX === 1 ? frame.widthPx - anchorX : dirX === -1 ? anchorX : 2 * Math.min(anchorX, frame.widthPx - anchorX);
    const roomY = dirY === 1 ? frame.heightPx - anchorY : dirY === -1 ? anchorY : 2 * Math.min(anchorY, frame.heightPx - anchorY);
    const smallest = Math.max(fromFixed(kCropMinimum, frame.widthPx), fromFixed(kCropMinimum, frame.heightPx) * ratio);
    const widthPx = Math.min(Math.max(wanted, smallest), roomX, roomY * ratio);
    const heightPx = widthPx / ratio;

    const x = dirX === 1 ? anchorX : dirX === -1 ? anchorX - widthPx : anchorX - widthPx / 2;
    const y = dirY === 1 ? anchorY : dirY === -1 ? anchorY - heightPx : anchorY - heightPx / 2;
    return toRect(frame, x, y, widthPx, heightPx);
}

// Which corner a fresh drag from (ax, ay) to (bx, by) is pulling, so a new box
// drawn under a held shape grows away from where the pointer went down.
export function dragCorner(ax: number, ay: number, bx: number, by: number): CropHandle {
    return `${by < ay ? "n" : "s"}${bx < ax ? "w" : "e"}` as CropHandle;
}

// --- decimation ----------------------------------------------------------------------

// Ramer–Douglas–Peucker over interleaved x,y pairs, keeping every point that
// lies further than `tolerance` from the line its neighbours would draw.
//
// Iterative, with an explicit stack: a recursive version on a long signature
// is a stack depth proportional to the stroke, on a main thread that owes the
// next frame. The tolerance is the caller's, and the editor passes half an
// OUTPUT pixel: a screen-pixel tolerance would keep detail the render cannot
// show, or drop detail it can, depending on the zoom.
export function simplify(points: readonly number[], tolerance: number): number[] {
    const count = points.length / 2;
    if (count <= 2) {
        return points.slice();
    }
    const keep = new Uint8Array(count);
    keep[0] = 1;
    keep[count - 1] = 1;
    const stack: number[] = [0, count - 1];
    const limit = tolerance * tolerance;
    while (stack.length > 0) {
        const last = stack.pop() ?? 0;
        const first = stack.pop() ?? 0;
        const ax = points[first * 2] ?? 0;
        const ay = points[first * 2 + 1] ?? 0;
        const dx = (points[last * 2] ?? 0) - ax;
        const dy = (points[last * 2 + 1] ?? 0) - ay;
        const lengthSq = dx * dx + dy * dy;
        let furthest = -1;
        let furthestSq = limit;
        for (let i = first + 1; i < last; i += 1) {
            const px = (points[i * 2] ?? 0) - ax;
            const py = (points[i * 2 + 1] ?? 0) - ay;
            let distanceSq: number;
            if (lengthSq === 0) {
                distanceSq = px * px + py * py;
            } else {
                const t = Math.min(1, Math.max(0, (px * dx + py * dy) / lengthSq));
                const ex = px - t * dx;
                const ey = py - t * dy;
                distanceSq = ex * ex + ey * ey;
            }
            if (distanceSq > furthestSq) {
                furthestSq = distanceSq;
                furthest = i;
            }
        }
        if (furthest >= 0) {
            keep[furthest] = 1;
            stack.push(first, furthest, furthest, last);
        }
    }
    const out: number[] = [];
    for (let i = 0; i < count; i += 1) {
        if (keep[i] === 1) {
            out.push(points[i * 2] ?? 0, points[i * 2 + 1] ?? 0);
        }
    }
    return out;
}

// A stroke's SVG path, in oriented pixels. A one-point stroke is a zero-length
// segment, which a round cap paints as the dot anvil's rasteriser draws for it.
export function strokePath(stroke: Stroke, oriented: { readonly widthPx: number; readonly heightPx: number }): string {
    let d = "";
    for (let i = 0; i + 1 < stroke.points.length; i += 2) {
        const x = fromFixed(stroke.points[i] ?? 0, oriented.widthPx);
        const y = fromFixed(stroke.points[i + 1] ?? 0, oriented.heightPx);
        d += `${i === 0 ? "M" : "L"}${round2(x)} ${round2(y)}`;
    }
    if (stroke.points.length === 2) {
        const x = fromFixed(stroke.points[0] ?? 0, oriented.widthPx);
        const y = fromFixed(stroke.points[1] ?? 0, oriented.heightPx);
        d += `L${round2(x)} ${round2(y)}`;
    }
    return d;
}

// Two decimals is a hundredth of an oriented pixel, below anything a render
// resolves, and it keeps a long path's `d` attribute from carrying seventeen
// digits per number.
function round2(value: number): string {
    return String(Math.round(value * 100) / 100);
}
