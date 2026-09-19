// The pools, and the two properties that make them worth having: a bound that
// refuses rather than grows, and a promise that always settles.
//
// The image pool's assertion is the one the phase gate names — at most two
// bitmaps, each closed on consume — and it is asserted against a stand-in that
// counts live handles, because "48 MB times three kills the tab" is not a thing
// a unit test can observe directly.

import { describe, expect, it } from "vitest";

import { WorkerPool, servePool } from "../../src/state/workers/pool.js";
import type { WorkerScope } from "../../src/state/workers/pool.js";
import { ImagePool, kImageWorkers } from "../../src/state/workers/image.js";
import { DecodePool, decodeWorkers, serveDecodePool } from "../../src/state/workers/decode.js";
import type { StateCount } from "../../src/state/counts.js";
import { FakeBitmap, WorkerRoom, settled } from "../support/fake_worker.js";

function signal(): AbortSignal {
    return new AbortController().signal;
}

describe("WorkerPool", () => {
    it("spawns no more workers than its size", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 2, maxWaiting: 4 });

        void pool.run({ message: 1, signal: signal() });
        void pool.run({ message: 2, signal: signal() });
        void pool.run({ message: 3, signal: signal() });
        await settled();

        expect(room.spawned).toBe(2);
        expect(pool.inFlight).toBe(2);
        expect(pool.queued).toBe(1);
    });

    it("spawns lazily, so an unused pool costs no threads", () => {
        const room = new WorkerRoom();
        new WorkerPool({ create: room.create, size: 2, maxWaiting: 0 });

        expect(room.spawned).toBe(0);
    });

    it("rejects rather than growing when the queue is full", async () => {
        const room = new WorkerRoom();
        const counts: StateCount[] = [];
        const pool = new WorkerPool({
            create: room.create,
            size: 1,
            maxWaiting: 1,
            count: (name) => counts.push(name),
        });

        void pool.run({ message: 1, signal: signal() });
        await settled();
        void pool.run({ message: 2, signal: signal() });
        const shed = await pool.run({ message: 3, signal: signal() });

        expect(shed.ok).toBe(false);
        expect(shed.ok ? null : shed.error).toEqual({ kind: "pool", cause: "queue-full" });
        expect(counts).toContain("pool-rejected");
        expect(pool.rejections).toBe(1);
    });

    it("settles a rejected task rather than leaving a promise nobody resolves", async () => {
        // An unsettled promise is a spinner that never stops, which is worse
        // than an error.
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 0 });

        void pool.run({ message: 1, signal: signal() });
        await settled();

        await expect(pool.run({ message: 2, signal: signal() })).resolves.toMatchObject({
            ok: false,
        });
    });

    it("hands a reply back to the task that asked for it", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 2 });

        const running = pool.run<number>({ message: { n: 2 }, signal: signal() });
        await settled();
        room.answerAll((message) => ({
            ok: true,
            value: ((message as { readonly task: { readonly n: number } }).task.n) * 10,
        }));

        await expect(running).resolves.toEqual({ ok: true, value: 20 });
    });

    it("runs a queued task once a worker is free", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 2 });

        const first = pool.run<number>({ message: 1, signal: signal() });
        await settled();
        const second = pool.run<number>({ message: 2, signal: signal() });

        room.answerAll(() => ({ ok: true, value: 1 }));
        await first;
        await settled();
        room.answerAll(() => ({ ok: true, value: 2 }));

        await expect(second).resolves.toEqual({ ok: true, value: 2 });
    });

    it("ignores a message that is not a reply to a task it dispatched", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 0 });

        const running = pool.run<number>({ message: 1, signal: signal() });
        await settled();
        const worker = Array.from(room.living)[0];

        // A browser extension, another library on the same worker, or a reply
        // to a task that was already aborted.
        worker?.emit("message", { tag: "somebody.else", id: 1, ok: true, value: 99 });
        worker?.emit("message", { tag: "hammer.pool", id: 77, ok: true, value: 99 });
        worker?.emit("message", null);
        await settled();

        room.answerAll(() => ({ ok: true, value: 5 }));
        await expect(running).resolves.toEqual({ ok: true, value: 5 });
    });

    it("settles and replaces a worker that threw out of its handler", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 2 });

        const running = pool.run({ message: 1, signal: signal() });
        await settled();
        room.failAll();

        await expect(running).resolves.toMatchObject({
            ok: false,
            error: { kind: "pool", cause: "task-failed" },
        });

        // A worker in an unknown state is one that will fail the task after this
        // one too, so it is terminated and the next task gets a fresh one.
        const next = pool.run({ message: 2, signal: signal() });
        await settled();
        expect(room.spawned).toBe(2);
        room.answerAll(() => ({ ok: true, value: null }));
        await expect(next).resolves.toMatchObject({ ok: true });
    });

    it("terminates the worker when a task is abandoned", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 0 });
        const controller = new AbortController();

        const running = pool.run({ message: 1, signal: controller.signal });
        await settled();
        const worker = Array.from(room.living)[0];
        controller.abort();

        // A worker cannot be told to stop mid-decode, and a decode nobody is
        // waiting for still holds its memory until it finishes.
        await expect(running).resolves.toMatchObject({
            ok: false,
            error: { kind: "pool", cause: "aborted" },
        });
        expect(worker?.terminated).toBe(true);
    });

    it("refuses a task whose signal was already aborted", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 0 });
        const controller = new AbortController();
        controller.abort();

        await expect(pool.run({ message: 1, signal: controller.signal })).resolves.toMatchObject({
            ok: false,
            error: { kind: "pool", cause: "aborted" },
        });
        expect(room.spawned).toBe(0);
    });

    it("settles a queued task whose caller gave up before it ran", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 2 });
        const controller = new AbortController();

        const first = pool.run({ message: 1, signal: signal() });
        await settled();
        const queued = pool.run({ message: 2, signal: controller.signal });
        controller.abort();

        room.answerAll(() => ({ ok: true, value: null }));
        await first;
        await settled();

        await expect(queued).resolves.toMatchObject({
            ok: false,
            error: { kind: "pool", cause: "aborted" },
        });
    });

    it("close settles everything outstanding and terminates every worker", async () => {
        const room = new WorkerRoom();
        const pool = new WorkerPool({ create: room.create, size: 1, maxWaiting: 2 });

        const running = pool.run({ message: 1, signal: signal() });
        await settled();
        const queued = pool.run({ message: 2, signal: signal() });

        pool.close();

        // A pool that dropped its queue on close would leave a screen spinning
        // on an answer that will never come.
        await expect(running).resolves.toMatchObject({ error: { cause: "closed" } });
        await expect(queued).resolves.toMatchObject({ error: { cause: "closed" } });
        expect(Array.from(room.living)).toHaveLength(0);
    });

    it("refuses a pool that has no workers", () => {
        const room = new WorkerRoom();
        expect(() => new WorkerPool({ create: room.create, size: 0, maxWaiting: 1 })).toThrow();
    });
});

