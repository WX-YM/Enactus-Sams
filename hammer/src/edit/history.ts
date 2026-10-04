// Undo and redo over whole recipes.
//
// Every entry is an immutable recipe, and successive recipes SHARE every stroke
// they have in common — a new stroke is one new object and one new array of
// references, never a copy of the drawing — so a long session holds each point
// once however many entries mention it.
//
// Bounded, and the oldest entry is what goes (`CLAUDE.md` §2.3): an editor
// left open for an afternoon is a tab that stays open for days.

import type { Recipe } from "./recipe.js";

export const kHistoryEntries = 100;

export class EditHistory {
    private past: Recipe[] = [];
    private future: Recipe[] = [];
    private present: Recipe;
    // Where a drag started, while one is in progress: the state its one entry
    // will undo back to.
    private dragFrom: Recipe | null = null;
    private readonly capacity: number;

    constructor(initial: Recipe, capacity: number = kHistoryEntries) {
        this.present = initial;
        this.capacity = Math.max(1, capacity);
    }

    get current(): Recipe {
        return this.present;
    }

    get canUndo(): boolean {
        return this.past.length > 0;
    }

    get canRedo(): boolean {
        return this.future.length > 0;
    }

    // A new state, which ends whatever could have been redone. Pushing the
    // recipe already current is not an entry: a click that changed nothing
    // should not cost an undo.
    push(next: Recipe): void {
        const from = this.dragFrom ?? this.present;
        this.dragFrom = null;
        if (next === from) {
            this.present = next;
            return;
        }
        this.past.push(from);
        if (this.past.length > this.capacity) {
            this.past.shift();
        }
        this.present = next;
        this.future = [];
    }

    // The present, replaced without an entry: the frames of a drag, which become
    // one entry when the drag ends and `push` is called with the result.
    replace(next: Recipe): void {
        if (this.dragFrom === null) {
            this.dragFrom = this.present;
        }
        this.present = next;
    }

    undo(): Recipe {
        // An undo in the middle of a drag abandons the drag.
        if (this.dragFrom !== null) {
            this.present = this.dragFrom;
            this.dragFrom = null;
            return this.present;
        }
        const previous = this.past.pop();
        if (previous !== undefined) {
            this.future.push(this.present);
            this.present = previous;
        }
        return this.present;
    }

    redo(): Recipe {
        if (this.dragFrom !== null) {
            return this.present;
        }
        const next = this.future.pop();
        if (next !== undefined) {
            this.past.push(this.present);
            this.present = next;
        }
        return this.present;
    }
}
