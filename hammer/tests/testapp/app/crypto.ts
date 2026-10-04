// `hammer/crypto` from outside the library, as an application reaches it.
//
// The ordinary consumer is the prehash worker, which never calls this
// directly. This is the direct use — the algorithm with the application's own
// parameters — proving the entry point stands on its own, with nothing below it
// but `hammer`.

import type { Result } from "hammer";
import type { Argon2Error, Argon2Params } from "hammer/crypto";
import { argon2, blake2b } from "hammer/crypto";

// This application's own choice. hammer has no default and never will: what a
// phone of an application's audience can afford is a product decision.
const kParams = { type: "argon2id", memoryKib: 19456, iterations: 2, parallelism: 1, hashBytes: 32 } as const;

export function deriveKey(password: string, salt: Uint8Array): Result<Uint8Array, Argon2Error> {
    const params: Argon2Params = {
        ...kParams,
        password: new TextEncoder().encode(password.normalize("NFC")),
        salt,
    };
    return argon2(params);
}

export function fingerprint(bytes: Uint8Array): Uint8Array {
    return blake2b(bytes, 16);
}
