// The arithmetic a charting dependency would have been bought for.
//
// Every case here is one somebody hits in the first month of having a dashboard:
// a metric that has not moved, a series with no data yet, and an axis whose
// labels have to be readable rather than merely correct.

import { describe, expect, it } from "../support/test.js";

import { extent, linear, ticks } from "../../src/chart/scale.js";

describe("a linear scale", () => {
    it("maps a domain onto a range", () => {
        const scale = linear({ from: 0, to: 10 }, { from: 0, to: 100 });
        expect(scale(0)).toBe(0);
        expect(scale(5)).toBe(50);
        expect(scale(10)).toBe(100);
    });

    it("maps an inverted range, which is what a screen's y axis is", () => {
        const scale = linear({ from: 0, to: 10 }, { from: 100, to: 0 });
        expect(scale(0)).toBe(100);
        expect(scale(10)).toBe(0);
    });

    // A flat series is an ordinary thing for a metric to be. Dividing by the
    // span would give `NaN` for every coordinate, which renders as nothing at
    // all with no error anywhere.
    it("puts a flat series in the middle rather than dividing by zero", () => {
        const scale = linear({ from: 7, to: 7 }, { from: 0, to: 100 });
        expect(scale(7)).toBe(50);
        expect(Number.isNaN(scale(7))).toBe(false);
    });
});

describe("an extent", () => {
    it("is the interval the values span", () => {
        expect(extent([3, 1, 4, 1, 5])).toEqual({ from: 1, to: 5 });
    });

    // A series with no data yet is the state every chart starts in.
    it("is null for nothing at all", () => {
        expect(extent([])).toBeNull();
    });

    // One bad sample must not take the whole axis with it.
    it("ignores a value that is not finite", () => {
        expect(extent([1, Number.NaN, 5, Number.POSITIVE_INFINITY])).toEqual({ from: 1, to: 5 });
    });

    it("is null when nothing in it was finite", () => {
        expect(extent([Number.NaN])).toBeNull();
    });
});

describe("ticks", () => {
    // A step of 3.7 divides the axis correctly and unreadably. These are the
    // intervals people read without doing arithmetic.
    it("chooses round numbers", () => {
        expect(ticks({ from: 0, to: 10 }, 5)).toEqual([0, 2, 4, 6, 8, 10]);
        expect(ticks({ from: 0, to: 100 }, 4)).toEqual([0, 20, 40, 60, 80, 100]);
    });

    // 0.1 is not representable in binary, so adding it ten times produces
    // 0.30000000000000004 — a correct float and an axis label somebody has to
    // read.
    it("does not accumulate float error along the axis", () => {
        expect(ticks({ from: 0, to: 1 }, 10)).toEqual([
            0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1,
        ]);
    });

    // The bare thresholds bias every decision upward and produce an axis with
    // three labels where somebody asked for five.
    it("lands near the count it was asked for", () => {
        for (const [from, to, count] of [[0, 100, 4], [0, 10, 5], [0, 1, 10]] as const) {
            const got = ticks({ from, to }, count);
            expect(got.length).toBeGreaterThanOrEqual(count);
            expect(got.length).toBeLessThanOrEqual(count * 2 + 1);
        }
    });

    it("gives one tick for a domain with no span", () => {
        expect(ticks({ from: 5, to: 5 }, 4)).toEqual([5]);
    });

    it("gives none for a count below one, rather than looping", () => {
        expect(ticks({ from: 0, to: 10 }, 0)).toEqual([]);
    });

    it("gives none for a domain that is not finite", () => {
        expect(ticks({ from: 0, to: Number.POSITIVE_INFINITY }, 4)).toEqual([]);
    });

    it("reads an interval given the other way round", () => {
        expect(ticks({ from: 10, to: 0 }, 5)).toEqual([0, 2, 4, 6, 8, 10]);
    });
});
