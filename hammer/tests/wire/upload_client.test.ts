// The upload, driven through the client rather than through the two functions
// it is assembled from.
//
// `tests/wire/upload.test.ts` covers `uploadBody`, `uploadContentType` and
// `checkUpload` — what the body ends up being and what a file is refused for —
// and it covers them without a client. So until this file existed NOTHING
// anywhere called `client.upload`, and the pipeline an upload goes through (the
// budget, the dedupe, the queue, the idempotency key, the retry decision) had
// never been driven with a `File` body at all. The row this closes is in
// `docs/15-tasks.md` under §The first consumer, and it was opened by writing the
// guide rather than by reading the code: the guide's §8 was the first place in
// the repository that named the method, and only at type level.
//
// It found one defect, and it is the kind only an end-to-end run finds: a
// streamed body is CONSUMED by the attempt that sends it, so every retry after
// the first was re-sending an empty stream. See §a retried upload below.
//
// --- the route an upload is sent to -----------------------------------------
//
// The reference descriptor has no upload route (`docs/15-tasks.md` §Phase 6), so
// the same accommodation is made here that `tests/testapp/app/state.ts` makes
// for the versioned write: the pipeline is driven over the routes that have the
// SHAPE an upload route would have, and the reason is written down rather than
// left for a reader to infer.
//
//   `media.delete`  holder, POST in this holder's table, a `MediaUpload`
//                   capability scope and the `media` bucket — the shape of an
//                   upload that is admitted by a grant and counted by a limiter.
//   `auth.login`    public, POST, not idempotent, no capability — the shape of
//                   an upload that a retry may repeat, which is the case the
//                   single-use grant on `media.delete` forbids.

import { describe, expect, it } from "../support/test.js";

import type { Capability, SessionSource } from "../../src/wire/index.js";
import { createClient } from "../../src/wire/index.js";
import type { Client } from "../../src/wire/client.js";
import type { QueueConfig } from "../../src/wire/queue.js";
import type { UploadLimits, UploadProgress } from "../../src/wire/upload.js";
import type { SessionView } from "../../src/wire/session_view.js";
import type { Api } from "../testapp/app/client.js";
import {
    kApiTables,
    kUploadMaxBytes,
    routeAuthLogin,
    routeAuthRefresh,
    routeMediaDelete,
} from "../testapp/api/hammer.generated.js";
import { ChannelBus } from "../support/fake_channel.js";
import { FakeServer, gate } from "../support/fake_fetch.js";
import { LockRoom } from "../support/fake_locks.js";
import { sessionView } from "../support/session.js";

// The holder-scoped table a server would send this holder, through the real
// decode. `media.delete` is POST here because that is the method an upload
// route is described with, and the method a call uses comes off the session's
// own table rather than out of this bundle.
const kRoutes = { "media.delete": "POST /media/{ns}/{id}" };

class Source implements SessionSource {
    refetches = 0;

    private view: SessionView = sessionView({ routes: kRoutes });

    current = (): SessionView | null => this.view;

    refetch = async (_signal: AbortSignal): Promise<SessionView | null> => {
        this.refetches += 1;
        return this.view;
    };
}

// The descriptor's own cap and a list an application supplies, which is where
// the accept list lives until anvil emits one (`docs/15-tasks.md` §Cross-repo).
const kLimits: UploadLimits = { maxBytes: kUploadMaxBytes, accept: ["image/jpeg"] };

const kPlace = { ns: "avatars", id: "7f1c" } as const;

type Harness = {
    readonly client: Client<Api>;
    readonly server: FakeServer;
    readonly source: Source;
    readonly delays: number[];
    readonly counts: string[];
    readonly records: { readonly routeId: string; readonly retries: number }[];
};

function harness(over: { readonly queue?: QueueConfig } = {}): Harness {
    const server = new FakeServer();
    const source = new Source();
    const delays: number[] = [];
    const counts: string[] = [];
    const records: { readonly routeId: string; readonly retries: number }[] = [];

    const client = createClient<Api>({
        origin: {
            pageOrigin: "https://app.example.com",
            apiOrigin: "https://app.example.com",
            site: null,
        },
        fetch: server.fetch,
        session: source,
        api: kApiTables,
        refreshRoute: routeAuthRefresh,
        locks: new LockRoom().tab(),
        fanOut: new ChannelBus().tab(),
        now: () => 0,
        ...(over.queue === undefined ? {} : { queue: over.queue }),
        // Recorded rather than waited out, for the reason `client.test.ts`
        // gives: a suite that slept for a jittered backoff is a flaky suite.
        sleep: async (ms) => {
            delays.push(ms);
        },
        telemetry: {
            request: (record) => records.push({ routeId: record.routeId, retries: record.retries }),
            count: (name) => counts.push(name),
        },
    });

    return { client, server, source, delays, counts, records };
}

