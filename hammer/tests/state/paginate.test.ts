// Cursor paging, and the absence of an offset.
//
// The route is the reference application's own list route, so the limit bound
// under test is the descriptor's rather than a number this file chose.

import { describe, expect, it } from "vitest";

import { Pager } from "../../src/state/paginate.js";
import type { Page } from "../../src/state/paginate.js";
import { routeMediaList } from "../testapp/api/hammer.generated.js";
import type { Api } from "../testapp/app/client.js";
import { FakeServer } from "../support/fake_fetch.js";
import { clientHarness } from "../support/state.js";

type Row = { readonly id: string };

function pager(
    server: FakeServer,
    over: {
        readonly limit?: number;
        readonly readPage?: (body: unknown) => Page<Row> | null;
    } = {},
) {
    const wire = clientHarness({ server });
    const held = new Pager<Api, typeof routeMediaList, Row>({
        client: wire.client,
        route: routeMediaList,
        params: { ns: "content", id: "7" },
        limit: over.limit ?? 2,
        readPage:
            over.readPage ??
            ((body) => {
                const shape = body as {
                    readonly items?: readonly Row[];
                    readonly next?: string | null;
                };
                if (!Array.isArray(shape.items)) {
                    return null;
                }
                return { items: shape.items, next: shape.next ?? null };
            }),
        // The query vocabulary is the application's: hammer names no parameter.
        query: ({ limit, after }) => ({ limit, after }),
    });
    return { pager: held, server, wire };
}

describe("Pager", () => {
    it("loads a page and takes hasMore from the cursor", async () => {
        const server = new FakeServer().always({
            body: { items: [{ id: "a" }, { id: "b" }], next: "c1" },
        });
        const { pager: held } = pager(server);

        await held.more();

        const state = held.store.get();
        expect(state.status).toBe("ready");
        expect(state.items).toEqual([{ id: "a" }, { id: "b" }]);
        expect(state.hasMore).toBe(true);
    });

    it("ends the list when the server hands back no cursor, even on a full page", async () => {
        // `items.length === limit` would show a "load more" that loads nothing.
        const server = new FakeServer().always({
            body: { items: [{ id: "a" }, { id: "b" }], next: null },
        });
        const { pager: held } = pager(server);

        await held.more();

        expect(held.store.get().hasMore).toBe(false);
    });

    it("keeps going on a short page the server gave a cursor for", async () => {
        // A server that filtered rows after the fetch returns a short page in
        // the middle of the list. A count-based `hasMore` stops there.
        const server = new FakeServer().always({ body: { items: [{ id: "a" }], next: "c1" } });
        const { pager: held } = pager(server);

        await held.more();

        expect(held.store.get().hasMore).toBe(true);
    });

    it("sends the server's cursor on the next page and appends the rows", async () => {
        const server = new FakeServer()
            .reply({ body: { items: [{ id: "a" }], next: "c1" } })
            .always({ body: { items: [{ id: "b" }], next: null } });
        const { pager: held } = pager(server);

        await held.more();
        await held.more();

        expect(held.store.get().items).toEqual([{ id: "a" }, { id: "b" }]);
        expect(server.requests[1]?.url).toContain("after=c1");
        expect(server.requests[0]?.url).not.toContain("after=");
    });

    it("asks for nothing once the list has ended", async () => {
        const server = new FakeServer().always({ body: { items: [{ id: "a" }], next: null } });
        const { pager: held } = pager(server);

        await held.more();
        await held.more();

        expect(server.calls).toBe(1);
    });

    it("joins a second request for the same page onto the first", async () => {
        const server = new FakeServer().always({ body: { items: [{ id: "a" }], next: "c1" } });
        const { pager: held } = pager(server);

        await Promise.all([held.more(), held.more()]);

        // A list that fires a second request because the person scrolled twice
        // is a list that will show a page twice.
        expect(server.calls).toBe(1);
    });

    it("refuses a limit above the descriptor's maximum for the route", async () => {
        const server = new FakeServer().always({ body: { items: [], next: null } });
        const { pager: held } = pager(server, { limit: 101 });

        await held.more();

        // A caller that asked for two hundred and silently received fifty
        // renders a list that looks complete and is not.
        const state = held.store.get();
        expect(state.status).toBe("failed");
        expect(state.error).toBe("limit-above-maximum");
        expect(server.calls).toBe(0);
    });

    it("reports a limit that is not a positive integer as what it is", async () => {
        const server = new FakeServer().always({ body: { items: [], next: null } });
        const { pager: held } = pager(server, { limit: 0 });

        await held.more();

        expect(held.store.get().error).toBe("limit-not-positive");
    });

    it("reports a body it cannot read rather than ending the list", async () => {
        const server = new FakeServer().always({ body: { rows: [] } });
        const { pager: held } = pager(server);

        await held.more();

        // An empty page ends a list, and ending a list because a response
        // changed shape is a list that silently loses its tail.
        const state = held.store.get();
        expect(state.status).toBe("failed");
        expect(state.error).toBe("unreadable-page");
        expect(state.hasMore).toBe(true);
    });

    it("keeps the pages already loaded when one fails", async () => {
        const server = new FakeServer()
            .reply({ body: { items: [{ id: "a" }], next: "c1" } })
            .always({ transport: true });
        const { pager: held } = pager(server);

        await held.more();
        await held.more();

        const state = held.store.get();
        expect(state.status).toBe("failed");
        expect(state.items).toEqual([{ id: "a" }]);
    });

    it("reset goes back to the first page and can load again", async () => {
        const server = new FakeServer().always({ body: { items: [{ id: "a" }], next: "c1" } });
        const { pager: held } = pager(server);

        await held.more();
        held.reset();

        expect(held.store.get().items).toEqual([]);
        expect(held.store.get().hasMore).toBe(true);

        // The controller is replaced rather than reused: a pager that kept an
        // aborted one would refuse every page after a reset.
        await held.more();
        expect(held.store.get().items).toEqual([{ id: "a" }]);
        expect(server.requests[1]?.url).not.toContain("after=");
    });

    it("loads nothing once it is closed", async () => {
        const server = new FakeServer().always({ body: { items: [{ id: "a" }], next: "c1" } });
        const { pager: held } = pager(server);

        held.close();
        await held.more();

        expect(server.calls).toBe(0);
    });
});
