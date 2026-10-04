// The recipe codec against anvil's golden vectors.
//
// `recipe_vectors.json` is printed by anvil's `testapp_emit_edit_vectors`:
//
//     ~/Code/anvil/build/release/tests/testapp_emit_edit_vectors > tests/edit/recipe_vectors.json
//
// The vectors themselves were produced by a third implementation written from
// anvil `docs/21-image-edits.md`, not from either codec, and anvil's suite asserts
// the same ones. `decoded` is what anvil's decoder read from each accepted
// vector, so this decoder is checked field by field and not only by whether it
// round-trips.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "../support/test.js";

import type { EditLimits, Recipe, RecipeFault, Stroke } from "../../src/edit/recipe.js";
import { decodeRecipe, encodeRecipe, kEmptyRecipe, kFaultWire, kMaxStrokeWidth, planEdit, roundDiv } from "../../src/edit/recipe.js";

type Vector = {
    readonly name: string;
    readonly recipe: string;
    readonly source: readonly [number, number];
    readonly fault: RecipeFault | null;
    readonly out: readonly [number, number] | null;
    readonly decoded: {
        readonly turns: number;
        readonly flip: boolean;
        readonly crop: readonly [number, number, number, number] | null;
        readonly long_edge_px: number | null;
        readonly strokes: readonly { readonly rgba: readonly number[]; readonly width: number; readonly points: readonly number[] }[];
    } | null;
};

type VectorFile = {
    readonly limits: { readonly max_strokes: number; readonly max_points: number; readonly max_edge_px: number; readonly min_edge_px: number };
    readonly vectors: readonly Vector[];
};

const kFile = JSON.parse(
    readFileSync(fileURLToPath(new URL("./recipe_vectors.json", import.meta.url)), "utf8"),
) as VectorFile;

const kLimits: EditLimits = {
    maxStrokes: kFile.limits.max_strokes,
    maxPoints: kFile.limits.max_points,
    maxEdgePx: kFile.limits.max_edge_px,
    minEdgePx: kFile.limits.min_edge_px,
};

function sourceOf(vector: Vector) {
    return { widthPx: vector.source[0], heightPx: vector.source[1] };
}

function rgbaOf(parts: readonly number[]): number {
    return (((parts[0] ?? 0) << 24) | ((parts[1] ?? 0) << 16) | ((parts[2] ?? 0) << 8) | (parts[3] ?? 0)) >>> 0;
}

describe("the golden vectors", () => {
    it("are all here, and between them name every fault", () => {
        expect(kFile.vectors.length).toBeGreaterThan(20);
        const seen = new Set(kFile.vectors.map((vector) => vector.fault));
        for (const fault of Object.keys(kFaultWire)) {
            expect(seen.has(fault as RecipeFault)).toBe(true);
        }
    });

    for (const vector of kFile.vectors) {
        it(vector.name, () => {
            const decoded = decodeRecipe(vector.recipe, kLimits);
            if (!decoded.ok) {
                // Refused by the codec: anvil refused it at the same stage.
                expect(vector.decoded).toBeNull();
                expect(decoded.error.fault).toBe(vector.fault);
                return;
            }

            // Read the same fields anvil read.
            const expected = vector.decoded;
            expect(expected).not.toBeNull();
            if (expected === null) return;
            const recipe: Recipe = decoded.value;
            expect(recipe.turns).toBe(expected.turns);
            expect(recipe.flip).toBe(expected.flip);
            expect(recipe.crop === null ? null : [recipe.crop.x, recipe.crop.y, recipe.crop.w, recipe.crop.h]).toEqual(
                expected.crop === null ? null : [...expected.crop],
            );
            expect(recipe.longEdgePx).toBe(expected.long_edge_px);
            expect(recipe.strokes.length).toBe(expected.strokes.length);
            recipe.strokes.forEach((stroke, i) => {
                const want = expected.strokes[i];
                expect(stroke.rgba).toBe(rgbaOf(want?.rgba ?? []));
                expect(stroke.width).toBe(want?.width);
                expect(Array.from(stroke.points)).toEqual([...(want?.points ?? [])]);
            });

            // Planned to the same outcome: the same fault, or the same pixels.
            const plan = planEdit(recipe, sourceOf(vector), kLimits);
            if (vector.fault !== null) {
                expect(plan.ok).toBe(false);
                if (!plan.ok) expect(plan.error.fault).toBe(vector.fault);
                return;
            }
            expect(plan.ok).toBe(true);
            if (!plan.ok) return;
            expect([plan.value.outWidthPx, plan.value.outHeightPx]).toEqual([...(vector.out ?? [])]);

            // And encoded back to the identical text: one edit, one spelling.
            const encoded = encodeRecipe(recipe, kLimits, sourceOf(vector));
            expect(encoded.ok).toBe(true);
            if (encoded.ok) expect(encoded.value.text).toBe(vector.recipe);
        });
    }
});

