import { describe, expect, it } from "vitest";

import { expect as unwrap, fail, isFail, isOk, ok } from "../../src/core/result.js";
import { isRetryableKind, isServerError } from "../../src/core/errors.js";
import type { HammerError } from "../../src/core/errors.js";

describe("Result", () => {
    it("narrows on ok", () => {
        const good = ok(42);
        expect(isOk(good)).toBe(true);
        if (good.ok) {
            expect(good.value).toBe(42);
        }
    });

    it("carries a failure without throwing", () => {
        const bad = fail("nope");
        expect(isFail(bad)).toBe(true);
        if (!bad.ok) {
            expect(bad.error).toBe("nope");
        }
    });

    it("throws only where the caller has already narrowed", () => {
        expect(unwrap(ok(1), "narrowed")).toBe(1);
        expect(() => unwrap(fail("x"), "not narrowed")).toThrow("not narrowed");
    });
});

describe("error kinds", () => {
    it("never carries a message", () => {
        // The absence is the assertion. A library that carried a sentence would
        // ship one language to every application at once.
        const error: HammerError = {
            kind: "server",
            code: "RATE_LIMITED",
            status: 429,
            requestId: "01J",
            fields: null,
        };
        expect(Object.keys(error)).not.toContain("message");
    });

    it("does not retry a client-side refusal", () => {
        // The cap the request hit locally is the same cap a retry would hit.
        const refused: HammerError = { kind: "client", cause: "too-large", retryAfterMs: null };
        expect(isRetryableKind(refused)).toBe(false);
        expect(isRetryableKind({ kind: "transport", cause: "network" })).toBe(true);
    });

    // The guard exists because a `code` is only on the shape a SERVER answered
    // with, and the getting-started guide got exactly this wrong: a transport
    // failure and a locally shed request have no code, because no server spoke.
    // A narrowing that is a published function is a narrowing every consumer
    // does the same way.
    it("narrows to the one shape that carries a code", () => {
        const answered: HammerError = {
            kind: "server",
            code: "RATE_LIMITED",
            status: 429,
            requestId: "01J",
            fields: null,
        };
        expect(isServerError(answered)).toBe(true);
        if (isServerError(answered)) {
            // The point of the guard is this line compiling at all.
            expect(answered.code).toBe("RATE_LIMITED");
        }
    });

    it("refuses the three shapes no server answered", () => {
        const silent: readonly HammerError[] = [
            { kind: "transport", cause: "network" },
            { kind: "client", cause: "queue-full", retryAfterMs: null },
            { kind: "stale-client", serverHash: "a", clientHash: "b" },
        ];
        for (const error of silent) {
            expect(isServerError(error)).toBe(false);
        }
    });
});
