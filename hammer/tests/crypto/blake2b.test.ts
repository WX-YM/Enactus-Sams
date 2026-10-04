// BLAKE2b, the hash under Argon2.
//
// The expected values come from RFC 7693 and the reference implementation's
// keyed known-answer file, and — for everything else — from node's own BLAKE2b,
// which is OpenSSL's and shares no code with this one.

import { createHash } from "node:crypto";

import { describe, expect, it } from "../support/test.js";

import { Blake2b, blake2b } from "../../src/crypto/blake2b.js";

function hex(bytes: Uint8Array): string {
    return Buffer.from(bytes).toString("hex");
}

function counting(length: number, first = 0): Uint8Array {
    const out = new Uint8Array(length);
    for (let i = 0; i < length; i += 1) {
        out[i] = (first + i) & 0xff;
    }
    return out;
}

describe("BLAKE2b", () => {
    it("matches RFC 7693 Appendix A", () => {
        expect(hex(blake2b(new TextEncoder().encode("abc")))).toBe(
            "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1" +
                "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923",
        );
    });

    it("matches the reference keyed known-answer vectors", () => {
        const key = counting(64);
        expect(hex(blake2b(new Uint8Array(0), 64, key))).toBe(
            "10ebb67700b1868efb4417987acf4690ae9d972fb7a590c2f02871799aaa4786" +
                "b5e996e8f0f4eb981fc214b005f42d2ff4233499391653df7aefcbc13fc51568",
        );
        expect(hex(blake2b(new Uint8Array([0]), 64, key))).toBe(
            "961f6dd1e4dd30f63901690c512e78e4b45e4742ed197c3c5e45c549fd25f2e4" +
                "187b0bc9fe30492b16b0d0bc4ef9b0f34c7003fac09a5ef1532e69430234cebd",
        );
    });

    it("agrees with OpenSSL on every side of the 128-byte block boundary", () => {
        // The final block is compressed with a flag the others are not, so the
        // lengths that end exactly on a boundary are the ones a buffering bug
        // gets wrong.
        for (const length of [0, 1, 127, 128, 129, 255, 256, 257, 1024, 4099]) {
            const input = counting(length, length);
            expect(hex(blake2b(input))).toBe(createHash("blake2b512").update(input).digest("hex"));
        }
    });

    it("gives one digest however the input is split", () => {
        const input = counting(1000, 7);
        const whole = hex(blake2b(input, 48));
        for (const cut of [1, 64, 127, 128, 129, 500]) {
            const split = new Blake2b(48)
                .update(input.subarray(0, cut))
                .update(new Uint8Array(0))
                .update(input.subarray(cut))
                .digest();
            expect(hex(split)).toBe(whole);
        }
    });

    it("parameterises the digest length rather than truncating", () => {
        // A 32-byte BLAKE2b is not the first half of a 64-byte one: the length is
        // in the parameter block. Argon2's variable-length hash depends on it.
        const input = new TextEncoder().encode("abc");
        expect(hex(blake2b(input, 32))).not.toBe(hex(blake2b(input, 64)).slice(0, 64));
        expect(blake2b(input, 1)).toHaveLength(1);
    });

    it("writes into a caller's array at an offset", () => {
        const out = new Uint8Array(40).fill(0xee);
        new Blake2b(32).update(new TextEncoder().encode("abc")).digest(out, 4);
        expect(out[3]).toBe(0xee);
        expect(hex(out.subarray(4, 36))).toBe(hex(blake2b(new TextEncoder().encode("abc"), 32)));
        expect(out[36]).toBe(0xee);
    });

    it("refuses what is a programming error rather than an input", () => {
        expect(() => new Blake2b(0)).toThrow(RangeError);
        expect(() => new Blake2b(65)).toThrow(RangeError);
        expect(() => new Blake2b(1.5)).toThrow(RangeError);
        expect(() => new Blake2b(64, new Uint8Array(65))).toThrow(RangeError);

        const spent = new Blake2b(32);
        spent.digest();
        expect(() => spent.digest()).toThrow();
        expect(() => spent.update(new Uint8Array(1))).toThrow();
    });
});
