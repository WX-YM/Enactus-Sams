// The Argon2 pool: one worker, no queue.
//
// --- one, and the reason it is a constant -------------------------------------
//
// An Argon2 task's cost is MEMORY, for the reason `imagePool`'s is: the matrix
// is the parameter `memory_kib`, allocated in full for the whole hash.
//
//   65536 KiB = 64 MiB per hash, at the parameters anvil's own plain mode uses
//   × 1 worker  =  64 MiB
//   × 2 workers = 128 MiB, against a tab budget near 350 MB on a mid-range phone
//
// A tab signs in once. Two concurrent hashes in one tab are a double submit,
// not a workload, and the answer to one is a refusal — never a queue, because a
// queued hash is a second matrix the moment it starts (`docs/00-architecture.md`
// §3).
//
// --- cancelling is terminating ----------------------------------------------------
//
// `argon2` is synchronous and cannot observe an abort. The pool terminates the
// worker instead, which is the only thing that ends the work and returns the
// 64 MiB promptly: an abandoned hash nobody terminated would hold it for
// seconds after the screen that wanted it was gone.
//
// --- close it when the screen goes ---------------------------------------------------
//
// An idle worker keeps its heap, and a heap that just ran Argon2 may still be
// holding a zeroed 64 MiB matrix the collector has not got round to. The pool's
// owner — the login or signup screen — closes it when it unmounts, and that
// terminates the worker and releases all of it at once.

import type { PrehashError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

import type { CountSink } from "../state/counts.js";
import type { PoolError, WorkerFactory } from "../state/workers/pool.js";
import { WorkerPool } from "../state/workers/pool.js";

export const kArgon2Workers = 1;
const kArgon2Waiting = 0;

// What crosses to the worker. Only argon2id: the prehash contract names no
// other, and a pool that would run any variant it was sent is a pool whose
// callers can each choose differently.
export type Argon2Task = {
    readonly password: Uint8Array;
    readonly salt: Uint8Array;
    readonly memoryKib: number;
    readonly iterations: number;
    readonly parallelism: number;
    readonly hashBytes: number;
};

// What comes back. A value rather than a thrown error so the one failure a
// phone produces on an ordinary day — no memory — keeps its name across the
// thread boundary, where a throw would arrive as an anonymous task failure.
export type Argon2Reply =
    | { readonly ok: true; readonly tag: Uint8Array }
    | { readonly ok: false; readonly cause: "bad-parameters" | "out-of-memory" };

function failure(cause: PrehashError["cause"]): PrehashError {
    return { kind: "prehash", cause };
}

function fromPool(error: PoolError): PrehashError {
    switch (error.cause) {
        case "queue-full":
            return failure("busy");
        case "aborted":
            return failure("aborted");
        case "task-failed":
        case "bad-request":
        case "closed":
            return failure("worker-failed");
    }
}

export class Argon2Pool {
    private readonly pool: WorkerPool;

    constructor(config: { readonly create: WorkerFactory; readonly count?: CountSink }) {
        this.pool = new WorkerPool({
            create: config.create,
            size: kArgon2Workers,
            maxWaiting: kArgon2Waiting,
            ...(config.count === undefined ? {} : { count: config.count }),
        });
    }

    // Matrices that exist right now: one per task a worker holds.
    get inFlight(): number {
        return this.pool.inFlight;
    }

    // The password's buffer is TRANSFERRED, not copied: after this call the
    // caller's array is detached and empty, so the main thread is left holding
    // no copy of it to forget to clear.
    async hash(task: Argon2Task, signal: AbortSignal): Promise<Result<Uint8Array, PrehashError>> {
        const answered = await this.pool.run<Argon2Reply>({
            message: task,
            transfer: [task.password.buffer],
            signal,
        });
        if (!answered.ok) {
            return fail(fromPool(answered.error));
        }
        const reply = answered.value;
        if (!reply.ok) {
            return fail(failure(reply.cause === "out-of-memory" ? "out-of-memory" : "bad-answer"));
        }
        return ok(reply.tag);
    }

    close(): void {
        this.pool.close();
    }
}
