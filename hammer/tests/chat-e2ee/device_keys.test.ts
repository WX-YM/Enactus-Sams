// Device keys and their signed messages, held to anvil's golden vectors.
//
// `anvil_chat_vectors.json` is printed by anvil's `testapp_emit_chat_vectors`.
// Its values came from a third implementation, a script over Python's
// `cryptography` written from anvil's byte table without reading either side's
// code, and anvil's own suite is held to every byte of it. So this suite agreeing
// with it is two implementations agreeing with a third, not with each other.

import { readFileSync } from "node:fs";

import { describe, expect, it } from "../support/test.js";

import type { DeviceBundle } from "../../src/chat-e2ee/device_keys.js";
import {
    checkBundle,
    isValidAgreementKey,
    isValidSigningKey,
    kEd25519SmallOrderY,
    lastResortMessage,
    linkMessage,
    sign,
    signedPrekeyMessage,
    verify,
} from "../../src/chat-e2ee/device_keys.js";
import { decodeBase64Url, encodeBase64Url } from "../../src/core/base64url.js";
import { Uuid } from "../../src/core/uuid.js";

type VectorDevice = {
    readonly name: string;
    readonly bundle: Readonly<Record<string, string | number>>;
    readonly private: Readonly<Record<string, string>>;
    readonly signed_messages_hex: Readonly<Record<string, string>>;
};

type Vectors = {
    readonly devices: readonly VectorDevice[];
    readonly link: {
        readonly account: string;
        readonly approver: string;
        readonly timestamp: number;
        readonly message_hex: string;
        readonly link_signature: string;
    };
    readonly rejections: readonly { readonly name: string; readonly field: string; readonly key: string }[];
};

const kVectors = JSON.parse(
    readFileSync(new URL("./anvil_chat_vectors.json", import.meta.url), "utf8"),
) as Vectors;

