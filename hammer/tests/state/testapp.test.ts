// The reference consumer, wired up and driven.
//
// Type-checking `tests/testapp/app/state.ts` proves the seams can be SATISFIED
// from outside hammer. This proves they can be satisfied at the same time: the
// session store and the client each need the other first, the invalidator needs
// the resource store, the analytics sink needs a beacon and a route, and a
// construction order that only works in one direction is a seam an application
// discovers at run time.

import { describe, expect, it } from "../support/test.js";
import { inProcessWorkers } from "../support/in_process_worker.js";
import { serveArgon2Pool } from "../../src/prehash/worker.js";

import { ChannelBus } from "../support/fake_channel.js";
import { FakeServer } from "../support/fake_fetch.js";
import { LockRoom } from "../support/fake_locks.js";
import { WorkerRoom } from "../support/fake_worker.js";
import { sessionPayload } from "../support/session.js";
import { kStateRoutes, settle } from "../support/state.js";
import type { StateCount } from "../../src/state/counts.js";
import { eventPageViewed } from "../testapp/api/hammer.generated.js";
import {
    aboutForm,
    answerConsent,
    appState,
    contentImage,
    mediaPager,
    openContent,
    renameContent,
    reportSignup,
    starContent,
} from "../testapp/app/state.js";

function app(server: FakeServer) {
    const counts: StateCount[] = [];
    const beaconed: string[] = [];
    const workers = new WorkerRoom();

    const state = appState({
        fetch: server.fetch,
        locks: new LockRoom().tab(),
        fanOut: new ChannelBus().tab(),
        pageOrigin: "https://app.example.com",
        apiOrigin: "https://app.example.com",
        imageWorker: workers.create,
        prehashWorker: inProcessWorkers(serveArgon2Pool).create,
        beaconTo: {
            sendBeacon: (_url, data) => {
                beaconed.push(String(data));
                return true;
            },
        },
        count: (name) => counts.push(name),
    });

    return { state, counts, beaconed, workers, server };
}

const kDocument = {
    body: { title: "one", starred: false, version: 1 },
    headers: { "Cache-Control": "max-age=600" },
};

const kSession = {
    body: { ...sessionPayload({ routes: kStateRoutes, bits: [0, 8] }), user: { id: "u1" } },
};

// Every route below `session.current` is a HOLDER route, so its address arrives
// with the session and there is nothing to call before one has been read. The
// first scripted answer is therefore the session, the way it is in a real
// application (`docs/00-architecture.md` §4.2).
async function signedIn(server: FakeServer) {
    const harness = app(server);
    await harness.state.session.load(new AbortController().signal);
    return harness;
}

function signal(): AbortSignal {
    return new AbortController().signal;
}

