// Bounded in-flight, FIFO, and a full queue sheds rather than grows.
//
// HTTP/2 accepts a hundred concurrent streams without complaint, which is how a
// list view fires a hundred requests and the one the person is actually waiting
// for arrives last (`docs/00-architecture.md` §3). A bound turns that into a
// queue whose head is the oldest request, which is the one closest to being
// useful.
//
// --- why it sheds ------------------------------------------------------------
//
// An unbounded client queue turns a slow network into an unbounded memory
// growth curve: every waiting request holds its body, its parameters and the
// closure of whatever screen made it, and none of them are released while the
// network is the thing that is slow. That is the browser's version of the OOM
// kill anvil's bounded queues exist to prevent, and it is worse here because the
// tab is what dies — with whatever the person had typed in it.
//
// So the waiting list has a ceiling and the request past it is refused locally.
// A screen that can reach the ceiling is a screen doing an N+1 over the network
// (`ENGINEERING_RULES.md` §7), and one refused request is a better report of that than a
// tab that gets slower for ninety seconds.
//
// --- the slot is transferred, never released and re-taken -------------------
//
// A release that decremented a counter and then woke a waiter would let a
// caller arriving in between take the slot the waiter was just promised, which
// is a queue that is FIFO except under the load that makes the order matter.
// The slot moves directly from the finishing call to the head of the line, and
// the count only falls when nobody is waiting for it.

import type { ClientError, TransportError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

export type QueueConfig = {
    // Concurrent requests to one origin. Six is the number browsers themselves
    // used for HTTP/1.1 connections per host, and it remains a reasonable
    // ceiling on how many answers a screen can use at once.
    readonly maxInFlight: number;

    // Requests allowed to wait. Past this the queue sheds.
    readonly maxWaiting: number;
};

export const kDefaultQueue: QueueConfig = { maxInFlight: 6, maxWaiting: 64 };

// Monotonic. A queue wait measured on the wall clock is a negative number the
// moment the operating system corrects the device's time.
export type MonotonicClock = () => number;

const kShed: ClientError = { kind: "client", cause: "queue-full", retryAfterMs: null };
const kAborted: TransportError = { kind: "transport", cause: "aborted" };

type Slot = "granted" | "aborted";

type Waiter = {
    readonly grant: (slot: Slot) => void;
    readonly signal: AbortSignal;
    readonly onAbort: () => void;
};

export type QueuePressure = {
    readonly inFlight: number;
    readonly waiting: number;
};

export class RequestQueue {
    private readonly config: QueueConfig;
    private readonly now: MonotonicClock;
    private readonly waiting: Waiter[];
    private inFlight: number;

    constructor(config: QueueConfig = kDefaultQueue, now: MonotonicClock = () => performance.now()) {
        this.config = config;
        this.now = now;
        this.waiting = [];
        this.inFlight = 0;
    }

    pressure(): QueuePressure {
        return { inFlight: this.inFlight, waiting: this.waiting.length };
    }

    // Runs the task once a slot is free, or reports why it will not.
    //
    // The wait is handed to the task rather than returned beside its value,
    // because the record it belongs in is written by the task's own caller and a
    // second return channel would be one more thing to thread through every
    // layer between them (`docs/00-architecture.md` §9).
    async run<T>(
        task: (queueWaitMs: number) => Promise<T>,
        signal: AbortSignal,
    ): Promise<Result<T, ClientError | TransportError>> {
        if (signal.aborted) {
            return fail(kAborted);
        }

        const enteredAtMs = this.now();
        const slot = this.take(signal);
        const granted = slot === "granted" ? "granted" : await slot;

        if (granted === "shed") {
            return fail(kShed);
        }
        if (granted === "aborted") {
            return fail(kAborted);
        }

        try {
            // The signal can fire between joining the queue and reaching the
            // front of it, and by then the slot is already held.
            if (signal.aborted) {
                return fail(kAborted);
            }
            return ok(await task(this.now() - enteredAtMs));
        } finally {
            this.release();
        }
    }

    private take(signal: AbortSignal): "granted" | Promise<Slot | "shed"> {
        if (this.inFlight < this.config.maxInFlight) {
            this.inFlight += 1;
            return "granted";
        }
        if (this.waiting.length >= this.config.maxWaiting) {
            return Promise.resolve("shed");
        }

        return new Promise<Slot>((resolve) => {
            const waiter: Waiter = {
                grant: resolve,
                signal,
                onAbort: () => {
                    const at = this.waiting.indexOf(waiter);
                    if (at >= 0) {
                        this.waiting.splice(at, 1);
                        signal.removeEventListener("abort", waiter.onAbort);
                        resolve("aborted");
                    }
                },
            };
            this.waiting.push(waiter);
            signal.addEventListener("abort", waiter.onAbort, { once: true });
        });
    }

    private release(): void {
        const next = this.waiting.shift();
        if (next === undefined) {
            this.inFlight -= 1;
            return;
        }
        // The slot moves; the count does not change. See the header comment.
        next.signal.removeEventListener("abort", next.onAbort);
        next.grant("granted");
    }
}