function hex(bytes: Uint8Array): string {
    return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

function fromHex(text: string): Uint8Array<ArrayBuffer> {
    return Uint8Array.from(text.match(/../g) ?? [], (pair) => Number.parseInt(pair, 16));
}

function key32(text: string | number | undefined): Uint8Array {
    const decoded = typeof text === "string" ? decodeBase64Url(text, 32) : null;
    if (decoded === null) {
        throw new Error("a vector key is 32 bytes of base64url");
    }
    return decoded;
}

function sig64(text: string | number | undefined): Uint8Array {
    const decoded = typeof text === "string" ? decodeBase64Url(text, 64) : null;
    if (decoded === null) {
        throw new Error("a vector signature is 64 bytes of base64url");
    }
    return decoded;
}

function uuid(text: string | number | undefined): Uuid {
    const parsed = Uuid.parse(String(text));
    if (!parsed.ok) {
        throw new Error("a vector id is a uuid");
    }
    return parsed.value;
}

// The fixed private halves, as PKCS #8 so WebCrypto will take them.
async function signingKeyFrom(seedHex: string | undefined): Promise<CryptoKey> {
    const pkcs8 = fromHex(`302e020100300506032b657004220420${seedHex ?? ""}`);
    return crypto.subtle.importKey("pkcs8", pkcs8, { name: "Ed25519" }, true, ["sign"]);
}

async function agreementKeyFrom(privateHex: string | undefined): Promise<CryptoKey> {
    const pkcs8 = fromHex(`302e020100300506032b656e04220420${privateHex ?? ""}`);
    return crypto.subtle.importKey("pkcs8", pkcs8, { name: "X25519" }, true, ["deriveBits"]);
}

async function publicOf(key: CryptoKey): Promise<string> {
    const jwk = await crypto.subtle.exportKey("jwk", key);
    return jwk.x ?? "";
}

function bundleOf(device: VectorDevice): DeviceBundle {
    return {
        device: uuid(device.bundle["device_id"]),
        suite: Number(device.bundle["suite"]),
        agreementKey: key32(device.bundle["agreement_key"]),
        signingKey: key32(device.bundle["signing_key"]),
        signedPrekey: key32(device.bundle["signed_prekey"]),
        signedPrekeySignature: sig64(device.bundle["signed_prekey_signature"]),
        lastResortKey: key32(device.bundle["last_resort_key"]),
        lastResortSignature: sig64(device.bundle["last_resort_signature"]),
    };
}

describe("anvil's device vectors", () => {
    it("derives every public key from its fixed private half", async () => {
        for (const device of kVectors.devices) {
            expect(await publicOf(await signingKeyFrom(device.private["signing_seed"]))).toBe(device.bundle["signing_key"]);
            expect(await publicOf(await agreementKeyFrom(device.private["agreement"]))).toBe(device.bundle["agreement_key"]);
            expect(await publicOf(await agreementKeyFrom(device.private["signed_prekey"]))).toBe(device.bundle["signed_prekey"]);
            expect(await publicOf(await agreementKeyFrom(device.private["last_resort"]))).toBe(device.bundle["last_resort_key"]);
        }
    });

    it("builds both prekey messages to the byte, and signs them to the byte", async () => {
        for (const device of kVectors.devices) {
            const bundle = bundleOf(device);
            const spk = signedPrekeyMessage(bundle.device, bundle.signedPrekey);
            const lrk = lastResortMessage(bundle.device, bundle.lastResortKey);
            expect(hex(spk)).toBe(device.signed_messages_hex["signed_prekey"]);
            expect(hex(lrk)).toBe(device.signed_messages_hex["last_resort"]);
            // Ed25519 is deterministic, so a signature from the same key over the
            // same bytes is the same signature.
            const signing = await signingKeyFrom(device.private["signing_seed"]);
            expect(encodeBase64Url(await sign(signing, spk))).toBe(device.bundle["signed_prekey_signature"]);
            expect(encodeBase64Url(await sign(signing, lrk))).toBe(device.bundle["last_resort_signature"]);
        }
    });

    it("accepts both whole bundles", async () => {
        for (const device of kVectors.devices) {
            const checked = await checkBundle(bundleOf(device));
            expect(checked.ok).toBe(true);
        }
    });

    it("builds the 119-byte link message, and the approver's signature verifies", async () => {
        const [approver, linked] = kVectors.devices;
        if (approver === undefined || linked === undefined) {
            throw new Error("the vectors carry two devices");
        }
        const newDevice = bundleOf(linked);
        const message = linkMessage({
            account: uuid(kVectors.link.account),
            device: newDevice.device,
            agreementKey: newDevice.agreementKey,
            signingKey: newDevice.signingKey,
            unixSeconds: kVectors.link.timestamp,
        });
        expect(message.length).toBe(119);
        expect(hex(message)).toBe(kVectors.link.message_hex);
        const signing = await signingKeyFrom(approver.private["signing_seed"]);
        expect(encodeBase64Url(await sign(signing, message))).toBe(kVectors.link.link_signature);
        const approverKey = key32(approver.bundle["signing_key"]);
        expect(await verify(approverKey, message, sig64(kVectors.link.link_signature))).toBe(true);
        // The new device cannot approve itself: its own key does not verify it.
        expect(await verify(newDevice.signingKey, message, sig64(kVectors.link.link_signature))).toBe(false);
    });

    it("refuses each key anvil refuses, under the field anvil names", async () => {
        const device = kVectors.devices[0];
        if (device === undefined) {
            throw new Error("the vectors carry a device");
        }
        for (const rejection of kVectors.rejections) {
            const bundle = bundleOf(device);
            const bad = key32(rejection.key);
            const changed: DeviceBundle =
                rejection.field === "agreement_key" ? { ...bundle, agreementKey: bad } : { ...bundle, signingKey: bad };
            const checked = await checkBundle(changed);
            expect(checked.ok || checked.error).toBe(rejection.field);
        }
    });

    it("refuses a signature by the wrong key, and a prekey swapped under a valid signature", async () => {
        const [first, second] = kVectors.devices;
        if (first === undefined || second === undefined) {
            throw new Error("the vectors carry two devices");
        }
        const bundle = bundleOf(first);
        const swapped = await checkBundle({ ...bundle, signedPrekey: bundleOf(second).signedPrekey });
        expect(swapped.ok || swapped.error).toBe("signed_prekey_signature");
        const lastResort = await checkBundle({ ...bundle, lastResortSignature: bundle.signedPrekeySignature });
        expect(lastResort.ok || lastResort.error).toBe("last_resort_signature");
    });
});

describe("the small-order lists", () => {
    // Edwards25519 in affine coordinates with BigInt. An oracle in the suite, never
    // in the library: the library hand-writes no curve arithmetic.
    const p = 2n ** 255n - 19n;
    const mod = (a: bigint): bigint => ((a % p) + p) % p;
    const pow = (base: bigint, exponent: bigint): bigint => {
        let result = 1n;
        let b = mod(base);
        let e = exponent;
        while (e > 0n) {
            if (e & 1n) result = mod(result * b);
            b = mod(b * b);
            e >>= 1n;
        }
        return result;
    };
    const inverse = (a: bigint): bigint => pow(a, p - 2n);
    const d = mod(-121665n * inverse(121666n));
    const sqrtMinusOne = pow(2n, (p - 1n) / 4n);

    function recoverX(y: bigint): bigint | null {
        const u = mod(y * y - 1n);
        const v = mod(d * y * y + 1n);
        let x = mod(u * pow(v, 3n) * pow(u * pow(v, 7n), (p - 5n) / 8n));
        if (mod(v * x * x) === mod(-u)) {
            x = mod(x * sqrtMinusOne);
        }
        return mod(v * x * x) === u ? x : null;
    }

    function add(a: readonly [bigint, bigint], b: readonly [bigint, bigint]): [bigint, bigint] {
        const product = mod(d * a[0] * b[0] * a[1] * b[1]);
        return [
            mod((a[0] * b[1] + b[0] * a[1]) * inverse(1n + product)),
            mod((a[1] * b[1] + a[0] * b[0]) * inverse(1n - product)),
        ];
    }

    function littleEndian(bytes: readonly number[]): bigint {
        return bytes.reduceRight((value, byte) => (value << 8n) + BigInt(byte), 0n);
    }

    it("lists exactly points whose order divides eight", () => {
        expect(kEd25519SmallOrderY.length).toBe(5);
        for (const bytes of kEd25519SmallOrderY) {
            const y = littleEndian(bytes);
            const x = recoverX(y);
            expect(x === null).toBe(false);
            if (x === null) continue;
            let point: [bigint, bigint] = [x, y];
            for (let i = 0; i < 3; i += 1) {
                point = add(point, point);
            }
            expect(point[0] === 0n && point[1] === 1n).toBe(true);
            // And the key is refused under either sign.
            const key = Uint8Array.from(bytes);
            expect(isValidSigningKey(key)).toBe(false);
            key[31] = (key[31] ?? 0) | 0x80;
            expect(isValidSigningKey(key)).toBe(false);
        }
    });

    it("refuses an X25519 key of low order by asking WebCrypto, and accepts a real one", async () => {
        const lowOrder = [
            new Uint8Array(32),
            Uint8Array.of(1, ...new Array<number>(31).fill(0)),
            Uint8Array.of(0xec, ...new Array<number>(30).fill(0xff), 0x7f),
        ];
        for (const key of lowOrder) {
            expect(await isValidAgreementKey(key)).toBe(false);
        }
        const real = await crypto.subtle.generateKey({ name: "X25519" }, true, ["deriveBits"]);
        if (!("publicKey" in real)) {
            throw new Error("X25519 made a pair");
        }
        const raw = new Uint8Array(await crypto.subtle.exportKey("raw", real.publicKey));
        expect(await isValidAgreementKey(raw)).toBe(true);
    });
});
