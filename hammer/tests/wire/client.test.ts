// The pipeline, end to end, against a fetch stand-in and a clock the test owns.
//
// Driven through the REFERENCE CONSUMER's own client rather than a hand-built
// one, because the thing worth asserting is that this is usable from outside
// hammer with nothing but a generated module and a deployment. Every route below
// is a generated `const` and every table is the generated table.

import { describe, expect, it } from "vitest";

import type { Capability, SessionSource } from "../../src/wire/index.js";
import { createClient } from "../../src/wire/index.js";
import type { Client } from "../../src/wire/client.js";
import type { SessionView } from "../../src/wire/session_view.js";
import type { Api } from "../testapp/app/client.js";
import {
    kApiTables,
    routeAuthLogin,
    routeAuthRefresh,
    routeContentDelete,
    routeContentGet,
    routeMediaList,
} from "../testapp/api/hammer.generated.js";
import { ChannelBus } from "../support/fake_channel.js";
import { FakeServer, gate } from "../support/fake_fetch.js";
import { LockRoom } from "../support/fake_locks.js";
import { sessionView } from "../support/session.js";

// The holder-scoped table a server would send this holder, through the real
// decode: a session assembled by hand is one no server could have sent.
const kRoutes = {
    "content.get": "GET /content/{id}",
    "content.delete": "DELETE /content/{id}",
    "media.list": "GET /media/{ns}/{id}",
};

class Source implements SessionSource {
    refetches = 0;

    constructor(private view: SessionView | null = sessionView({ routes: kRoutes })) {}

    current = (): SessionView | null => this.view;

    refetch = async (_signal: AbortSignal): Promise<SessionView | null> => {
        this.refetches += 1;
        return this.view;
    };
}

type Harness = {
    readonly client: Client<Api>;
    readonly server: FakeServer;
    readonly source: Source;
    readonly delays: number[];
    readonly records: unknown[];
    readonly counts: string[];
    tick: (ms: number) => void;
};

function harness(
    over: {
        readonly source?: Source;
        readonly server?: FakeServer;
        readonly room?: LockRoom;
        readonly bus?: ChannelBus;
    } = {},
): Harness {
    const server = over.server ?? new FakeServer();
    const source = over.source ?? new Source();
    const delays: number[] = [];
    const records: unknown[] = [];
    const counts: string[] = [];
    let clock = 0;

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
        locks: (over.room ?? new LockRoom()).tab(),
        fanOut: (over.bus ?? new ChannelBus()).tab(),
        now: () => clock,
        leaderWaitMs: kLeaderWaitMs,
        // The backoff is recorded rather than waited out: a suite that slept for
        // its own retries would be slow, and a suite that slept for a jittered
        // one would be flaky.
        //
        // The follower's leader window is the one wait that is NOT collapsed.
        // Collapsing it would have every follower conclude in the same
        // microtask that the leader had died, take the lock behind it and
        // refresh a second time — which is the rotation race, manufactured by
        // the test harness rather than found by it.
        sleep: async (ms) => {
            delays.push(ms);
            if (ms === kLeaderWaitMs) {
                await new Promise<void>(() => {});
            }
        },
        telemetry: {
            request: (record) => records.push(record),
            count: (name) => counts.push(name),
        },
    });

    return {
        client,
        server,
        source,
        delays,
        records,
        counts,
        tick: (ms: number) => {
            clock += ms;
        },
    };
}

const kLeaderWaitMs = 10_000;

function signal(): AbortSignal {
    return new AbortController().signal;
}

// Enough turns of the microtask queue for a call to have reached its `fetch`:
// route resolution, the build and queue admission are each asynchronous, so
// "both callers have joined" is a state a test has to wait for rather than
// assume.
async function settle(): Promise<void> {
    for (let i = 0; i < 8; i += 1) {
        await Promise.resolve();
    }
}

async function capability(host: Harness): Promise<Capability<"ContentDelete">> {
    host.server.reply({ status: 200, body: { token: "cap-token" } });
    const minted = await host.client.mint<typeof routeAuthLogin, "ContentDelete">(
        routeAuthLogin,
        { signal: signal() },
        (body) => (body as { readonly token: string }).token,
    );
    if (!minted.ok) {
        throw new Error("the test's own capability did not mint");
    }
    return minted.value;
}

