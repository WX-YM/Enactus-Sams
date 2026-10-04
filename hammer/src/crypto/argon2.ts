// Argon2 (RFC 9106, version 0x13): argon2d, argon2i and argon2id.
//
// The reusable component behind client-side password prehashing
// (`docs/01-seams.md` §21), and nothing in it knows that: it takes bytes and
// parameters and returns bytes. Byte-for-byte what libargon2 computes — the
// suite pins it to RFC 9106's vectors, to vectors from the reference libargon2
// command, and to node's own Argon2 over random parameters.
//
// --- where it runs ------------------------------------------------------------
//
// NEVER on the main thread at real parameters. 64 MiB at three passes is on the
// order of a second on a desktop and several on a mid-range phone, and a frame is
// 16.7 ms (`CLAUDE.md` §4). `hammer/prehash` runs it on a worker pool of one,
// whose size is a memory cap for the reason `imagePool`'s is.
//
// It is synchronous and takes no AbortSignal, deliberately. A signal is
// delivered by the event loop this function is blocking, so no abort could ever
// be observed mid-hash; the pool cancels by terminating the worker, which is
// also the only way the 64 MiB is actually given back promptly.
//
// --- why JavaScript and not WebAssembly ----------------------------------------
//
// Compiling WebAssembly needs `'wasm-unsafe-eval'` in the application's
// `script-src`, and anvil's CSP has no unsafe source of any kind (anvil
// `docs/19` §7). A library that required one would be asking every consumer to
// weaken the policy that bounds an XSS, to make a sign-in faster.
//
// --- the arithmetic ---------------------------------------------------------------
//
// 64-bit words are (lo, hi) pairs in a Uint32Array, as in `blake2b.ts` and for
// the same reason. The one operation BLAKE2b does not have is BlaMka's
// `2 · lo32(x) · lo32(y)`, a 32×32→64 multiply: the low half is `Math.imul`,
// and the high half is schoolbook over 16-bit limbs, every partial product of
// which is exact in a double.

import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

import { Blake2b } from "./blake2b.js";

export type Argon2Type = "argon2d" | "argon2i" | "argon2id";

export type Argon2Params = {
    readonly type: Argon2Type;
    readonly password: Uint8Array;
    readonly salt: Uint8Array;
    readonly memoryKib: number;
    readonly iterations: number;
    readonly parallelism: number;
    readonly hashBytes: number;
    // RFC 9106's K and X. Optional because the ordinary use has neither.
    readonly secret?: Uint8Array;
    readonly associatedData?: Uint8Array;
};

// Local to this layer rather than a member of `HammerError`. Nothing here talks
// to a server, and the layer that does (`hammer/prehash`) decides what an
// Argon2 failure means to an application — a server that sent parameters out of
// range and a device that could not find the memory are different stories there.
export type Argon2Error = {
    readonly kind: "argon2";
    readonly cause:
        // Outside RFC 9106's bounds. The parameters at the call sites that use
        // this come from a server response, so this is an expected condition.
        | "bad-parameters"
        // The memory matrix could not be allocated: a low-memory device, or a
        // tab already near its ceiling. Reported, never thrown, because it is
        // the one failure a phone produces on an ordinary day.
        | "out-of-memory";
};

const kBadParameters: Argon2Error = { kind: "argon2", cause: "bad-parameters" };
const kOutOfMemory: Argon2Error = { kind: "argon2", cause: "out-of-memory" };

const kVersion = 0x13;
const kSyncPoints = 4;
const kBlockWords = 256; // 1024 bytes as 32-bit halves
const kBlockBytes = 1024;
const kAddressesPerBlock = 128;
const kPrehashBytes = 64;
const kMaxU32 = 0xffffffff;
const kMaxLanes = 0xffffff;
const kTwo32 = 4294967296;

const kTypeCode: Readonly<Record<Argon2Type, number>> = { argon2d: 0, argon2i: 1, argon2id: 2 };

function isU32(value: number, min: number): boolean {
    return Number.isInteger(value) && value >= min && value <= kMaxU32;
}

