// The key that makes at-least-once on the wire at-most-once at the server.
//
// A retry without one is a duplicate write (`ENGINEERING_RULES.md` §6): a second charge, a
// second message, a second row. anvil closes the gap from its side —
// `http/idempotency.h` records the response against the key and answers a
// repeat from the record rather than performing it again — and this is the half
// that has to hold on the client, which is the half that decides WHICH
// requests are the same request.
//
// --- one key per call, not per route and not per (route, params) ------------
//
// The temptation is a table keyed by the route and its parameters, so a retry
// finds the key its first attempt used. It is wrong in a way that fails
// loudly and inconveniently: anvil's claim carries a FINGERPRINT of the request
// alongside the key, precisely so that a key reused for a DIFFERENT request is
// refused rather than answered with the other request's response. Two deliberate
// submissions of the same form are two different requests with the same route
// and the same parameters, and under a shared key the second one is a conflict
// for an operation nothing was wrong with.
//
// So a key belongs to one CALL, it is minted where that call begins, and it
// lives on that call's stack for as long as the call is retrying. "Dropped when
// the call finally settles" is then not a cache eviction policy — it is what
// happens to a local when the function returns, which is the version of that
// rule with no leak in it.
//
// --- what a retry is, and what it is not ------------------------------------
//
// Every attempt this library makes at one call carries the SAME key: a transport
// retry, a backoff retry and the single replay after a credential refresh are
// all attempts at the same request. A user pressing the button again is not —
// that is a new call, a new key and a second write they asked for.

import type { Brand } from "../core/brand.js";
import { brand } from "../core/brand.js";

export type IdempotencyKey = Brand<string, "idempotency-key">;

// The IETF draft's spelling, which is the one a proxy or a gateway in front of
// anvil is most likely to already understand. anvil's framework reads no header
// itself — `IdempotencyStore::claim` takes the key as an argument, and its own
// header says whether a keyless request reaches it at all is the caller's
// decision — so the name is hammer's and the application's handler is what
// forwards it (`docs/01-seams.md` §17).
export const kIdempotencyHeader = "Idempotency-Key";

// anvil's bound: `kMaxIdempotencyKeyBytes`. It is not about the key's content,
// which is hashed and therefore needs no opinion, but about its length — a key
// without a bound is one request making the server hash a megabyte. A UUID is
// 36 characters, so nothing minted here approaches it; the constant is here
// because the bound is part of the contract and a contract that lives only in
// the other repository is one this side eventually violates.
export const kMaxKeyChars = 255;

// The canonical hyphenated length, which is all this needs to check: the source
// below is the platform's CSPRNG, and the only way to reach this with something
// else is to inject one.
const kUuidChars = 36;

export type KeySource = () => string;

// `crypto.randomUUID` and nothing else. A hand-rolled v4 over
// `getRandomValues` is four lines and one of them is the version nibble
// somebody eventually gets wrong, and a fallback to a weaker source is a
// GUESSABLE idempotency key — which is not a weaker guarantee but an attacker
// able to collide with somebody else's in-flight write.
//
// Deliberately not routed through `core/uuid.ts`. That type exists so an id is
// sixteen bytes rather than a thirty-six character string, and a key is the
// opposite case: it is never compared, never sorted and never stored, it is
// written into one header and forgotten, so parsing it into bytes in order to
// format it back is a round trip that buys nothing.
const platformKey: KeySource = () => crypto.randomUUID();

// The source is a parameter so a test can supply a deterministic one. It is not
// a seam an application reaches: `src/wire/index.ts` exports neither this
// function nor the type of its argument.
export function mintIdempotencyKey(source: KeySource = platformKey): IdempotencyKey {
    const key = source();
    if (key.length !== kUuidChars) {
        // A violated precondition rather than a failure to report: the only
        // caller that can reach this passed a source that is not a UUID
        // generator, which is programmer error and is what a throw is for
        // (`ENGINEERING_RULES.md` §3.1).
        throw new Error("an idempotency key source must return a UUID");
    }
    return brand<string, "idempotency-key">(key);
}

// Whether this route's calls carry one.
//
// Read from the descriptor, which is the only participant that knows: the
// method does not decide it. anvil describes `auth.refresh` as NOT idempotent
// even though repeating it succeeds inside the sixty-second rotation grace,
// because past that grace the same token is a REPLAY — the session is revoked
// and the epoch bumped. A client told otherwise would sign its user out by
// retrying after a backoff.
export function carriesIdempotencyKey(route: { readonly idempotent: boolean }): boolean {
    return !route.idempotent;
}
