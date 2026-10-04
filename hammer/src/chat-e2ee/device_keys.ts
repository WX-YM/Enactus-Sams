// A device's public keys and the three messages its keys sign
// (anvil `docs/22-chat.md` §7.3.1, byte for byte).
//
// | Message            | Signed by           | Bytes                                                  |
// |--------------------|---------------------|--------------------------------------------------------|
// | signed prekey      | the device itself   | "anvil-chat-spk"(14) ‖ device(16) ‖ spk(32)            |
// | last-resort prekey | the device itself   | "anvil-chat-lrk"(14) ‖ device(16) ‖ lrk(32)            |
// | link               | the APPROVER        | "anvil-chat-link"(15) ‖ account(16) ‖ device(16) ‖     |
// |                    |                     | agreement key(32) ‖ signing key(32) ‖ unix seconds(u64)|
//
// Signatures are Ed25519, from WebCrypto, with each device's separate signing key
// rather than XEdDSA, which WebCrypto does not have (`docs/05-chat.md` §9.2).
//
// --- why the client checks keys the server already checked --------------------------
//
// anvil refuses a malformed key before it stores one. The client checks a peer's
// keys again because the peer's keys reach it THROUGH the server, and the server
// is one of the adversaries end-to-end encryption exists for. A server that served
// a small-order signing key as somebody's first device would hand every client
// that pinned it a root under which a forged signature verifies for any message.
// So this refuses exactly what anvil refuses, in anvil's order, naming the field
// anvil names:
//
//   X25519: 32 bytes, CANONICAL (bit 255 clear, u below p), and not of low order.
//   Low order is decided by WebCrypto itself rather than by a table: the spec
//   requires X25519 `deriveBits` to throw when the shared secret is all zero,
//   which is exactly the property that matters, so the check derives against a
//   throwaway key and asks.
//
//   Ed25519: 32 bytes, canonical y (below p), and not one of the points of order
//   dividing 8, under either sign. WebCrypto has no such hook, so these five
//   y-coordinates are listed, and the suite proves each one's order by arithmetic
//   rather than trusting the list.

import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import type { Uuid } from "../core/uuid.js";

// The one suite version 1 speaks: X25519 agreement, Ed25519 signatures.
export const kSuiteSignalX25519Ed25519 = 1;

export const kPublicKeyBytes = 32;
export const kSignatureBytes = 64;

// A device's public half, as the device routes carry it.
export type DeviceBundle = {
    readonly device: Uuid;
    readonly suite: number;
    readonly agreementKey: Uint8Array;
    readonly signingKey: Uint8Array;
    readonly signedPrekey: Uint8Array;
    readonly signedPrekeySignature: Uint8Array;
    readonly lastResortKey: Uint8Array;
    readonly lastResortSignature: Uint8Array;
};

// The names anvil reports a refused key under.
export type DeviceKeyField =
    | "device_id"
    | "suite"
    | "agreement_key"
    | "signing_key"
    | "signed_prekey"
    | "signed_prekey_signature"
    | "last_resort_key"
    | "last_resort_signature";

const kSignedPrekeyDomain = "anvil-chat-spk";
const kLastResortDomain = "anvil-chat-lrk";
const kLinkDomain = "anvil-chat-link";

function ascii(text: string): Uint8Array {
    return Uint8Array.from(text, (character) => character.charCodeAt(0));
}

function concat(parts: readonly Uint8Array[]): Uint8Array<ArrayBuffer> {
    const out = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
    let at = 0;
    for (const part of parts) {
        out.set(part, at);
        at += part.length;
    }
    return out;
}

function exactly(bytes: Uint8Array, length: number, what: string): Uint8Array {
    if (bytes.length !== length) {
        throw new Error(`${what}: not ${length} bytes`);
    }
    return bytes;
}

export function signedPrekeyMessage(device: Uuid, signedPrekey: Uint8Array): Uint8Array<ArrayBuffer> {
    return concat([ascii(kSignedPrekeyDomain), device.bytes(), exactly(signedPrekey, kPublicKeyBytes, "prekey")]);
}

export function lastResortMessage(device: Uuid, lastResortKey: Uint8Array): Uint8Array<ArrayBuffer> {
    return concat([ascii(kLastResortDomain), device.bytes(), exactly(lastResortKey, kPublicKeyBytes, "prekey")]);
}

// The timestamp is the SERVER's, in whole seconds, read from a response's `Date`
// header and never from `Date.now()`: anvil refuses a link more than five minutes
// from its own clock, and a device whose clock was out by more could otherwise
// link nothing, with no error that says why (`docs/05-chat.md` §9.4.3).
export function linkMessage(link: {
    readonly account: Uuid;
    readonly device: Uuid;
    readonly agreementKey: Uint8Array;
    readonly signingKey: Uint8Array;
    readonly unixSeconds: number;
}): Uint8Array<ArrayBuffer> {
    if (!Number.isSafeInteger(link.unixSeconds) || link.unixSeconds < 0) {
        throw new Error("a link timestamp is a non-negative whole number of seconds");
    }
    const seconds = new Uint8Array(8);
    new DataView(seconds.buffer).setBigUint64(0, BigInt(link.unixSeconds));
    return concat([
        ascii(kLinkDomain),
        link.account.bytes(),
        link.device.bytes(),
        exactly(link.agreementKey, kPublicKeyBytes, "agreement_key"),
        exactly(link.signingKey, kPublicKeyBytes, "signing_key"),
        seconds,
    ]);
}

