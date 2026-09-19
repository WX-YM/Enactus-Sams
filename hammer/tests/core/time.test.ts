// The assertions that matter here are the ones that do not run: a `ServerInstant`
// and `Date.now()` must not be combinable, and the only way to keep that true is
// a test that fails the build when it becomes possible.

import { describe, expect, it } from "vitest";

import { Countdown, ServerInstant, kHourMs, kMinuteMs, kSecondMs } from "../../src/core/time.js";

function instant(epochMs: number): ServerInstant {
    const made = ServerInstant.fromServerEpochMs(epochMs);
    if (!made.ok) {
        throw new Error("expected an instant");
    }
    return made.value;
}

describe("the durations this module publishes", () => {
    // Milliseconds, and the name says so. Asserted because they are published
    // for an application to build its own durations out of, and a wrong
    // constant here is a wrong timeout in every consumer at once.
    it("is each the number of milliseconds its name claims", () => {
        expect(kSecondMs).toBe(1000);
        expect(kMinuteMs).toBe(60_000);
        expect(kHourMs).toBe(3_600_000);
    });

    // There is no day, on purpose: a day is a calendar unit rather than a
    // duration, and 86,400,000 ms is wrong twice a year in every zone that
    // changes. Asserted as an absence, because an absence with a reason is the
    // kind somebody helpfully fills in.
    it("offers no day, which is a calendar unit rather than a duration", async () => {
        const time: Record<string, unknown> = await import("../../src/core/time.js");
        expect(Object.keys(time).filter((name) => /day/i.test(name))).toEqual([]);
    });
});

describe("a server instant is not a number", () => {
    it("cannot be subtracted from the device clock", () => {
        const expiresAt = instant(1_700_000_000_000);
        // @ts-expect-error the device clock is user-settable and routinely
        // minutes out, so this subtraction is a number that is wrong by an
        // amount nothing on the device can measure.
        const wrong = expiresAt - Date.now();
        expect(Number.isNaN(wrong)).toBe(true);
    });

    it("has no accessor that hands the number back", () => {
        const expiresAt = instant(1_700_000_000_000);
        // @ts-expect-error a public epochMs would put the value back within
        // reach of Date.now(), which is the whole of what this type prevents.
        const escaped = expiresAt.epochMs;
        expect(escaped).toBe(1_700_000_000_000);
    });

    it("is not assignable to a number", () => {
        // @ts-expect-error assigning it to a number is the same defect one step
        // removed: everything downstream would then be arithmetic.
        const asNumber: number = instant(1);
        expect(typeof asNumber).toBe("object");
    });
});

describe("comparing two server instants", () => {
    const earlier = instant(1_700_000_000_000);
    const later = instant(1_700_000_060_000);

    it("orders them, because they came off the same clock", () => {
        expect(earlier.isBefore(later)).toBe(true);
        expect(later.isAfter(earlier)).toBe(true);
        expect(earlier.compare(later)).toBe(-1);
        expect(later.compare(earlier)).toBe(1);
        expect(earlier.compare(instant(1_700_000_000_000))).toBe(0);
        expect(earlier.equals(instant(1_700_000_000_000))).toBe(true);
    });

    it("measures a duration the server's clock measured on both ends", () => {
        expect(later.sinceMs(earlier)).toBe(kMinuteMs);
    });

    it("parses what the server sent", () => {
        const parsed = ServerInstant.fromServerIso("2023-11-14T22:13:20.000Z");
        expect(parsed.ok).toBe(true);
        if (!parsed.ok) return;
        expect(parsed.value.equals(earlier)).toBe(true);
    });

    it("refuses a value that is not an instant rather than producing NaN", () => {
        // A NaN instant compares false against everything, including itself, so
        // an expiry check silently passes.
        expect(ServerInstant.fromServerIso("tomorrow")).toMatchObject({
            ok: false,
            error: "not-an-instant",
        });
        expect(ServerInstant.fromServerEpochMs(Number.NaN)).toMatchObject({
            ok: false,
            error: "not-an-instant",
        });
    });

    it("renders through a Date at the edge", () => {
        expect(earlier.toDate().toISOString()).toBe("2023-11-14T22:13:20.000Z");
    });
});

describe("a countdown runs on a monotonic clock", () => {
    it("counts down the duration the server sent", () => {
        // The clock readings are supplied, not read: the caller passes
        // performance.now(), and a test passes whatever it likes. A module that
        // read the clock itself would be a global nothing can replace.
        const started = 10_000;
        const countdown = Countdown.fromDurationMs(30 * kSecondMs, started);

        expect(countdown.remainingMs(started)).toBe(30 * kSecondMs);
        expect(countdown.remainingMs(started + 10 * kSecondMs)).toBe(20 * kSecondMs);
        expect(countdown.hasElapsed(started + 10 * kSecondMs)).toBe(false);
    });

    it("floors at zero rather than going negative", () => {
        const countdown = Countdown.fromDurationMs(kSecondMs, 0);
        expect(countdown.remainingMs(5 * kSecondMs)).toBe(0);
        expect(countdown.hasElapsed(5 * kSecondMs)).toBe(true);
    });

    it("is unaffected by the wall clock moving", () => {
        // The failure this prevents: a session that appears to expire in
        // negative four minutes because the operating system corrected the
        // device clock while the tab was open.
        const countdown = Countdown.fromDurationMs(kMinuteMs, 1_000);
        const afterTwoSeconds = countdown.remainingMs(3_000);
        expect(afterTwoSeconds).toBe(58 * kSecondMs);
    });
});
