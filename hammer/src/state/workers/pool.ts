// A bounded pool of workers, and the queue in front of it.
//
// A single shared worker couples every workload: one twelve-megapixel decode
// occupies the only thread and the export parse that would have rendered the
// table waits behind it. So there is a pool per workload class, each with its
// own sizing rule and its own bound (`docs/00-architecture.md` §3), and this is
// the machinery both of them are.
//
// --- the bound is the feature ------------------------------------------------
//
// A full queue REJECTS. It does not grow, and it does not wait: growing turns a
// slow device into unbounded memory growth, which on the pools above is the
// failure that kills the tab rather than the one that slows it down. The caller
// is told, synchronously as far as it can tell, and can do the thing only the
// caller can do — ask for a smaller page, refuse the file, show the error.
//
// --- every task body catches -------------------------------------------------
//
// An unhandled rejection in a worker leaves a promise nobody settles, and a
// spinner that never stops is worse than an error message. So every path out of
// `run` settles: a worker that throws, a worker that dies, a message that is not
// a reply, an abort, and a pool that closed underneath the task.
//
// --- the worker is injected --------------------------------------------------
//
// `new Worker(new URL("./x.js", import.meta.url))` is a bundler contract, and
// this library has no bundler and takes no dependency on one. The application
// hands over a factory — two lines in its own worker entry — and a test hands
// over an object, which is the only reason any of this is testable at all
// (`CLAUDE.md` §3.3).
//
// --- nothing posted to a pool captures a DOM node ----------------------------
//
// The screen that asked may be gone by the time an answer arrives. The result
// goes back through the promise, and from there to the store that owns it.

import type { Result } from "../../core/result.js";
import { fail, ok } from "../../core/result.js";

import type { CountSink } from "../counts.js";
import { kNoCounts } from "../counts.js";

// The slice of `Worker` this library uses, as a type it can be handed. A test
// supplies its own; the platform's `Worker` satisfies it structurally, and
// `tests/testapp/app/workers.ts` is what keeps that sentence true.
//
// The transfer list is a mutable array and always passed, because that is how
// the platform declares it. Declared `readonly` and optional, this type was one
// a real `Worker` did NOT satisfy — every application would have needed a cast
// to hand one over, and nothing noticed, because nothing had ever tried.
export type WorkerLike = {
    readonly postMessage: (message: unknown, transfer: Transferable[]) => void;
    readonly addEventListener: (type: string, handler: (event: Event) => void) => void;
    readonly removeEventListener: (type: string, handler: (event: Event) => void) => void;
    readonly terminate: () => void;
};

export type WorkerFactory = () => WorkerLike;

export type PoolError = {
    readonly kind: "pool";
    readonly cause:
        // The pool is at its bound and the queue is full. On `imagePool` this is
        // the memory cap doing its job: the alternative to the refusal is three
        // concurrent decodes and a killed tab.
        | "queue-full"
        // The worker reported a failure, or died. The reason is not carried: a
        // worker's error message is a string this library would have to render,
        // and the words belong to the application (`CLAUDE.md` §1).
        | "task-failed"
        // The caller asked for something this pool will not attempt. Refused on
        // the main thread, before a worker is spawned for a task that was going
        // to fail in it.
        | "bad-request"
        | "aborted"
        | "closed";
};

const kQueueFull: PoolError = { kind: "pool", cause: "queue-full" };
const kTaskFailed: PoolError = { kind: "pool", cause: "task-failed" };
export const kBadRequest: PoolError = { kind: "pool", cause: "bad-request" };
const kAborted: PoolError = { kind: "pool", cause: "aborted" };
const kClosed: PoolError = { kind: "pool", cause: "closed" };

export type PoolConfig = {
    readonly create: WorkerFactory;

    // How many workers. On `imagePool` this is a memory cap rather than a tuning
    // knob, and the module that sets it says so.
    readonly size: number;

    // How many tasks may wait. Zero is a legitimate value and is what a pool
    // whose cost is memory rather than time uses: reject, never queue.
    readonly maxWaiting: number;

    readonly count?: CountSink;
};

// The envelope, so that a message from something that is not this pool — a
// browser extension, another library sharing the worker — is ignored rather than
// resolving somebody's task with it.
const kTag = "hammer.pool";

type Request = { readonly tag: typeof kTag; readonly id: number; readonly task: unknown };

// What a task is, once the optional fields have defaults.
type Task = {
    readonly task: unknown;
    readonly transfer: readonly Transferable[];
    readonly signal: AbortSignal;
};

type Waiting = Task & {
    readonly settle: (outcome: Result<unknown, PoolError>) => void;
};