describe("the reference consumer's state layer", () => {
    it("constructs in one order, with the client and the session each needing the other", async () => {
        const server = new FakeServer().always(kSession);
        const { state } = app(server);

        await state.session.load(signal());

        expect(state.session.identity()).toBe("u1");
        expect(state.session.store.get().status).toBe("active");
        // Nothing is stale, and the reason is the honest one rather than the one
        // this case used to assert: the session response carries no hash at all,
        // because anvil publishes no writer that puts one there, so a client
        // talking to it can make no staleness claim. `null` is "not told", which
        // is a different answer from "told the same hash".
        expect(state.session.store.get().stale).toBeNull();
        expect(state.session.store.get().view?.serverHash).toBeNull();

        state.close();
    });

    it("reads a document and serves the second read from the cache", async () => {
        const server = new FakeServer().reply(kSession).always(kDocument);
        const { state } = await signedIn(server);
        const before = server.calls;

        const first = openContent(state, "7");
        await settle();
        first.release();

        const again = openContent(state, "7");
        await settle();

        expect(server.calls - before).toBe(1);
        expect(again.state.get().value?.title).toBe("one");
        again.release();
        state.close();
    });

    it("pages a list by cursor and takes hasMore from the server", async () => {
        const server = new FakeServer()
            .reply(kSession)
            .reply({ body: { items: [{ id: "a" }], next: "c1" } })
            .always({ body: { items: [{ id: "b" }], next: null } });
        const { state } = await signedIn(server);

        const pager = mediaPager(state, { ns: "content", id: "7" });
        await pager.more();
        await pager.more();

        expect(pager.store.get().items).toEqual([{ id: "a" }, { id: "b" }]);
        expect(pager.store.get().hasMore).toBe(false);
        expect(server.requests[2]?.url).toContain("after=c1");

        pager.close();
        state.close();
    });

    it("builds a form from the section's own rows and the application's control map", () => {
        const form = aboutForm();

        expect(form).not.toBeNull();
        form?.set("title", "x".repeat(81));
        form?.touch("title");

        // 80 code points is the bound the descriptor declared for this field.
        expect(form?.field("title")?.reason).toBe("TOO_LONG");
        form?.close();
    });

    it("carries the version a rename read, and reconciles a mismatch", async () => {
        const server = new FakeServer()
            .reply(kSession)
            .reply(kDocument)
            .reply({ status: 409, body: { error: { code: "VERSION_MISMATCH" } } })
            .always({
                body: { title: "theirs", starred: false, version: 9 },
                headers: kDocument.headers,
            });
        const { state } = await signedIn(server);

        const outcome = await renameContent(state, "7", "mine", signal());

        expect(outcome.kind).toBe("conflict");
        expect(outcome.current?.version).toBe(9);
        state.close();
    });

    it("applies an optimistic star and confirms against the server's document", async () => {
        const server = new FakeServer()
            .reply(kSession)
            .reply(kDocument)
            .always({
                body: { title: "one", starred: true, version: 2 },
                headers: kDocument.headers,
            });
        const { state } = await signedIn(server);

        const outcome = await starContent(state, "7", signal());

        expect(outcome.kind).toBe("confirmed");
        expect(outcome.document?.version).toBe(2);
        state.close();
    });

    it("builds a srcset from the generated width table", () => {
        const sources = contentImage("abc");

        expect(sources.ok && sources.value.widths).toEqual([320, 1024, 1600, 2560]);
        expect(sources.ok && sources.value.src).toContain("https://media.example.com/");
    });

    it("queues an event that needs no consent and refuses one that does", async () => {
        const server = new FakeServer().always({ body: {} });
        const { state, counts, beaconed } = app(server);

        // `SignupCompleted` is declared `requires_consent: false` server-side,
        // so it is queued whatever the answer — the half of the gate that is easy
        // to get wrong in the other direction.
        reportSignup(state, "web");
        expect(state.analytics.pending).toBe(1);
        expect(counts).not.toContain("consent-refused");

        // `PageViewed` needs consent and nobody has given one.
        state.analytics.report(eventPageViewed, { surface: "web", referrer: "direct" });
        expect(state.analytics.pending).toBe(1);
        expect(counts).toContain("consent-refused");

        answerConsent(state, "granted");
        state.analytics.report(eventPageViewed, { surface: "web", referrer: "search" });
        expect(state.analytics.pending).toBe(2);

        state.analytics.flushFinal();
        expect(beaconed).toHaveLength(1);
        state.close();
    });

    it("closes everything it opened", () => {
        const server = new FakeServer().always({ body: {} });
        const { state } = app(server);

        // Nothing outlives what created it: one call, and every store, pool,
        // stream and listener this application built is released.
        expect(() => state.close()).not.toThrow();
    });
});

// A construction-order regression would show up as a throw rather than a failed
// assertion, so this is the one case where reaching the end IS the assertion.
describe("construction", () => {
    it("reads a session through the route the application supplied", async () => {
        const server = new FakeServer().always({
            body: { ...sessionPayload({ routes: kStateRoutes }), user: { id: "u1" } },
        });
        const { state } = app(server);

        await state.session.load(signal());
        const asked = server.requests[0];

        expect(asked?.url).toBe("https://app.example.com/session");
        expect(asked?.credentials).toBe("include");
        state.close();
    });
});