function valid(params: Argon2Params): boolean {
    const secretBytes = params.secret?.length ?? 0;
    const adBytes = params.associatedData?.length ?? 0;
    return (
        Object.prototype.hasOwnProperty.call(kTypeCode, params.type) &&
        params.password.length <= kMaxU32 &&
        params.salt.length >= 8 &&
        params.salt.length <= kMaxU32 &&
        secretBytes <= kMaxU32 &&
        adBytes <= kMaxU32 &&
        isU32(params.hashBytes, 4) &&
        isU32(params.iterations, 1) &&
        Number.isInteger(params.parallelism) &&
        params.parallelism >= 1 &&
        params.parallelism <= kMaxLanes &&
        isU32(params.memoryKib, 8 * params.parallelism)
    );
}

// --- the hashing helpers -----------------------------------------------------------

function le32(value: number): Uint8Array {
    const out = new Uint8Array(4);
    out[0] = value & 0xff;
    out[1] = (value >>> 8) & 0xff;
    out[2] = (value >>> 16) & 0xff;
    out[3] = (value >>> 24) & 0xff;
    return out;
}

// H' (RFC 9106 §3.3): a hash of any length, from BLAKE2b's 64-byte maximum.
function hashLong(out: Uint8Array, input: readonly Uint8Array[]): void {
    const length = out.length;
    if (length <= kPrehashBytes) {
        const h = new Blake2b(length).update(le32(length));
        for (const part of input) {
            h.update(part);
        }
        h.digest(out);
        return;
    }

    const v = new Uint8Array(kPrehashBytes);
    const first = new Blake2b(kPrehashBytes).update(le32(length));
    for (const part of input) {
        first.update(part);
    }
    first.digest(v);
    out.set(v.subarray(0, 32), 0);

    let written = 32;
    while (length - written > kPrehashBytes) {
        new Blake2b(kPrehashBytes).update(v).digest(v);
        out.set(v.subarray(0, 32), written);
        written += 32;
    }
    const tail = new Blake2b(length - written).update(v).digest();
    out.set(tail, written);
    v.fill(0);
    tail.fill(0);
}

// (x · y) >> 32 for two unsigned 32-bit numbers, exactly.
function mulHi(x: number, y: number): number {
    const x0 = x & 0xffff;
    const x1 = x >>> 16;
    const y0 = y & 0xffff;
    const y1 = y >>> 16;
    const t = x0 * y0;
    const m1 = x1 * y0 + (t >>> 16);
    const m2 = x0 * y1 + (m1 & 0xffff);
    return (x1 * y1 + (m1 >>> 16) + (m2 >>> 16)) >>> 0;
}

// --- the compression function ----------------------------------------------------
//
// GB, with BLaMka in place of BLAKE2b's addition, on the (lo, hi) words at
// offsets a, b, c and d of `v`. Written out rather than factored into a helper
// that returns two numbers: returning a pair is an allocation, and this runs
// 512 times per block.

