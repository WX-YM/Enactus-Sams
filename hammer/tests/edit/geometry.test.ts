// Where things are in an image edit, and that turning the picture carries what
// was drawn on it.

import { describe, expect, it } from "../support/test.js";

import {
    cropBetween,
    dragCorner,
    fromFixed,
    holdsAspect,
    kCropMinimum,
    largestCrop,
    lockCrop,
    mirror,
    nudgeCrop,
    orientTransform,
    rotate,
    simplify,
    strokePath,
    toFixed,
} from "../../src/edit/geometry.js";
import type { QuarterTurns, Recipe } from "../../src/edit/recipe.js";
import { kEmptyRecipe, kFixedOne, orientedSize } from "../../src/edit/recipe.js";

const kSource = { widthPx: 400, heightPx: 300 };

// Applies an SVG matrix string to a point.
function apply(matrix: string, x: number, y: number): [number, number] {
    const [a, b, c, d, e, f] = (matrix.match(/-?\d+(\.\d+)?/g) ?? []).map(Number) as [number, number, number, number, number, number];
    return [a * x + c * y + e, b * x + d * y + f];
}

describe("orientTransform", () => {
    it("turns clockwise, as vips_rot does: the top-left corner goes to the top right", () => {
        expect(apply(orientTransform(kSource, 1, false), 0, 0)).toEqual([300, 0]);
        expect(apply(orientTransform(kSource, 2, false), 0, 0)).toEqual([400, 300]);
        expect(apply(orientTransform(kSource, 3, false), 0, 0)).toEqual([0, 400]);
    });

    it("flips after turning, so the source always lands inside the oriented frame", () => {
        for (const turns of [0, 1, 2, 3] as const) {
            for (const flip of [false, true]) {
                const frame = orientedSize(kSource, turns);
                const matrix = orientTransform(kSource, turns, flip);
                for (const [x, y] of [[0, 0], [400, 0], [0, 300], [400, 300]] as const) {
                    const [ox, oy] = apply(matrix, x, y);
                    expect(ox >= 0 && ox <= frame.widthPx && oy >= 0 && oy <= frame.heightPx).toBe(true);
                }
            }
        }
    });
});

describe("rotate and mirror", () => {
    const drawn: Recipe = {
        ...kEmptyRecipe,
        crop: { x: 1000, y: 2000, w: 30000, h: 20000 },
        strokes: [{ rgba: 0xff0000ff, width: 500, points: Uint16Array.of(100, 200, 60000, 5000) }],
    };

    it("undo each other exactly, on the integers the recipe stores", () => {
        expect(rotate(rotate(drawn, true), false)).toEqual(drawn);
        expect(mirror(mirror(drawn))).toEqual(drawn);
        let spun = drawn;
        for (let i = 0; i < 4; i += 1) spun = rotate(spun, true);
        expect(spun).toEqual(drawn);
    });

    it("keep a stroke on the same spot of the SOURCE, through every orientation", () => {
        // The point a stroke sits on, pulled back to source pixels through the
        // recipe's own transform, must not move when the picture is turned or
        // mirrored — that is the whole claim, stated in the frame nothing moves.
        const sourcePoint = (recipe: Recipe): [number, number] => {
            const frame = orientedSize(kSource, recipe.turns);
            const stroke = recipe.strokes[0];
            const x = ((stroke?.points[0] ?? 0) * frame.widthPx) / kFixedOne;
            const y = ((stroke?.points[1] ?? 0) * frame.heightPx) / kFixedOne;
            // Invert the orientation by searching the four corners' images is
            // overkill; invert the 2×2 instead.
            const [a, b, c, d, e, f] = (orientTransform(kSource, recipe.turns, recipe.flip).match(/-?\d+(\.\d+)?/g) ?? []).map(Number) as [number, number, number, number, number, number];
            const det = a * d - b * c;
            const px = x - e;
            const py = y - f;
            return [Math.round((d * px - c * py) / det), Math.round((-b * px + a * py) / det)];
        };
        const home = sourcePoint(drawn);
        let current = drawn;
        for (const step of ["cw", "flip", "cw", "ccw", "flip", "cw", "cw"] as const) {
            current = step === "flip" ? mirror(current) : rotate(current, step === "cw");
            expect(sourcePoint(current)).toEqual(home);
        }
    });

    it("rotates a mirrored picture the way it looks, not the way the source is stored", () => {
        const mirrored = mirror(kEmptyRecipe);
        // Clockwise on screen is one turn FEWER of the source once it is flipped.
        expect(rotate(mirrored, true).turns).toBe(3 as QuarterTurns);
        expect(rotate(kEmptyRecipe, true).turns).toBe(1 as QuarterTurns);
    });
});

describe("the crop", () => {
    it("moves as a box without leaving the frame", () => {
        const crop = { x: 60000, y: 0, w: 5000, h: 5000 };
        expect(nudgeCrop(crop, "move", 10000, -100)).toEqual({ x: kFixedOne - 5000, y: 0, w: 5000, h: 5000 });
    });

    it("resizes by an edge and never folds to nothing", () => {
        const crop = { x: 1000, y: 1000, w: 2000, h: 2000 };
        const shrunk = nudgeCrop(crop, "e", -100000, 0);
        expect(shrunk.w).toBe(kCropMinimum);
        const grown = nudgeCrop(crop, "nw", -5000, -5000);
        expect(grown).toEqual({ x: 0, y: 0, w: 3000, h: 3000 });
    });

    it("starts from the whole frame when there is no crop yet", () => {
        expect(nudgeCrop(null, "s", 0, -655)).toEqual({ x: 0, y: 0, w: kFixedOne, h: kFixedOne - 655 });
    });

    it("is drawn between two corners in either order", () => {
        expect(cropBetween(5000, 6000, 1000, 2000)).toEqual({ x: 1000, y: 2000, w: 4000, h: 4000 });
    });
});

