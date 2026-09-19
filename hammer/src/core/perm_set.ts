// The same 128 bits anvil has, as sixteen bytes and one AND.
//
// The alternative a client usually ships is a list of permission names: an array
// of strings, a linear scan and a string compare per check, several hundred
// bytes on every session response, forever. This is 24 base64url characters on
// the wire and two typed-array reads per check, and the names exist in a bundle
// only where code actually spells one.
//
// --- the wire format is anvil's, byte for byte ------------------------------
//
// anvil/core/perm_set.h fixes it explicitly: little-endian words, low word
// first, so bit n lives in byte n >> 3 at position n & 7. It is asserted here
// against vectors produced by anvil's own encoder rather than by reading that
// header and believing the reading — the two implementations are in different
// languages in different repositories, and a disagreement is an authority check
// reading the wrong bit, silently, and only for the bits above 63.

import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

export const kPermBytes = 16;
export const kPermBits = kPermBytes * 8;

// Exactly 22 base64url characters carry 132 bits, of which 128 are the set and
// four are slack. No padding, because anvil's encoder emits none.
const kEncodedLength = 22;

const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Reverse lookup as a 128-entry table rather than indexOf per character: a scan
// of a 64-character string per character is how a decode that should be
// invisible becomes measurable on a session response.
const kReverse: Int8Array = (() => {
    const table = new Int8Array(128).fill(-1);
    for (let i = 0; i < kAlphabet.length; i += 1) {
        table[kAlphabet.charCodeAt(i)] = i;
    }
    return table;
})();

export type PermSetDecodeError = "bad-length" | "bad-character" | "non-canonical";

export class PermSet {
    // Not readonly as a type, because a Uint8Array is never deeply frozen; it is
    // private and never handed out, and `bytes()` returns a copy.
    private readonly words: Uint8Array;

    private constructor(words: Uint8Array) {
        this.words = words;
    }

    static empty(): PermSet {
        return new PermSet(new Uint8Array(kPermBytes));
    }

    // From the bit numbers a caller names. Used for building a required mask in
    // code; a held set always comes off the wire.
    static of(...bits: readonly number[]): PermSet {
        const set = PermSet.empty();
        for (const bit of bits) {
            if (!Number.isInteger(bit) || bit < 0 || bit >= kPermBits) {
                continue;
            }
            const index = bit >>> 3;
            const current = set.words[index];
            if (current === undefined) {
                continue;
            }
            set.words[index] = current | (1 << (bit & 7));
        }
        return set;
    }

    // Decoding is hand-written rather than `atob`, for three reasons that all
    // matter here: atob does not know base64URL's alphabet, it throws rather
    // than returning a failure, and it accepts inputs this must refuse.
    //
    // It fails CLOSED on every malformed input — a permission set that cannot be
    // decoded is the empty set and an error, never a partially decoded one.
    static fromBase64Url(encoded: string): Result<PermSet, PermSetDecodeError> {
        if (encoded.length !== kEncodedLength) {
            return fail("bad-length");
        }

        const bytes = new Uint8Array(kPermBytes);
        let accumulator = 0;
        let bitsHeld = 0;
        let written = 0;

        for (let i = 0; i < encoded.length; i += 1) {
            const code = encoded.charCodeAt(i);
            const value = code < 128 ? kReverse[code] : -1;
            if (value === undefined || value < 0) {
                return fail("bad-character");
            }

            accumulator = (accumulator << 6) | value;
            bitsHeld += 6;

            if (bitsHeld >= 8) {
                bitsHeld -= 8;
                if (written < kPermBytes) {
                    bytes[written] = (accumulator >>> bitsHeld) & 0xff;
                    written += 1;
                }
            }
        }

        // The four slack bits of the final character must be zero. A non-zero
        // remainder is not a longer permission set — it is a value somebody
        // edited by hand, and accepting it would mean two encodings of one set.
        if ((accumulator & ((1 << bitsHeld) - 1)) !== 0) {
            return fail("non-canonical");
        }

        return ok(new PermSet(bytes));
    }

    static fromBytes(bytes: Uint8Array): Result<PermSet, PermSetDecodeError> {
        if (bytes.length !== kPermBytes) {
            return fail("bad-length");
        }
        return ok(new PermSet(Uint8Array.from(bytes)));
    }

    // One AND. This is the operation the whole representation exists for, and it
    // is called once per affordance on every render.
    has(bit: number): boolean {
        if (!Number.isInteger(bit) || bit < 0 || bit >= kPermBits) {
            return false;
        }
        const byte = this.words[bit >>> 3];
        return byte !== undefined && (byte & (1 << (bit & 7))) !== 0;
    }

    hasAll(required: PermSet): boolean {
        for (let i = 0; i < kPermBytes; i += 1) {
            const held = this.words[i] ?? 0;
            const want = required.words[i] ?? 0;
            if ((held & want) !== want) {
                return false;
            }
        }
        return true;
    }

    hasAny(mask: PermSet): boolean {
        for (let i = 0; i < kPermBytes; i += 1) {
            if (((this.words[i] ?? 0) & (mask.words[i] ?? 0)) !== 0) {
                return true;
            }
        }
        return false;
    }

    isEmpty(): boolean {
        for (let i = 0; i < kPermBytes; i += 1) {
            if ((this.words[i] ?? 0) !== 0) {
                return false;
            }
        }
        return true;
    }

    // A copy, because the caller must not be able to reach in and grant itself a
    // bit. It is sixteen bytes; the copy is not the cost anybody should optimise.
    bytes(): Uint8Array {
        return Uint8Array.from(this.words);
    }

    toBase64Url(): string {
        let out = "";
        let accumulator = 0;
        let bitsHeld = 0;

        for (let i = 0; i < kPermBytes; i += 1) {
            accumulator = (accumulator << 8) | (this.words[i] ?? 0);
            bitsHeld += 8;
            while (bitsHeld >= 6) {
                bitsHeld -= 6;
                out += kAlphabet[(accumulator >>> bitsHeld) & 0x3f];
            }
        }

        if (bitsHeld > 0) {
            out += kAlphabet[(accumulator << (6 - bitsHeld)) & 0x3f];
        }
        return out;
    }
}
