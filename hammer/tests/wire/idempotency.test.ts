// The key, and the two decisions around it that are easy to get backwards.
//
// The behavioural half of this — every attempt at one call carrying the same
// key, and a replay after a refresh counting as an attempt — is asserted where
// it is visible, against the client in tests/wire/client.test.ts. What is here
// is the part that has to hold before a call exists at all.

import { describe, expect, it } from "vitest";

import {
    carriesIdempotencyKey,
    kIdempotencyHeader,
    kMaxKeyChars,
    mintIdempotencyKey,
} from "../../src/wire/idempotency.js";

describe("minting", () => {
    it("produces a distinct key every time", () => {
        const keys = new Set<string>();
        for (let i = 0; i < 64; i += 1) {
            keys.add(mintIdempotencyKey());
        }
        expect(keys.size).toBe(64);
    });

    // anvil hashes the key, so its content needs no opinion; its LENGTH does,
    // because a key without a bound is one request making the server hash a
    // megabyte. A UUID is nowhere near it, which is the point of asserting it.
    it("produces a key well inside the bound anvil enforces", () => {
        expect(mintIdempotencyKey().length).toBeLessThan(kMaxKeyChars);
    });

    it("names the header the IETF draft spells", () => {
        expect(kIdempotencyHeader).toBe("Idempotency-Key");
    });

    it("refuses a source that is not a UUID generator", () => {
        expect(() => mintIdempotencyKey(() => "1")).toThrow();
    });
});

describe("which routes carry one", () => {
    it("carries one for a route the descriptor marks not idempotent", () => {
        expect(carriesIdempotencyKey({ idempotent: false })).toBe(true);
    });

    // Not because repeating it is free, but because repeating it is already
    // safe: there is nothing for a key to protect.
    it("carries none for an idempotent route", () => {
        expect(carriesIdempotencyKey({ idempotent: true })).toBe(false);
    });
});