type Busy = {
    // Settles the promise held by whoever is waiting on this worker, and
    // detaches its listeners. Kept so that `close()` can settle a task that is
    // still out rather than leaving a screen spinning on an answer that will
    // never come.
    readonly settle: (outcome: Result<unknown, PoolError>) => void;
};

export class WorkerPool {
    private readonly config: PoolConfig;
    private readonly count: CountSink;
    private readonly idle: WorkerLike[];
    private readonly busy: Map<WorkerLike, Busy>;
    private readonly waiting: Waiting[];

    private spawned: number;
    private nextId: number;
    private closed: boolean;

    // What a memory test reads. `inFlight` is the number of tasks a worker is
    // holding right now, which on `imagePool` is the number of decoded bitmaps
    // that exist.
    rejections = 0;

    constructor(config: PoolConfig) {
        if (!Number.isInteger(config.size) || config.size < 1) {
            throw new Error("a pool needs at least one worker");
        }
        this.config = config;
        this.count = config.count ?? kNoCounts;
        this.idle = [];
        this.busy = new Map();
        this.waiting = [];
        this.spawned = 0;
        this.nextId = 1;
        this.closed = false;
    }

    get inFlight(): number {
        return this.busy.size;
    }

    get queued(): number {
        return this.waiting.length;
    }

    // Runs a task, or says why it will not.
    //
    // Failure is in the return type (`CLAUDE.md` §3.1): a rejection here is a
    // certainty rather than an exception — it is what the pool DOES when it is
    // full — and a caller that has to write a `catch` for the expected case is a
    // caller that will not.
    async run<T>(task: {
        readonly message: unknown;

        // Handles moved rather than copied. An `ImageBitmap` or an
        // `ArrayBuffer` transferred is a pointer handed over; the same value
        // copied is a second allocation of the thing the pool exists to bound.
        readonly transfer?: readonly Transferable[];

        readonly signal: AbortSignal;
    }): Promise<Result<T, PoolError>> {
        if (this.closed) {
            return fail(kClosed);
        }
        if (task.signal.aborted) {
            return fail(kAborted);
        }

        const worker = this.take();
        if (worker === null) {
            if (this.waiting.length >= this.config.maxWaiting) {
                this.rejections += 1;
                this.count("pool-rejected");
                return fail(kQueueFull);
            }
            return (await new Promise<Result<unknown, PoolError>>((resolve) => {
                this.waiting.push({
                    task: task.message,
                    transfer: task.transfer ?? [],
                    signal: task.signal,
                    settle: resolve,
                });
            })) as Result<T, PoolError>;
        }

        return (await this.dispatch(worker, {
            task: task.message,
            transfer: task.transfer ?? [],
            signal: task.signal,
        })) as Result<T, PoolError>;
    }

    // Every worker terminated and every waiting task settled. A pool that
    // dropped its queue on close would leave a screen spinning on a task that
    // will never be answered.
    close(): void {
        this.closed = true;
        for (const [worker, held] of this.busy) {
            held.settle(fail(kClosed));
            worker.terminate();
        }
        this.busy.clear();
        for (const worker of this.idle) {
            worker.terminate();
        }
        this.idle.length = 0;
        for (const held of this.waiting) {
            held.settle(fail(kClosed));
        }
        this.waiting.length = 0;
    }

    // A worker, spawned lazily. A pool of two that is never used should not cost
    // two threads and two module instantiations on a device with four cores.
    private take(): WorkerLike | null {
        const free = this.idle.pop();
        if (free !== undefined) {
            return free;
        }
        if (this.spawned < this.config.size) {
            this.spawned += 1;
            return this.config.create();
        }
        return null;
    }