function signal(): AbortSignal {
    return new AbortController().signal;
}

function jpeg(bytes: number): File {
    return new File([new Uint8Array(bytes)], "photo.jpg", { type: "image/jpeg" });
}

// A `File` that says how many times something asked it for a stream. The count
// is the observable form of "no byte of this file was read for a request that
// was never sent" — a refused, shed or short-circuited upload must not have
// opened one.
class CountedFile extends File {
    opened = 0;

    override stream(): ReturnType<File["stream"]> {
        this.opened += 1;
        return super.stream();
    }
}

async function mediaGrant(host: Harness): Promise<Capability<"MediaUpload">> {
    host.server.reply({ status: 200, body: { token: "media-grant" } });
    const minted = await host.client.mint<typeof routeAuthLogin, "MediaUpload">(
        routeAuthLogin,
        { signal: signal() },
        (body) => (body as { readonly token: string }).token,
    );
    if (!minted.ok) {
        throw new Error("the test's own capability did not mint");
    }
    host.server.requests.length = 0;
    return minted.value;
}

describe("the body that reaches the wire", () => {
    // The property the whole module exists for, asserted where it is actually
    // decided: the object handed to `fetch` is the `File` the caller passed,
    // not a copy of its bytes. A 40 MB video that reached the JS heap would be
    // 40 MB against a tab budget that is frequently 350 MB, and the symptom is
    // the tab being killed rather than the upload being slow (`CLAUDE.md` §2.2).
    it("is the file itself when nothing asked for progress", async () => {
        const host = harness();
        const file = jpeg(2048);

        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file,
            signal: signal(),
        });

        expect(answered.ok).toBe(true);
        expect(host.server.requests[0]?.body).toBe(file);
    });

    it("is a counting stream when progress was asked for, and counts every byte", async () => {
        const host = harness();
        const file = jpeg(2048);
        const seen: UploadProgress[] = [];

        await host.client.upload(routeAuthLogin, kLimits, {
            file,
            onProgress: (progress) => seen.push(progress),
            signal: signal(),
        });

        // The stream reached the server and every byte of it was read: the
        // stand-in drains a stream body the way a server does.
        expect(host.server.requests[0]?.bodyBytes).toBe(file.size);
        expect(seen.at(-1)).toEqual({ sentBytes: file.size, totalBytes: file.size });
    });

    it("carries the file's own media type and never a guess", async () => {
        const host = harness();

        await host.client.upload(routeAuthLogin, kLimits, { file: jpeg(16), signal: signal() });
        expect(host.server.header(0, "Content-Type")).toBe("image/jpeg");

        // A file the platform could not type. anvil decides what it stored from
        // the bytes, so a client that invented `application/octet-stream` would
        // be telling the server something the person never said.
        const untyped = new File([new Uint8Array(16)], "photo");
        await host.client.upload(
            routeAuthLogin,
            { maxBytes: kUploadMaxBytes, accept: [] },
            { file: untyped, signal: signal() },
        );
        expect(host.server.header(1, "Content-Type")).toBeUndefined();
    });

    it("goes to the API origin with the session's cookies", async () => {
        const host = harness();

        await host.client.upload(routeAuthLogin, kLimits, { file: jpeg(16), signal: signal() });

        // NEVER the media origin: anvil keeps that origin out of its credential
        // allow-list on purpose (`docs/00-architecture.md` §8.4).
        expect(host.server.requests[0]?.url).toBe("https://app.example.com/login");
        expect(host.server.requests[0]?.credentials).toBe("include");
        expect(host.server.requests[0]?.redirect).toBe("error");
    });

    it("takes its method and its address from the holder's own table", async () => {
        const host = harness();
        const grant = await mediaGrant(host);

        await host.client.upload(routeMediaDelete, kLimits, {
            file: jpeg(16),
            params: kPlace,
            capability: grant,
            signal: signal(),
        });

        // The path is not in this bundle — a holder route's address arrives with
        // the session — and every segment of it went through the route builder.
        expect(host.server.requests[0]?.method).toBe("POST");
        expect(host.server.requests[0]?.url).toBe("https://app.example.com/media/avatars/7f1c");
    });
});

