// BLAKE2b (RFC 7693), the hash Argon2 is built on.
//
// --- 64-bit words as pairs of 32-bit halves -----------------------------------
//
// Every word is a (lo, hi) pair in a Uint32Array: word k lives at [2k] and
// [2k + 1]. BigInt would read more naturally and runs roughly an order of
// magnitude slower, because every operation on it allocates; in the function
// Argon2 calls a few hundred million times per sign-in, that is the difference
// between a pause and a timeout. Storing into a Uint32Array wraps modulo 2^32
// for free, so an addition is "add the halves, carry if the low half
// overflowed" and nothing more.
//
// --- throw, not Result --------------------------------------------------------
//
// Every argument here is chosen by code — Argon2 asks for 64 bytes, or for the
// tail of a longer output — never by a server or a person. A digest length
// outside 1..64 is a violated precondition, which is what `throw` is reserved for
// (`CLAUDE.md` §3.1).

const kBlockBytes = 128;
const kMaxOutBytes = 64;
const kMaxKeyBytes = 64;
const kTwo32 = 4294967296;

// The initialisation vector, as (lo, hi) halves: SHA-512's, per RFC 7693 §2.6.
const kIv = [
    0xf3bcc908, 0x6a09e667, 0x84caa73b, 0xbb67ae85, 0xfe94f82b, 0x3c6ef372, 0x5f1d36f1, 0xa54ff53a,
    0xade682d1, 0x510e527f, 0x2b3e6c1f, 0x9b05688c, 0xfb41bd6b, 0x1f83d9ab, 0x137e2179, 0x5be0cd19,
] as const;

// The message schedule, pre-doubled so each entry indexes the lo half of a word
// directly. Rounds 10 and 11 reuse rows 0 and 1.
const kSigma = [
    0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
    28, 20, 8, 16, 18, 30, 26, 12, 2, 24, 0, 4, 22, 14, 10, 6,
    22, 16, 24, 0, 10, 4, 30, 26, 20, 28, 6, 12, 14, 2, 18, 8,
    14, 18, 6, 2, 26, 24, 22, 28, 4, 12, 10, 20, 8, 0, 30, 16,
    18, 0, 10, 14, 4, 8, 20, 30, 28, 2, 22, 24, 12, 16, 6, 26,
    4, 24, 12, 20, 0, 22, 16, 6, 8, 26, 14, 10, 30, 28, 2, 18,
    24, 10, 2, 30, 28, 26, 8, 20, 0, 14, 12, 6, 18, 4, 16, 22,
    26, 22, 14, 28, 24, 2, 6, 18, 10, 0, 30, 8, 16, 12, 4, 20,
    12, 30, 28, 18, 22, 6, 0, 16, 24, 4, 26, 14, 2, 8, 20, 10,
    20, 4, 16, 8, 14, 12, 2, 10, 30, 22, 18, 28, 6, 24, 26, 0,
] as const;

// An incremental BLAKE2b. One instance produces one digest; `digest()` spends it.
export class Blake2b {
    private readonly h: Uint32Array;
    private readonly v: Uint32Array;
    private readonly m: Uint32Array;
    private readonly block: Uint8Array;
    private readonly outBytes: number;
    private filled: number;
    // Bytes compressed so far. A double carries it exactly to 2^53 bytes, which
    // no input that fits in a tab will reach.
    private counted: number;
    private spent: boolean;

    constructor(outBytes: number, key?: Uint8Array) {
        if (!Number.isInteger(outBytes) || outBytes < 1 || outBytes > kMaxOutBytes) {
            throw new RangeError("a BLAKE2b digest is 1 to 64 bytes");
        }
        const keyBytes = key?.length ?? 0;
        if (keyBytes > kMaxKeyBytes) {
            throw new RangeError("a BLAKE2b key is at most 64 bytes");
        }

        this.h = new Uint32Array(16);
        this.v = new Uint32Array(32);
        this.m = new Uint32Array(32);
        this.block = new Uint8Array(kBlockBytes);
        this.outBytes = outBytes;
        this.filled = 0;
        this.counted = 0;
        this.spent = false;

        for (let i = 0; i < 16; i += 1) {
            this.h[i] = kIv[i] ?? 0;
        }
        // The parameter block's first word: digest length, key length, fanout 1,
        // depth 1. Everything else in it is zero for sequential hashing.
        this.h[0] = (this.h[0] ?? 0) ^ 0x01010000 ^ (keyBytes << 8) ^ outBytes;

        if (key !== undefined && keyBytes > 0) {
            // The key is hashed as a whole first block, zero-padded.
            this.block.set(key);
            this.filled = kBlockBytes;
        }
    }

    update(input: Uint8Array): this {
        if (this.spent) {
            throw new Error("a BLAKE2b instance produces one digest");
        }
        let consumed = 0;
        while (consumed < input.length) {
            // The last block is held back rather than compressed on arrival:
            // only `digest()` knows whether it is the final one, and the final
            // compression is flagged differently.
            if (this.filled === kBlockBytes) {
                this.counted += kBlockBytes;
                this.compress(false);
                this.filled = 0;
            }
            const take = Math.min(kBlockBytes - this.filled, input.length - consumed);
            this.block.set(input.subarray(consumed, consumed + take), this.filled);
            this.filled += take;
            consumed += take;
        }
        return this;
    }

