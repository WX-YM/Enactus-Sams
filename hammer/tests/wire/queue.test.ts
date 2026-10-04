// The bound, the order, and the shed.
//
// Every task here is a promise the test resolves by hand rather than a timer:
// what is being asserted is how many things are running at once, which a delay
// would only approximate.

import { describe, expect, it } from "../support/test.js";

import { RequestQueue, kDefaultQueue } from "../../src/wire/queue.js";

// A task whose completion the test controls, and which records the queue wait
// it was handed.
function pending() {
    let settle = (): void => {};
    const done = new Promise<void>((resolve) => {
        settle = resolve;
    });
    let waitedMs = -1;
    const task = async (queueWaitMs: number): Promise<string> => {
        waitedMs = queueWaitMs;
        await done;
        return "answered";
    };
    return {
        task,
        settle: () => settle(),
        get waitedMs() {
            return waitedMs;
        },
    };
}

function signal(): AbortSignal {
    return new AbortController().signal;
}

const kTwo = { maxInFlight: 2, maxWaiting: 2 };

describe("the bound", () => {
    it("runs up to the in-flight ceiling at once", async () => {
        const queue = new RequestQueue(kTwo);
        const first = pending();
        const second = pending();

        void queue.run(first.task, signal());
        void queue.run(second.task, signal());
        await Promise.resolve();

        expect(queue.pressure()).toEqual({ inFlight: 2, waiting: 0 });
    });

    it("holds the rest back rather than sending them", async () => {
        const queue = new RequestQueue(kTwo);
        const running = [pending(), pending()];
        const held = pending();

        running.forEach((one) => void queue.run(one.task, signal()));
        void queue.run(held.task, signal());
        await Promise.resolve();

        expect(queue.pressure()).toEqual({ inFlight: 2, waiting: 1 });
        expect(held.waitedMs).toBe(-1);
    });

    it("admits the head of the line when a slot frees", async () => {
        const queue = new RequestQueue(kTwo);
        const running = [pending(), pending()];
        const queued = pending();

        const answers = running.map((one) => queue.run(one.task, signal()));
        void queue.run(queued.task, signal());
        await Promise.resolve();

        running[0]?.settle();
        await answers[0];
        await Promise.resolve();

        expect(queued.waitedMs).toBeGreaterThanOrEqual(0);
        expect(queue.pressure()).toEqual({ inFlight: 2, waiting: 0 });
    });

    // A release that decremented a counter and then woke a waiter would let a
    // caller arriving in between take the slot the waiter was promised — a queue
    // that is FIFO except under the load that makes the order matter.
    it("hands the slot to the waiter rather than to a caller arriving in between", async () => {
        const queue = new RequestQueue(kTwo);
        const running = [pending(), pending()];
        const queued = pending();
        const latecomer = pending();

        const answers = running.map((one) => queue.run(one.task, signal()));
        void queue.run(queued.task, signal());
        await Promise.resolve();

        running[0]?.settle();
        await answers[0];
        void queue.run(latecomer.task, signal());
        await Promise.resolve();

        expect(queued.waitedMs).toBeGreaterThanOrEqual(0);
        expect(latecomer.waitedMs).toBe(-1);
    });
});

describe("what a full queue does", () => {
    // An unbounded client queue turns a slow network into an unbounded memory
    // curve, and the tab is what dies.
    it("sheds locally instead of growing", async () => {
        const queue = new RequestQueue(kTwo);
        const running = [pending(), pending()];
        const queued = [pending(), pending()];

        running.forEach((one) => void queue.run(one.task, signal()));
        queued.forEach((one) => void queue.run(one.task, signal()));
        await Promise.resolve();

        const shed = pending();
        await expect(queue.run(shed.task, signal())).resolves.toEqual({
            ok: false,
            error: { kind: "client", cause: "queue-full", retryAfterMs: null },
        });
        expect(shed.waitedMs).toBe(-1);
    });
});

