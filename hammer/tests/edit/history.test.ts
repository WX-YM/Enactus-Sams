// Undo and redo over whole recipes: bounded, shared, and a drag is one entry.

import { describe, expect, it } from "../support/test.js";

import { EditHistory, kHistoryEntries } from "../../src/edit/history.js";
import type { Recipe } from "../../src/edit/recipe.js";
import { kEmptyRecipe } from "../../src/edit/recipe.js";

function flipped(from: Recipe): Recipe {
    return { ...from, flip: !from.flip };
}

describe("EditHistory", () => {
    it("undoes and redoes, and a new change ends the redo", () => {
        const history = new EditHistory(kEmptyRecipe);
        const one = flipped(kEmptyRecipe);
        const two = { ...one, longEdgePx: 800 };
        history.push(one);
        history.push(two);
        expect(history.undo()).toBe(one);
        expect(history.undo()).toBe(kEmptyRecipe);
        expect(history.canUndo).toBe(false);
        expect(history.redo()).toBe(one);
        history.push(flipped(one));
        expect(history.canRedo).toBe(false);
    });

    it("does not spend an entry on a change that changed nothing", () => {
        const history = new EditHistory(kEmptyRecipe);
        history.push(kEmptyRecipe);
        expect(history.canUndo).toBe(false);
    });

    it("is bounded, and forgets the oldest", () => {
        const history = new EditHistory(kEmptyRecipe, 3);
        let current = kEmptyRecipe;
        for (let i = 1; i <= 5; i += 1) {
            current = { ...current, longEdgePx: i * 100 };
            history.push(current);
        }
        let undone = 0;
        while (history.canUndo) {
            history.undo();
            undone += 1;
        }
        expect(undone).toBe(3);
        expect(history.current.longEdgePx).toBe(200);
    });

    it("holds a hundred entries by default", () => {
        const history = new EditHistory(kEmptyRecipe);
        let current = kEmptyRecipe;
        for (let i = 1; i <= kHistoryEntries + 20; i += 1) {
            current = { ...current, longEdgePx: i };
            history.push(current);
        }
        let undone = 0;
        while (history.canUndo) {
            history.undo();
            undone += 1;
        }
        expect(undone).toBe(kHistoryEntries);
    });

    it("makes a drag one entry, whatever it replaced on the way", () => {
        const history = new EditHistory(kEmptyRecipe);
        for (let x = 1; x <= 10; x += 1) {
            history.replace({ ...kEmptyRecipe, crop: { x, y: 0, w: 1000, h: 1000 } });
        }
        history.push(history.current);
        expect(history.undo()).toBe(kEmptyRecipe);
        expect(history.canUndo).toBe(false);
    });

    it("abandons a drag that is undone before it ends", () => {
        const history = new EditHistory(kEmptyRecipe);
        const kept = flipped(kEmptyRecipe);
        history.push(kept);
        history.replace({ ...kept, longEdgePx: 500 });
        expect(history.undo()).toBe(kept);
        expect(history.undo()).toBe(kEmptyRecipe);
    });

    it("shares every stroke between entries rather than copying the drawing", () => {
        const stroke = { rgba: 0xff, width: 100, points: new Uint16Array(4096) };
        const history = new EditHistory({ ...kEmptyRecipe, strokes: [stroke] });
        history.push({ ...history.current, flip: true });
        history.undo();
        // The same array, by identity: a long session holds each point once.
        expect(history.current.strokes[0]?.points).toBe(stroke.points);
    });
});
