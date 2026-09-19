// The policy, driven as a pure function.
//
// Every case here is one row of the error table in docs/00-architecture.md §6,
// and the ones worth reading twice are the refusals: a client that retries a
// 403, a spent capability or a version mismatch converts a detected conflict
// into a silent overwrite, a failed report of a succeeded operation, or an
// account lockout.

import { describe, expect, it } from "vitest";

import type { HammerError, ServerError } from "../../src/core/errors.js";
import type { RetryFacts } from "../../src/wire/retry.js";
import {
    fullJitter,
    kDefaultRetryPolicy,
    parseRetryAfter,
    randomUnit,
    retryAction,
} from "../../src/wire/retry.js";

function server(status: number): ServerError {
    return { kind: "server", code: "Unknown", status, requestId: null, fields: null };
}

// A GET: repeatable by its own nature, carrying nothing that can be spent.
function facts(error: HammerError, over: Partial<RetryFacts> = {}): RetryFacts {
    return {
        error,
        attempt: 1,
        idempotent: true,
        carriesKey: false,
        burnsCapability: false,
        retryAfterMs: null,
        replayed: false,
        ...over,
    };
}

// A unit source that always returns the top of the interval, so a jittered
// delay is the full backoff and the assertions are about the policy rather than
// about a draw.
const kTop = () => 0.9999999;

describe("what is never retried", () => {
    it("gives up on a local refusal, because the retry hits the same cap", () => {
        const error: HammerError = { kind: "client", cause: "queue-full", retryAfterMs: null };
        expect(retryAction(facts(error))).toEqual({ kind: "give-up" });
    });

    it("gives up on a stale client, which is not a failure of the request", () => {
        const error: HammerError = { kind: "stale-client", serverHash: "a", clientHash: "b" };
        expect(retryAction(facts(error))).toEqual({ kind: "give-up" });
    });

    it("gives up on an abort, which is the screen saying it stopped caring", () => {
        expect(retryAction(facts({ kind: "transport", cause: "aborted" }))).toEqual({
            kind: "give-up",
        });
    });

    it("gives up when the circuit is open, rather than arguing with itself", () => {
        expect(retryAction(facts({ kind: "transport", cause: "circuit-open" }))).toEqual({
            kind: "give-up",
        });
    });

    // The server read the request and disagreed. The same request gets the same
    // answer, and for a version mismatch a retry is a lost update with extra
    // steps.
    it.each([400, 404, 409, 413, 415, 428, 507])("gives up on %i", (status) => {
        expect(retryAction(facts(server(status)))).toEqual({ kind: "give-up" });
    });
});

describe("a credential that expired", () => {
    it("asks for one refresh and a replay", () => {
        expect(retryAction(facts(server(401)))).toEqual({ kind: "refresh" });
    });

    // A second 401 after a successful refresh is a rejection, not a race.
    it("gives up on a second 401 after the replay", () => {
        expect(retryAction(facts(server(401), { replayed: true }))).toEqual({ kind: "give-up" });
    });
});

describe("a 403", () => {
    // Never retried, and never rendered as a denial where a stealth route would
    // have answered 404. What it does say is that the local permission copy
    // disagrees with the server's.
    it("refetches the session rather than retrying", () => {
        expect(retryAction(facts(server(403)))).toEqual({ kind: "stale-session" });
    });

    // anvil maps a spent capability to 403 as well, so the status alone cannot
    // separate the two. What this client knows is whether it presented one.
    it("does not refetch when the call presented a capability", () => {
        expect(retryAction(facts(server(403), { burnsCapability: true }))).toEqual({
            kind: "give-up",
        });
    });
});

describe("what is retried", () => {
    it.each([408, 429, 502, 503, 504])("retries %i for a repeatable call", (status) => {
        expect(retryAction(facts(server(status)), kDefaultRetryPolicy, kTop)).toMatchObject({
            kind: "retry",
        });
    });

    it("retries a network failure", () => {
        expect(
            retryAction(facts({ kind: "transport", cause: "network" }), kDefaultRetryPolicy, kTop),
        ).toMatchObject({ kind: "retry" });
    });

    it("stops at the attempt cap", () => {
        const third = facts({ kind: "transport", cause: "timeout" }, { attempt: 3 });
        expect(retryAction(third)).toEqual({ kind: "give-up" });
    });

    // A proxy's 502 arrives with no envelope and therefore no code, which is
    // exactly the failure a code-keyed policy would have had nothing to say
    // about — and exactly the one where retrying is right.
    it("retries a failure that never reached the server at all", () => {
        const bodyless = server(502);
        expect(retryAction(facts(bodyless), kDefaultRetryPolicy, kTop)).toMatchObject({
            kind: "retry",
        });
    });
});

