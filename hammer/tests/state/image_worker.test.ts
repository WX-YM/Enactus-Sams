// The worker side of the image pool, executed.
//
// `tests/state/workers.test.ts` drives `ImagePool` — the bound, the refusal, the
// bitmap accounting — from the MAIN thread, against a worker stand-in that
// answers whatever the test tells it to. That is the right way to assert the
// pool, and it means the worker's own body had never run: `serveImagePool` was
// exported, documented as a two-line application entry point, and called by
// nothing (`tools/check-surface-coverage.sh`).
//
// What had therefore never been executed is the arithmetic that decides what
// the person actually uploads, and the `close()` that decides whether the tab
// survives doing it.
//
// --- the two globals this stands in for -------------------------------------
//
// `createImageBitmap` and `OffscreenCanvas` exist in a worker and in no Node.
// They are platform globals, so they are stood in for the way `fetch` already
// is in the wire suite: a real object speaking the real shape, recording what it
// was asked. The decode and the encode are the platform's and are not what is
// under test here; the geometry between them is entirely hammer's.

import { afterEach, beforeEach, describe, expect, it } from "../support/test.js";

import type { DownscaleRequest, DownscaleResult } from "../../src/state/workers/image.js";
import { serveImagePool } from "../../src/state/workers/image.js";
import { FakeBitmap, WorkerRoom, settled } from "../support/fake_worker.js";

// The envelope `servePool` reads, spelled here the way `workers.test.ts` spells
// it: the tag is private to the module and a test that imported it would be
// asserting the protocol against itself.
const kTag = "hammer.pool";

type Drawn = {
    readonly widthCssPx: number;
    readonly heightCssPx: number;
};

// What the worker draws into. It records the size it was constructed at and the
// size it was asked to draw, because those are two separate decisions in the
// body and only one of them is visible in the result.
class FakeCanvas {
    static readonly built: Drawn[] = [];
    static readonly drawn: Drawn[] = [];
    static readonly encoded: unknown[] = [];

    // Set by a case that wants the encode to fail, which is the path the
    // `finally` exists for.
    static encodeThrows = false;
    static noContext = false;

    constructor(
        readonly width: number,
        readonly height: number,
    ) {
        FakeCanvas.built.push({ widthCssPx: width, heightCssPx: height });
    }

    getContext(kind: string): { drawImage: (...args: readonly unknown[]) => void } | null {
        if (FakeCanvas.noContext || kind !== "2d") {
            return null;
        }
        return {
            drawImage: (...args: readonly unknown[]) => {
                FakeCanvas.drawn.push({
                    widthCssPx: Number(args[3] ?? 0),
                    heightCssPx: Number(args[4] ?? 0),
                });
            },
        };
    }

    async convertToBlob(options: unknown): Promise<Blob> {
        FakeCanvas.encoded.push(options);
        if (FakeCanvas.encodeThrows) {
            throw new Error("the encoder gave up");
        }
        return new Blob(["encoded"], { type: "image/webp" });
    }

    static reset(): void {
        FakeCanvas.built.length = 0;
        FakeCanvas.drawn.length = 0;
        FakeCanvas.encoded.length = 0;
        FakeCanvas.encodeThrows = false;
        FakeCanvas.noContext = false;
    }
}

// The room is what counts live bitmaps, which is the assertion the whole pool
// exists for: a 12 MP photo is 48 MB of RGBA, and one that is not closed is
// 48 MB held until the tab is killed.
let room: WorkerRoom;
let bitmap: FakeBitmap | undefined;

type Scope = {
    readonly posted: unknown[];
    readonly deliver: (task: DownscaleRequest, id?: number) => void;
};

function serving(): Scope {
    const posted: unknown[] = [];
    let handler: ((event: Event) => void) | null = null;

    serveImagePool({
        addEventListener: (_type, given) => {
            handler = given;
        },
        postMessage: (message) => posted.push(message),
    });

    return {
        posted,
        deliver: (task, id = 1) => {
            handler?.({ data: { tag: kTag, id, task } } as unknown as Event);
        },
    };
}