    private dispatch(worker: WorkerLike, held: Task): Promise<Result<unknown, PoolError>> {
        const id = this.nextId;
        this.nextId += 1;

        return new Promise<Result<unknown, PoolError>>((resolve) => {
            let settled = false;

            const detach = (): void => {
                worker.removeEventListener("message", onMessage);
                worker.removeEventListener("error", onError);
                worker.removeEventListener("messageerror", onError);
                held.signal.removeEventListener("abort", onAbort);
            };

            const settle = (outcome: Result<unknown, PoolError>): void => {
                if (settled) {
                    return;
                }
                settled = true;
                detach();
                resolve(outcome);
            };

            const onMessage = (event: Event): void => {
                const reply = decodeReply((event as MessageEvent).data);
                // A message that is not this task's reply is not this task's
                // business. A worker that is also talking to something else, or
                // a reply that arrived after an abort, must not settle a
                // promise that belongs to a different task.
                if (reply === null || reply.id !== id) {
                    return;
                }
                this.release(worker);
                settle(reply.ok ? ok(reply.value) : fail(kTaskFailed));
            };

            const onError = (): void => {
                // The worker threw out of its handler, or the message could not
                // be structured-cloned. Either way this worker is not trusted
                // with the next task: it is terminated and the pool spawns a
                // fresh one, because a worker in an unknown state is a worker
                // that will fail the task after this one too.
                this.discard(worker);
                settle(fail(kTaskFailed));
            };

            const onAbort = (): void => {
                // The task is abandoned, not cancelled: a worker cannot be told
                // to stop mid-decode. It is terminated so the work actually
                // ends, which is the whole point on a pool whose bound is
                // memory — a decode nobody is waiting for still holds 48 MB
                // until it finishes.
                this.discard(worker);
                settle(fail(kAborted));
            };

            worker.addEventListener("message", onMessage);
            worker.addEventListener("error", onError);
            worker.addEventListener("messageerror", onError);
            held.signal.addEventListener("abort", onAbort, { once: true });

            this.busy.set(worker, { settle });

            const request: Request = { tag: kTag, id, task: held.task };
            try {
                worker.postMessage(request, [...held.transfer]);
            } catch {
                // A message that will not clone — a function, a DOM node, a
                // value with a cycle. Programmer error, but thrown from inside
                // a promise nobody is catching, so it is reported instead.
                this.discard(worker);
                settle(fail(kTaskFailed));
            }
        });
    }

    private release(worker: WorkerLike): void {
        this.busy.delete(worker);
        if (this.closed) {
            worker.terminate();
            return;
        }
        this.idle.push(worker);
        this.pump();
    }

    private discard(worker: WorkerLike): void {
        this.busy.delete(worker);
        worker.terminate();
        this.spawned -= 1;
        this.pump();
    }

    private pump(): void {
        while (this.waiting.length > 0) {
            const next = this.waiting[0];
            if (next === undefined) {
                return;
            }
            if (next.signal.aborted) {
                this.waiting.shift();
                next.settle(fail(kAborted));
                continue;
            }
            const worker = this.take();
            if (worker === null) {
                return;
            }
            this.waiting.shift();
            void this.dispatch(worker, next).then(next.settle, () => {
                next.settle(fail(kTaskFailed));
            });
        }
    }
}

type Reply = { readonly id: number; readonly ok: boolean; readonly value: unknown };

// A reply from this pool and not from something else on the same worker. The
// data is not trusted for the same reason a broadcast message is not: it comes
// from a program with its own lifecycle, which may be a different build.
function decodeReply(message: unknown): Reply | null {
    if (typeof message !== "object" || message === null) {
        return null;
    }
    const shape = message as {
        readonly tag?: unknown;
        readonly id?: unknown;
        readonly ok?: unknown;
        readonly value?: unknown;
    };
    if (shape.tag !== kTag || typeof shape.id !== "number" || typeof shape.ok !== "boolean") {
        return null;
    }
    return { id: shape.id, ok: shape.ok, value: shape.value };
}

// The worker side of the protocol, so an application's worker entry is two
// lines and the envelope is spelled in exactly one place.
//
// It is a function rather than a module with a top-level listener because this
// library has no top-level side effects (`CLAUDE.md` §2.1): a module that
// registered a handler on import is a module a bundler cannot drop and a
// promise `"sideEffects": false` stops keeping.
export type WorkerScope = {
    readonly addEventListener: (type: string, handler: (event: Event) => void) => void;
    // The same shape as `WorkerLike`'s, for the same reason: it is what a
    // dedicated worker's global scope declares.
    readonly postMessage: (message: unknown, transfer: Transferable[]) => void;
};

export function servePool(
    scope: WorkerScope,
    handle: (task: unknown) => Promise<{ readonly value: unknown; readonly transfer?: readonly Transferable[] }>,
): void {
    scope.addEventListener("message", (event: Event) => {
        const data = (event as MessageEvent).data as {
            readonly tag?: unknown;
            readonly id?: unknown;
            readonly task?: unknown;
        };
        if (data?.tag !== kTag || typeof data.id !== "number") {
            return;
        }
        const { id } = data;

        // Every task body catches, on this side too. A rejection here is an
        // unhandled one in a context nobody is watching, and the promise on the
        // other side is never settled at all.
        void handle(data.task).then(
            (answer) => {
                scope.postMessage({ tag: kTag, id, ok: true, value: answer.value }, [...(answer.transfer ?? [])]);
            },
            () => {
                scope.postMessage({ tag: kTag, id, ok: false, value: null }, []);
            },
        );
    });
}
