// The salt route's answer and the base64url beneath it.
//
// The codec is checked against node's own base64url over every length that can
// end a group differently, because the salt and the credential are the two
// values in this contract a one-bit slip turns into a failed sign-in with no
// other symptom.

import { describe, expect, it } from "../support/test.js";

import { decodeSaltAnswer, kPrehashKeyBytes, kPrehashSaltBytes } from "../../src/prehash/answer.js";
import { decodeBase64Url, encodeBase64Url } from "../../src/core/base64url.js";

describe("base64url", () => {
    it("agrees with node in both directions at every length up to 40", () => {
        for (let length = 0; length <= 40; length += 1) {
            const bytes = new Uint8Array(length).map((_, i) => (i * 97 + length * 13) & 0xff);
            const reference = Buffer.from(bytes).toString("base64url");
            expect(encodeBase64Url(bytes)).toBe(reference);
            expect(Array.from(decodeBase64Url(reference, length) ?? [-1])).toEqual(Array.from(bytes));
        }
    });

    it("refuses the standard alphabet, padding, the wrong length and stray bits", () => {
        const bytes = new Uint8Array(16).fill(0xfb);
        const good = encodeBase64Url(bytes);
        expect(good).toMatch(/^[A-Za-z0-9_-]{22}$/);
        expect(decodeBase64Url(good.replace(/_/g, "/"), 16)).toBeNull();
        expect(decodeBase64Url(`${good}==`, 16)).toBeNull();
        expect(decodeBase64Url(good.slice(1), 16)).toBeNull();
        // 22 characters carry 132 bits; the last four must be zero.
        expect(decodeBase64Url(`${good.slice(0, 21)}B`, 16)).toBeNull();
    });
});

describe("decodeSaltAnswer", () => {
    const answer = {
        algorithm: "argon2id",
        version: 19,
        salt: "YW52aWwtcHJlaGFzaC12MQ",
        memory_kib: 65536,
        iterations: 3,
        parallelism: 1,
        hash_bytes: 32,
    };

    it("decodes the contract's shape", () => {
        const decoded = decodeSaltAnswer(answer);
        expect(decoded.ok).toBe(true);
        if (decoded.ok) {
            expect(new TextDecoder().decode(decoded.value.salt)).toBe("anvil-prehash-v1");
            expect(decoded.value.memoryKib).toBe(65536);
            expect(decoded.value.iterations).toBe(3);
            expect(decoded.value.parallelism).toBe(1);
        }
        expect(kPrehashSaltBytes).toBe(16);
        expect(kPrehashKeyBytes).toBe(32);
    });

    it("refuses anything that is not an object", () => {
        for (const body of [null, undefined, "x", 3, [], [answer]]) {
            expect(decodeSaltAnswer(body)).toEqual({ ok: false, error: { kind: "prehash", cause: "bad-answer" } });
        }
    });

    it("refuses a missing field rather than defaulting it", () => {
        for (const key of Object.keys(answer)) {
            const partial: Record<string, unknown> = { ...answer };
            delete partial[key];
            expect(decodeSaltAnswer(partial).ok).toBe(false);
        }
    });
});