describe("the origin a client is pointed at", () => {
    // Everything but the origin, so each case below states the one fact it is
    // about. The client never gets far enough to use the rest.
    function build(origin: {
        readonly pageOrigin: string;
        readonly apiOrigin: string;
        readonly site: string | null;
    }): Client<Api> {
        return createClient<Api>({
            origin,
            fetch: new FakeServer().fetch,
            session: new Source(),
            api: kApiTables,
            refreshRoute: routeAuthRefresh,
        });
    }

    it("is checked at construction rather than at the first request", () => {
        // A cross-site API is not a degraded client. `SameSite=Lax` cookies are
        // not sent on a cross-site subresource request, so every call would go
        // out anonymously, the first would 401, the refresh would 401 too, and
        // the person would be signed out by a deployment mistake that reads as
        // an authentication bug.
        expect(() =>
            build({
                pageOrigin: "https://app.example.com",
                apiOrigin: "https://api.elsewhere.test",
                site: null,
            }),
        ).toThrow(/cross-site/);
    });

    it("says which check refused it", () => {
        // The reason is a value the origin module already returns, and it is
        // carried into the message rather than flattened: "the origin is wrong"
        // is a sentence somebody has to reconstruct a deployment to act on.
        expect(() =>
            build({
                pageOrigin: "https://app.example.com",
                apiOrigin: "http://api.example.com",
                site: null,
            }),
        ).toThrow(/not-https/);
        expect(() =>
            build({
                pageOrigin: "https://app.example.com",
                apiOrigin: "https://api.example.com/v1",
                site: null,
            }),
        ).toThrow(/not-an-origin/);
    });

    it("accepts a deployment that splits the hosts inside a declared site", () => {
        const client = build({
            pageOrigin: "https://app.example.com",
            apiOrigin: "https://api.example.com",
            site: "example.com",
        });
        // Carried rather than recomputed, and readable, because the preflight it
        // records is a cost paid on every mutating request and invisible in a
        // waterfall unless somebody is looking for the OPTIONS.
        expect(client.origin.crossOrigin).toBe(true);
        expect(client.origin.origin).toBe("https://api.example.com");
        client.close();
    });

    it("records that a same-origin deployment pays no preflight", () => {
        const client = build({
            pageOrigin: "https://app.example.com",
            apiOrigin: "https://app.example.com",
            site: null,
        });
        expect(client.origin.crossOrigin).toBe(false);
        client.close();
    });
});

describe("a request that is made", () => {
    it("goes to the address the compiled route names", async () => {
        const host = harness();
        host.server.reply({ status: 200, body: { ok: true } });

        await expect(
            host.client.call(routeAuthLogin, { body: { email: "a@b.test" }, signal: signal() }),
        ).resolves.toEqual({ ok: true, value: { ok: true } });

        expect(host.server.requests[0]?.url).toBe("https://app.example.com/login");
        expect(host.server.requests[0]?.method).toBe("POST");
    });

    // The path is not in any bundle; it arrives with the session, scoped by the
    // server to what this holder reaches (docs/01-seams.md §4.1).
    it("goes to the address the session named for a holder route", async () => {
        const host = harness();
        await host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        expect(host.server.requests[0]?.url).toBe("https://app.example.com/content/7");
    });

    it("sends the cookies, which are the session", async () => {
        const host = harness();
        await host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        expect(host.server.requests[0]?.credentials).toBe("include");
    });

    // A redirect is an instruction to send this request somewhere else, and
    // "somewhere else" is what every path check in this library exists to bound.
    it("refuses to follow a redirect", async () => {
        const host = harness();
        await host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        expect(host.server.requests[0]?.redirect).toBe("error");
    });

    it("encodes a path parameter rather than interpolating it", async () => {
        const host = harness();
        await host.client.call(routeContentGet, { params: { id: "a b/c" }, signal: signal() });
        expect(host.server.requests[0]?.url).toBe("https://app.example.com/content/a%20b%2Fc");
    });

    it("puts a cursor page's bounds in the query, sorted", async () => {
        const host = harness();
        await host.client.call(routeMediaList, {
            params: { ns: "content", id: "7" },
            query: { limit: 100, after: "abc" },
            signal: signal(),
        });
        expect(host.server.requests[0]?.url).toBe(
            "https://app.example.com/media/content/7?after=abc&limit=100",
        );
    });
});

