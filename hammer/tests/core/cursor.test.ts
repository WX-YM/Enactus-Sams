// Half of this file is type-level, and that half is the point: it asserts what
// cannot be WRITTEN, not what happens when it runs. Every `@ts-expect-error`
// below fails the build the day the line it guards starts compiling, which is
// the only way a rule about what a caller can express stays true.

import { describe, expect, it } from "../support/test.js";

import type { Cursor, PageRequest } from "../../src/core/cursor.js";
import { cursorFromServer, pageRequest } from "../../src/core/cursor.js";

const kRouteId = "content.list";
type RouteId = typeof kRouteId;

describe("a cursor is the server's value", () => {
    it("carries the string through unchanged", () => {
        // Opaque: the library does not parse it, and a client that read it
        // would break the day the server changed its sort key.
        const cursor = cursorFromServer(kRouteId, "eyJpZCI6");
        expect(String(cursor)).toBe("eyJpZCI6");
    });

    it("cannot be written down by a caller", () => {
        // @ts-expect-error a plain string is not a cursor: it never came from a
        // server, and the type is evidence that it did.
        const forged: Cursor<RouteId> = "eyJpZCI6";
        expect(String(forged)).toBe("eyJpZCI6");
    });

    it("cannot be a position", () => {
        // @ts-expect-error a number is not a cursor, which is the whole of the
        // rule: there is no page 5 to ask for.
        const positional: Cursor<RouteId> = 5;
        expect(positional).toBe(5);
    });

    it("does not travel between routes", () => {
        const cursor = cursorFromServer(kRouteId, "eyJpZCI6");
        // @ts-expect-error a cursor encodes the sort key of the route that
        // issued it. Handed to another list it is a decode error at best and a
        // page of the wrong rows at worst.
        const elsewhere: Cursor<"media.list"> = cursor;
        expect(String(elsewhere)).toBe("eyJpZCI6");
    });
});

describe("a page request", () => {
    const first = pageRequest<RouteId>({ limit: 20, maxLimit: 50, after: null });

    it("starts with an explicit null rather than an absent key", () => {
        // An omitted key and a key holding a cursor look identical at a call
        // site, and the one that silently means "start again" is the one that
        // shows up as a list that never advances.
        expect(first).toMatchObject({ ok: true, value: { limit: 20, after: null } });
    });

    it("advances by the cursor the server returned", () => {
        const next = pageRequest<RouteId>({
            limit: 20,
            maxLimit: 50,
            after: cursorFromServer(kRouteId, "eyJpZCI6"),
        });
        expect(next.ok).toBe(true);
        if (!next.ok) return;
        expect(String(next.value.after)).toBe("eyJpZCI6");
    });

    it("refuses a limit above the descriptor's maximum rather than clamping", () => {
        // A caller that asked for two hundred and silently received fifty
        // renders a list that looks complete and is not.
        expect(pageRequest<RouteId>({ limit: 200, maxLimit: 50, after: null })).toMatchObject({
            ok: false,
            error: "limit-above-maximum",
        });
    });

    it("refuses a limit that is not a count", () => {
        for (const limit of [0, -1, 1.5, Number.NaN]) {
            expect(
                pageRequest<RouteId>({ limit, maxLimit: 50, after: null }),
                `limit ${limit}`,
            ).toMatchObject({ ok: false, error: "limit-not-positive" });
        }
    });

    it("has no way to express a position", () => {
        // @ts-expect-error there is no offset field, and an excess property is
        // a compile error on an object literal — which is where every call site
        // that wanted one would try to put it.
        const skipping: PageRequest<RouteId> = { limit: 20, after: null, offset: 40 };
        expect(skipping.limit).toBe(20);
    });
});
