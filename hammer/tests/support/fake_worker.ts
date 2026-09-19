// A worker, as an object, with a script.
//
// Not a mocking framework (`docs/16-test-plan.md`). It speaks the real pool
// protocol — the same envelope `servePool` writes — so the decode under test is
// the one that runs in a browser, and it records what it was ASKED, because half
// of what the pool has to get right is in the dispatch rather than in the reply.
//
// It also counts what is alive. `ImagePool`'s whole reason for existing is that
// no more than two bitmaps exist at once, and a stand-in that did not model a
// bitmap could not assert it.

import type { WorkerLike } from "../../src/state/workers/pool.js";

type Handler = (event: Event) => void;

// A handle that has to be closed, standing in for an `ImageBitmap`. The pool
// never sees one — it lives in the worker — so this is what the WORKER side
// would be holding, and the room below is what knows how many there are.
export class FakeBitmap {
    closed = false;

    // An `ImageBitmap` has dimensions, and the worker body reads them to decide
    // what it is scaling to. They default so that every caller that only cares
    // about the handle being closed passes nothing.
    constructor(
        private readonly room: WorkerRoom,
        readonly width = 1,
        readonly height = 1,
    ) {
        room.alive += 1;
        room.peakAlive = Math.max(room.peakAlive, room.alive);
    }

    close(): void {
        if (this.closed) {
            return;
        }
        this.closed = true;
        this.room.alive -= 1;
    }
}

export class FakeWorker implements WorkerLike {
    readonly received: unknown[] = [];
    terminated = false;

    private readonly listeners = new Map<string, Set<Handler>>();

    constructor(private readonly room: WorkerRoom) {}

    postMessage = (message: unknown): void => {
        this.received.push(message);
        this.room.pending.push({ worker: this, message });
        this.room.wake();
    };

    addEventListener = (type: string, handler: Handler): void => {
        const held = this.listeners.get(type) ?? new Set<Handler>();
        held.add(handler);
        this.listeners.set(type, held);
    };

    removeEventListener = (type: string, handler: Handler): void => {
        this.listeners.get(type)?.delete(handler);
    };

    terminate = (): void => {
        this.terminated = true;
        this.room.living.delete(this);
    };

    // Delivers a reply the way the platform does: as an event, asynchronously.
    emit(type: string, data: unknown): void {
        const held = this.listeners.get(type);
        if (held === undefined) {
            return;
        }
        const event = { type, data } as unknown as Event;
        for (const handler of Array.from(held)) {
            handler(event);
        }
    }
}

type Pending = { readonly worker: FakeWorker; readonly message: unknown };

// Every worker one pool made, and every task they have been given.
export class WorkerRoom {
    readonly living = new Set<FakeWorker>();
    readonly pending: Pending[] = [];

    // What the memory assertion reads.
    alive = 0;
    peakAlive = 0;

    spawned = 0;

    private waiters: (() => void)[] = [];

    create = (): FakeWorker => {
        this.spawned += 1;
        const worker = new FakeWorker(this);
        this.living.add(worker);
        return worker;
    };

    wake(): void {
        const held = this.waiters;
        this.waiters = [];
        for (const waiter of held) {
            waiter();
        }
    }

    // Answers every task in flight, in order, as a real worker would: one
    // message event carrying the pool's envelope.
    answerAll(reply: (message: unknown) => { readonly ok: boolean; readonly value?: unknown }): void {
        const held = this.pending.splice(0, this.pending.length);
        for (const { worker, message } of held) {
            const envelope = message as { readonly tag: string; readonly id: number };
            const answer = reply(message);
            worker.emit("message", {
                tag: envelope.tag,
                id: envelope.id,
                ok: answer.ok,
                value: answer.value ?? null,
            });
        }
    }

    // Fails every task in flight the way a worker that threw out of its handler
    // does: an `error` event and no reply.
    failAll(): void {
        const held = this.pending.splice(0, this.pending.length);
        for (const { worker } of held) {
            worker.emit("error", null);
        }
    }
}

// Enough turns of the microtask queue for a dispatch to have happened.
export async function settled(turns = 8): Promise<void> {
    for (let i = 0; i < turns; i += 1) {
        await Promise.resolve();
    }
}
