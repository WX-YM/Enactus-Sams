// Where an image comes from, and how one gets sent.
//
// The widths are the reference application's generated table, so what is
// asserted is the ladder anvil published rather than one this file invented.

import { describe, expect, it } from "vitest";

import { imageSource, imageSources, uploadImage } from "../../src/state/media.js";
import type { MediaConfig } from "../../src/state/media.js";
import { ImagePool } from "../../src/state/workers/image.js";
import { kMediaDefaultRole, kMediaWidths, routeMediaList } from "../testapp/api/hammer.generated.js";
import { WorkerRoom, settled } from "../support/fake_worker.js";
import { FakeServer } from "../support/fake_fetch.js";
import { clientHarness } from "../support/state.js";

// anvil's public grammar. The reference descriptor carries no route for it yet,
// so the application supplies the target — the same discomfort the session route
// has, and recorded in `docs/15-tasks.md` §Cross-repo for the same reason.
const kMedia: MediaConfig = {
    origin: "https://media.example.com",
    route: { method: "GET", path: "/media/{ns}/{id}/{role}" },
    widths: kMediaWidths,
    defaultRole: kMediaDefaultRole,
};

function signal(): AbortSignal {
    return new AbortController().signal;
}

describe("imageSources", () => {
    it("builds a srcset naming the width of every role the namespace serves", () => {
        const sources = imageSources(kMedia, { ns: "content", id: "abc" });

        expect(sources.ok).toBe(true);
        if (!sources.ok) {
            return;
        }
        expect(sources.value.srcset).toBe(
            "https://media.example.com/media/content/abc/thumb 320w, " +
                "https://media.example.com/media/content/abc/card 1024w, " +
                "https://media.example.com/media/content/abc/hero 1600w, " +
                "https://media.example.com/media/content/abc/full 2560w",
        );
        expect(sources.value.widths).toEqual([320, 1024, 1600, 2560]);
    });

    it("points src at the default role, so a request without one still resolves", () => {
        const sources = imageSources(kMedia, { ns: "content", id: "abc" });

        expect(sources.ok && sources.value.src).toBe(
            "https://media.example.com/media/content/abc/card",
        );
    });

    it("points at the media origin and never at the API origin", () => {
        // `MEDIA_ORIGIN` never receives credentials and never issues a state
        // change (`docs/00-architecture.md` §8.7).
        const sources = imageSources(kMedia, { ns: "content", id: "abc" });

        expect(sources.ok && sources.value.srcset).not.toContain("app.example.com");
    });

    it("encodes the subject rather than concatenating it", () => {
        const sources = imageSources(kMedia, { ns: "content", id: "a b/c" });

        expect(sources.ok && sources.value.src).toBe(
            "https://media.example.com/media/content/a%20b%2Fc/card",
        );
    });

    it("refuses a subject the route builder will not build", () => {
        // `..` is unreserved, so `encodeURIComponent` leaves it alone and a URL
        // parser resolves it before a byte leaves the device (`wire/route.ts`).
        expect(imageSources(kMedia, { ns: "content", id: ".." }).ok).toBe(false);
        expect(imageSources(kMedia, { ns: "content", id: "" }).ok).toBe(false);
        // Half a surrogate pair, which is what a string truncated in the middle
        // of an emoji is.
        expect(imageSources(kMedia, { ns: "content", id: "a\ud83d" }).ok).toBe(false);
    });

    it("refuses a namespace the descriptor does not carry", () => {
        const sources = imageSources(kMedia, { ns: "invented", id: "abc" });

        expect(sources.ok ? null : sources.error).toBe("unknown-namespace");
    });

    it("does not read a namespace off Object's prototype", () => {
        const sources = imageSources(kMedia, { ns: "constructor", id: "abc" });

        expect(sources.ok ? null : sources.error).toBe("unknown-namespace");
    });

    it("refuses when the namespace does not serve the default role", () => {
        const broken: MediaConfig = { ...kMedia, defaultRole: "poster" };

        // Forgetting the segment would otherwise be a 404 found in production.
        expect(imageSources(broken, { ns: "content", id: "abc" })).toEqual({
            ok: false,
            error: "no-default-role",
        });
    });
});

describe("imageSource", () => {
    it("builds one address for one role", () => {
        expect(imageSource(kMedia, { ns: "media", id: "7" }, "thumb")).toEqual({
            ok: true,
            value: "https://media.example.com/media/media/7/thumb",
        });
    });

    it("refuses a role the namespace does not serve", () => {
        expect(imageSource(kMedia, { ns: "media", id: "7" }, "poster")).toEqual({
            ok: false,
            error: "unknown-role",
        });
    });
});

