// The wire format is a contract with another repository, in another language.
//
// Every vector below was produced by anvil's own encoder — PermSet::to_bytes()
// through crypto::base64url_encode — and not by reading anvil/core/perm_set.h
// and believing the reading. A disagreement between the two implementations is
// an authority check reading the wrong bit: silent, and for the bits above 63
// only, which is exactly the half no small test would cover.

import { describe, expect, it } from "vitest";

import { PermSet, kPermBits, kPermBytes } from "../../src/core/perm_set.js";
import { Prng } from "../support/prng.js";

const kVectors: ReadonlyArray<readonly [string, readonly number[]]> = [
    ["AAAAAAAAAAAAAAAAAAAAAA", []],
    ["AQAAAAAAAAAAAAAAAAAAAA", [0]],
    ["gAAAAAAAAAAAAAAAAAAAAA", [7]],
    ["AAEAAAAAAAAAAAAAAAAAAA", [8]],
    ["AAAAAAAAAIAAAAAAAAAAAA", [63]],
    ["AAAAAAAAAAABAAAAAAAAAA", [64]],
    ["AAAAAAAAAAAAAAAAAAAAgA", [127]],
    ["BwMDAAAAAAAAAAAAAAAAAA", [0, 1, 2, 8, 9, 16, 17]],
];

describe("the size of the set", () => {
    // Both numbers are published, and they are the one place a client and a
    // server agree on how wide a permission set is. anvil stores 128 bits as
    // BinData of 16 bytes; a client that thought it was 8 would read every bit
    // above 63 as absent — which hides an affordance rather than granting one,
    // and is therefore the kind of disagreement nothing reports.
    it("is sixteen bytes, which is the hundred and twenty-eight bits anvil stores", () => {
        expect(kPermBytes).toBe(16);
        expect(kPermBits).toBe(kPermBytes * 8);
        expect(kPermBits).toBe(128);
    });

    it("is the width the encoder actually writes", () => {
        // Against the codec rather than against the constant a second time: the
        // assertion above is a claim about the number, and this one is a claim
        // about the bytes, which is what crosses the wire.
        expect(PermSet.of(kPermBits - 1).bytes()).toHaveLength(kPermBytes);
        expect(PermSet.empty().bytes()).toHaveLength(kPermBytes);

        // And the decoder refuses anything else, which is the half that keeps
        // the width a contract rather than a convention.
        expect(PermSet.fromBytes(new Uint8Array(kPermBytes - 1)).ok).toBe(false);
        expect(PermSet.fromBytes(new Uint8Array(kPermBytes + 1)).ok).toBe(false);
        expect(PermSet.fromBytes(new Uint8Array(kPermBytes)).ok).toBe(true);
    });
});

describe("PermSet wire format", () => {
    it("decodes every vector anvil produced", () => {
        for (const [encoded, bits] of kVectors) {
            const decoded = PermSet.fromBase64Url(encoded);
            expect(decoded.ok, `${encoded} should decode`).toBe(true);
            if (!decoded.ok) continue;

            for (let bit = 0; bit < kPermBits; bit += 1) {
                expect(decoded.value.has(bit), `${encoded} bit ${bit}`).toBe(bits.includes(bit));
            }
        }
    });

    it("re-encodes to the same bytes anvil emitted", () => {
        for (const [encoded, bits] of kVectors) {
            expect(PermSet.of(...bits).toBase64Url()).toBe(encoded);
        }
    });

    it("round-trips the all-ones set", () => {
        // anvil emits `_____________________w`: the final character carries four
        // meaningful bits, so its low two are zero. A decoder that accepted a
        // different spelling of the same set would accept two encodings of one
        // authority.
        const all = "_____________________w";
        const decoded = PermSet.fromBase64Url(all);
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;
        expect(decoded.value.toBase64Url()).toBe(all);
        expect(decoded.value.has(0)).toBe(true);
        expect(decoded.value.has(127)).toBe(true);
    });
});

describe("PermSet decoding fails closed", () => {
    it("refuses a value of the wrong length", () => {
        expect(PermSet.fromBase64Url("")).toMatchObject({ ok: false, error: "bad-length" });
        expect(PermSet.fromBase64Url("AQAAAAAAAAAAAAAAAAAAA")).toMatchObject({
            ok: false,
            error: "bad-length",
        });
        // Padding is not part of anvil's encoding, so a padded value is a value
        // from somewhere else.
        expect(PermSet.fromBase64Url("AQAAAAAAAAAAAAAAAAAAAA==")).toMatchObject({
            ok: false,
            error: "bad-length",
        });
    });

    it("refuses standard-base64 characters", () => {
        // `+` and `/` are base64, not base64url. Accepting them would mean two
        // alphabets decode to one set and neither side knows which was sent.
        expect(PermSet.fromBase64Url("+QAAAAAAAAAAAAAAAAAAAA")).toMatchObject({
            ok: false,
            error: "bad-character",
        });
        expect(PermSet.fromBase64Url("/QAAAAAAAAAAAAAAAAAAAA")).toMatchObject({
            ok: false,
            error: "bad-character",
        });
    });

    it("refuses a non-canonical trailing character", () => {
        // The last character's low two bits are slack and must be zero. `B` sets
        // one of them, so this string is a second encoding of a set that already
        // has one.
        expect(PermSet.fromBase64Url("AAAAAAAAAAAAAAAAAAAAAB")).toMatchObject({
            ok: false,
            error: "non-canonical",
        });
    });
});

