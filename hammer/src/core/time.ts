// Time the client is allowed to do arithmetic on, and time it is not.
//
// The device clock is user-settable and is routinely minutes out — deliberately,
// on a phone whose owner moved it to win a game, and accidentally, on one whose
// battery died. So a value the server produced and a value `Date.now()` produced
// are not two points on one line, and subtracting them yields a number that is
// wrong by an amount nothing on the device can measure. Expiry belongs to the
// server; a countdown is rendered from a DURATION the server sent
// (`CLAUDE.md` §6).
//
// That is a rule people forget, so it is a type rule here instead.
// `ServerInstant` is not a number: it holds one privately, it has no accessor
// that returns one, and every operation on it takes another `ServerInstant`.
// `instant - Date.now()` is a compile error rather than a review comment, and
// the type-level tests in tests/core/time.test.ts fail the build the day it
// stops being one.

import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

// Durations are milliseconds and the name says so (`CLAUDE.md` §10). There is no
// day here on purpose: a day is a calendar unit rather than a duration, and
// treating it as 86,400,000 ms is wrong twice a year in every zone that changes.
export const kSecondMs = 1000;
export const kMinuteMs = 60 * kSecondMs;
export const kHourMs = 60 * kMinuteMs;

export type TimeParseError = "not-an-instant";

export class ServerInstant {
    // Private, and there is no getter. A public one would put the number back
    // within reach of `Date.now()`, which is the entire thing being prevented.
    private readonly epochMs: number;

    private constructor(epochMs: number) {
        this.epochMs = epochMs;
    }

    // Named for where the number has to come from, because that is the part
    // this type cannot check: response shapes are the application's
    // (`docs/01-seams.md` §4), so the application is what turns a field of a
    // decoded body into an instant. What the type DOES guarantee is the half
    // that gets forgotten — whatever the number's provenance, it cannot be
    // subtracted from the device clock once it is in here.
    static fromServerEpochMs(epochMs: number): Result<ServerInstant, TimeParseError> {
        if (!Number.isFinite(epochMs)) {
            return fail("not-an-instant");
        }
        return ok(new ServerInstant(epochMs));
    }

    static fromServerIso(text: string): Result<ServerInstant, TimeParseError> {
        const parsed = Date.parse(text);
        if (Number.isNaN(parsed)) {
            return fail("not-an-instant");
        }
        return ok(new ServerInstant(parsed));
    }

    // Two instants from the same server are two points on the same line, so
    // comparing them is meaningful in a way comparing one to the device is not.
    isBefore(other: ServerInstant): boolean {
        return this.epochMs < other.epochMs;
    }

    isAfter(other: ServerInstant): boolean {
        return this.epochMs > other.epochMs;
    }

    equals(other: ServerInstant): boolean {
        return this.epochMs === other.epochMs;
    }

    compare(other: ServerInstant): number {
        if (this.epochMs === other.epochMs) {
            return 0;
        }
        return this.epochMs < other.epochMs ? -1 : 1;
    }

    // The elapsed time between two SERVER instants, which is a duration the
    // server's clock measured on both ends.
    sinceMs(earlier: ServerInstant): number {
        return this.epochMs - earlier.epochMs;
    }

    // For a formatter, at the edge, and for nothing else. `Intl.DateTimeFormat`
    // takes a `Date`, so one has to come out somewhere; what must not follow is
    // arithmetic against a second clock.
    toDate(): Date {
        return new Date(this.epochMs);
    }
}

// A countdown measured against a monotonic clock, from a duration the server
// sent.
//
// Monotonic — `performance.now()`, injected rather than read here so a test can
// supply its own and so this module keeps no global (`CLAUDE.md` §3.3). A
// countdown driven by the wall clock jumps when the user changes the device
// time, or when the operating system corrects it, and a session that appears to
// expire in negative four minutes is a session the user reports as broken.
//
// It is a hint about the UI and never a guarantee about the work. The tab can be
// frozen or discarded at any point and the number stops advancing with it;
// anything that must happen happens server-side (`CLAUDE.md` §6).
export class Countdown {
    private readonly totalMs: number;
    private readonly startedAtMs: number;

    private constructor(totalMs: number, startedAtMs: number) {
        this.totalMs = totalMs;
        this.startedAtMs = startedAtMs;
    }

    static fromDurationMs(totalMs: number, monotonicNowMs: number): Countdown {
        return new Countdown(Math.max(0, totalMs), monotonicNowMs);
    }

    remainingMs(monotonicNowMs: number): number {
        const elapsed = monotonicNowMs - this.startedAtMs;
        const remaining = this.totalMs - elapsed;
        return remaining > 0 ? remaining : 0;
    }

    hasElapsed(monotonicNowMs: number): boolean {
        return this.remainingMs(monotonicNowMs) === 0;
    }
}