describe("encodeRecipe", () => {
    const source = { widthPx: 2000, heightPx: 1000 };
    const dot: Stroke = { rgba: 0xff0000ff, width: 655, points: Uint16Array.of(32768, 32768) };

    it("sends a crop dragged out to the whole frame as no crop", () => {
        const whole = encodeRecipe(
            { ...kEmptyRecipe, flip: true, crop: { x: 0, y: 0, w: 65535, h: 65535 } },
            kLimits,
            source,
        );
        const none = encodeRecipe({ ...kEmptyRecipe, flip: true }, kLimits, source);
        expect(whole.ok && none.ok).toBe(true);
        if (whole.ok && none.ok) expect(whole.value.text).toBe(none.value.text);
    });

    it("refuses what cannot be sent, with the server's fault", () => {
        const faultOf = (recipe: Recipe) => {
            const encoded = encodeRecipe(recipe, kLimits, source);
            return encoded.ok ? null : encoded.error.fault;
        };
        expect(faultOf(kEmptyRecipe)).toBe("edit.empty");
        expect(faultOf({ ...kEmptyRecipe, crop: { x: 0.5, y: 0, w: 100, h: 100 } })).toBe("edit.crop");
        expect(faultOf({ ...kEmptyRecipe, strokes: [{ ...dot, rgba: 0xff000000 }] })).toBe("edit.stroke");
        expect(faultOf({ ...kEmptyRecipe, strokes: [{ ...dot, width: 9000 }] })).toBe("edit.stroke");
        expect(faultOf({ ...kEmptyRecipe, strokes: Array.from({ length: 65 }, () => dot) })).toBe("edit.bounds");
        expect(faultOf({ ...kEmptyRecipe, longEdgePx: 2000 })).toBe("edit.upscale");
        expect(faultOf({ ...kEmptyRecipe, crop: { x: 0, y: 0, w: 6553, h: 65535 } })).toBe("edit.too_small");
    });

    it("takes a stroke up to an eighth of the short edge wide, and no wider", () => {
        const at = (width: number) => encodeRecipe({ ...kEmptyRecipe, strokes: [{ ...dot, width }] }, kLimits, source).ok;
        expect(at(kMaxStrokeWidth)).toBe(true);
        expect(at(kMaxStrokeWidth + 1)).toBe(false);
    });

    it("never produces a float on the wire", () => {
        const encoded = encodeRecipe({ ...kEmptyRecipe, crop: { x: 100.25, y: 0, w: 1000, h: 1000 } }, kLimits, source);
        expect(encoded.ok).toBe(false);
    });
});

describe("the arithmetic", () => {
    it("rounds half up, as the server does", () => {
        expect(roundDiv(1, 2)).toBe(1);
        expect(roundDiv(3, 2)).toBe(2);
        expect(roundDiv(5, 4)).toBe(1);
        // The largest product the contract makes stays exact.
        expect(roundDiv(65535 * 12000, 65535)).toBe(12000);
    });
});