describe("what never leaves the device", () => {
    it("refuses a path parameter a URL parser would resolve", async () => {
        const host = harness();
        await expect(
            host.client.call(routeContentGet, { params: { id: ".." }, signal: signal() }),
        ).resolves.toEqual({
            ok: false,
            error: { kind: "client", cause: "bad-parameter", retryAfterMs: null },
        });
        expect(host.server.calls).toBe(0);
    });

    it("refuses a body past the descriptor's cap before a byte is sent", async () => {
        const host = harness();
        const huge = { note: "x".repeat(kApiTables.bodyMaxBytes) };
        await expect(
            host.client.call(routeAuthLogin, { body: huge, signal: signal() }),
        ).resolves.toMatchObject({ ok: false, error: { cause: "too-large" } });
        expect(host.server.calls).toBe(0);
    });

    it("sends nothing for a signal that has already fired", async () => {
        const host = harness();
        const controller = new AbortController();
        controller.abort();
        await expect(
            host.client.call(routeContentGet, { params: { id: "7" }, signal: controller.signal }),
        ).resolves.toMatchObject({ ok: false, error: { kind: "transport", cause: "aborted" } });
        expect(host.server.calls).toBe(0);
    });

    // A missing address is a missing address, never a refusal: reporting it as
    // one would rebuild the oracle anvil's stealth 404 removed.
    it("reports a route the session does not carry as not-found", async () => {
        const host = harness({ source: new Source(sessionView({ routes: {} })) });
        await expect(
            host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() }),
        ).resolves.toMatchObject({ ok: false, error: { kind: "client", cause: "no-route" } });
        expect(host.source.refetches).toBe(1);
        expect(host.server.calls).toBe(0);
    });
});

describe("the idempotency key", () => {
    it("is on a route the descriptor marks not idempotent", async () => {
        const host = harness();
        await host.client.call(routeAuthLogin, { body: {}, signal: signal() });
        expect(host.server.header(0, "Idempotency-Key")).toMatch(/^[0-9a-f-]{36}$/);
    });

    it("is absent from a read, which is already safe to repeat", async () => {
        const host = harness();
        await host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        expect(host.server.header(0, "Idempotency-Key")).toBeUndefined();
    });

    // Every attempt at one call is one request. A retry with a fresh key is a
    // second write with extra steps.
    it("is the same on every attempt at one call", async () => {
        const host = harness();
        host.server.reply({ status: 503 }, { status: 200, body: {} });

        await host.client.call(routeAuthLogin, { body: {}, signal: signal() });

        expect(host.server.calls).toBe(2);
        expect(host.server.header(0, "Idempotency-Key")).toBe(
            host.server.header(1, "Idempotency-Key"),
        );
    });

    it("is different for a second call the person asked for", async () => {
        const host = harness();
        await host.client.call(routeAuthLogin, { body: {}, signal: signal() });
        await host.client.call(routeAuthLogin, { body: {}, signal: signal() });
        expect(host.server.header(0, "Idempotency-Key")).not.toBe(
            host.server.header(1, "Idempotency-Key"),
        );
    });
});

describe("a capability", () => {
    it("travels in a header and not in the URL", async () => {
        const host = harness();
        const grant = await capability(host);

        await host.client.call(routeContentDelete, {
            params: { id: "7" },
            capability: grant,
            signal: signal(),
        });

        const at = host.server.calls - 1;
        expect(host.server.header(at, "Capability")).toBe("cap-token");
        expect(host.server.requests[at]?.url).not.toContain("cap-token");
    });

    // The server consumed the token whether or not the response arrived, so a
    // retry reports a failure for an operation that succeeded.
    it("is never auto-retried when redemption burns it", async () => {
        const host = harness();
        const grant = await capability(host);
        const before = host.server.calls;
        host.server.reply({ status: 503 });

        await host.client.call(routeContentDelete, {
            params: { id: "7" },
            capability: grant,
            signal: signal(),
        });

        expect(host.server.calls - before).toBe(1);
    });
});