describe("servePool", () => {
    // The worker half, driven directly: it is the only place the reply envelope
    // is written, and a task body that does not catch is an unhandled rejection
    // in a context nobody is watching.
    function scope() {
        const posted: unknown[] = [];
        let onMessage: ((event: Event) => void) | null = null;
        const held: WorkerScope = {
            addEventListener: (_type, handler) => {
                onMessage = handler;
            },
            postMessage: (message) => posted.push(message),
        };
        return {
            scope: held,
            posted,
            deliver: (data: unknown) => onMessage?.({ data } as unknown as Event),
        };
    }

    it("answers a task with the value its handler produced", async () => {
        const room = scope();
        servePool(room.scope, async (task) => ({ value: (task as number) + 1 }));

        room.deliver({ tag: "hammer.pool", id: 4, task: 1 });
        await settled();

        expect(room.posted).toEqual([{ tag: "hammer.pool", id: 4, ok: true, value: 2 }]);
    });

    it("answers rather than rejecting when the handler throws", async () => {
        const room = scope();
        servePool(room.scope, async () => {
            throw new Error("thrown inside the worker");
        });

        room.deliver({ tag: "hammer.pool", id: 4, task: 1 });
        await settled();

        expect(room.posted).toEqual([{ tag: "hammer.pool", id: 4, ok: false, value: null }]);
    });

    it("ignores a message that is not a task", async () => {
        const room = scope();
        servePool(room.scope, async () => ({ value: 1 }));

        room.deliver({ tag: "somebody.else", id: 4, task: 1 });
        room.deliver({ tag: "hammer.pool", id: "four", task: 1 });
        room.deliver(null);
        await settled();

        expect(room.posted).toEqual([]);
    });
});