function gb(v: Uint32Array, a: number, b: number, c: number, d: number): void {
    let al = v[a] ?? 0;
    let ah = v[a + 1] ?? 0;
    let bl = v[b] ?? 0;
    let bh = v[b + 1] ?? 0;
    let cl = v[c] ?? 0;
    let ch = v[c + 1] ?? 0;
    let dl = v[d] ?? 0;
    let dh = v[d + 1] ?? 0;

    let x0: number;
    let x1: number;
    let t: number;
    let m1: number;
    let m2: number;
    let pl: number;
    let ph: number;
    let s: number;

    // a = a + b + 2·lo(a)·lo(b)
    x0 = al & 0xffff; x1 = al >>> 16; t = bl & 0xffff; m1 = bl >>> 16;
    s = x0 * t; m2 = x1 * t + (s >>> 16); t = x0 * m1 + (m2 & 0xffff);
    ph = (x1 * m1 + (m2 >>> 16) + (t >>> 16)) >>> 0; pl = Math.imul(al, bl) >>> 0;
    s = al + bl + ((pl << 1) >>> 0);
    al = s >>> 0;
    ah = (ah + bh + (((ph << 1) | (pl >>> 31)) >>> 0) + (s - al) / kTwo32) >>> 0;
    // d = rotr64(d ^ a, 32)
    x0 = dl ^ al; dl = dh ^ ah; dh = x0;

    // c = c + d + 2·lo(c)·lo(d)
    x0 = cl & 0xffff; x1 = cl >>> 16; t = dl & 0xffff; m1 = dl >>> 16;
    s = x0 * t; m2 = x1 * t + (s >>> 16); t = x0 * m1 + (m2 & 0xffff);
    ph = (x1 * m1 + (m2 >>> 16) + (t >>> 16)) >>> 0; pl = Math.imul(cl, dl) >>> 0;
    s = cl + (dl >>> 0) + ((pl << 1) >>> 0);
    cl = s >>> 0;
    ch = (ch + (dh >>> 0) + (((ph << 1) | (pl >>> 31)) >>> 0) + (s - cl) / kTwo32) >>> 0;
    // b = rotr64(b ^ c, 24)
    x0 = bl ^ cl; x1 = bh ^ ch;
    bl = ((x0 >>> 24) | (x1 << 8)) >>> 0; bh = ((x1 >>> 24) | (x0 << 8)) >>> 0;

    // a = a + b + 2·lo(a)·lo(b)
    x0 = al & 0xffff; x1 = al >>> 16; t = bl & 0xffff; m1 = bl >>> 16;
    s = x0 * t; m2 = x1 * t + (s >>> 16); t = x0 * m1 + (m2 & 0xffff);
    ph = (x1 * m1 + (m2 >>> 16) + (t >>> 16)) >>> 0; pl = Math.imul(al, bl) >>> 0;
    s = al + bl + ((pl << 1) >>> 0);
    al = s >>> 0;
    ah = (ah + bh + (((ph << 1) | (pl >>> 31)) >>> 0) + (s - al) / kTwo32) >>> 0;
    // d = rotr64(d ^ a, 16)
    x0 = dl ^ al; x1 = dh ^ ah;
    dl = ((x0 >>> 16) | (x1 << 16)) >>> 0; dh = ((x1 >>> 16) | (x0 << 16)) >>> 0;

    // c = c + d + 2·lo(c)·lo(d)
    x0 = cl & 0xffff; x1 = cl >>> 16; t = dl & 0xffff; m1 = dl >>> 16;
    s = x0 * t; m2 = x1 * t + (s >>> 16); t = x0 * m1 + (m2 & 0xffff);
    ph = (x1 * m1 + (m2 >>> 16) + (t >>> 16)) >>> 0; pl = Math.imul(cl, dl) >>> 0;
    s = cl + dl + ((pl << 1) >>> 0);
    cl = s >>> 0;
    ch = (ch + dh + (((ph << 1) | (pl >>> 31)) >>> 0) + (s - cl) / kTwo32) >>> 0;
    // b = rotr64(b ^ c, 63), which is a left rotation by one
    x0 = bl ^ cl; x1 = bh ^ ch;
    bl = ((x0 << 1) | (x1 >>> 31)) >>> 0; bh = ((x1 << 1) | (x0 >>> 31)) >>> 0;

    v[a] = al; v[a + 1] = ah;
    v[b] = bl; v[b + 1] = bh;
    v[c] = cl; v[c + 1] = ch;
    v[d] = dl; v[d + 1] = dh;
}

// BLAKE2b's round without a message, over sixteen words at the given offsets.
function round(
    v: Uint32Array,
    o0: number, o1: number, o2: number, o3: number,
    o4: number, o5: number, o6: number, o7: number,
    o8: number, o9: number, o10: number, o11: number,
    o12: number, o13: number, o14: number, o15: number,
): void {
    gb(v, o0, o4, o8, o12);
    gb(v, o1, o5, o9, o13);
    gb(v, o2, o6, o10, o14);
    gb(v, o3, o7, o11, o15);
    gb(v, o0, o5, o10, o15);
    gb(v, o1, o6, o11, o12);
    gb(v, o2, o7, o8, o13);
    gb(v, o3, o4, o9, o14);
}

// Scratch for one hash: two working blocks. Allocated per call — a module-level
// buffer is a top-level side effect, and would be shared by two hashes on one
// thread (`CLAUDE.md` §2.1).
type Scratch = { readonly r: Uint32Array; readonly q: Uint32Array };

