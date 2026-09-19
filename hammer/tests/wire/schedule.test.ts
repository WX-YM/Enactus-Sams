// Waiting, and the two things about it that matter.
//
// Real timers here rather than a fake clock, because what is being asserted is
// the abort plumbing rather than a duration: every delay is a handful of
// milliseconds, and the case that would be slow is the one that never resolves.

import { describe, expect, it } from "vitest";

import { sleep } from "../../src/wire/schedule.js";

describe("a wait", () => {
    it("resolves after the delay", async () => {
        const started = performance.now();
        await sleep(10, new AbortController().signal);
        expect(performance.now() - started).toBeGreaterThanOrEqual(8);
    });

    it("resolves immediately for a delay that has already passed", async () => {
        await sleep(0, new AbortController().signal);
        await sleep(-1, new AbortController().signal);
    });

    it("resolves immediately on a signal that is already aborted", async () => {
        const controller = new AbortController();
        controller.abort();
        const started = performance.now();
        await sleep(10_000, controller.signal);
        expect(performance.now() - started).toBeLessThan(50);
    });

    // Resolving rather than rejecting: an abort is an expected condition on
    // every screen that unmounts, and an exception thrown for an expected
    // condition is a catch somebody forgets to write.
    it("resolves rather than rejecting when the signal fires mid-wait", async () => {
        const controller = new AbortController();
        const waiting = sleep(10_000, controller.signal);
        controller.abort();
        await expect(waiting).resolves.toBeUndefined();
    });

    // A listener left attached holds its closure — and whatever that closure
    // captured — for the life of the signal, which on a long-lived screen is the
    // life of the tab. The signal is watched through a proxy rather than a
    // stand-in so that the wait is driven by a real AbortSignal.
    it("detaches its listener once it has resolved", async () => {
        const controller = new AbortController();
        let removals = 0;
        const watched = new Proxy(controller.signal, {
            get(target, property, receiver) {
                if (property === "removeEventListener") {
                    return (...args: Parameters<AbortSignal["removeEventListener"]>) => {
                        removals += 1;
                        target.removeEventListener(...args);
                    };
                }
                const value = Reflect.get(target, property, receiver);
                return typeof value === "function" ? value.bind(target) : value;
            },
        });

        await sleep(1, watched);
        expect(removals).toBe(1);
    });
});
