// SHA-256 (FIPS 180-4), incremental.
//
// WebCrypto has SHA-256, and this exists only because WebCrypto's `digest` is
// one-shot: it takes the whole input at once. anvil checks a sealed upload by
// comparing the SHA-256 the client declared with the hash of what arrived, and a
// client that may not hold a 25 MB ciphertext in memory to hash it has to hash it
// as it is produced, chunk by chunk (`docs/05-chat.md` §9.11). That is the one
// reason, and it is why this sits beside BLAKE2b rather than replacing a call to
// WebCrypto anywhere else.
//
// It is a hash over ciphertext. No secret passes through it, so the
// data-dependent timing of a JavaScript implementation leaks nothing.
//
// --- throw, not Result --------------------------------------------------------
//
// As for BLAKE2b: every argument is chosen by code, so a spent instance used
// again is a violated precondition (`CLAUDE.md` §3.1).

const kBlockBytes = 64;
const kDigestBytes = 32;

// The first 32 bits of the fractional parts of the cube roots of the first
// sixty-four primes (FIPS 180-4 §4.2.2).
const kRoundConstants = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
] as const;

// The first 32 bits of the fractional parts of the square roots of the first
// eight primes (FIPS 180-4 §5.3.3).
const kInitialHash = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
] as const;

function rotr(x: number, n: number): number {
    return (x >>> n) | (x << (32 - n));
}

export class Sha256 {
    private readonly h: Uint32Array;
    private readonly w: Uint32Array;
    private readonly block: Uint8Array;
    private filled: number;
    // Bytes hashed so far. A double carries it exactly to 2^53 bytes, which no
    // input that fits in a tab will reach.
    private counted: number;
    private spent: boolean;

    constructor() {
        this.h = Uint32Array.from(kInitialHash);
        this.w = new Uint32Array(64);
        this.block = new Uint8Array(kBlockBytes);
        this.filled = 0;
        this.counted = 0;
        this.spent = false;
    }

    update(input: Uint8Array): this {
        if (this.spent) {
            throw new Error("a SHA-256 instance produces one digest");
        }
        this.counted += input.length;
        let at = 0;
        // Top up a partial block first; then whole blocks straight from the
        // input, with no copy; then keep the tail.
        if (this.filled > 0) {
            const take = Math.min(kBlockBytes - this.filled, input.length);
            this.block.set(input.subarray(0, take), this.filled);
            this.filled += take;
            at = take;
            if (this.filled < kBlockBytes) {
                return this;
            }
            this.compress(this.block, 0);
            this.filled = 0;
        }
        for (; at + kBlockBytes <= input.length; at += kBlockBytes) {
            this.compress(input, at);
        }
        if (at < input.length) {
            this.block.set(input.subarray(at), 0);
            this.filled = input.length - at;
        }
        return this;
    }

    // Writes the digest into `out` from `at`, or into a new array.
    digest(out?: Uint8Array, at = 0): Uint8Array {
        if (this.spent) {
            throw new Error("a SHA-256 instance produces one digest");
        }
        this.spent = true;
        const bits = this.counted * 8;
        // The 0x80 terminator, zeros to 56 mod 64, then the length in bits as a
        // big-endian 64-bit number.
        this.block[this.filled] = 0x80;
        this.block.fill(0, this.filled + 1);
        if (this.filled + 1 > kBlockBytes - 8) {
            this.compress(this.block, 0);
            this.block.fill(0);
        }
        const view = new DataView(this.block.buffer);
        view.setUint32(kBlockBytes - 8, Math.floor(bits / 4294967296));
        view.setUint32(kBlockBytes - 4, bits >>> 0);
        this.compress(this.block, 0);

        const target = out ?? new Uint8Array(kDigestBytes);
        if (at < 0 || at + kDigestBytes > target.length) {
            throw new RangeError("a SHA-256 digest needs 32 bytes of room");
        }
        const written = new DataView(target.buffer, target.byteOffset + at, kDigestBytes);
        for (let i = 0; i < 8; i += 1) {
            written.setUint32(i * 4, this.h[i] ?? 0);
        }
        return target;
    }

    private compress(source: Uint8Array, at: number): void {
        const w = this.w;
        for (let i = 0; i < 16; i += 1) {
            const o = at + i * 4;
            w[i] =
                ((source[o] ?? 0) << 24) | ((source[o + 1] ?? 0) << 16) | ((source[o + 2] ?? 0) << 8) | (source[o + 3] ?? 0);
        }
        for (let i = 16; i < 64; i += 1) {
            const w15 = w[i - 15] ?? 0;
            const w2 = w[i - 2] ?? 0;
            const s0 = rotr(w15, 7) ^ rotr(w15, 18) ^ (w15 >>> 3);
            const s1 = rotr(w2, 17) ^ rotr(w2, 19) ^ (w2 >>> 10);
            w[i] = (w[i - 16] ?? 0) + s0 + (w[i - 7] ?? 0) + s1;
        }

        const h = this.h;
        let a = h[0] ?? 0;
        let b = h[1] ?? 0;
        let c = h[2] ?? 0;
        let d = h[3] ?? 0;
        let e = h[4] ?? 0;
        let f = h[5] ?? 0;
        let g = h[6] ?? 0;
        let k = h[7] ?? 0;
        for (let i = 0; i < 64; i += 1) {
            const t1 = (k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + (kRoundConstants[i] ?? 0) + (w[i] ?? 0)) | 0;
            const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
            k = g;
            g = f;
            f = e;
            e = (d + t1) | 0;
            d = c;
            c = b;
            b = a;
            a = (t1 + t2) | 0;
        }
        // Storing into the Uint32Array wraps each sum modulo 2^32.
        h[0] = (h[0] ?? 0) + a;
        h[1] = (h[1] ?? 0) + b;
        h[2] = (h[2] ?? 0) + c;
        h[3] = (h[3] ?? 0) + d;
        h[4] = (h[4] ?? 0) + e;
        h[5] = (h[5] ?? 0) + f;
        h[6] = (h[6] ?? 0) + g;
        h[7] = (h[7] ?? 0) + k;
    }
}

export function sha256(input: Uint8Array): Uint8Array {
    return new Sha256().update(input).digest();
}
