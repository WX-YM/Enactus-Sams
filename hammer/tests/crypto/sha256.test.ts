// Incremental SHA-256 against two oracles neither of which is this code: the
// known answers FIPS 180's examples publish, and Node's `createHash`, which is
// OpenSSL's. The second is swept over every length that crosses the padding's
// two cases (a terminator that fits the last block and one that needs another)
// and fed in every chunking that crosses a block boundary, because the
// incremental path is the only reason this exists and is where it would be wrong.

import { createHash } from "node:crypto";

import { describe, expect, it } from "../support/test.js";

import { Sha256, sha256 } from "../../src/crypto/sha256.js";

function hex(bytes: Uint8Array): string {
    return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

function ascii(text: string): Uint8Array {
    return Uint8Array.from(text, (character) => character.charCodeAt(0));
}

function oracle(bytes: Uint8Array): string {
    return createHash("sha256").update(bytes).digest("hex");
}

// Deterministic bytes, so a failure names an input that can be rebuilt.
function pattern(length: number): Uint8Array {
    return Uint8Array.from({ length }, (_, i) => (i * 131 + 7) & 0xff);
}

describe("SHA-256", () => {
    it("gives FIPS 180's known answers", () => {
        expect(hex(sha256(new Uint8Array(0)))).toBe(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        );
        expect(hex(sha256(ascii("abc")))).toBe("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        expect(hex(sha256(ascii("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))).toBe(
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        );
        // A million 'a', fed in uneven pieces.
        const hash = new Sha256();
        const piece = new Uint8Array(997).fill(0x61);
        let fed = 0;
        while (fed + piece.length <= 1_000_000) {
            hash.update(piece);
            fed += piece.length;
        }
        hash.update(new Uint8Array(1_000_000 - fed).fill(0x61));
        expect(hex(hash.digest())).toBe("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    });

    it("agrees with OpenSSL at every length across the padding's two cases", () => {
        for (let length = 0; length <= 300; length += 1) {
            const input = pattern(length);
            expect(hex(sha256(input))).toBe(oracle(input));
        }
    });

    it("gives one answer however the input is chunked", () => {
        const input = pattern(1000);
        const expected = oracle(input);
        for (const size of [1, 7, 55, 56, 63, 64, 65, 127, 128, 129, 500]) {
            const hash = new Sha256();
            for (let at = 0; at < input.length; at += size) {
                hash.update(input.subarray(at, at + size));
            }
            expect(hex(hash.digest())).toBe(expected);
        }
    });

    it("writes into a caller's buffer at an offset, and refuses one without room", () => {
        const out = new Uint8Array(40).fill(0xee);
        new Sha256().update(ascii("abc")).digest(out, 4);
        expect(hex(out.subarray(4, 36))).toBe("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        expect(out[3]).toBe(0xee);
        expect(out[36]).toBe(0xee);
        expect(() => new Sha256().digest(new Uint8Array(31))).toThrow();
    });

    it("produces one digest per instance", () => {
        const hash = new Sha256();
        hash.digest();
        expect(() => hash.digest()).toThrow();
        expect(() => hash.update(new Uint8Array(1))).toThrow();
    });
});
