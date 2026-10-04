// The `hammer/crypto` entry point: BLAKE2b and Argon2, in plain JavaScript.
//
// Machinery with no application data in it — no parameters, no salts, no
// policy. Its first consumer is client-side password prehashing
// (`docs/01-seams.md` §21), which runs it on a worker; nothing stops an
// application using it for anything else Argon2 is for.
//
// Its own entry point, and one that imports only `hammer`, for the reason
// `hammer/chart` is one: most of what an application ships never hashes a
// password, and a login screen should not pay for it in every other bundle.

export { Blake2b, blake2b } from "./blake2b.js";

export { Sha256, sha256 } from "./sha256.js";

export type { Argon2Error, Argon2Params, Argon2Type } from "./argon2.js";
export { argon2 } from "./argon2.js";