describe("a credential that expired", () => {
    it("refreshes once and replays the request", async () => {
        const host = harness();
        host.server
            .reply({ status: 401, body: { error: { code: "UNAUTHENTICATED" } } })
            .reply({ status: 200, body: {} })
            .reply({ status: 200, body: { id: "7" } });

        await expect(
            host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() }),
        ).resolves.toEqual({ ok: true, value: { id: "7" } });

        expect(host.server.requests.map((one) => one.url)).toEqual([
            "https://app.example.com/content/7",
            "https://app.example.com/auth/refresh",
            "https://app.example.com/content/7",
        ]);
        expect(host.counts).toContain("replay");
    });

    // A second 401 after a successful refresh is a rejection, not a race, and
    // looping on it is how a client hammers a server that has said no.
    it("gives up after one replay", async () => {
        const host = harness();
        host.server.always({ status: 401, body: { error: { code: "UNAUTHENTICATED" } } });
        host.server.reply(
            { status: 401, body: { error: { code: "UNAUTHENTICATED" } } },
            { status: 200, body: {} },
        );

        const answered = await host.client.call(routeContentGet, {
            params: { id: "7" },
            signal: signal(),
        });

        expect(answered).toMatchObject({ ok: false, error: { code: "UNAUTHENTICATED" } });
        expect(host.server.calls).toBe(3);
    });

    it("reports the failure when the refresh itself is refused", async () => {
        const host = harness();
        host.server
            .reply({ status: 401, body: { error: { code: "UNAUTHENTICATED" } } })
            .reply({ status: 401, body: { error: { code: "UNAUTHENTICATED" } } });

        const answered = await host.client.call(routeContentGet, {
            params: { id: "7" },
            signal: signal(),
        });
        expect(answered).toMatchObject({ ok: false });
        expect(host.server.calls).toBe(2);
    });
});

// The phase gate, in the form the plan states it: two tabs, one expiry, exactly
// one POST to the refresh route. anvil rotates the refresh token as a
// compare-and-swap, so a second concurrent one is a rotation race whose loser is
// signed out with a valid session, in the tab they were using.
describe("two tabs on one session", () => {
    it("makes exactly one request to the refresh route", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new FakeServer();
        const first = harness({ server, room, bus });
        const second = harness({ server, room, bus });

        const held = gate();
        server.always({ status: 200, body: { id: "7" } });
        server.reply(
            { status: 401, body: { error: { code: "UNAUTHENTICATED" } } },
            { status: 401, body: { error: { code: "UNAUTHENTICATED" } } },
            { status: 200, body: {}, until: held.until },
        );

        const both = Promise.all([
            first.client.call(routeContentGet, { params: { id: "7" }, signal: signal() }),
            second.client.call(routeContentGet, { params: { id: "7" }, signal: signal() }),
        ]);
        await settle();
        held.open();

        await expect(both).resolves.toEqual([
            { ok: true, value: { id: "7" } },
            { ok: true, value: { id: "7" } },
        ]);

        const refreshes = server.requests.filter((one) => one.url.endsWith("/auth/refresh"));
        expect(refreshes).toHaveLength(1);
        expect(refreshes[0]?.method).toBe("POST");
    });
});

describe("a 403", () => {
    // Never retried, and the local permission copy is now known to disagree
    // with the server's: anvil's perm_epoch exists because a copy goes stale.
    it("refetches the session and reports the failure", async () => {
        const host = harness();
        host.server.reply({ status: 403, body: { error: { code: "FORBIDDEN" } } });

        const answered = await host.client.call(routeContentGet, {
            params: { id: "7" },
            signal: signal(),
        });

        expect(answered).toMatchObject({ ok: false, error: { code: "FORBIDDEN" } });
        expect(host.server.calls).toBe(1);
        expect(host.source.refetches).toBe(1);
        expect(host.counts).toContain("stale-session");
    });
});

