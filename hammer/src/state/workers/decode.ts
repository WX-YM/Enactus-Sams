// Parsing something large off the main thread.
//
// `JSON.parse` on a megabyte is tens of milliseconds on a mid-range phone, and a
// frame is 16.7 ms of which script's share is about eight
// (`docs/00-architecture.md` §3). A parse that long during a scroll is a dropped
// frame, which is the performance defect people actually report.
//
// --- the sizing rule, and why it is not the image pool's ----------------------
//
// `min(hardwareConcurrency - 1, 2)`. This pool's cost is TIME rather than
// memory: a parse holds a string and the object it builds, not a 48 MB bitmap,
// so the thing being bounded is how much of the device is taken away from the
// thread that renders. Minus one leaves the main thread a core; the ceiling of
// two is because a device reporting sixteen cores is usually reporting sixteen
// hardware threads on a machine already doing something else, and a pool sized
// from that number schedules against itself.
//
// And because the cost is time rather than memory, this pool MAY queue, where
// `imagePool` may not. A queued parse is a parse that happens a moment later; a
// queued decode of a photograph is a file handle that becomes 48 MB the moment a
// worker picks it up.
//
// --- what comes back ---------------------------------------------------------
//
// A structured clone of the parsed value, which is a copy — the one place in
// this library where a large allocation crosses a thread boundary. It is still
// the right trade: the alternative is the same allocation on the thread that is
// trying to render, plus the parse.

import type { Result } from "../../core/result.js";

import type { CountSink } from "../counts.js";

import type { PoolError, WorkerFactory, WorkerScope } from "./pool.js";
import { WorkerPool, servePool } from "./pool.js";

// Enough that a second export does not wait behind the first, and not so many
// that they wait behind each other. See the header for why it is not larger.
export const kDecodeCeiling = 2;

// Deep enough to absorb a screen that asks for several at once, and bounded so
// that a screen that asks for forty is told no rather than growing the tab.
const kDecodeWaiting = 8;

export function decodeWorkers(hardwareConcurrency: number): number {
    if (!Number.isFinite(hardwareConcurrency) || hardwareConcurrency < 2) {
        // A device that reports one core, or reports nothing. One worker still
        // gets the parse off the thread that renders, which is the whole point;
        // zero would mean doing it on that thread instead.
        return 1;
    }
    return Math.min(Math.floor(hardwareConcurrency) - 1, kDecodeCeiling);
}

export type DecodeTask = {
    // The text to parse. A string crossing to a worker is copied, which is the
    // cost this pool is buying its way out of a dropped frame with.
    readonly text: string;
};

export class DecodePool {
    private readonly pool: WorkerPool;

    constructor(config: {
        readonly create: WorkerFactory;

        // Injected rather than read off `navigator`, because the state layer
        // reaches no global (`tools/check-layering.sh`) and because a test that
        // could not choose this number could not test the sizing rule.
        readonly hardwareConcurrency: number;

        readonly count?: CountSink;
    }) {
        this.pool = new WorkerPool({
            create: config.create,
            size: decodeWorkers(config.hardwareConcurrency),
            maxWaiting: kDecodeWaiting,
            ...(config.count === undefined ? {} : { count: config.count }),
        });
    }

    get inFlight(): number {
        return this.pool.inFlight;
    }

    get rejections(): number {
        return this.pool.rejections;
    }

    // A rejection here is the caller's cue to ask for a smaller page, which is
    // the one thing only the caller can do (`docs/00-architecture.md` §3).
    async decode<T>(task: DecodeTask, signal: AbortSignal): Promise<Result<T, PoolError>> {
        return await this.pool.run<T>({ message: task, signal });
    }

    close(): void {
        this.pool.close();
    }
}

// The worker side. Two lines in the application's worker entry, for the reason
// `serveImagePool` gives.
export function serveDecodePool(scope: WorkerScope): void {
    servePool(scope, async (task) => {
        const { text } = task as DecodeTask;
        // No `reviver`. A reviver runs a function per key over the whole
        // document, which is the parse done twice — and the shape of a response
        // is the application's, so there is nothing here that knows what a date
        // field is called (`docs/01-seams.md` §4).
        return { value: JSON.parse(text) as unknown };
    });
}
