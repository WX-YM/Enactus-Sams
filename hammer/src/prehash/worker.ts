// The `hammer/prehash-worker` entry point: the worker side of the Argon2 pool.
//
// An application's worker entry is two lines:
//
//     import { serveArgon2Pool } from "hammer/prehash-worker";
//     serveArgon2Pool(self);
//
// Its own entry point, separate from `hammer/prehash`, because the two halves
// run in different bundles: the main thread needs the pool and never the
// algorithm, and the worker needs the algorithm and never the pool's client.
// One entry holding both would put Argon2 in the page and the pool in the
// worker, each paying for the half it cannot use.

import { argon2 } from "../crypto/argon2.js";

import type { WorkerScope } from "../state/workers/pool.js";
import { servePool } from "../state/workers/pool.js";

import type { Argon2Reply, Argon2Task } from "./pool.js";

function isBytes(value: unknown): value is Uint8Array {
    return value instanceof Uint8Array;
}

export function serveArgon2Pool(scope: WorkerScope): void {
    servePool(scope, async (message) => {
        // The main thread built this, but it crossed a boundary as `unknown`,
        // and a message this cannot read is answered, not trusted.
        const task = message as Partial<Argon2Task> | null;
        const password = task?.password;
        const salt = task?.salt;
        if (!isBytes(password) || !isBytes(salt)) {
            const refused: Argon2Reply = { ok: false, cause: "bad-parameters" };
            return { value: refused };
        }

        const result = argon2({
            type: "argon2id",
            password,
            salt,
            memoryKib: task?.memoryKib ?? 0,
            iterations: task?.iterations ?? 0,
            parallelism: task?.parallelism ?? 0,
            hashBytes: task?.hashBytes ?? 0,
        });
        // This worker's copy of the password, the only one left anywhere once
        // the main thread transferred its buffer here.
        password.fill(0);

        if (!result.ok) {
            const refused: Argon2Reply = { ok: false, cause: result.error.cause };
            return { value: refused };
        }
        const reply: Argon2Reply = { ok: true, tag: result.value };
        return { value: reply, transfer: [result.value.buffer] };
    });
}
