// Who this tab belongs to, and what it must not conclude from a failure.
//
// The payloads go through the real decode rather than around it: a `SessionView`
// assembled by hand is one no server could have sent, and every assertion built
// on it holds for a shape that does not exist.

import { describe, expect, it } from "../support/test.js";

import { fail, ok } from "../../src/core/result.js";
import type { HammerError } from "../../src/core/errors.js";
import { SessionStore } from "../../src/state/session.js";
import type { StateCount } from "../../src/state/counts.js";
import { routeContentGet, routeIdentityMe } from "../testapp/api/hammer.generated.js";
import { kServerHash, sessionPayload, kBits,} from "../support/session.js";
import { kStateRoutes } from "../support/state.js";

function signal(): AbortSignal {
    return new AbortController().signal;
}

function store(over: { readonly clientHash?: string } = {}) {
    const counts: StateCount[] = [];
    const session = new SessionStore({
        clientHash: over.clientHash ?? kServerHash,
        permissionBits: kBits,
        identityOf: (body) => {
            const who = (body as { readonly user?: { readonly id?: unknown } }).user?.id;
            return typeof who === "string" ? who : null;
        },
        count: (name) => counts.push(name),
    });
    return { session, counts };
}

// Carries a `hash`, which anvil's reference application does not send: the
// staleness cases below are about what a client does when it IS told, and a
// payload with no hash makes no claim at all. `kNoHashPayload` is the shape the
// reference application actually writes, and there is a case for that too.
const kPayload = {
    ...sessionPayload({ routes: kStateRoutes, bits: [0, 17], hash: kServerHash }),
    user: { id: "u1" },
};

const kNoHashPayload = {
    ...sessionPayload({ routes: kStateRoutes, bits: [0, 17] }),
    user: { id: "u1" },
};

