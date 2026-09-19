// Downscaling a photograph before it is uploaded, without killing the tab.
//
// **The size of this pool is a memory cap, not a tuning knob.** A 12 MP photo
// decoded to RGBA is 4032 × 3024 × 4 ≈ 48 MB. Three of those concurrently is
// 144 MB against a tab budget frequently around 350 MB on a mid-range phone, and
// the failure mode is not slowness — the tab is killed, with whatever the person
// had typed in it. Two workers, one bitmap each, and `close()` on the instant it
// is consumed (`docs/00-architecture.md` §3).
//
// The queue in front of it is ZERO deep, and that is the same decision. A pool
// whose cost is time may queue; a pool whose cost is memory may not, because a
// queue of photographs is a queue of file handles that each become 48 MB the
// moment they reach a worker. A third concurrent request is refused and the
// caller decides — one at a time, a smaller target, or a refusal on screen.
//
// --- what crosses the boundary, in each direction ---------------------------
//
// OUT: the `File`. A `File` is a handle, not bytes, until something reads it, so
// posting one costs nothing and the original never enters any JS heap
// (`ENGINEERING_RULES.md` §2.2). There is no `readAsArrayBuffer` in this library and a
// script fails the build if one appears.
//
// BACK: a `Blob`, which is also a handle. The re-encoded image is the one piece
// of binary this library produces, and producing it is the entire point: it is
// smaller than the original, which is how a person on a congested network
// uploads a photograph at all. It goes straight to `client.upload` as the body
// and is never read.

import type { Result } from "../../core/result.js";
import { fail, ok } from "../../core/result.js";

import type { CountSink } from "../counts.js";

import type { PoolError, WorkerFactory, WorkerScope } from "./pool.js";
import { WorkerPool, kBadRequest, servePool } from "./pool.js";

// Two. It is here as a constant with its arithmetic beside it so that raising it
// is a change somebody has to argue with rather than a number in a config file.
//
//   4032 × 3024 × 4 bytes ≈ 48 MB per decoded bitmap
//   × 2 workers            ≈ 96 MB
//   × 3 workers            ≈ 144 MB, against ~350 MB of tab
export const kImageWorkers = 2;

// Never queue. See the header.
const kImageWaiting = 0;

export type DownscaleRequest = {
    readonly file: Blob;

    // CSS pixels, named in the field (`ENGINEERING_RULES.md` §10). The image is scaled to
    // fit within this on its longest edge and is never scaled UP: enlarging a
    // small image produces a larger file with no more detail in it, which is the
    // opposite of what this pool is for.
    readonly maxEdgeCssPx: number;

    // The media type to re-encode as, and the quality for a lossy one. Both are
    // the application's: which formats a namespace accepts is the server's
    // business and is not in the descriptor yet (`docs/15-tasks.md` §Cross-repo).
    readonly type: string;
    readonly quality?: number;
};

export type DownscaleResult = {
    readonly blob: Blob;
    readonly widthCssPx: number;
    readonly heightCssPx: number;
};

export class ImagePool {
    private readonly pool: WorkerPool;

    constructor(config: { readonly create: WorkerFactory; readonly count?: CountSink }) {
        this.pool = new WorkerPool({
            create: config.create,
            size: kImageWorkers,
            maxWaiting: kImageWaiting,
            ...(config.count === undefined ? {} : { count: config.count }),
        });
    }

    // How many bitmaps exist right now. It is the assertion the memory test
    // makes, and it is a property of the pool rather than a number this class
    // keeps beside it: one task holds one bitmap, so the two cannot disagree.
    get bitmapsHeld(): number {
        return this.pool.inFlight;
    }

    get rejections(): number {
        return this.pool.rejections;
    }

    async downscale(
        request: DownscaleRequest,
        signal: AbortSignal,
    ): Promise<Result<DownscaleResult, PoolError>> {
        const answered = await this.pool.run<DownscaleResult>({ message: request, signal });
        if (!answered.ok) {
            return answered;
        }
        return ok(answered.value);
    }

    close(): void {
        this.pool.close();
    }
}

// The worker side. An application's worker entry is two lines:
//
//     import { serveImagePool } from "hammer/state";
//     serveImagePool(self);
//
// A function rather than a module with a top-level listener, because this
// library has no top-level side effects (`ENGINEERING_RULES.md` §2.1) — and because a
// worker entry is the application's file to name, since only its bundler can
// turn one into a URL.
export function serveImagePool(scope: WorkerScope): void {
    servePool(scope, async (task) => {
        const request = task as DownscaleRequest;
        const source = await createImageBitmap(request.file);

        // `close()` in a `finally`, so the 48 MB goes back whether the encode
        // succeeded, threw or was interrupted. A bitmap released only on the
        // happy path is a leak that appears exactly when the device is already
        // struggling, which is when the encode is most likely to fail.
        try {
            const scale = Math.min(
                1,
                request.maxEdgeCssPx / Math.max(source.width, source.height),
            );
            const widthCssPx = Math.max(1, Math.round(source.width * scale));
            const heightCssPx = Math.max(1, Math.round(source.height * scale));

            const canvas = new OffscreenCanvas(widthCssPx, heightCssPx);
            const context = canvas.getContext("2d");
            if (context === null) {
                throw new Error("no 2d context in this worker");
            }
            context.drawImage(source, 0, 0, widthCssPx, heightCssPx);

            const blob = await canvas.convertToBlob({
                type: request.type,
                ...(request.quality === undefined ? {} : { quality: request.quality }),
            });
            const answer: DownscaleResult = { blob, widthCssPx, heightCssPx };
            return { value: answer };
        } finally {
            source.close();
        }
    });
}

// The one refusal this module makes on the main thread, so a caller that is
// about to ask for something impossible is told before a worker is spawned.
export function checkDownscale(request: DownscaleRequest): Result<DownscaleRequest, PoolError> {
    if (!Number.isFinite(request.maxEdgeCssPx) || request.maxEdgeCssPx < 1) {
        return fail(kBadRequest);
    }
    return ok(request);
}
