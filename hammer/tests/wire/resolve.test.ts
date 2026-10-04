// Where a route's address comes from, and what happens when there is not one.
//
// The session source is a small hand-written stand-in rather than a mock: a mock
// configured to return what the test expects asserts that the test knows what it
// expects. It counts its own refetches, because "exactly once" is the assertion
// that keeps a client from hammering its own session endpoint every time
// somebody clicks a button they do not have.

import { describe, expect, it } from "../support/test.js";

import type { ClientError } from "../../src/core/errors.js";
import type { ResolvableRoute, SessionSource } from "../../src/wire/resolve.js";
import { resolveRoute } from "../../src/wire/resolve.js";
import type { SessionView } from "../../src/wire/session_view.js";
import { sessionView } from "../support/session.js";

const kPublicRoute: ResolvableRoute = {
    id: "auth.login",
    visibility: "public",
    method: "POST",
    path: "/login",
};

const kHolderRoute: ResolvableRoute = {
    id: "audit.list",
    visibility: "holder",
    method: null,
    path: null,
};

class Source implements SessionSource {
    refetches = 0;

    constructor(
        private held: SessionView | null,
        private readonly afterRefetch: SessionView | null = null,
    ) {}

    current = (): SessionView | null => this.held;

    refetch = async (_signal: AbortSignal): Promise<SessionView | null> => {
        this.refetches += 1;
        this.held = this.afterRefetch;
        return this.held;
    };
}

function signal(): AbortSignal {
    return new AbortController().signal;
}

describe("a public route", () => {
    it("resolves from the compiled table", async () => {
        const source = new Source(null);
        await expect(resolveRoute(kPublicRoute, source, signal())).resolves.toMatchObject({
            ok: true,
            value: { method: "POST", path: "/login" },
        });
    });

    it("never consults the session", async () => {
        // It is reachable with no credential at all, so resolving it must not
        // depend on there being a session — and must not fetch one to find out.
        const source = new Source(null);
        await resolveRoute(kPublicRoute, source, signal());
        expect(source.refetches).toBe(0);
    });

    it("refuses a compiled route whose method is not a method", async () => {
        const any: ResolvableRoute = { ...kPublicRoute, method: "ANY" };
        await expect(resolveRoute(any, new Source(null), signal())).resolves.toMatchObject({
            ok: false,
            error: { kind: "client", cause: "unusable-route" },
        });
    });

    it("refuses a compiled path that is not one", async () => {
        const forged: ResolvableRoute = { ...kPublicRoute, path: "//evil.example/login" };
        await expect(resolveRoute(forged, new Source(null), signal())).resolves.toMatchObject({
            ok: false,
            error: { kind: "client", cause: "no-route" },
        });
    });
});

describe("a holder route", () => {
    it("resolves from the table the session delivered", async () => {
        const source = new Source(sessionView({ routes: { "audit.list": "GET /audit" } }));
        await expect(resolveRoute(kHolderRoute, source, signal())).resolves.toMatchObject({
            ok: true,
            value: { method: "GET", path: "/audit" },
        });
        expect(source.refetches).toBe(0);
    });

    it("refetches once when the entry is absent, and resolves if it arrives", async () => {
        // The local table is a copy and anvil's perm_epoch exists because a copy
        // goes stale: an absent entry is as likely to be a grant made thirty
        // seconds ago as a route this holder will never reach.
        const source = new Source(
            sessionView({ routes: {} }),
            sessionView({ routes: { "audit.list": "GET /audit" } }),
        );
        await expect(resolveRoute(kHolderRoute, source, signal())).resolves.toMatchObject({
            ok: true,
            value: { method: "GET", path: "/audit" },
        });
        expect(source.refetches).toBe(1);
    });

    it("refetches exactly once and then stops", async () => {
        const source = new Source(sessionView({ routes: {} }));
        await resolveRoute(kHolderRoute, source, signal());
        expect(source.refetches).toBe(1);
    });

    it("reports a missing address when there is no session at all", async () => {
        const source = new Source(null);
        await expect(resolveRoute(kHolderRoute, source, signal())).resolves.toMatchObject({
            ok: false,
            error: { kind: "client", cause: "no-route" },
        });
        expect(source.refetches).toBe(1);
    });

    it("reports a route the server named but nothing can call", async () => {
        const source = new Source(
            sessionView({ routes: { "audit.list": "ANY /audit" } }),
        );
        await expect(resolveRoute(kHolderRoute, source, signal())).resolves.toMatchObject({
            ok: false,
            error: { kind: "client", cause: "unusable-route" },
        });
        // Uncallable is a different fact from absent, and refetching cannot
        // change it.
        expect(source.refetches).toBe(0);
    });
});

describe("the shape a missing address is reported in", () => {
    it("is the one everything absent is reported in", async () => {
        const source = new Source(sessionView({ routes: {} }));
        const resolved = await resolveRoute(kHolderRoute, source, signal());
        expect(resolved.ok).toBe(false);
        if (resolved.ok) return;
        expect(resolved.error).toEqual({
            kind: "client",
            cause: "no-route",
            retryAfterMs: null,
        });
    });

    it("cannot be a permission, because no such cause exists", () => {
        // Type-level, and it is the whole of §4.2's "never a permission-flavoured
        // error". A client that said "forbidden" where the server would have said
        // 404 rebuilds the oracle anvil removed, in the one place the server
        // cannot reach — so the union has no member to say it with.
        // @ts-expect-error there is no locally-denied cause in ClientError.
        const denied: ClientError["cause"] = "forbidden";
        // @ts-expect-error nor under any other spelling of the same idea.
        const refused: ClientError["cause"] = "permission-denied";
        expect([denied, refused]).toHaveLength(2);
    });
});

describe("a resolution nothing is waiting for", () => {
    it("stops before it fetches", async () => {
        const controller = new AbortController();
        controller.abort();
        const source = new Source(sessionView({ routes: {} }));

        await expect(
            resolveRoute(kHolderRoute, source, controller.signal),
        ).resolves.toMatchObject({ ok: false, error: { kind: "transport", cause: "aborted" } });
        expect(source.refetches).toBe(0);
    });

    it("stops after a refetch the screen no longer wants", async () => {
        const controller = new AbortController();
        const source: SessionSource = {
            current: () => sessionView({ routes: {} }),
            refetch: async () => {
                controller.abort();
                return sessionView({ routes: { "audit.list": "GET /audit" } });
            },
        };

        await expect(
            resolveRoute(kHolderRoute, source, controller.signal),
        ).resolves.toMatchObject({ ok: false, error: { kind: "transport", cause: "aborted" } });
    });
});
