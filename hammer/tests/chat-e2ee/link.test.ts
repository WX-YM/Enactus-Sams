// The link offer, and the approver's refusal of a request the server swapped.

import { describe, expect, it } from "../support/test.js";

import type { LinkRequest } from "../../src/chat-e2ee/link.js";
import {
    decodeLinkOffer,
    encodeLinkOffer,
    kLinkOfferVersion,
    linkRequestDigest,
    relayedRequestMatches,
} from "../../src/chat-e2ee/link.js";
import { encodeBase64Url } from "../../src/core/base64url.js";
import { Uuid } from "../../src/core/uuid.js";

function request(): LinkRequest {
    return {
        account: Uuid.random(),
        device: Uuid.random(),
        agreementKey: crypto.getRandomValues(new Uint8Array(32)),
        signingKey: crypto.getRandomValues(new Uint8Array(32)),
    };
}

const kToken = encodeBase64Url(crypto.getRandomValues(new Uint8Array(32)));

describe("the link offer", () => {
    it("is 66 characters, and comes back as it went", async () => {
        const made = request();
        const text = encodeLinkOffer({ token: kToken, digest: await linkRequestDigest(made) });
        expect(text.length).toBe(66);
        const read = decodeLinkOffer(text);
        expect(read.ok).toBe(true);
        if (!read.ok) return;
        expect(read.value.token).toBe(kToken);
        expect(await relayedRequestMatches(read.value, made)).toBe(true);
    });

    it("refuses a relayed request whose keys the server swapped, by one bit", async () => {
        // The attack this exists for: the relay hands the approver the server's
        // own keys under the person's token, and the person's own phone would sign
        // the server's ghost device.
        const made = request();
        const offer = { token: kToken, digest: await linkRequestDigest(made) };
        const signing = Uint8Array.from(made.signingKey);
        signing[0] = (signing[0] ?? 0) ^ 0x01;
        expect(await relayedRequestMatches(offer, { ...made, signingKey: signing })).toBe(false);
        expect(await relayedRequestMatches(offer, { ...made, device: Uuid.random() })).toBe(false);
        expect(await relayedRequestMatches(offer, { ...made, account: Uuid.random() })).toBe(false);
    });

    it("binds the digest to its purpose, so it is not a SHA-256 of the bare keys", async () => {
        const made = request();
        const bare = new Uint8Array([...made.account.bytes(), ...made.device.bytes(), ...made.agreementKey, ...made.signingKey]);
        const plain = new Uint8Array(await crypto.subtle.digest("SHA-256", bare)).slice(0, 16);
        expect(Array.from(await linkRequestDigest(made))).not.toEqual(Array.from(plain));
    });

    it("refuses an offer of the wrong length or an unknown version", async () => {
        const text = encodeLinkOffer({ token: kToken, digest: await linkRequestDigest(request()) });
        expect(decodeLinkOffer(text.slice(0, 60)).ok).toBe(false);
        expect(decodeLinkOffer(`${text}AA`).ok).toBe(false);
        const bytes = new Uint8Array(49);
        bytes[0] = kLinkOfferVersion + 1;
        const future = decodeLinkOffer(encodeBase64Url(bytes));
        expect(future.ok || future.error).toBe("unknown-version");
        // A paste with a trailing newline is still the offer.
        expect(decodeLinkOffer(`${text}\n`).ok).toBe(true);
    });
});