export async function sign(signingKey: CryptoKey, message: Uint8Array<ArrayBuffer>): Promise<Uint8Array> {
    return new Uint8Array(await crypto.subtle.sign({ name: "Ed25519" }, signingKey, message));
}

// False for a key that is not a valid signing key, as well as for a signature
// that does not verify: a peer's key is an input like any other.
export async function verify(
    signingKey: Uint8Array,
    message: Uint8Array<ArrayBuffer>,
    signature: Uint8Array,
): Promise<boolean> {
    if (!isValidSigningKey(signingKey) || signature.length !== kSignatureBytes) {
        return false;
    }
    try {
        const key = await crypto.subtle.importKey("raw", Uint8Array.from(signingKey), { name: "Ed25519" }, false, ["verify"]);
        return await crypto.subtle.verify({ name: "Ed25519" }, key, Uint8Array.from(signature), message);
    } catch {
        return false;
    }
}

// --- the keys themselves --------------------------------------------------------------

// The little-endian 255-bit value is below p = 2^255 - 19, with bit 255 clear: the
// one spelling of each value. A key with two spellings breaks byte comparison and
// therefore the security code.
function isCanonicalFieldElement(bytes: Uint8Array): boolean {
    if (bytes.length !== kPublicKeyBytes) {
        return false;
    }
    if (((bytes[31] ?? 0) & 0x80) !== 0) {
        return false;
    }
    // Below p unless it is 0x7fff…ff ed or above: bytes 1–30 all 0xff, the top
    // byte 0x7f, and the bottom byte at least 0xed.
    if ((bytes[31] ?? 0) !== 0x7f) {
        return true;
    }
    for (let i = 30; i >= 1; i -= 1) {
        if (bytes[i] !== 0xff) {
            return true;
        }
    }
    return (bytes[0] ?? 0) < 0xed;
}

// The y-coordinates of the Edwards points of order dividing 8 — the identity, the
// point of order two, the two of order four (y = 0), and the four of order eight —
// as little-endian bytes with the sign bit clear. `tests/chat-e2ee/device_keys.test.ts`
// recovers each point and multiplies it by eight.
export const kEd25519SmallOrderY: readonly (readonly number[])[] = [
    [0x00, ...new Array<number>(31).fill(0x00)],
    [0x01, ...new Array<number>(31).fill(0x00)],
    [0xec, ...new Array<number>(30).fill(0xff), 0x7f],
    [
        0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
        0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05,
    ],
    [
        0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
        0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a,
    ],
];


function hasSmallOrderY(bytes: Uint8Array): boolean {
    return kEd25519SmallOrderY.some((y) =>
        y.every((byte, i) => (i === 31 ? ((bytes[31] ?? 0) & 0x7f) === byte : bytes[i] === byte)),
    );
}

export function isValidSigningKey(bytes: Uint8Array): boolean {
    // The sign bit is the top bit, and the field element is the rest of it.
    if (bytes.length !== kPublicKeyBytes) {
        return false;
    }
    const y = Uint8Array.from(bytes);
    y[31] = (y[31] ?? 0) & 0x7f;
    return isCanonicalFieldElement(y) && !hasSmallOrderY(bytes);
}

export async function isValidAgreementKey(bytes: Uint8Array): Promise<boolean> {
    if (!isCanonicalFieldElement(bytes)) {
        return false;
    }
    try {
        const peer = await crypto.subtle.importKey("raw", Uint8Array.from(bytes), { name: "X25519" }, false, []);
        const mine = await crypto.subtle.generateKey({ name: "X25519" }, false, ["deriveBits"]);
        if (!("privateKey" in mine)) {
            return false;
        }
        // Throws when the shared secret is all zero, which is what a low-order
        // point produces whatever the private key.
        await crypto.subtle.deriveBits({ name: "X25519", public: peer }, mine.privateKey, 256);
        return true;
    } catch {
        return false;
    }
}

// The whole bundle, in anvil's order, so a refusal names the field anvil names.
export async function checkBundle(bundle: DeviceBundle): Promise<Result<void, DeviceKeyField>> {
    if (bundle.device.bytes().every((octet) => octet === 0)) {
        return fail("device_id");
    }
    if (bundle.suite !== kSuiteSignalX25519Ed25519) {
        return fail("suite");
    }
    if (!(await isValidAgreementKey(bundle.agreementKey))) {
        return fail("agreement_key");
    }
    if (!isValidSigningKey(bundle.signingKey)) {
        return fail("signing_key");
    }
    if (!(await isValidAgreementKey(bundle.signedPrekey))) {
        return fail("signed_prekey");
    }
    if (
        !(await verify(bundle.signingKey, signedPrekeyMessage(bundle.device, bundle.signedPrekey), bundle.signedPrekeySignature))
    ) {
        return fail("signed_prekey_signature");
    }
    if (!(await isValidAgreementKey(bundle.lastResortKey))) {
        return fail("last_resort_key");
    }
    if (!(await verify(bundle.signingKey, lastResortMessage(bundle.device, bundle.lastResortKey), bundle.lastResortSignature))) {
        return fail("last_resort_signature");
    }
    return ok();
}