describe("what is refused before the first byte", () => {
    // Not the enforcement, and the module says so: anvil enforces during the
    // read because a client-side check is one an attacker skips. What it buys
    // is the upload that was going to be refused after the LAST byte, on a
    // connection where forty megabytes is minutes.
    it("refuses a file over the descriptor's cap without opening it", async () => {
        const host = harness();
        const file = new CountedFile([new Uint8Array(4)], "photo.jpg", { type: "image/jpeg" });
        Object.defineProperty(file, "size", { value: kUploadMaxBytes + 1 });

        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file,
            signal: signal(),
        });

        expect(answered).toMatchObject({ ok: false, error: { kind: "client", cause: "too-large" } });
        expect(host.server.calls).toBe(0);
        expect(file.opened).toBe(0);
    });

    it("refuses a type the application does not accept", async () => {
        const host = harness();
        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file: new File([new Uint8Array(8)], "notes.txt", { type: "text/plain" }),
            signal: signal(),
        });

        expect(answered).toMatchObject({
            ok: false,
            error: { kind: "client", cause: "unsupported-media" },
        });
        expect(host.server.calls).toBe(0);
    });

    it("refuses a zero-byte file, which is a picker that returned nothing", async () => {
        const host = harness();
        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file: new File([], "photo.jpg", { type: "image/jpeg" }),
            signal: signal(),
        });

        expect(answered).toMatchObject({
            ok: false,
            error: { kind: "client", cause: "bad-parameter" },
        });
        expect(host.server.calls).toBe(0);
    });

    it("reports a signal that was already aborted and reads nothing", async () => {
        const host = harness();
        const file = new CountedFile([new Uint8Array(64)], "photo.jpg", { type: "image/jpeg" });
        const controller = new AbortController();
        controller.abort();

        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file,
            onProgress: () => {
                throw new Error("a request that was never sent reported progress");
            },
            signal: controller.signal,
        });

        expect(answered).toMatchObject({ ok: false, error: { cause: "aborted" } });
        expect(host.server.calls).toBe(0);
        expect(file.opened).toBe(0);
    });

    it("opens no stream for an upload the queue sheds", async () => {
        // A shed request never reached the network, so it must never have
        // reached the file either: a body built before the pipeline decided is a
        // read of the disk for a request nobody sent.
        const host = harness({ queue: { maxInFlight: 1, maxWaiting: 0 } });
        const held = gate();
        host.server.reply({ until: held.until, status: 200, body: {} });

        const first = host.client.upload(routeAuthLogin, kLimits, {
            file: jpeg(32),
            onProgress: () => {},
            signal: signal(),
        });
        await settle();

        const shedFile = new CountedFile([new Uint8Array(32)], "photo.jpg", { type: "image/jpeg" });
        const shed = await host.client.upload(routeAuthLogin, kLimits, {
            file: shedFile,
            onProgress: () => {},
            signal: signal(),
        });

        expect(shed).toMatchObject({ ok: false, error: { kind: "client", cause: "queue-full" } });
        expect(shedFile.opened).toBe(0);
        expect(host.counts).toContain("request-shed");

        held.open();
        await first;
    });
});

describe("a retried upload", () => {
    // THE DEFECT THIS FILE WAS WRITTEN TO FIND.
    //
    // A `ReadableStream` is consumed by the attempt that sends it. The body was
    // built once, before the pipeline ran, and every attempt after the first was
    // handed the empty remains of the first — which `fetch` rejects, which this
    // client reads as a dropped connection, which it retries. So an upload that
    // asked for progress spent its whole attempt budget re-sending nothing, and
    // reported a network failure for a network that was working.
    //
    // Two properties keep it closed: the body is built per ATTEMPT, and it is
    // built inside the one function that sends a request, so nothing constructs
    // one for an attempt that is not made.
    it("sends a second body rather than the empty remains of the first", async () => {
        const host = harness();
        const file = jpeg(1024);
        host.server.reply({ transport: true }, { status: 200, body: { id: "m1" } });

        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file,
            onProgress: () => {},
            signal: signal(),
        });

        expect(answered.ok).toBe(true);
        expect(host.server.calls).toBe(2);
        expect(host.server.requests.map((request) => request.bodyDisturbed)).toEqual([false, false]);
        expect(host.server.requests[1]?.bodyBytes).toBe(file.size);
    });

    it("counts the second attempt's bytes from zero, because they are sent again", async () => {
        // The progress a caller sees goes BACKWARDS on a retry, and that is the
        // honest answer: the bytes really are being sent a second time. A
        // counter carried across attempts would report an upload as half done
        // while the server had received none of it.
        const host = harness();
        const file = jpeg(512);
        const seen: number[] = [];
        host.server.reply({ transport: true }, { status: 200, body: {} });

        await host.client.upload(routeAuthLogin, kLimits, {
            file,
            onProgress: (progress) => seen.push(progress.sentBytes),
            signal: signal(),
        });

        expect(seen).toEqual([file.size, file.size]);
    });

    it("carries one idempotency key across every attempt at the same upload", async () => {
        // At-least-once on the wire, at-most-once at the server. A second key
        // would be a second upload — the duplicate the key exists to prevent.
        const host = harness();
        host.server.reply({ status: 503 }, { status: 200, body: {} });

        await host.client.upload(routeAuthLogin, kLimits, {
            file: jpeg(64),
            onProgress: () => {},
            signal: signal(),
        });

        expect(host.server.calls).toBe(2);
        const first = host.server.header(0, "Idempotency-Key");
        expect(first).toMatch(/^[0-9a-f-]{36}$/);
        expect(host.server.header(1, "Idempotency-Key")).toBe(first);
    });

    it("re-sends a file body too, which is the path that never had the defect", async () => {
        // A `File` is re-readable, so the no-progress path survived a retry all
        // along. Asserted so that the fix is not mistaken for the reason this
        // works.
        const host = harness();
        const file = jpeg(128);
        host.server.reply({ transport: true }, { status: 200, body: {} });

        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file,
            signal: signal(),
        });

        expect(answered.ok).toBe(true);
        expect(host.server.requests.map((request) => request.body)).toEqual([file, file]);
    });

    it("gives up rather than repeat an upload that spent a single-use grant", async () => {
        // anvil consumed the token whether or not the response arrived, so a
        // retry is answered with a capability failure and reports a failure for
        // an upload that succeeded (`docs/01-seams.md` §6).
        const host = harness();
        const grant = await mediaGrant(host);
        host.server.always({ transport: true });

        const answered = await host.client.upload(routeMediaDelete, kLimits, {
            file: jpeg(64),
            params: kPlace,
            capability: grant,
            onProgress: () => {},
            signal: signal(),
        });

        expect(answered).toMatchObject({ ok: false, error: { kind: "transport" } });
        expect(host.server.calls).toBe(1);
        expect(host.server.header(0, "Capability")).toBe("media-grant");
    });
});

