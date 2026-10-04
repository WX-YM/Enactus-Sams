// A read, held for as long as something is looking at it.
//
// Freshness is the subject, so the clock is a number this file moves. Every
// answer is a real `Response` through the fetch stand-in, which is what makes
// the `Cache-Control` assertions assertions about the header rather than about a
// value the test handed itself.

import { describe, expect, it } from "../support/test.js";

import { ResourceStore } from "../../src/state/resource.js";
import type { StateCount } from "../../src/state/counts.js";
import { routeContentGet, routeMediaList } from "../testapp/api/hammer.generated.js";
import type { Api } from "../testapp/app/client.js";
import { FakeServer } from "../support/fake_fetch.js";
import { clientHarness, settle } from "../support/state.js";

const kCache = { classes: { doc: 4 }, defaultMaxEntries: 8 };

function harness(server = new FakeServer()) {
    const wire = clientHarness({ server });
    const counts: StateCount[] = [];
    const store = new ResourceStore<Api>({
        client: wire.client,
        cache: kCache,
        now: wire.now,
        count: (name) => counts.push(name),
    });
    return { ...wire, store, counts };
}

describe("ResourceStore", () => {
    it("loads a body and reports it ready", async () => {
        const server = new FakeServer().always({ body: { title: "one" } });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" }, class: "doc" });
        expect(resource.state.get().status).toBe("loading");

        await settle();

        const state = resource.state.get();
        expect(state.status).toBe("ready");
        expect(state.value).toEqual({ title: "one" });
        expect(state.stale).toBe(false);

        resource.release();
    });

    it("two readers of one address are one request and one store", async () => {
        const server = new FakeServer().always({ body: { title: "one" } });
        const { store } = harness(server);

        const first = store.open(routeContentGet, { params: { id: "7" } });
        const second = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        expect(server.calls).toBe(1);
        expect(first.state).toBe(second.state);

        first.release();
        second.release();
    });

    it("serves a fresh entry without asking again", async () => {
        const server = new FakeServer().always({
            body: { title: "one" },
            headers: { "Cache-Control": "max-age=60" },
        });
        const { store, tick } = harness(server);

        const first = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        first.release();
        expect(server.calls).toBe(1);

        tick(30_000);
        const again = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        expect(server.calls).toBe(1);
        expect(again.state.get().status).toBe("ready");
        again.release();
    });

    it("asks again once the freshness the server granted has run out", async () => {
        const server = new FakeServer().always({
            body: { title: "one" },
            headers: { "Cache-Control": "max-age=60" },
        });
        const { store, tick } = harness(server);

        const first = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        first.release();

        tick(61_000);
        const again = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        expect(server.calls).toBe(2);
        again.release();
    });

    it("invents no freshness when the server granted none", async () => {
        // A response with no `Cache-Control` is one the server said nothing
        // about, and saying nothing is not permission.
        const server = new FakeServer().always({ body: { title: "one" } });
        const { store } = harness(server);

        const first = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        first.release();

        const second = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        second.release();

        expect(server.calls).toBe(2);
    });

    it("serves stale while revalidating, but only where that was granted", async () => {
        const server = new FakeServer()
            .reply({ body: { title: "one" }, headers: { "Cache-Control": "max-age=10, stale-while-revalidate=600" } })
            .always({ body: { title: "two" }, headers: { "Cache-Control": "max-age=10, stale-while-revalidate=600" } });
        const { store, tick, counts } = harness(server);

        const first = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        first.release();

        tick(30_000);
        const again = store.open(routeContentGet, { params: { id: "7" } });

        // The stale body is on screen immediately, marked stale so that nothing
        // downstream mistakes it for a confirmation.
        const immediate = again.state.get();
        expect(immediate.status).toBe("ready");
        expect(immediate.value).toEqual({ title: "one" });
        expect(immediate.stale).toBe(true);
        expect(counts).toContain("served-stale");

        // And the revalidation is not optional: serving stale without refreshing
        // is a cache that quietly extended the freshness it was given.
        await settle();
        expect(server.calls).toBe(2);
        expect(again.state.get().value).toEqual({ title: "two" });
        expect(again.state.get().stale).toBe(false);

        again.release();
    });

    it("never stores a response marked no-store", async () => {
        const server = new FakeServer().always({
            body: { secret: true },
            headers: { "Cache-Control": "no-store" },
        });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        expect(resource.state.get().status).toBe("ready");
        resource.release();

        const again = store.open(routeContentGet, { params: { id: "7" } });
        expect(again.state.get().status).toBe("loading");
        await settle();
        expect(server.calls).toBe(2);
        again.release();
    });

    it("keeps a value on screen when a refresh fails, and marks it stale", async () => {
        const server = new FakeServer()
            .reply({ body: { title: "one" }, headers: { "Cache-Control": "max-age=60" } })
            .always({ transport: true });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        await resource.refresh();
        await settle();

        const state = resource.state.get();
        // Blanking the screen on a dropped connection is a worse answer than a
        // stale row with an error beside it.
        expect(state.status).toBe("failed");
        expect(state.value).toEqual({ title: "one" });
        expect(state.stale).toBe(true);
        expect(state.error).toEqual({ kind: "transport", cause: "network" });

        resource.release();
    });

    it("reports a failure with no value when there was never one", async () => {
        const server = new FakeServer().always({ status: 404, body: { error: { code: "NOT_FOUND" } } });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        const state = resource.state.get();
        expect(state.status).toBe("failed");
        expect(state.value).toBeNull();
        expect(state.stale).toBe(false);

        resource.release();
    });

    it("aborts the request when the last watcher lets go", async () => {
        const server = new FakeServer().always({ body: {} });
        const { store } = harness(server);

        const first = store.open(routeContentGet, { params: { id: "7" } });
        const second = store.open(routeContentGet, { params: { id: "7" } });

        first.release();
        second.release();
        await settle();

        // Nothing is watching, so nothing was written back; the store was closed
        // rather than left holding a value nobody can reach.
        const again = store.open(routeContentGet, { params: { id: "7" } });
        expect(again.state.get().status).toBe("loading");
        again.release();
    });

    it("releasing twice does not drop a watcher somebody else added", async () => {
        const server = new FakeServer().always({ body: { title: "one" } });
        const { store } = harness(server);

        const first = store.open(routeContentGet, { params: { id: "7" } });
        first.release();
        first.release();

        const second = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        expect(second.state.get().status).toBe("ready");
        second.release();
    });

    it("an invalidation re-reads what is on screen and drops what is not", async () => {
        const server = new FakeServer().always({
            body: { title: "one" },
            headers: { "Cache-Control": "max-age=600" },
        });
        const { store, counts } = harness(server);

        const watched = store.open(routeMediaList, { params: { ns: "content", id: "1" } });
        await settle();
        const unmounted = store.open(routeMediaList, { params: { ns: "content", id: "2" } });
        await settle();
        unmounted.release();
        expect(server.calls).toBe(2);

        store.invalidate(["media.list"]);
        await settle();

        // The watched one is read again; the unwatched one is simply gone, which
        // is what keeps an invalidation from being a thundering herd.
        expect(server.calls).toBe(3);
        expect(counts).toContain("invalidated");

        const unwatched = store.open(routeMediaList, { params: { ns: "content", id: "2" } });
        expect(unwatched.state.get().status).toBe("loading");

        watched.release();
        unwatched.release();
    });

    it("an identity change drops everything and re-reads what is mounted", async () => {
        const server = new FakeServer().always({
            body: { title: "one" },
            headers: { "Cache-Control": "max-age=600" },
        });
        const { store, counts } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        const before = resource.key();
        expect(server.calls).toBe(1);

        store.adopt("u1");
        expect(resource.state.get().status).toBe("loading");
        await settle();

        expect(counts).toContain("identity-dropped");
        expect(server.calls).toBe(2);
        // The key moved with the identity, so nothing the previous identity
        // wrote can be reached under it.
        expect(resource.key()).not.toBe(before);

        resource.release();
    });

    it("a body from the previous identity is never written under the new key", async () => {
        const server = new FakeServer().always({
            body: { title: "one" },
            headers: { "Cache-Control": "max-age=600" },
        });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        // The login lands while the anonymous read is still on the wire.
        store.adopt("u1");
        await settle();

        expect(resource.state.get().status).toBe("ready");
        // Two requests: the first was abandoned rather than filed under the new
        // identity's key.
        expect(server.calls).toBe(2);

        resource.release();
    });

    it("overwrite publishes a document the caller already has", async () => {
        // The body is the shape the reference application DECLARED for this
        // route (`tests/testapp/app/client.ts`), so a partial document does not
        // compile — which is the response-augmentation seam doing its job.
        const stored = { title: "one", starred: false, version: 1 };
        const server = new FakeServer().always({
            body: stored,
            headers: { "Cache-Control": "max-age=600" },
        });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        const confirmed = { title: "confirmed", starred: true, version: 2 };
        resource.overwrite(confirmed);
        expect(resource.state.get().value).toEqual(confirmed);

        resource.release();
        const again = store.open(routeContentGet, { params: { id: "7" } });
        await settle();
        expect(server.calls).toBe(1);
        expect(again.state.get().value).toEqual(confirmed);
        again.release();
    });

    it("forget drops the value and reads again", async () => {
        const server = new FakeServer()
            .reply({ body: { title: "one" }, headers: { "Cache-Control": "max-age=600" } })
            .always({ body: { title: "two" }, headers: { "Cache-Control": "max-age=600" } });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        resource.forget();
        expect(resource.state.get().status).toBe("loading");
        await settle();

        expect(resource.state.get().value).toEqual({ title: "two" });
        resource.release();
    });

    it("clear empties the cache and every live entry", async () => {
        const server = new FakeServer().always({
            body: { title: "one" },
            headers: { "Cache-Control": "max-age=600" },
        });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        store.clear();

        expect(resource.state.get().status).toBe("loading");
        resource.release();
    });

    it("ready resolves on the first state that is not loading", async () => {
        const server = new FakeServer().always({ body: { title: "one", starred: false, version: 1 } });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        const settled = await resource.ready(new AbortController().signal);

        expect(settled.status).toBe("ready");
        expect(settled.value?.title).toBe("one");
        resource.release();
    });

    it("ready resolves immediately for an entry that has already settled", async () => {
        const server = new FakeServer().always({ body: { title: "one", starred: false, version: 1 } });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        await settle();

        await expect(resource.ready(new AbortController().signal)).resolves.toMatchObject({
            status: "ready",
        });
        resource.release();
    });

    it("ready hands back what is held when the caller gives up", async () => {
        const server = new FakeServer().always({ body: { title: "one", starred: false, version: 1 } });
        const { store } = harness(server);
        const controller = new AbortController();

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        const waiting = resource.ready(controller.signal);
        controller.abort();

        // The caller is abandoning the wait, not discovering an error: a
        // rejection would be an exception thrown for an expected condition.
        await expect(waiting).resolves.toMatchObject({ status: "loading" });
        resource.release();
    });

    it("ready resolves on a failure as well as on a value", async () => {
        const server = new FakeServer().always({ transport: true });
        const { store } = harness(server);

        const resource = store.open(routeContentGet, { params: { id: "7" } });
        const settled = await resource.ready(new AbortController().signal);

        expect(settled.status).toBe("failed");
        resource.release();
    });

    it("close aborts every entry and leaves nothing running", async () => {
        const server = new FakeServer().always({ body: {} });
        const { store } = harness(server);

        store.open(routeContentGet, { params: { id: "7" } });
        store.close();
        await settle();

        expect(store.identity()).toBeNull();
    });
});