describe("a rate limit", () => {
    it("honours Retry-After exactly rather than inventing a backoff", async () => {
        const host = harness();
        host.server
            .reply({ status: 429, body: { error: { code: "RATE_LIMITED" } }, headers: { "Retry-After": "2" } })
            .reply({ status: 200, body: {} });

        await host.client.call(routeMediaList, {
            params: { ns: "content", id: "7" },
            signal: signal(),
        });

        expect(host.delays).toEqual([2000]);
    });

    // Retrying the other nine calls while one waits is how a rate limit becomes
    // a lockout.
    it("holds back the whole bucket, not the one call", async () => {
        const host = harness();
        host.server.always({
            status: 429,
            body: { error: { code: "RATE_LIMITED" } },
            headers: { "Retry-After": "30" },
        });

        await host.client.call(routeMediaList, {
            params: { ns: "content", id: "7" },
            signal: signal(),
        });
        const before = host.server.calls;

        const second = await host.client.call(routeMediaList, {
            params: { ns: "content", id: "8" },
            signal: signal(),
        });

        expect(second).toMatchObject({ ok: false, error: { cause: "budget-spent" } });
        expect(host.server.calls).toBe(before);
    });
});

describe("one request per address in flight", () => {
    it("joins a duplicate onto the first rather than sending it", async () => {
        const host = harness();
        const held = gate();
        host.server.reply({ status: 200, body: { id: "7" }, until: held.until });

        const first = host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        const second = host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        held.open();

        await expect(Promise.all([first, second])).resolves.toEqual([
            { ok: true, value: { id: "7" } },
            { ok: true, value: { id: "7" } },
        ]);
        expect(host.server.calls).toBe(1);
    });

    it("does not join two calls with different parameters", async () => {
        const host = harness();
        const held = gate();
        host.server.always({ status: 200, body: {}, until: held.until });

        const first = host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        const second = host.client.call(routeContentGet, { params: { id: "8" }, signal: signal() });
        held.open();
        await Promise.all([first, second]);

        expect(host.server.calls).toBe(2);
    });

    // Two identical writes are two writes somebody asked for, and merging them
    // would be this client deciding one of them did not happen. What protects a
    // mutation from its own duplicate is the idempotency key.
    it("never joins two writes", async () => {
        const host = harness();
        const held = gate();
        host.server.always({ status: 200, body: {}, until: held.until });

        const first = host.client.call(routeAuthLogin, { body: {}, signal: signal() });
        const second = host.client.call(routeAuthLogin, { body: {}, signal: signal() });
        held.open();
        await Promise.all([first, second]);

        expect(host.server.calls).toBe(2);
    });

    // One screen unmounting must not cancel a request the other four are still
    // waiting for, which is a dedupe that makes things worse under exactly the
    // conditions that produced the duplicate.
    it("keeps the request alive while another joiner is still waiting", async () => {
        const host = harness();
        const held = gate();
        host.server.reply({ status: 200, body: { id: "7" }, until: held.until });

        const leaving = new AbortController();
        const first = host.client.call(routeContentGet, {
            params: { id: "7" },
            signal: leaving.signal,
        });
        const staying = host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        await settle();

        leaving.abort();
        await expect(first).resolves.toMatchObject({ ok: false, error: { cause: "aborted" } });

        held.open();
        await expect(staying).resolves.toEqual({ ok: true, value: { id: "7" } });
        expect(host.server.calls).toBe(1);
    });

    // Nothing outlives what created it. When the last joiner goes, the request
    // goes with it rather than holding a queue slot the next screen needs.
    it("abandons the request once every joiner has gone", async () => {
        const host = harness();
        const held = gate();
        host.server.always({ status: 200, body: { id: "7" }, until: held.until });

        const first = new AbortController();
        const second = new AbortController();
        const both = Promise.all([
            host.client.call(routeContentGet, { params: { id: "7" }, signal: first.signal }),
            host.client.call(routeContentGet, { params: { id: "7" }, signal: second.signal }),
        ]);
        await settle();

        first.abort();
        second.abort();
        await expect(both).resolves.toEqual([
            { ok: false, error: { kind: "transport", cause: "aborted" } },
            { ok: false, error: { kind: "transport", cause: "aborted" } },
        ]);

        // And the next caller starts a new request rather than joining the
        // abandoned one.
        held.open();
        await host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });
        expect(host.server.calls).toBe(2);
    });
});

