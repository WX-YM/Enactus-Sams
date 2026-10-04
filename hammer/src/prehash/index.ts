// The `hammer/prehash` entry point: client-side password prehashing, the
// main-thread half (`docs/01-seams.md` §21).
//
// A `Prehasher` turns a form's body into the one a login or a registration
// sends — the password replaced by a credential derived from it — using an
// `Argon2Pool` whose worker runs `hammer/prehash-worker`. Only screens that
// send a password need it, so it is its own entry point and the algorithm
// itself is not in it: that lives in the worker's bundle, not the page's.

export type { Argon2Reply, Argon2Task } from "./pool.js";
export { Argon2Pool, kArgon2Workers } from "./pool.js";

export type { SaltAnswer } from "./answer.js";
export { decodeSaltAnswer, kPrehashKeyBytes, kPrehashSaltBytes } from "./answer.js";

export type { PrehashBounds, PrehashFailure, PrehashFields, PrehasherConfig, SaltPurpose } from "./prehasher.js";
export { Prehasher, kMaxSecretBytes } from "./prehasher.js";