// The bitmap the next decode resolves to. Set per case, because its dimensions
// are the input to every geometry assertion below.
//
// The one it replaces is closed here rather than left alive: `room.alive` is the
// assertion that the worker released its 48 MB, and a bitmap this harness made
// and abandoned would sit in that count looking exactly like one the worker
// leaked.
function source(widthCssPx: number, heightCssPx: number): void {
    bitmap?.close();
    bitmap = new FakeBitmap(room, widthCssPx, heightCssPx);
}

const kOriginals: Record<string, unknown> = {};

beforeEach(() => {
    room = new WorkerRoom();
    bitmap = undefined;
    source(1, 1);
    FakeCanvas.reset();

    kOriginals["createImageBitmap"] = Reflect.get(globalThis, "createImageBitmap");
    kOriginals["OffscreenCanvas"] = Reflect.get(globalThis, "OffscreenCanvas");
    Reflect.set(globalThis, "createImageBitmap", async () => bitmap);
    Reflect.set(globalThis, "OffscreenCanvas", FakeCanvas);
});

afterEach(() => {
    // Restored rather than left installed: a global a test file leaks is a
    // global the next file in the same worker inherits, which is the flake
    // nobody can reproduce alone.
    for (const name of ["createImageBitmap", "OffscreenCanvas"]) {
        const original = kOriginals[name];
        if (original === undefined) {
            Reflect.deleteProperty(globalThis, name);
        } else {
            Reflect.set(globalThis, name, original);
        }
    }
});

function request(over: Partial<DownscaleRequest> = {}): DownscaleRequest {
    return {
        file: new Blob(["original"], { type: "image/jpeg" }),
        maxEdgeCssPx: 1024,
        type: "image/webp",
        ...over,
    };
}

async function answer(scope: Scope): Promise<{ readonly ok: boolean; readonly value: unknown }> {
    await settled();
    const posted = scope.posted[0] as { readonly ok: boolean; readonly value: unknown };
    expect(posted).toBeDefined();
    return posted;
}

describe("the size the worker scales to", () => {
    // Every case states the bitmap it was handed and the box it was given, and
    // asserts the three numbers that follow. None of this had ever run: the
    // whole of it lives inside the task body, and every test above it answers
    // for the worker rather than being it.
    it("fits the longest edge of a landscape photograph into the box", async () => {
        // 4032 x 3024 is the 12 MP the module's own memory arithmetic is written
        // about, so it is the shape worth spelling.
        source(4032, 3024);
        const scope = serving();
        scope.deliver(request({ maxEdgeCssPx: 1024 }));

        const posted = await answer(scope);
        expect(posted.ok).toBe(true);
        expect(posted.value).toMatchObject({ widthCssPx: 1024, heightCssPx: 768 });
    });

    it("fits the longest edge of a portrait photograph, which is the other one", async () => {
        source(3024, 4032);
        const scope = serving();
        scope.deliver(request({ maxEdgeCssPx: 1024 }));

        expect((await answer(scope)).value).toMatchObject({
            widthCssPx: 768,
            heightCssPx: 1024,
        });
    });

    it("leaves a square square", async () => {
        source(2000, 2000);
        const scope = serving();
        scope.deliver(request({ maxEdgeCssPx: 500 }));

        expect((await answer(scope)).value).toMatchObject({
            widthCssPx: 500,
            heightCssPx: 500,
        });
    });

    // The guard that is easiest to lose in a refactor and costs the most: an
    // enlarged image is a bigger file with no more detail in it, which is the
    // opposite of what this pool is for. `Math.min(1, ...)` is what stops it.
    it("never scales an image UP to fill the box", async () => {
        source(320, 240);
        const scope = serving();
        scope.deliver(request({ maxEdgeCssPx: 1024 }));

        expect((await answer(scope)).value).toMatchObject({
            widthCssPx: 320,
            heightCssPx: 240,
        });
    });

    // A canvas of zero height throws on construction in a browser, so the guard
    // is not a rounding nicety — it is the difference between a panorama being
    // re-encoded and the upload failing with nothing on screen to explain it.
    it("never rounds the short edge of an extreme panorama to nothing", async () => {
        source(10_000, 3);
        const scope = serving();
        scope.deliver(request({ maxEdgeCssPx: 100 }));

        expect((await answer(scope)).value).toMatchObject({
            widthCssPx: 100,
            heightCssPx: 1,
        });
    });

    it("builds the canvas and draws at the same size it reports", async () => {
        // Three numbers that could drift apart: what the canvas was made at,
        // what was drawn into it, and what the caller is told it got. A result
        // that disagreed with the canvas is an image silently cropped.
        source(4032, 3024);
        const scope = serving();
        scope.deliver(request({ maxEdgeCssPx: 1024 }));

        const value = (await answer(scope)).value as DownscaleResult;
        const size = { widthCssPx: value.widthCssPx, heightCssPx: value.heightCssPx };
        expect(FakeCanvas.built).toEqual([size]);
        expect(FakeCanvas.drawn).toEqual([size]);
    });
});