describe("uploadImage", () => {
    const kLimits = { maxBytes: 1024, accept: ["image/jpeg"] };

    function harness() {
        const room = new WorkerRoom();
        const pool = new ImagePool({ create: room.create });
        const server = new FakeServer().always({ body: { id: "stored" } });
        const wire = clientHarness({ server });
        return { room, pool, server, wire };
    }

    function photograph(bytes: number, type = "image/jpeg"): File {
        return new File([new Uint8Array(bytes)], "photo.jpg", { type });
    }

    it("checks the original against the route's limits before spawning a worker", async () => {
        // A 40 MB file the server will refuse should cost nothing at all, and a
        // client that downscaled first would spend the decode to find that out.
        const { pool, room, wire } = harness();

        const answered = await uploadImage(wire.client, pool, {
            route: routeMediaList,
            params: { ns: "content", id: "7" },
            file: photograph(4096),
            limits: kLimits,
            downscale: { maxEdgeCssPx: 1024, type: "image/webp" },
            signal: signal(),
        });

        expect(answered.ok).toBe(false);
        expect(answered.ok ? null : answered.error).toMatchObject({ cause: "too-large" });
        expect(room.spawned).toBe(0);
        pool.close();
    });

    it("refuses a media type the route does not accept", async () => {
        const { pool, room, wire } = harness();

        const answered = await uploadImage(wire.client, pool, {
            route: routeMediaList,
            params: { ns: "content", id: "7" },
            file: photograph(16, "image/gif"),
            limits: kLimits,
            signal: signal(),
        });

        expect(answered.ok ? null : answered.error).toMatchObject({ cause: "unsupported-media" });
        expect(room.spawned).toBe(0);
        pool.close();
    });

    it("sends the file as it is when no downscale was asked for", async () => {
        const { pool, room, server, wire } = harness();

        const answered = await uploadImage(wire.client, pool, {
            route: routeMediaList,
            params: { ns: "content", id: "7" },
            file: photograph(16),
            limits: kLimits,
            signal: signal(),
        });

        expect(answered.ok).toBe(true);
        expect(room.spawned).toBe(0);
        expect(server.calls).toBe(1);
        pool.close();
    });

    it("downscales through the pool and uploads what came back", async () => {
        const { pool, room, server, wire } = harness();

        const running = uploadImage(wire.client, pool, {
            route: routeMediaList,
            params: { ns: "content", id: "7" },
            file: photograph(512),
            // The route takes what the downscale PRODUCES, which is the type the
            // preflight has to be reading.
            limits: { maxBytes: 1024, accept: ["image/webp"] },
            downscale: { maxEdgeCssPx: 1024, type: "image/webp", quality: 0.8 },
            signal: signal(),
        });
        await settled();

        expect(room.spawned).toBe(1);
        room.answerAll(() => ({
            ok: true,
            value: {
                blob: new Blob([new Uint8Array(64)], { type: "image/webp" }),
                widthCssPx: 1024,
                heightCssPx: 768,
            },
        }));

        const answered = await running;
        expect(answered.ok).toBe(true);
        expect(server.calls).toBe(1);
        pool.close();
    });

    it("hands a full pool back to the caller rather than holding a second photograph", async () => {
        const { pool, room, server, wire } = harness();
        const request = {
            route: routeMediaList,
            params: { ns: "content", id: "7" },
            file: photograph(512),
            limits: { maxBytes: 1024, accept: ["image/webp"] },
            downscale: { maxEdgeCssPx: 1024, type: "image/webp" },
            signal: signal(),
        } as const;

        const running = [
            uploadImage(wire.client, pool, request),
            uploadImage(wire.client, pool, request),
            uploadImage(wire.client, pool, request),
        ];
        await settled();

        // Send the original, ask again later, or say so on screen — the answer
        // is one only the caller has.
        const third = await running[2];
        expect(third?.ok).toBe(false);
        expect(third?.ok === false ? third.error : null).toMatchObject({ cause: "queue-full" });
        expect(server.calls).toBe(0);

        room.answerAll(() => ({
            ok: true,
            value: {
                blob: new Blob([new Uint8Array(64)], { type: "image/webp" }),
                widthCssPx: 1024,
                heightCssPx: 768,
            },
        }));
        await Promise.all([running[0], running[1]]);
        pool.close();
    });

    it("refuses on the type the upload will carry, not the one it started as", async () => {
        // A photograph re-encoded to WebP is a WebP by the time the server sees
        // it. Checking the JPEG it started as would refuse exactly the
        // conversion the downscale exists to perform.
        const { pool, room, wire } = harness();

        const answered = await uploadImage(wire.client, pool, {
            route: routeMediaList,
            params: { ns: "content", id: "7" },
            file: photograph(16),
            limits: kLimits,
            downscale: { maxEdgeCssPx: 1024, type: "image/webp" },
            signal: signal(),
        });

        expect(answered.ok ? null : answered.error).toMatchObject({ cause: "unsupported-media" });
        expect(room.spawned).toBe(0);
        pool.close();
    });

    it("refuses a downscale target that is not a size", async () => {
        const { pool, room, wire } = harness();

        const answered = await uploadImage(wire.client, pool, {
            route: routeMediaList,
            params: { ns: "content", id: "7" },
            file: photograph(16),
            limits: { maxBytes: 1024, accept: ["image/webp"] },
            downscale: { maxEdgeCssPx: Number.NaN, type: "image/webp" },
            signal: signal(),
        });

        expect(answered.ok ? null : answered.error).toMatchObject({ cause: "bad-request" });
        expect(room.spawned).toBe(0);
        pool.close();
    });
});
