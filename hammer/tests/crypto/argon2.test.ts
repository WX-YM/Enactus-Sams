// Argon2, against three witnesses that share no code with it or each other:
// RFC 9106's own test vectors, vectors computed by the reference libargon2
// command, and node's Argon2 over randomly drawn parameters.
//
// The libargon2 vectors are the prehash contract's. anvil's suite asserts the
// same numbers against the libargon2 it links, so a browser and a server that
// both pass agree on the credential byte for byte — which is the one property a
// sign-in cannot survive without.

import * as nodeCrypto from "node:crypto";

import { describe, expect, it } from "../support/test.js";

import type { Argon2Params, Argon2Type } from "../../src/crypto/argon2.js";
import { argon2 } from "../../src/crypto/argon2.js";

function hex(bytes: Uint8Array): string {
    return Buffer.from(bytes).toString("hex");
}

function filled(length: number, value: number): Uint8Array {
    return new Uint8Array(length).fill(value);
}

function tagOf(params: Argon2Params): string {
    const result = argon2(params);
    if (!result.ok) {
        throw new Error(`argon2 refused ${JSON.stringify(result.error)}`);
    }
    return hex(result.value);
}

// RFC 9106 §5: every type over the same inputs, with a secret and associated
// data, at 32 KiB, three passes and four lanes.
const kRfcInputs = {
    password: filled(32, 0x01),
    salt: filled(16, 0x02),
    secret: filled(8, 0x03),
    associatedData: filled(12, 0x04),
    memoryKib: 32,
    iterations: 3,
    parallelism: 4,
    hashBytes: 32,
} as const;

// UTF-8 of "pässwörd كلمة 🔑", decomposed as some keyboards
// produce it, and the same after NFC.
const kTypedHex = "7061cc887373776fcc88726420d983d984d985d8a920f09f9491";
const kNfcHex = "70c3a4737377c3b6726420d983d984d985d8a920f09f9491";
const kSalt = new TextEncoder().encode("anvil-prehash-v1");

describe("Argon2 against RFC 9106", () => {
    const rows: readonly (readonly [Argon2Type, string])[] = [
        ["argon2d", "512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb"],
        ["argon2i", "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8"],
        ["argon2id", "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659"],
    ];
    it.each(rows)("%s produces §5's tag", (type, expected) => {
        expect(tagOf({ ...kRfcInputs, type })).toBe(expected);
    });
});

describe("Argon2 against the reference libargon2 (the prehash contract)", () => {
    it("normalises the typed password to exactly the bytes the vectors hash", () => {
        // The layer above does this step; pinning it here means a change in the
        // platform's NFC tables fails a test rather than a sign-in.
        const typed = new TextDecoder().decode(Buffer.from(kTypedHex, "hex"));
        expect(hex(new TextEncoder().encode(typed.normalize("NFC")))).toBe(kNfcHex);
        expect(typed.normalize("NFC")).not.toBe(typed);
    });

    const rows: readonly (readonly [number, number, number, string])[] = [
        [64, 3, 1, "57511e047b73ea51cd3542a631286e863e307ef117268122253bf47c4c6ce6e9"],
        [256, 2, 2, "9778ab2647a695a1beeda77c64afeb0cae8016bbc217caea2927a56a62570a39"],
        [65536, 3, 1, "68b44374338bc1f32e154d12fed4f7897e5d046d730815aaab695ef09c1f583a"],
    ];
    it.each(rows)("m=%i t=%i p=%i", (memoryKib, iterations, parallelism, expected) => {
        expect(
            tagOf({
                type: "argon2id",
                password: Buffer.from(kNfcHex, "hex"),
                salt: kSalt,
                memoryKib,
                iterations,
                parallelism,
                hashBytes: 32,
            }),
        ).toBe(expected);
    });
});