describe("PermSet membership", () => {
    it("answers false for every bit of the empty set", () => {
        const empty = PermSet.empty();
        for (let bit = 0; bit < kPermBits; bit += 1) {
            expect(empty.has(bit)).toBe(false);
        }
        expect(empty.isEmpty()).toBe(true);
    });

    it("treats an empty requirement as satisfied", () => {
        // The identity case, and the one a bitset is most often wrong about — it
        // is reachable from any route table entry that requires no bit.
        expect(PermSet.empty().hasAll(PermSet.empty())).toBe(true);
        expect(PermSet.of(3).hasAll(PermSet.empty())).toBe(true);
        expect(PermSet.empty().hasAny(PermSet.of(3))).toBe(false);
    });

    it("requires every bit of a multi-bit mask", () => {
        const held = PermSet.of(0, 1, 64);
        expect(held.hasAll(PermSet.of(0, 64))).toBe(true);
        expect(held.hasAll(PermSet.of(0, 65))).toBe(false);
        expect(held.hasAny(PermSet.of(65, 64))).toBe(true);
    });

    it("refuses a bit outside the set rather than reading past it", () => {
        const held = PermSet.of(0);
        expect(held.has(128)).toBe(false);
        expect(held.has(-1)).toBe(false);
        expect(held.has(1.5)).toBe(false);
    });

    it("hands out a copy of its bytes", () => {
        // A caller that could reach in could grant itself a bit.
        const held = PermSet.of(1);
        const bytes = held.bytes();
        bytes[0] = 0xff;
        expect(held.has(7)).toBe(false);
    });
});

// --- properties -------------------------------------------------------------
//
// The vectors above are the cases anvil's suite happens to cover. These are the
// rest of the space: a thousand random sets through the codec and through the
// membership operations, against a reference implementation that is obviously
// correct and far too slow to ship.

function referenceBits(prng: Prng): Set<number> {
    const bits = new Set<number>();
    const count = prng.below(kPermBits + 1);
    for (let i = 0; i < count; i += 1) {
        bits.add(prng.below(kPermBits));
    }
    return bits;
}

function setOf(bits: ReadonlySet<number>): PermSet {
    return PermSet.of(...bits);
}

describe("PermSet properties", () => {
    it("round-trips every set through the wire format", () => {
        for (let seed = 1; seed <= 300; seed += 1) {
            const prng = new Prng(seed);
            const bits = referenceBits(prng);
            const encoded = setOf(bits).toBase64Url();

            expect(encoded.length, `seed ${seed}`).toBe(22);

            const decoded = PermSet.fromBase64Url(encoded);
            expect(decoded.ok, `seed ${seed} (${encoded})`).toBe(true);
            if (!decoded.ok) continue;

            expect(decoded.value.toBase64Url(), `seed ${seed}`).toBe(encoded);
            for (let bit = 0; bit < kPermBits; bit += 1) {
                expect(decoded.value.has(bit), `seed ${seed} bit ${bit}`).toBe(bits.has(bit));
            }
        }
    });

    it("answers membership the way a set of numbers does", () => {
        for (let seed = 1; seed <= 300; seed += 1) {
            const prng = new Prng(seed);
            const held = referenceBits(prng);
            const required = referenceBits(prng);

            const hasAll = [...required].every((bit) => held.has(bit));
            const hasAny = [...required].some((bit) => held.has(bit));

            expect(setOf(held).hasAll(setOf(required)), `seed ${seed} all`).toBe(hasAll);
            expect(setOf(held).hasAny(setOf(required)), `seed ${seed} any`).toBe(hasAny);
            expect(setOf(held).isEmpty(), `seed ${seed} empty`).toBe(held.size === 0);
        }
    });

    it("survives the byte form as well as the character form", () => {
        for (let seed = 1; seed <= 300; seed += 1) {
            const prng = new Prng(seed);
            const bits = referenceBits(prng);
            const original = setOf(bits);

            const rebuilt = PermSet.fromBytes(original.bytes());
            expect(rebuilt.ok, `seed ${seed}`).toBe(true);
            if (!rebuilt.ok) continue;
            expect(rebuilt.value.toBase64Url(), `seed ${seed}`).toBe(original.toBase64Url());
        }
    });

    it("refuses every mutation of a valid encoding that is not one", () => {
        // A permission set arrives on a session response. One that decoded
        // "close enough" would be an authority check against a set nobody
        // issued, so the codec has to refuse a value it cannot reproduce.
        for (let seed = 1; seed <= 200; seed += 1) {
            const prng = new Prng(seed);
            const encoded = setOf(referenceBits(prng)).toBase64Url();
            const at = prng.below(encoded.length);
            const replacement = prng.pick(["+", "/", "=", " ", "٣"]);
            const mutated = encoded.slice(0, at) + replacement + encoded.slice(at + 1);

            expect(PermSet.fromBase64Url(mutated).ok, `seed ${seed} (${mutated})`).toBe(false);
        }
    });
});
