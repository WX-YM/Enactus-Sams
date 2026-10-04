// The observable everything in this layer is built on.
//
// What is asserted here is the set of things a hand-rolled observable gets wrong
// on the first attempt: an unsubscribe that fires twice, a listener that
// unsubscribes another from inside its own callback, and one subscriber throwing
// where the other four still have to run.

import { describe, expect, it, fn } from "../support/test.js";

import { Store } from "../../src/state/store.js";

describe("Store", () => {
    it("hands the new value to every subscriber, synchronously", () => {
        const store = new Store(0);
        const seen: number[] = [];
        store.subscribe((value) => seen.push(value));
        store.subscribe((value) => seen.push(value * 10));

        store.set(1);

        // Synchronous, and asserted as such: a subscriber told about a change
        // after the fact reads a value that may already have moved on, which is a
        // render of a state that never existed.
        expect(seen).toEqual([1, 10]);
        expect(store.get()).toBe(1);
    });

    it("does not notify when the value did not change", () => {
        const store = new Store("a");
        const listener = fn();
        store.subscribe(listener);

        store.set("a");
        expect(listener).not.toHaveBeenCalled();

        store.set("b");
        expect(listener).toHaveBeenCalledTimes(1);
    });

    it("compares with Object.is, so NaN is not a change", () => {
        const store = new Store(Number.NaN);
        const listener = fn();
        store.subscribe(listener);

        store.set(Number.NaN);

        expect(listener).not.toHaveBeenCalled();
    });

    it("unsubscribes exactly once, so a second call does not drop a live listener", () => {
        const store = new Store(0);
        const listener = fn();

        const off = store.subscribe(listener);
        off();
        // The same function subscribed again is one `Set` entry, so a stale
        // unsubscribe calling `delete` a second time would remove the new
        // subscription.
        store.subscribe(listener);
        off();

        store.set(1);
        expect(listener).toHaveBeenCalledTimes(1);
    });

    it("does not call a listener another listener removed during the same notify", () => {
        const store = new Store(0);
        const second = fn();
        let off = (): void => {};

        store.subscribe(() => {
            off();
        });
        off = store.subscribe(second);

        store.set(1);

        expect(second).not.toHaveBeenCalled();
    });

    it("does not call a listener subscribed during the same notify", () => {
        const store = new Store(0);
        const late = fn();

        store.subscribe(() => {
            store.subscribe(late);
        });

        store.set(1);

        // It hears about the NEXT change, not the one that was in progress: a
        // listener attached halfway through a notify would otherwise see a value
        // it never asked about, and whether it did would depend on insertion
        // order.
        expect(late).not.toHaveBeenCalled();
        store.set(2);
        expect(late).toHaveBeenCalledTimes(1);
    });

    it("keeps notifying after a subscriber throws, and surfaces the throw", async () => {
        const store = new Store(0);
        const after = fn();

        store.subscribe(() => {
            throw new Error("thrown by a subscriber");
        });
        store.subscribe(after);

        // The throw is rethrown from a microtask rather than swallowed, so it
        // reaches the platform's unhandled-error path where an application's
        // reporter can see it. Caught here so that surfacing it does not take the
        // suite down with it — which is the same reason the store does not let it
        // unwind the notify loop.
        const previous = process.listeners("uncaughtException");
        process.removeAllListeners("uncaughtException");
        const escaped: unknown[] = [];
        process.on("uncaughtException", (thrown) => escaped.push(thrown));
        try {
            store.set(1);

            // The subscriber after the one that threw still ran.
            expect(after).toHaveBeenCalledWith(1);

            await new Promise<void>((resolve) => setTimeout(resolve, 0));
            expect(escaped).toHaveLength(1);
        } finally {
            process.removeAllListeners("uncaughtException");
            for (const listener of previous) {
                process.on("uncaughtException", listener);
            }
        }
    });

    it("update derives the next value from the current one", () => {
        const store = new Store({ count: 1 });
        store.update((current) => ({ count: current.count + 1 }));
        expect(store.get()).toEqual({ count: 2 });
    });

    it("close drops every listener", () => {
        const store = new Store(0);
        const listener = fn();
        store.subscribe(listener);

        store.close();
        store.set(1);

        expect(listener).not.toHaveBeenCalled();
    });
});