describe("Argon2 against node's implementation", () => {
    type NodeArgon2 = (
        algorithm: Argon2Type,
        options: {
            readonly message: Uint8Array;
            readonly nonce: Uint8Array;
            readonly parallelism: number;
            readonly tagLength: number;
            readonly memory: number;
            readonly passes: number;
            readonly secret?: Uint8Array;
            readonly associatedData?: Uint8Array;
        },
    ) => Uint8Array;
    const nodeArgon2 = (nodeCrypto as { readonly argon2Sync?: NodeArgon2 }).argon2Sync;

    it("is available, so the differential below is not vacuous", () => {
        // Node 24.7 added it. A runtime without it would pass the next test by
        // running zero cases, which is the green that means nothing.
        expect(typeof nodeArgon2).toBe("function");
    });

    it("agrees over randomly drawn parameters, lanes and tag lengths", () => {
        if (nodeArgon2 === undefined) {
            return;
        }
        const types: readonly Argon2Type[] = ["argon2d", "argon2i", "argon2id"];
        // Tag lengths either side of 64 and of the 32-byte steps in H', where the
        // variable-length hash changes shape.
        const tagLengths = [4, 31, 32, 33, 63, 64, 65, 96, 97, 128, 129, 200];
        for (let i = 0; i < 48; i += 1) {
            const draw = nodeCrypto.randomBytes(8);
            const type = types[(draw[0] ?? 0) % types.length] ?? "argon2id";
            const parallelism = 1 + ((draw[1] ?? 0) % 6);
            const memoryKib = 8 * parallelism + ((draw[2] ?? 0) % 160);
            const iterations = 1 + ((draw[3] ?? 0) % 4);
            const hashBytes = tagLengths[(draw[4] ?? 0) % tagLengths.length] ?? 32;
            const password = nodeCrypto.randomBytes((draw[5] ?? 0) % 64);
            const salt = nodeCrypto.randomBytes(8 + ((draw[6] ?? 0) % 32));
            const withExtras = ((draw[7] ?? 0) & 1) === 1;
            const secret = nodeCrypto.randomBytes((draw[7] ?? 0) % 33);
            const associatedData = nodeCrypto.randomBytes((draw[6] ?? 0) % 21);

            const expected = nodeArgon2(type, {
                message: password,
                nonce: salt,
                parallelism,
                tagLength: hashBytes,
                memory: memoryKib,
                passes: iterations,
                ...(withExtras ? { secret, associatedData } : {}),
            });
            const actual = tagOf({
                type,
                password,
                salt,
                memoryKib,
                iterations,
                parallelism,
                hashBytes,
                ...(withExtras ? { secret, associatedData } : {}),
            });
            expect(`${type} m=${memoryKib} t=${iterations} p=${parallelism} T=${hashBytes}: ${actual}`).toBe(
                `${type} m=${memoryKib} t=${iterations} p=${parallelism} T=${hashBytes}: ${hex(expected)}`,
            );
        }
    });
});

describe("Argon2 parameters", () => {
    const good: Argon2Params = {
        type: "argon2id",
        password: filled(8, 1),
        salt: filled(16, 2),
        memoryKib: 64,
        iterations: 1,
        parallelism: 1,
        hashBytes: 32,
    };

    it("refuses everything outside RFC 9106's bounds as a value, not a throw", () => {
        // These arrive from a server response at the call sites that use this.
        const bad: readonly Partial<Argon2Params>[] = [
            { salt: filled(7, 2) },
            { hashBytes: 3 },
            { hashBytes: 32.5 },
            { iterations: 0 },
            { parallelism: 0 },
            { parallelism: 0x1000000 },
            { memoryKib: 15, parallelism: 2 },
            { memoryKib: Number.NaN },
            { memoryKib: 2 ** 32 },
            { type: "argon2x" as Argon2Type },
        ];
        for (const change of bad) {
            expect(argon2({ ...good, ...change })).toEqual({
                ok: false,
                error: { kind: "argon2", cause: "bad-parameters" },
            });
        }
        expect(argon2(good).ok).toBe(true);
    });

    it("reports a matrix it cannot allocate instead of throwing", () => {
        // 2^32 − 1 KiB is four terabytes. Valid by the RFC, and not a thing any
        // device will hand over, which is exactly the failure a phone produces on
        // an ordinary day at a hundredth of the size.
        expect(argon2({ ...good, memoryKib: 0xffffffff })).toEqual({
            ok: false,
            error: { kind: "argon2", cause: "out-of-memory" },
        });
    });

    it("hashes an empty password, which RFC 9106 permits", () => {
        // Refusing one is the application's policy, not this function's.
        expect(argon2({ ...good, password: new Uint8Array(0) }).ok).toBe(true);
    });

    it("does not mutate the password or the salt", () => {
        const password = filled(8, 9);
        const salt = filled(16, 8);
        argon2({ ...good, password, salt });
        expect(hex(password)).toBe(hex(filled(8, 9)));
        expect(hex(salt)).toBe(hex(filled(16, 8)));
    });
});