describe("a failure that never reached the server", () => {
    it("decodes a proxy's HTML error page to Unknown, carrying the status", async () => {
        const host = harness();
        host.server.reply({ status: 502, text: "<html>502 Bad Gateway</html>" });
        host.server.reply({ status: 502, text: "<html>502 Bad Gateway</html>" });
        host.server.reply({ status: 502, text: "<html>502 Bad Gateway</html>" });

        const answered = await host.client.call(routeContentGet, {
            params: { id: "7" },
            signal: signal(),
        });

        expect(answered).toMatchObject({
            ok: false,
            error: { kind: "server", code: "Unknown", status: 502, requestId: null },
        });
    });

    it("retries a dropped connection and gives up at the cap", async () => {
        const host = harness();
        host.server.always({ transport: true });

        const answered = await host.client.call(routeContentGet, {
            params: { id: "7" },
            signal: signal(),
        });

        expect(answered).toMatchObject({ ok: false, error: { kind: "transport", cause: "network" } });
        expect(host.server.calls).toBe(3);
        expect(host.delays).toHaveLength(2);
    });

    // Twenty tabs retrying independently is a self-inflicted denial of service.
    it("opens the circuit and then fails without sending anything", async () => {
        const host = harness();
        host.server.always({ transport: true });

        for (let i = 0; i < 3; i += 1) {
            await host.client.call(routeContentGet, { params: { id: String(i) }, signal: signal() });
        }
        const sent = host.server.calls;

        const refused = await host.client.call(routeContentGet, {
            params: { id: "9" },
            signal: signal(),
        });

        expect(refused).toMatchObject({ ok: false, error: { cause: "circuit-open" } });
        expect(host.server.calls).toBe(sent);
        expect(host.counts).toContain("circuit-open");
    });
});

describe("what is written down about a request", () => {
    it("records the route, the status and the retries, and no URL", async () => {
        const host = harness();
        host.server.reply({ status: 503 }, { status: 200, body: {} });
        host.tick(5);

        await host.client.call(routeMediaList, {
            params: { ns: "content", id: "7" },
            signal: signal(),
        });

        expect(host.records).toHaveLength(1);
        const record = host.records[0] as Record<string, unknown>;
        expect(record["routeId"]).toBe("media.list");
        expect(record["status"]).toBe(200);
        expect(record["retries"]).toBe(1);
        expect(Object.values(record).join(" ")).not.toContain("https://");
    });

    it("records the request id the server sent on a failure", async () => {
        const host = harness();
        host.server.reply({
            status: 409,
            body: { error: { code: "CONFLICT", request_id: "01JABC" } },
        });

        await host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() });

        const record = host.records[0] as Record<string, unknown>;
        expect(record["requestId"]).toBe("01JABC");
    });
});

// The half of this module a green suite says nothing about.
//
// Each line below fails the build on the day it starts compiling, which is the
// only way a rule about what a caller can EXPRESS stays true. They are written
// against the reference consumer's client, so what is being asserted is what an
// application sees rather than what hammer's own types happen to allow.
describe("what a call site cannot write", () => {
    it("cannot call a destructive route without the grant it requires", async () => {
        const host = harness();
        // @ts-expect-error the route declares a capability scope, so the call
        // does not type-check without one. This is where "the user confirmed
        // it" is enforced: a reviewer cannot forget what the compiler refuses.
        const answered = await host.client.call(routeContentDelete, {
            params: { id: "7" },
            signal: signal(),
        });
        // At run time it is an ordinary call that the server would refuse with
        // CAPABILITY_REQUIRED. The refusal that matters is the one above it.
        expect(answered).toBeDefined();
    });

    it("cannot satisfy one scope with another scope's grant", async () => {
        const host = harness();
        const grant = await capability(host);
        // @ts-expect-error a grant for ContentDelete is not a grant for
        // MediaUpload. The server would refuse it — the binding is in the lookup
        // filter — and refusing it here costs no round trip and no spent token.
        const wrong: Capability<"MediaUpload"> = grant;
        expect(typeof wrong).toBe("string");
    });

    it("cannot offer a capability to a route that takes none", async () => {
        const host = harness();
        const grant = await capability(host);
        const answered = await host.client.call(routeContentGet, {
            params: { id: "7" },
            // @ts-expect-error the route declares no scope, so there is nothing
            // for a capability to satisfy and passing one is a call site that
            // believes something about the route that is not true.
            capability: grant,
            signal: signal(),
        });
        expect(answered.ok).toBe(true);
    });

    it("cannot call a parameterised route without its parameters", async () => {
        const host = harness();
        // @ts-expect-error `content.get` takes an id. A route that does not
        // exist cannot be spelled and a parameter that does cannot be omitted.
        const answered = await host.client.call(routeContentGet, { signal: signal() });
        expect(answered.ok).toBe(false);
    });

    it("cannot call anything without a signal", async () => {
        const host = harness();
        // @ts-expect-error a request nothing can cancel is a request that
        // outlives the screen that wanted it (ENGINEERING_RULES.md §3.1). There is no
        // default, because a default is the thing every call site quietly
        // accepts.
        const answered = host.client.call(routeAuthLogin, { body: {} });
        await expect(answered).rejects.toThrow();
    });

    it("takes no parameters where the route has none", async () => {
        const host = harness();
        // The positive half: a route with no parameters is called without a
        // `params: {}` nobody meant to write.
        await expect(host.client.call(routeAuthLogin, { signal: signal() })).resolves.toMatchObject(
            { ok: true },
        );
    });
});