describe("SessionStore", () => {
    it("starts unknown, which is not the same as anonymous", async () => {
        const { session } = store();

        // Rendering a signed-out surface before the first read has answered is a
        // login form that flashes at every person who is already signed in.
        expect(session.store.get().status).toBe("unknown");
    });

    it("decodes a session and publishes the identity and the table", async () => {
        const { session } = store();
        session.readsFrom(async () => ok(kPayload));

        await session.load(signal());

        const state = session.store.get();
        expect(state.status).toBe("active");
        expect(state.identity).toBe("u1");
        expect(session.affords(routeIdentityMe)).toBe(true);
        expect(session.holds([17])).toBe(true);
        expect(session.holds([25])).toBe(false);
    });

    it("reports a stale client when the server was built from another descriptor", async () => {
        const { session, counts } = store({ clientHash: "0".repeat(64) });
        session.readsFrom(async () => ok(kPayload));

        await session.load(signal());

        const state = session.store.get();
        expect(state.status).toBe("active");
        expect(state.stale).toEqual({
            kind: "stale-client",
            serverHash: kServerHash,
            clientHash: "0".repeat(64),
        });
        expect(counts).toContain("stale-client");
        // Surfaced and never acted on: the session is still usable, and nothing
        // here reloads a page.
        expect(session.current()).not.toBeNull();
    });

    it("makes no staleness claim about a server that sent no hash", async () => {
        // The shape anvil's reference application actually writes. Putting the
        // descriptor hash on this response is the application controller's job
        // and no anvil writer does it — so reporting every such session as stale
        // would put "this page is out of date" on every screen of an application
        // whose only defect is that nobody has taught its controller to send it.
        const { session, counts } = store({ clientHash: "0".repeat(64) });
        session.readsFrom(async () => ok(kNoHashPayload));

        await session.load(signal());

        const state = session.store.get();
        expect(state.status).toBe("active");
        expect(state.stale).toBeNull();
        expect(state.view?.serverHash).toBeNull();
        expect(counts).not.toContain("stale-client");
    });

    it("a transport failure is not a logout", async () => {
        const { session } = store();
        session.readsFrom(async () => ok(kPayload));
        await session.load(signal());

        const dropped: HammerError = { kind: "transport", cause: "network" };
        const { session: second } = store();
        second.readsFrom(async () => fail(dropped));
        await second.load(signal());

        // A store that cleared its identity on a dropped connection would sign
        // somebody out of a valid session every time a train went into a tunnel.
        expect(session.identity()).toBe("u1");
        expect(second.store.get().status).toBe("unknown");
    });

    it("a 503 leaves the session it already had", async () => {
        const { session } = store();
        let answer = 0;
        session.readsFrom(async () =>
            answer++ === 0
                ? ok(kPayload)
                : fail({
                      kind: "server",
                      code: "SERVICE_UNAVAILABLE",
                      status: 503,
                      requestId: null,
                      fields: null,
                  } satisfies HammerError),
        );

        await session.load(signal());
        await session.load(signal());

        expect(session.store.get().status).toBe("active");
        expect(session.identity()).toBe("u1");
    });

    it("a 401 that survived the refresh ends the session", async () => {
        const { session } = store();
        session.readsFrom(async () =>
            fail({
                kind: "server",
                code: "UNAUTHENTICATED",
                status: 401,
                requestId: null,
                fields: null,
            } satisfies HammerError),
        );

        await session.load(signal());

        // By the time a 401 reaches here the refresh machinery has had its turn,
        // so this is the server saying there is nobody there.
        const state = session.store.get();
        expect(state.status).toBe("anonymous");
        expect(state.view).toBeNull();
    });

    it("refuses a malformed payload whole rather than in part", async () => {
        const { session } = store();
        session.readsFrom(async () =>
            ok({ ...sessionPayload({ routes: { "x.y": "GET //evil.example/x" } }) }),
        );

        await session.load(signal());

        // Every path in that table becomes the path of an authenticated request.
        // Half a table is a table nothing checked.
        const state = session.store.get();
        expect(state.status).toBe("anonymous");
        expect(state.error).toBe("bad-route-target");
    });

    it("joins concurrent reads onto one", async () => {
        const { session } = store();
        let reads = 0;
        session.readsFrom(async () => {
            reads += 1;
            await Promise.resolve();
            return ok(kPayload);
        });

        await Promise.all([session.load(signal()), session.load(signal()), session.load(signal())]);

        // The 403 path, a holder route that resolved to nothing and the
        // application's own first load all reach here.
        expect(reads).toBe(1);
    });

    it("reads again after the first read settled", async () => {
        const { session } = store();
        let reads = 0;
        session.readsFrom(async () => {
            reads += 1;
            return ok(kPayload);
        });

        await session.load(signal());
        await session.load(signal());

        expect(reads).toBe(2);
    });

    it("serves the client a source that can read but not clear", async () => {
        const { session } = store();
        session.readsFrom(async () => ok(kPayload));

        const source = session.source;
        expect(source.current()).toBeNull();
        await source.refetch(signal());
        expect(source.current()).not.toBeNull();
        expect(Object.keys(source)).toEqual(["current", "refetch"]);
    });

    it("clear empties the session without reading anything", () => {
        const { session } = store();
        session.clear();

        const state = session.store.get();
        expect(state.status).toBe("anonymous");
        expect(state.error).toBeNull();
    });

    it("refuses a second reader", () => {
        const { session } = store();
        session.readsFrom(async () => ok(kPayload));

        // Two readers is two opinions about who is signed in.
        expect(() => session.readsFrom(async () => ok(kPayload))).toThrow();
    });

    it("reports no session when nothing has told it how to read one", async () => {
        const { session } = store();

        // Not a throw: a resolution reaching here during construction would
        // otherwise take down the first call an application makes.
        await expect(session.load(signal())).resolves.toBeNull();
        expect(session.affords(routeContentGet)).toBe(false);
    });
});