describe("simplify", () => {
    it("drops points on a straight line and keeps both ends", () => {
        const line = [0, 0, 1, 1, 2, 2, 3, 3, 4, 4];
        expect(simplify(line, 0.5)).toEqual([0, 0, 4, 4]);
    });

    it("keeps a corner", () => {
        const corner = [0, 0, 5, 0, 10, 0, 10, 5, 10, 10];
        expect(simplify(corner, 0.5)).toEqual([0, 0, 10, 0, 10, 10]);
    });

    it("keeps detail larger than the tolerance and drops detail smaller", () => {
        const wobble = [0, 0, 5, 0.3, 10, 0];
        expect(simplify(wobble, 0.5)).toEqual([0, 0, 10, 0]);
        expect(simplify(wobble, 0.1)).toEqual(wobble);
    });
});

describe("paths and fractions", () => {
    it("draws a one-point stroke as a zero-length segment, which a round cap makes a dot", () => {
        const d = strokePath({ rgba: 0xff, width: 100, points: Uint16Array.of(32768, 32768) }, { widthPx: 100, heightPx: 100 });
        expect(d).toBe("M50 50L50 50");
    });

    it("rounds a position to the u16 the wire carries and clamps it to the frame", () => {
        expect(toFixed(50, 100)).toBe(32768);
        expect(toFixed(-5, 100)).toBe(0);
        expect(toFixed(150, 100)).toBe(kFixedOne);
    });
});

describe("a crop of a fixed shape", () => {
    const frame = { widthPx: 1600, heightPx: 1200 };
    const wide = { num: 16, den: 9 };
    const px = (crop: { x: number; y: number; w: number; h: number }) => ({
        left: fromFixed(crop.x, frame.widthPx),
        top: fromFixed(crop.y, frame.heightPx),
        width: fromFixed(crop.w, frame.widthPx),
        height: fromFixed(crop.h, frame.heightPx),
    });

    it("opens on the largest centred box of the shape", () => {
        const crop = largestCrop(frame, wide);
        if (crop === null) throw new Error("a 4:3 frame is not 16:9");
        const box = px(crop);
        expect(Math.round(box.width)).toBe(1600);
        expect(Math.round(box.height)).toBe(900);
        expect(Math.round(box.top)).toBe(150);
        expect(holdsAspect(crop, frame, wide)).toBe(true);
    });

    it("is no crop at all when the frame already has the shape", () => {
        expect(largestCrop(frame, { num: 4, den: 3 })).toBeNull();
        expect(holdsAspect(null, frame, { num: 4, den: 3 })).toBe(true);
        expect(holdsAspect(null, frame, wide)).toBe(false);
    });

    it("keeps the shape and the opposite corner through every handle, and never leaves the frame", () => {
        const from = largestCrop(frame, { num: 1, den: 1 });
        if (from === null) throw new Error("a 4:3 frame is not square");
        for (const handle of ["n", "s", "e", "w", "ne", "nw", "se", "sw"] as const) {
            for (const [dx, dy] of [[-9000, -4000], [3000, 7000], [-60000, 60000], [20000, -1000]] as const) {
                const locked = lockCrop(nudgeCrop(from, handle, dx, dy), handle, frame, { num: 1, den: 1 });
                const box = px(locked);
                expect(Math.abs(box.width - box.height)).toBeLessThanOrEqual(1);
                expect(locked.x + locked.w).toBeLessThanOrEqual(kFixedOne);
                expect(locked.y + locked.h).toBeLessThanOrEqual(kFixedOne);
                expect(locked.w).toBeGreaterThanOrEqual(kCropMinimum - 1);
                const before = px(from);
                if (handle === "se") {
                    expect(Math.abs(box.left - before.left)).toBeLessThanOrEqual(1);
                    expect(Math.abs(box.top - before.top)).toBeLessThanOrEqual(1);
                }
                if (handle === "nw") {
                    expect(Math.abs(box.left + box.width - (before.left + before.width))).toBeLessThanOrEqual(1);
                    expect(Math.abs(box.top + box.height - (before.top + before.height))).toBeLessThanOrEqual(1);
                }
            }
        }
    });

    it("grows an edge about the box's centre, so the right edge does not drag the top", () => {
        const from = { x: 16384, y: 16384, w: 16384, h: 21845 };
        const locked = lockCrop(nudgeCrop(from, "e", 8000, 0), "e", frame, { num: 1, den: 1 });
        const before = px(from);
        const after = px(locked);
        expect(Math.abs(after.left - before.left)).toBeLessThanOrEqual(1);
        expect(Math.abs(after.top + after.height / 2 - (before.top + before.height / 2))).toBeLessThanOrEqual(1);
    });

    it("leaves a moved box as it is", () => {
        const from = { x: 1000, y: 2000, w: 30000, h: 20000 };
        expect(lockCrop(from, "move", frame, wide)).toBe(from);
    });

    it("names the corner a fresh drag is pulling", () => {
        expect(dragCorner(100, 100, 50, 150)).toBe("sw");
        expect(dragCorner(100, 100, 150, 50)).toBe("ne");
    });
});