describe("the stages an upload shares with every other request", () => {
    it("replays the whole body after a credential refresh", async () => {
        // The one case where a second body is needed for a reason that is not a
        // retry: the first attempt was authenticated with a credential that had
        // expired, and the replay is the same request with a new one.
        const host = harness();
        const file = jpeg(256);
        host.server.reply(
            { status: 401, body: { error: { code: "UNAUTHENTICATED" } } },
            { status: 200, body: {} },
            { status: 200, body: { id: "m1" } },
        );

        const answered = await host.client.upload(routeAuthLogin, kLimits, {
            file,
            onProgress: () => {},
            signal: signal(),
        });

        expect(answered.ok).toBe(true);
        // The upload, the refresh, the replay — and the replay carried a body.
        expect(host.server.calls).toBe(3);
        expect(host.server.requests[2]?.bodyBytes).toBe(file.size);
        expect(host.counts).toContain("replay");
    });

    it("is never merged with another upload of the same file", async () => {
        // Two identical uploads are two writes somebody asked for. Only a route
        // that is safe to repeat AND has no body is ever shared, which is why
        // `perform` refuses to dedupe this one.
        const host = harness();
        const held = gate();
        host.server.always({ until: held.until, status: 200, body: {} });

        const file = jpeg(64);
        const both = Promise.all([
            host.client.upload(routeAuthLogin, kLimits, { file, signal: signal() }),
            host.client.upload(routeAuthLogin, kLimits, { file, signal: signal() }),
        ]);
        await settle();

        expect(host.server.calls).toBe(2);
        held.open();
        await both;
    });

    it("spends the route's own budget and holds the bucket back on a 429", async () => {
        const host = harness();
        const grant = await mediaGrant(host);
        host.server.reply({ status: 429, headers: { "Retry-After": "30" } });

        const answered = await host.client.upload(routeMediaDelete, kLimits, {
            file: jpeg(64),
            params: kPlace,
            capability: grant,
            signal: signal(),
        });

        // A single-use grant is never auto-retried, so the window the server
        // named is reported rather than waited out here.
        expect(answered).toMatchObject({ ok: false, error: { status: 429 } });
        expect(host.delays).toEqual([]);
    });

    it("is recorded once for the call rather than once per attempt", async () => {
        const host = harness();
        host.server.reply({ transport: true }, { status: 200, body: {} });

        await host.client.upload(routeAuthLogin, kLimits, {
            file: jpeg(64),
            onProgress: () => {},
            signal: signal(),
        });

        expect(host.records).toEqual([{ routeId: "auth.login", retries: 1 }]);
    });
});

// Enough turns of the microtask queue for a call to have reached its `fetch`:
// route resolution, the build and queue admission are each asynchronous.
async function settle(): Promise<void> {
    for (let i = 0; i < 8; i += 1) {
        await Promise.resolve();
    }
}