describe("an abort", () => {
    it("is reported without sending the request", async () => {
        const queue = new RequestQueue(kTwo);
        const controller = new AbortController();
        controller.abort();

        const never = pending();
        await expect(queue.run(never.task, controller.signal)).resolves.toEqual({
            ok: false,
            error: { kind: "transport", cause: "aborted" },
        });
        expect(never.waitedMs).toBe(-1);
    });

    it("leaves the queue while waiting, freeing the place it held", async () => {
        const queue = new RequestQueue(kTwo);
        const running = [pending(), pending()];
        const leaving = pending();
        const controller = new AbortController();

        running.forEach((one) => void queue.run(one.task, signal()));
        const answer = queue.run(leaving.task, controller.signal);
        await Promise.resolve();
        expect(queue.pressure()).toEqual({ inFlight: 2, waiting: 1 });

        controller.abort();
        await expect(answer).resolves.toMatchObject({ ok: false, error: { cause: "aborted" } });
        expect(queue.pressure()).toEqual({ inFlight: 2, waiting: 0 });
        expect(leaving.waitedMs).toBe(-1);
    });
});

describe("what the queue records", () => {
    it("hands the task how long it waited", async () => {
        let clock = 0;
        const queue = new RequestQueue(kTwo, () => clock);
        const running = [pending(), pending()];
        const queued = pending();

        const answers = running.map((one) => queue.run(one.task, signal()));
        void queue.run(queued.task, signal());

        clock = 250;
        running[0]?.settle();
        await answers[0];
        await Promise.resolve();

        expect(queued.waitedMs).toBe(250);
    });

    it("records no wait for a request that went straight out", async () => {
        const queue = new RequestQueue(kTwo, () => 1000);
        const first = pending();
        void queue.run(first.task, signal());
        await Promise.resolve();
        expect(first.waitedMs).toBe(0);
    });

    it("returns what the task answered", async () => {
        const queue = new RequestQueue(kTwo);
        const one = pending();
        const answer = queue.run(one.task, signal());
        one.settle();
        await expect(answer).resolves.toEqual({ ok: true, value: "answered" });
    });

    // A task that throws must still give its slot back: a queue that leaks one
    // per failure is a queue that stops admitting anything after six of them.
    it("frees the slot when the task throws", async () => {
        const queue = new RequestQueue(kTwo);
        const thrower = async (): Promise<never> => {
            throw new Error("the task threw");
        };
        await expect(queue.run(thrower, signal())).rejects.toThrow();
        expect(queue.pressure()).toEqual({ inFlight: 0, waiting: 0 });
    });
});

describe("the default this library ships", () => {
    // Six concurrent is the number browsers themselves used per host over
    // HTTP/1.1, and it stays the ceiling under HTTP/2 for the reason the module
    // gives: a protocol that accepts a hundred streams without complaint is how
    // a list view fires a hundred requests and the one the person is waiting for
    // arrives last.
    it("bounds in-flight at six and the wait at sixty-four", () => {
        expect(kDefaultQueue).toEqual({ maxInFlight: 6, maxWaiting: 64 });
    });

    it("is what a queue built with no configuration uses", async () => {
        // Against behaviour, because a constant nothing reads is a constant that
        // can be right while the default is wrong.
        const queue = new RequestQueue(kDefaultQueue);
        const release: (() => void)[] = [];
        const running: Promise<unknown>[] = [];
        const signal = new AbortController().signal;

        for (let i = 0; i < kDefaultQueue.maxInFlight; i += 1) {
            running.push(
                queue.run(async () => {
                    await new Promise<void>((resolve) => release.push(resolve));
                }, signal),
            );
        }
        for (let i = 0; i < 8; i += 1) {
            await Promise.resolve();
        }
        expect(release).toHaveLength(kDefaultQueue.maxInFlight);

        for (const open of release) {
            open();
        }
        await Promise.all(running);
    });
});