describe("ImagePool", () => {
    it("holds at most two bitmaps and closes each on consume", async () => {
        // 4032 × 3024 × 4 ≈ 48 MB per decoded bitmap. Three concurrently is
        // 144 MB against a tab budget around 350 MB, and the failure is a killed
        // tab rather than a slow one.
        const room = new WorkerRoom();
        const pool = new ImagePool({ create: room.create });
        const file = new Blob(["x"]);
        const request = { file, maxEdgeCssPx: 1024, type: "image/webp" } as const;

        const running = [
            pool.downscale(request, signal()),
            pool.downscale(request, signal()),
            pool.downscale(request, signal()),
        ];
        await settled();

        expect(kImageWorkers).toBe(2);
        expect(pool.bitmapsHeld).toBe(2);

        // The third was refused at the door rather than queued: a queue of
        // photographs is a queue of handles that each become 48 MB.
        await expect(running[2]).resolves.toMatchObject({
            ok: false,
            error: { cause: "queue-full" },
        });

        // The worker decodes, and closes on the way out.
        const held: FakeBitmap[] = [];
        room.answerAll(() => {
            const bitmap = new FakeBitmap(room);
            held.push(bitmap);
            bitmap.close();
            return { ok: true, value: { blob: file, widthCssPx: 1024, heightCssPx: 768 } };
        });
        await settled();

        expect(room.peakAlive).toBeLessThanOrEqual(kImageWorkers);
        expect(held.every((bitmap) => bitmap.closed)).toBe(true);
        expect(pool.bitmapsHeld).toBe(0);
        await expect(running[0]).resolves.toMatchObject({ ok: true });

        pool.close();
    });
});

describe("DecodePool", () => {
    it("sizes itself from the cores the device reports, with a ceiling", () => {
        // A device reporting sixteen is usually reporting sixteen hardware
        // threads on a machine already doing something else.
        expect(decodeWorkers(16)).toBe(2);
        expect(decodeWorkers(4)).toBe(2);
        expect(decodeWorkers(3)).toBe(2);
        expect(decodeWorkers(2)).toBe(1);
        // One core, or a device that reports nothing. One worker still gets the
        // parse off the thread that renders.
        expect(decodeWorkers(1)).toBe(1);
        expect(decodeWorkers(Number.NaN)).toBe(1);
    });

    it("queues, where the image pool refuses", async () => {
        const room = new WorkerRoom();
        const pool = new DecodePool({ create: room.create, hardwareConcurrency: 2 });

        void pool.decode({ text: "[1]" }, signal());
        await settled();
        void pool.decode({ text: "[2]" }, signal());
        await settled();

        // A queued parse happens a moment later. This pool's cost is time, not
        // memory.
        expect(pool.inFlight).toBe(1);
        expect(pool.rejections).toBe(0);
        pool.close();
    });

    it("parses off the thread and hands the value back", async () => {
        const room = new WorkerRoom();
        const pool = new DecodePool({ create: room.create, hardwareConcurrency: 4 });

        const running = pool.decode<readonly number[]>({ text: "[1,2,3]" }, signal());
        await settled();
        room.answerAll((message) => {
            const { task } = message as { readonly task: { readonly text: string } };
            return { ok: true, value: JSON.parse(task.text) as unknown };
        });

        await expect(running).resolves.toEqual({ ok: true, value: [1, 2, 3] });
        pool.close();
    });

    it("reports a parse failure rather than throwing out of the worker", async () => {
        // A handler that rejected would leave the promise on the other side
        // unsettled, which is the spinner that never stops.
        const room = {
            posted: [] as unknown[],
            handler: null as ((event: Event) => void) | null,
        };
        serveDecodePool({
            addEventListener: (_type, handler) => {
                room.handler = handler;
            },
            postMessage: (message) => room.posted.push(message),
        });

        room.handler?.({
            data: { tag: "hammer.pool", id: 1, task: { text: "{" } },
        } as unknown as Event);
        await settled();

        expect(room.posted).toEqual([{ tag: "hammer.pool", id: 1, ok: false, value: null }]);
    });
});