describe("what makes a call safe to repeat", () => {
    it("does not retry a non-idempotent call with no key", () => {
        const write = facts(server(503), { idempotent: false, carriesKey: false });
        expect(retryAction(write)).toEqual({ kind: "give-up" });
    });

    // The key is what turns at-least-once on the wire into at-most-once at the
    // server, which is the whole of the bargain.
    it("retries a non-idempotent call that carries a key", () => {
        const write = facts(server(503), { idempotent: false, carriesKey: true });
        expect(retryAction(write, kDefaultRetryPolicy, kTop)).toMatchObject({ kind: "retry" });
    });

    // The server consumed the token whether or not the response arrived. A
    // retry is answered with a capability failure and reports a failure for an
    // operation that succeeded.
    it("never retries a call that burned a single-use capability", () => {
        const burned = facts(server(503), { carriesKey: true, burnsCapability: true });
        expect(retryAction(burned)).toEqual({ kind: "give-up" });
    });
});

describe("a 500", () => {
    // The handler ran and threw, so the work may already have happened. A key
    // protects a repeat only if the application wired that route to anvil's
    // store, and a key this client cannot verify is not evidence.
    it("is retried for a naturally idempotent route", () => {
        expect(retryAction(facts(server(500)), kDefaultRetryPolicy, kTop)).toMatchObject({
            kind: "retry",
        });
    });

    it("is not retried for a write, key or no key", () => {
        const write = facts(server(500), { idempotent: false, carriesKey: true });
        expect(retryAction(write)).toEqual({ kind: "give-up" });
    });
});

describe("how long it waits", () => {
    it("honours Retry-After exactly, without jittering it", () => {
        const limited = facts(server(429), { retryAfterMs: 3000 });
        expect(retryAction(limited, kDefaultRetryPolicy, () => 0.1)).toEqual({
            kind: "retry",
            delayMs: 3000,
        });
    });

    it("doubles the backoff per attempt when the server named nothing", () => {
        expect(fullJitter(1, kDefaultRetryPolicy, kTop)).toBe(199);
        expect(fullJitter(2, kDefaultRetryPolicy, kTop)).toBe(399);
        expect(fullJitter(3, kDefaultRetryPolicy, kTop)).toBe(799);
    });

    it("clamps the doubling so a backoff cannot outlive the screen", () => {
        expect(fullJitter(20, kDefaultRetryPolicy, kTop)).toBe(kDefaultRetryPolicy.maxDelayMs - 1);
    });

    // Full jitter rather than a floor plus a wobble: a variant that keeps a
    // floor leaves every client that failed together in the same narrow band.
    it("spreads across the whole interval, floor included", () => {
        expect(fullJitter(4, kDefaultRetryPolicy, () => 0)).toBe(0);
    });
});

describe("the jitter source", () => {
    // `tools/check-source-bans.sh` is what proves Math.random is not behind
    // this; what is asserted here is that the draws are in range and are not
    // one repeated value, because jitter from a source that does not vary is
    // the synchronised retry it exists to prevent.
    it("draws distinct values in [0, 1)", () => {
        const draws = new Set<number>();
        for (let i = 0; i < 1000; i += 1) {
            const draw = randomUnit();
            expect(draw).toBeGreaterThanOrEqual(0);
            expect(draw).toBeLessThan(1);
            draws.add(draw);
        }
        expect(draws.size).toBeGreaterThan(990);
    });
});

describe("reading Retry-After", () => {
    it("reads the whole seconds anvil sends", () => {
        expect(parseRetryAfter("2", null)).toBe(2000);
    });

    it("is null when the header is absent", () => {
        expect(parseRetryAfter(null, null)).toBe(null);
    });

    // The device clock is user-settable and routinely minutes out, so the only
    // honest reading of an HTTP-date is against the server's own Date header:
    // both values were produced by one clock.
    it("reads an HTTP-date against the server's own clock", () => {
        expect(
            parseRetryAfter("Wed, 21 Oct 2026 07:28:30 GMT", "Wed, 21 Oct 2026 07:28:00 GMT"),
        ).toBe(30_000);
    });

    it("declines an HTTP-date with no server clock to read it against", () => {
        expect(parseRetryAfter("Wed, 21 Oct 2026 07:28:30 GMT", null)).toBe(null);
    });

    it("declines a date that has already passed on the server's clock", () => {
        expect(
            parseRetryAfter("Wed, 21 Oct 2026 07:28:00 GMT", "Wed, 21 Oct 2026 07:28:30 GMT"),
        ).toBe(null);
    });

    it("declines a value that is neither", () => {
        expect(parseRetryAfter("soon", "Wed, 21 Oct 2026 07:28:00 GMT")).toBe(null);
    });

    // A screen that has silently decided to do nothing for an hour is
    // indistinguishable from one that is broken.
    it("clamps a window no screen can wait out", () => {
        expect(parseRetryAfter("86400", null)).toBe(120_000);
    });
});