    // Writes the digest into `out` from `at`, or into a new array.
    digest(out?: Uint8Array, at = 0): Uint8Array {
        if (this.spent) {
            throw new Error("a BLAKE2b instance produces one digest");
        }
        this.spent = true;

        this.counted += this.filled;
        this.block.fill(0, this.filled);
        this.compress(true);

        const target = out ?? new Uint8Array(this.outBytes);
        for (let i = 0; i < this.outBytes; i += 1) {
            const word = this.h[i >> 2] ?? 0;
            target[at + i] = (word >>> ((i & 3) * 8)) & 0xff;
        }

        // The state is a function of every byte hashed, and Argon2 hashes the
        // password through here. A tab lives for days and a released
        // ArrayBuffer is not zeroed.
        this.h.fill(0);
        this.v.fill(0);
        this.m.fill(0);
        this.block.fill(0);
        return target;
    }

    private compress(last: boolean): void {
        const { h, v, m, block } = this;

        for (let i = 0; i < 32; i += 1) {
            const at = i * 4;
            m[i] =
                (block[at] ?? 0) |
                ((block[at + 1] ?? 0) << 8) |
                ((block[at + 2] ?? 0) << 16) |
                ((block[at + 3] ?? 0) << 24);
        }
        for (let i = 0; i < 16; i += 1) {
            v[i] = h[i] ?? 0;
            v[i + 16] = kIv[i] ?? 0;
        }

        v[24] = (v[24] ?? 0) ^ this.counted;
        v[25] = (v[25] ?? 0) ^ Math.floor(this.counted / kTwo32);
        if (last) {
            v[28] = ~(v[28] ?? 0);
            v[29] = ~(v[29] ?? 0);
        }

        for (let round = 0; round < 12; round += 1) {
            const s = (round % 10) * 16;
            mix(v, m, 0, 8, 16, 24, kSigma[s] ?? 0, kSigma[s + 1] ?? 0);
            mix(v, m, 2, 10, 18, 26, kSigma[s + 2] ?? 0, kSigma[s + 3] ?? 0);
            mix(v, m, 4, 12, 20, 28, kSigma[s + 4] ?? 0, kSigma[s + 5] ?? 0);
            mix(v, m, 6, 14, 22, 30, kSigma[s + 6] ?? 0, kSigma[s + 7] ?? 0);
            mix(v, m, 0, 10, 20, 30, kSigma[s + 8] ?? 0, kSigma[s + 9] ?? 0);
            mix(v, m, 2, 12, 22, 24, kSigma[s + 10] ?? 0, kSigma[s + 11] ?? 0);
            mix(v, m, 4, 14, 16, 26, kSigma[s + 12] ?? 0, kSigma[s + 13] ?? 0);
            mix(v, m, 6, 8, 18, 28, kSigma[s + 14] ?? 0, kSigma[s + 15] ?? 0);
        }

        for (let i = 0; i < 16; i += 1) {
            h[i] = (h[i] ?? 0) ^ (v[i] ?? 0) ^ (v[i + 16] ?? 0);
        }
    }
}

// v[a] += v[b] + m[x], as 64-bit words.
function add3(v: Uint32Array, a: number, b: number, m: Uint32Array, x: number): void {
    const lo = (v[a] ?? 0) + (v[b] ?? 0) + (m[x] ?? 0);
    v[a] = lo;
    v[a + 1] = (v[a + 1] ?? 0) + (v[b + 1] ?? 0) + (m[x + 1] ?? 0) + Math.floor(lo / kTwo32);
}

// v[c] += v[d], as 64-bit words.
function add2(v: Uint32Array, c: number, d: number): void {
    const lo = (v[c] ?? 0) + (v[d] ?? 0);
    v[c] = lo;
    v[c + 1] = (v[c + 1] ?? 0) + (v[d + 1] ?? 0) + (lo >= kTwo32 ? 1 : 0);
}

// v[d] = rotr64(v[d] ^ v[a], n), for the four rotations BLAKE2b uses.
function xorRotate(v: Uint32Array, d: number, a: number, n: 16 | 24 | 32 | 63): void {
    const lo = (v[d] ?? 0) ^ (v[a] ?? 0);
    const hi = (v[d + 1] ?? 0) ^ (v[a + 1] ?? 0);
    if (n === 32) {
        v[d] = hi;
        v[d + 1] = lo;
    } else if (n === 63) {
        // Right by 63 is left by 1.
        v[d] = (lo << 1) | (hi >>> 31);
        v[d + 1] = (hi << 1) | (lo >>> 31);
    } else {
        v[d] = (lo >>> n) | (hi << (32 - n));
        v[d + 1] = (hi >>> n) | (lo << (32 - n));
    }
}

function mix(v: Uint32Array, m: Uint32Array, a: number, b: number, c: number, d: number, x: number, y: number): void {
    add3(v, a, b, m, x);
    xorRotate(v, d, a, 32);
    add2(v, c, d);
    xorRotate(v, b, c, 24);
    add3(v, a, b, m, y);
    xorRotate(v, d, a, 16);
    add2(v, c, d);
    xorRotate(v, b, c, 63);
}

// A one-shot digest.
export function blake2b(input: Uint8Array, outBytes = kMaxOutBytes, key?: Uint8Array): Uint8Array {
    return new Blake2b(outBytes, key).update(input).digest();
}