// out[outAt] = G(x[xAt], y[yAt]), or out ^= G(...) when `withXor` (every pass
// after the first, version 0x13). `out` may alias `y`: both inputs are read in
// full before the first word of the output is written.
function fill(
    scratch: Scratch,
    x: Uint32Array, xAt: number,
    y: Uint32Array, yAt: number,
    out: Uint32Array, outAt: number,
    withXor: boolean,
): void {
    const { r, q } = scratch;
    for (let i = 0; i < kBlockWords; i += 1) {
        const word = (x[xAt + i] ?? 0) ^ (y[yAt + i] ?? 0);
        r[i] = word;
        q[i] = withXor ? word ^ (out[outAt + i] ?? 0) : word;
    }
    // Rows: eight runs of sixteen consecutive 64-bit words.
    for (let i = 0; i < 8; i += 1) {
        const b = i * 32;
        round(r, b, b + 2, b + 4, b + 6, b + 8, b + 10, b + 12, b + 14,
            b + 16, b + 18, b + 20, b + 22, b + 24, b + 26, b + 28, b + 30);
    }
    // Columns: word pairs (2i, 2i+1), stepping sixteen words down the block.
    for (let i = 0; i < 8; i += 1) {
        const b = i * 4;
        round(r, b, b + 2, b + 32, b + 34, b + 64, b + 66, b + 96, b + 98,
            b + 128, b + 130, b + 160, b + 162, b + 192, b + 194, b + 224, b + 226);
    }
    for (let i = 0; i < kBlockWords; i += 1) {
        out[outAt + i] = (q[i] ?? 0) ^ (r[i] ?? 0);
    }
}

function bytesToWords(bytes: Uint8Array, words: Uint32Array, at: number): void {
    for (let i = 0; i < kBlockWords; i += 1) {
        const b = i * 4;
        words[at + i] =
            (bytes[b] ?? 0) | ((bytes[b + 1] ?? 0) << 8) | ((bytes[b + 2] ?? 0) << 16) | ((bytes[b + 3] ?? 0) << 24);
    }
}

// --- the algorithm --------------------------------------------------------------------

