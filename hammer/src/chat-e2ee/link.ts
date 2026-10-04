// The link offer: what travels from the new device's screen to the approving
// device's, outside the server (`docs/05-chat.md` §9.4.3).
//
// anvil relays a link through a short-lived mailbox: the new device leaves its id
// and public keys under a token, the approver reads them by the token and leaves
// its signature, and the new device collects it. The relay holds the request,
// so a malicious server could swap in keys of its own and have the person's own
// phone sign its ghost device into their account.
//
// The offer is what stops that. It carries the token AND a 128-bit digest of the
// request the new device actually made, and the approver refuses a relayed
// request whose digest is not the offer's. A short code compared by eye would not
// do: six digits is 20 bits, and a server generating keypairs matches it in
// seconds. A 128-bit digest cannot be ground, so the offer is 66 characters of
// base64url — fine for a QR code, tedious to type, and the honest cost of a code
// a server cannot match.
//
//   offer = version(1) ‖ token(32) ‖ digest(16)
//   digest = SHA-256("anvil-chat-offer" ‖ account ‖ device ‖ agreement key ‖ signing key)[0..16]

import { decodeBase64Url, encodeBase64Url } from "../core/base64url.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import type { Uuid } from "../core/uuid.js";

import { kPublicKeyBytes } from "./device_keys.js";

export const kLinkOfferVersion = 1;

const kTokenBytes = 32;
const kDigestBytes = 16;
const kOfferBytes = 1 + kTokenBytes + kDigestBytes;
const kOfferDomain = "anvil-chat-offer";

// What the new device left in the relay, and what the approver reads back.
export type LinkRequest = {
    readonly account: Uuid;
    readonly device: Uuid;
    readonly agreementKey: Uint8Array;
    readonly signingKey: Uint8Array;
};

export type LinkOffer = {
    // The relay's token, as anvil wrote it: 43 characters of base64url.
    readonly token: string;
    readonly digest: Uint8Array;
};

export type LinkOfferError = "malformed" | "unknown-version";

export async function linkRequestDigest(request: LinkRequest): Promise<Uint8Array> {
    if (request.agreementKey.length !== kPublicKeyBytes || request.signingKey.length !== kPublicKeyBytes) {
        throw new Error("link request: a public key is not 32 bytes");
    }
    const domain = Uint8Array.from(kOfferDomain, (character) => character.charCodeAt(0));
    const input = new Uint8Array(domain.length + 16 + 16 + kPublicKeyBytes * 2);
    let at = 0;
    for (const part of [domain, request.account.bytes(), request.device.bytes(), request.agreementKey, request.signingKey]) {
        input.set(part, at);
        at += part.length;
    }
    const full = new Uint8Array(await crypto.subtle.digest("SHA-256", input));
    return full.slice(0, kDigestBytes);
}

export function encodeLinkOffer(offer: LinkOffer): string {
    const token = decodeBase64Url(offer.token, kTokenBytes);
    if (token === null || offer.digest.length !== kDigestBytes) {
        throw new Error("link offer: a token is 32 bytes and a digest 16");
    }
    const bytes = new Uint8Array(kOfferBytes);
    bytes[0] = kLinkOfferVersion;
    bytes.set(token, 1);
    bytes.set(offer.digest, 1 + kTokenBytes);
    return encodeBase64Url(bytes);
}

// The offer arrives from a camera or a paste, so it is an input like any other.
export function decodeLinkOffer(text: string): Result<LinkOffer, LinkOfferError> {
    const bytes = decodeBase64Url(text.trim(), kOfferBytes);
    if (bytes === null) {
        return fail("malformed");
    }
    if (bytes[0] !== kLinkOfferVersion) {
        return fail("unknown-version");
    }
    return ok({
        token: encodeBase64Url(bytes.slice(1, 1 + kTokenBytes)),
        digest: bytes.slice(1 + kTokenBytes),
    });
}

// The approver's check, before it signs anything: the request the server relayed
// is the request the new device made. The digest is not a secret, so an ordinary
// comparison is enough.
export async function relayedRequestMatches(offer: LinkOffer, relayed: LinkRequest): Promise<boolean> {
    const digest = await linkRequestDigest(relayed);
    return digest.length === offer.digest.length && digest.every((byte, i) => byte === offer.digest[i]);
}
