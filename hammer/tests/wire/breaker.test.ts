// The circuit, and the property the whole thing exists for: after it opens,
// exactly one request per window reaches the origin.

import { describe, expect, it } from "vitest";

import { CircuitBreaker, kDefaultBreaker } from "../../src/wire/breaker.js";

const kConfig = { failureThreshold: 3, openMs: 1000 };

function opened(): CircuitBreaker {
    const breaker = new CircuitBreaker(kConfig);
    for (let i = 0; i < 3; i += 1) {
        expect(breaker.admit(0)).toMatchObject({ ok: true });
        breaker.failed(0);
    }
    return breaker;
}

describe("while the origin is answering", () => {
    it("admits everything", () => {
        const breaker = new CircuitBreaker(kConfig);
        for (let i = 0; i < 50; i += 1) {
            expect(breaker.admit(i)).toMatchObject({ ok: true });
            breaker.succeeded();
        }
        expect(breaker.state(50)).toBe("closed");
    });

    // Consecutive, not a rate. A success is evidence the origin is up, and a
    // client that counted failures across one would open the circuit on a form
    // somebody is filling in slowly.
    it("forgets failures that were not consecutive", () => {
        const breaker = new CircuitBreaker(kConfig);
        for (let i = 0; i < 10; i += 1) {
            breaker.failed(i);
            breaker.failed(i);
            breaker.succeeded();
        }
        expect(breaker.admit(10)).toMatchObject({ ok: true });
    });
});

describe("once it opens", () => {
    it("fails fast rather than waiting for a timeout", () => {
        const breaker = opened();
        expect(breaker.admit(0)).toEqual({
            ok: false,
            error: { kind: "transport", cause: "circuit-open" },
        });
        expect(breaker.state(0)).toBe("open");
    });

    // The property the mechanism exists for. Twenty of these in twenty tabs is
    // twenty requests per window against a server that is already down, rather
    // than twenty per retry.
    it("lets exactly one probe through when the window elapses", () => {
        const breaker = opened();
        expect(breaker.admit(1000)).toMatchObject({ ok: true });
        for (let i = 0; i < 10; i += 1) {
            expect(breaker.admit(1000 + i)).toMatchObject({ ok: false });
        }
    });

    it("closes on a probe that succeeds", () => {
        const breaker = opened();
        breaker.admit(1000);
        breaker.succeeded();

        expect(breaker.state(1000)).toBe("closed");
        for (let i = 0; i < 10; i += 1) {
            expect(breaker.admit(1000)).toMatchObject({ ok: true });
        }
    });

    it("reopens on a probe that fails, and restarts the window from then", () => {
        const breaker = opened();
        breaker.admit(1000);
        breaker.failed(1000);

        expect(breaker.admit(1999)).toMatchObject({ ok: false });
        expect(breaker.admit(2000)).toMatchObject({ ok: true });
    });

    // A probe that has not come back yet is not a licence for a second one:
    // "let one through" and "let everything through" are very different amounts
    // of load on a server that has just come back.
    it("admits no second probe while the first is in flight", () => {
        const breaker = opened();
        expect(breaker.admit(1000)).toMatchObject({ ok: true });
        expect(breaker.admit(9999)).toMatchObject({ ok: false });
        expect(breaker.state(9999)).toBe("probing");
    });
});

describe("the default this library ships", () => {
    // Published, because a deployment that knows its origin is flakier or
    // steadier than most can say so. Asserted against the breaker's own
    // behaviour rather than against the literal twice: the number matters only
    // because it is the attempt on which the circuit opens.
    it("opens on the fifth consecutive failure and holds for five seconds", () => {
        expect(kDefaultBreaker).toEqual({ failureThreshold: 5, openMs: 5000 });

        const breaker = new CircuitBreaker();
        for (let i = 0; i < kDefaultBreaker.failureThreshold - 1; i += 1) {
            breaker.failed(0);
            expect(breaker.admit(0).ok).toBe(true);
        }
        breaker.failed(0);
        expect(breaker.admit(0).ok).toBe(false);

        expect(breaker.admit(kDefaultBreaker.openMs - 1).ok).toBe(false);
        expect(breaker.admit(kDefaultBreaker.openMs).ok).toBe(true);
    });
});