export function argon2(params: Argon2Params): Result<Uint8Array, Argon2Error> {
    if (!valid(params)) {
        return fail(kBadParameters);
    }

    const lanes = params.parallelism;
    const passes = params.iterations;
    const typeCode = kTypeCode[params.type];
    const secret = params.secret ?? new Uint8Array(0);
    const ad = params.associatedData ?? new Uint8Array(0);

    // m' = 4·p·floor(m / 4p): whole segments, the same number in every lane.
    const segmentLength = Math.floor(params.memoryKib / (kSyncPoints * lanes));
    const laneLength = segmentLength * kSyncPoints;
    const blocks = laneLength * lanes;

    let memory: Uint32Array;
    try {
        memory = new Uint32Array(blocks * kBlockWords);
    } catch {
        return fail(kOutOfMemory);
    }

    const h0 = new Uint8Array(kPrehashBytes + 8);
    new Blake2b(kPrehashBytes)
        .update(le32(lanes))
        .update(le32(params.hashBytes))
        .update(le32(params.memoryKib))
        .update(le32(passes))
        .update(le32(kVersion))
        .update(le32(typeCode))
        .update(le32(params.password.length))
        .update(params.password)
        .update(le32(params.salt.length))
        .update(params.salt)
        .update(le32(secret.length))
        .update(secret)
        .update(le32(ad.length))
        .update(ad)
        .digest(h0);

    const blockBytes = new Uint8Array(kBlockBytes);
    for (let lane = 0; lane < lanes; lane += 1) {
        for (let column = 0; column < 2; column += 1) {
            h0.set(le32(column), kPrehashBytes);
            h0.set(le32(lane), kPrehashBytes + 4);
            hashLong(blockBytes, [h0]);
            bytesToWords(blockBytes, memory, (lane * laneLength + column) * kBlockWords);
        }
    }

    const scratch: Scratch = { r: new Uint32Array(kBlockWords), q: new Uint32Array(kBlockWords) };
    const zero = new Uint32Array(kBlockWords);
    const input = new Uint32Array(kBlockWords);
    const address = new Uint32Array(kBlockWords);

    for (let pass = 0; pass < passes; pass += 1) {
        for (let slice = 0; slice < kSyncPoints; slice += 1) {
            for (let lane = 0; lane < lanes; lane += 1) {
                const independent =
                    params.type === "argon2i" || (params.type === "argon2id" && pass === 0 && slice < kSyncPoints / 2);

                if (independent) {
                    input.fill(0);
                    input[0] = pass;
                    input[2] = lane;
                    input[4] = slice;
                    input[6] = blocks;
                    input[7] = Math.floor(blocks / kTwo32);
                    input[8] = passes;
                    input[10] = typeCode;
                }

                let start = 0;
                if (pass === 0 && slice === 0) {
                    // The first two blocks of every lane came from H0.
                    start = 2;
                    if (independent) {
                        input[12] = (input[12] ?? 0) + 1;
                        fill(scratch, zero, 0, input, 0, address, 0, false);
                        fill(scratch, zero, 0, address, 0, address, 0, false);
                    }
                }

                let current = lane * laneLength + slice * segmentLength + start;
                let previous = current % laneLength === 0 ? current + laneLength - 1 : current - 1;

                for (let index = start; index < segmentLength; index += 1, current += 1, previous += 1) {
                    if (current % laneLength === 1) {
                        previous = current - 1;
                    }

                    let j1: number;
                    let j2: number;
                    if (independent) {
                        const slot = index % kAddressesPerBlock;
                        if (slot === 0) {
                            input[12] = (input[12] ?? 0) + 1;
                            fill(scratch, zero, 0, input, 0, address, 0, false);
                            fill(scratch, zero, 0, address, 0, address, 0, false);
                        }
                        j1 = address[slot * 2] ?? 0;
                        j2 = address[slot * 2 + 1] ?? 0;
                    } else {
                        j1 = memory[previous * kBlockWords] ?? 0;
                        j2 = memory[previous * kBlockWords + 1] ?? 0;
                    }

                    const refLane = pass === 0 && slice === 0 ? lane : j2 % lanes;
                    const sameLane = refLane === lane;

                    // RFC 9106 §3.4.1.2: how much of the lane may be referenced, and
                    // where in it, with the square biasing toward recent blocks.
                    let area: number;
                    if (pass === 0) {
                        if (slice === 0) {
                            area = index - 1;
                        } else if (sameLane) {
                            area = slice * segmentLength + index - 1;
                        } else {
                            area = slice * segmentLength + (index === 0 ? -1 : 0);
                        }
                    } else if (sameLane) {
                        area = laneLength - segmentLength + index - 1;
                    } else {
                        area = laneLength - segmentLength + (index === 0 ? -1 : 0);
                    }
                    const relative = area - 1 - mulHi(area, mulHi(j1, j1));
                    const startAt =
                        pass === 0 || slice === kSyncPoints - 1 ? 0 : (slice + 1) * segmentLength;
                    const refIndex = (startAt + relative) % laneLength;

                    fill(
                        scratch,
                        memory, previous * kBlockWords,
                        memory, (refLane * laneLength + refIndex) * kBlockWords,
                        memory, current * kBlockWords,
                        pass > 0,
                    );
                }
            }
        }
    }

    // The last block of every lane, XORed together.
    const last = new Uint32Array(kBlockWords);
    for (let lane = 0; lane < lanes; lane += 1) {
        const at = (lane * laneLength + laneLength - 1) * kBlockWords;
        for (let i = 0; i < kBlockWords; i += 1) {
            last[i] = (last[i] ?? 0) ^ (memory[at + i] ?? 0);
        }
    }
    for (let i = 0; i < kBlockWords; i += 1) {
        const word = last[i] ?? 0;
        const b = i * 4;
        blockBytes[b] = word & 0xff;
        blockBytes[b + 1] = (word >>> 8) & 0xff;
        blockBytes[b + 2] = (word >>> 16) & 0xff;
        blockBytes[b + 3] = (word >>> 24) & 0xff;
    }

    const tag = new Uint8Array(params.hashBytes);
    hashLong(tag, [blockBytes]);

    // Every one of these is a function of the password. The matrix is the one
    // that matters — 64 MiB of it — and a released ArrayBuffer is not zeroed
    // before the allocator hands it to something else in a tab that lives for
    // days.
    memory.fill(0);
    h0.fill(0);
    blockBytes.fill(0);
    last.fill(0);
    scratch.r.fill(0);
    scratch.q.fill(0);
    address.fill(0);
    return ok(tag);
}
