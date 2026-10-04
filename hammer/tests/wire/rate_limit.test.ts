// The local budget, driven by a clock the test owns.
//
// Real timers would make every one of these either slow or flaky, and flaky is
// worse (docs/16-test-plan.md). The clock is a parameter for that reason and for
// one other: it is the MONOTONIC clock, and a bucket driven by the wall clock
// refills by an hour when the operating system corrects the device's time.

import { describe, expect, it } from "../support/test.js";

import { RateLimiter } from "../../src/wire/rate_limit.js";

// Two buckets with deliberately different shapes: one that refills quickly and
// one whose window is long enough that refill is the interesting part.
const kTable = {
    fast: { windowMs: 1000, maxEvents: 4 },
    slow: { windowMs: 60_000, maxEvents: 2 },
} as const;

describe("spending the budget", () => {
    it("admits up to the declared count and then refuses", () => {
        const limiter = new RateLimiter(kTable);
        for (let i = 0; i < 4; i += 1) {
            expect(limiter.admit("fast", 0)).toMatchObject({ ok: true });
        }
        expect(limiter.admit("fast", 0)).toMatchObject({
            ok: false,
            error: { kind: "client", cause: "budget-spent" },
        });
    });

    it("names how long the caller would have to wait", () => {
        const limiter = new RateLimiter(kTable);
        for (let i = 0; i < 2; i += 1) {
            limiter.admit("slow", 0);
        }
        const refused = limiter.admit("slow", 0);
        expect(refused).toMatchObject({ ok: false, error: { retryAfterMs: 30_000 } });
    });

    // A bucket that refilled in whole events would admit nothing for twelve
    // minutes and then admit two at once.
    it("refills continuously rather than in steps", () => {
        const limiter = new RateLimiter(kTable);
        limiter.admit("slow", 0);
        limiter.admit("slow", 0);
        expect(limiter.admit("slow", 29_000)).toMatchObject({ ok: false });
        expect(limiter.admit("slow", 30_000)).toMatchObject({ ok: true });
    });

    it("never refills past the declared maximum", () => {
        const limiter = new RateLimiter(kTable);
        limiter.admit("fast", 0);
        // An hour later the bucket is full, not four hours' worth of full.
        for (let i = 0; i < 4; i += 1) {
            expect(limiter.admit("fast", 3_600_000)).toMatchObject({ ok: true });
        }
        expect(limiter.admit("fast", 3_600_000)).toMatchObject({ ok: false });
    });
});

describe("what has no budget here", () => {
    it("admits a route that names no bucket", () => {
        const limiter = new RateLimiter(kTable);
        for (let i = 0; i < 100; i += 1) {
            expect(limiter.admit(null, 0)).toMatchObject({ ok: true });
        }
    });

    // A bound this client was not given is the server's to enforce, and always
    // was. Inventing one here would be the library deciding a policy nobody
    // handed it.
    it("admits a bucket the descriptor does not carry", () => {
        const limiter = new RateLimiter(kTable);
        for (let i = 0; i < 100; i += 1) {
            expect(limiter.admit("added-this-morning", 0)).toMatchObject({ ok: true });
        }
    });
});

describe("a 429", () => {
    it("holds back the whole bucket for exactly the duration the server named", () => {
        const limiter = new RateLimiter(kTable);
        limiter.holdBack("fast", 5000, 0);

        expect(limiter.admit("fast", 4999)).toMatchObject({
            ok: false,
            error: { cause: "budget-spent", retryAfterMs: 1 },
        });
        expect(limiter.admit("fast", 5000)).toMatchObject({ ok: true });
    });

    // Resuming with a full bucket the moment the hold expires is the burst that
    // earned the refusal in the first place.
    it("empties the bucket as well as holding it", () => {
        const limiter = new RateLimiter(kTable);
        limiter.holdBack("fast", 1000, 0);
        expect(limiter.admit("fast", 1000)).toMatchObject({ ok: true });
        expect(limiter.admit("fast", 1000)).toMatchObject({ ok: false });
    });

    // The other direction of the same correction. The server named one second
    // against a bucket whose window is a minute; a client that then refused for
    // half a minute would have overridden the only participant entitled to name
    // that number.
    it("has exactly one event available when a short hold lifts", () => {
        const limiter = new RateLimiter(kTable);
        limiter.holdBack("slow", 1000, 0);
        expect(limiter.admit("slow", 999)).toMatchObject({ ok: false });
        expect(limiter.admit("slow", 1000)).toMatchObject({ ok: true });
        expect(limiter.admit("slow", 1000)).toMatchObject({ ok: false });
    });

    it("never shortens a hold that is already longer", () => {
        const limiter = new RateLimiter(kTable);
        limiter.holdBack("fast", 10_000, 0);
        limiter.holdBack("fast", 1000, 0);
        expect(limiter.admit("fast", 5000)).toMatchObject({ ok: false });
    });

    it("ignores a hold for a bucket that is not declared", () => {
        const limiter = new RateLimiter(kTable);
        limiter.holdBack("added-this-morning", 10_000, 0);
        expect(limiter.admit("added-this-morning", 0)).toMatchObject({ ok: true });
    });
});