describe("the bitmap the worker holds while it works", () => {
    // The 48 MB. `close()` is in a `finally` precisely so it runs on the paths
    // where something went wrong — which is when the device is already
    // struggling and the encode is most likely to be what failed.
    it("is closed after a successful encode", async () => {
        source(2000, 1000);
        const scope = serving();
        scope.deliver(request());

        await answer(scope);
        expect(bitmap?.closed).toBe(true);
        expect(room.alive).toBe(0);
    });

    it("is closed when the encode throws", async () => {
        source(2000, 1000);
        FakeCanvas.encodeThrows = true;
        const scope = serving();
        scope.deliver(request());

        expect((await answer(scope)).ok).toBe(false);
        expect(bitmap?.closed).toBe(true);
        expect(room.alive).toBe(0);
    });

    it("is closed when the worker cannot get a context at all", async () => {
        source(2000, 1000);
        FakeCanvas.noContext = true;
        const scope = serving();
        scope.deliver(request());

        expect((await answer(scope)).ok).toBe(false);
        expect(bitmap?.closed).toBe(true);
        expect(room.alive).toBe(0);
    });
});

describe("what the worker reports back", () => {
    it("answers a failure rather than leaving the caller waiting", async () => {
        // A handler that rejected would leave the promise on the other side
        // unsettled, and a UI that waits forever is worse than one that reports
        // an error (`CLAUDE.md` §4).
        FakeCanvas.encodeThrows = true;
        const scope = serving();
        scope.deliver(request(), 7);

        await settled();
        expect(scope.posted).toEqual([{ tag: kTag, id: 7, ok: false, value: null }]);
    });

    it("carries the quality only when the caller named one", async () => {
        // `exactOptionalPropertyTypes` is on, and the difference is real at the
        // platform: `{quality: undefined}` is not the same request as one with
        // no quality in it, and for a lossless type it is not a meaningful one
        // at all.
        const withoutQuality = serving();
        withoutQuality.deliver(request({ type: "image/png" }));
        await answer(withoutQuality);
        expect(FakeCanvas.encoded).toEqual([{ type: "image/png" }]);

        FakeCanvas.reset();
        const withQuality = serving();
        withQuality.deliver(request({ type: "image/webp", quality: 0.8 }));
        await answer(withQuality);
        expect(FakeCanvas.encoded).toEqual([{ type: "image/webp", quality: 0.8 }]);
    });

    it("hands back a Blob, which is a handle and not bytes", async () => {
        // The one piece of binary this library produces. It goes straight to
        // `client.upload` as the body and is never read (`CLAUDE.md` §2.2).
        const scope = serving();
        scope.deliver(request());

        const value = (await answer(scope)).value as DownscaleResult;
        expect(value.blob).toBeInstanceOf(Blob);
    });

    it("ignores a message that is not the pool's", async () => {
        const scope = serving();
        scope.deliver(request());
        // Same shape, another program's tag: two libraries sharing one worker
        // scope is not a thing this has to support, but answering somebody
        // else's message is a thing it must not do.
        scope.deliver(request(), 2);

        await settled();
        expect(scope.posted).toHaveLength(2);
    });
});