// A second application's generated module, reduced to the two names a client is
// built from.
//
// Every member below is structurally what the reference application's is — four
// string-keyed records and a number — which is the point: until the hash was
// emitted there was NOTHING that could tell one generated module's tables from
// another's. What stopped them being crossed was that both names came out of one
// file, which is a convention, and a convention holds exactly until an
// application has two generated modules (`docs/15-tasks.md` §The first consumer).
const kOtherHash = "4c1d0e9a7b3f26580d41ca9e8f7205b3d6e1948cf03a27be5d8169ca4f2e70bd";

type OtherApi = {
    readonly params: { readonly "thing.get": { readonly id: string } };
    readonly responses: { readonly "thing.get": unknown };
    readonly code: "THING_MISSING";
    readonly reason: "TOO_LONG";
    readonly hash: typeof kOtherHash;
};

const kOtherApiTables = {
    codes: { OK: 0, THING_MISSING: 1 },
    reasons: { OK: 0, TOO_LONG: 1 },
    rateLimits: {},
    singleUse: {},
    bodyMaxBytes: 1024,
    hash: kOtherHash,
} as const;

describe("two applications, two generated modules", () => {
    // Everything a client needs except the tables, so each case below states the
    // one fact it is about.
    const platform = {
        origin: {
            pageOrigin: "https://app.example.com",
            apiOrigin: "https://app.example.com",
            site: null,
        },
        fetch: new FakeServer().fetch,
        session: new Source(),
        refreshRoute: routeAuthRefresh,
    };

    it("cannot decode this application's answers with another's tables", () => {
        createClient<Api>({
            ...platform,
            // @ts-expect-error the tables carry another descriptor's hash and
            // `Api` demands this one's. Crossed, the client would narrow every
            // error code to a vocabulary the server does not speak, spend a
            // rate-limit budget that belongs to somebody else's buckets, and
            // refuse a body at a cap nothing on the wire agrees with — all of it
            // silently, because both sides type-check on their own.
            api: kOtherApiTables,
        });
    });

    it("cannot decode another application's answers with this one's tables", () => {
        createClient<OtherApi>({
            ...platform,
            // @ts-expect-error and the other direction, which is the one an
            // application actually writes: two generated modules in one bundle
            // and the wrong import completed.
            api: kApiTables,
        });
    });

    it("pairs the tables the generator emitted beside the type it emitted", () => {
        // The positive half, and the reason the check is the hash rather than a
        // brand: the pairing must still be satisfiable from a module that
        // imports nothing (`tests/codegen/emit.test.ts`).
        const client = createClient<Api>({ ...platform, api: kApiTables });
        expect(client.origin.crossOrigin).toBe(false);
        client.close();
    });
});

describe("a response with no body", () => {
    it("is a success carrying nothing rather than a decode failure", async () => {
        const host = harness();
        host.server.reply({ status: 204 });
        await expect(
            host.client.call(routeContentGet, { params: { id: "7" }, signal: signal() }),
        ).resolves.toEqual({ ok: true, value: null });
    });
});
