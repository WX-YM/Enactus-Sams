// The two write mechanisms, over a real resource.
//
// Both are about a value that is on screen and not yet true, so both are driven
// against a `Resource` rather than a stand-in: what makes an optimistic value
// safe is that it reaches the store and not the cache, and a stand-in for the
// resource would assert that the test knows that.

import { describe, expect, it } from "../support/test.js";

import { fail, ok } from "../../src/core/result.js";
import type { ResourceFailure } from "../../src/state/resource.js";
import { ResourceStore } from "../../src/state/resource.js";
import { optimistic } from "../../src/state/optimistic.js";
import { writeVersioned } from "../../src/state/versioned.js";
import type { StateCount } from "../../src/state/counts.js";
import type { ErrorCode } from "../testapp/api/hammer.generated.js";
import { routeContentGet } from "../testapp/api/hammer.generated.js";
import type { Api } from "../testapp/app/client.js";
import { FakeServer } from "../support/fake_fetch.js";
import { clientHarness, settle } from "../support/state.js";

type Doc = { readonly title: string; readonly version?: number };

function signal(): AbortSignal {
    return new AbortController().signal;
}

// Typed as the failure the generated vocabulary produces, not as the loose
// `HammerError`: the code is a member of a literal union here, so a test that
// misspelled one would not compile.
function serverSaid(code: ErrorCode): ResourceFailure<Api> {
    return { kind: "server", code, status: 409, requestId: null, fields: null };
}

async function opened(server: FakeServer) {
    const wire = clientHarness({ server });
    const counts: StateCount[] = [];
    const store = new ResourceStore<Api>({
        client: wire.client,
        cache: { classes: {}, defaultMaxEntries: 8 },
        now: wire.now,
        count: (name) => counts.push(name),
    });
    const resource = store.open(routeContentGet, { params: { id: "7" } });
    await settle();
    return { store, resource, counts, count: (name: StateCount) => counts.push(name), server, wire };
}

const kDocument = {
    body: { title: "one", version: 3 },
    headers: { "Cache-Control": "max-age=600" },
};

describe("writeVersioned", () => {
    it("carries the version the document on screen was read at", async () => {
        const { resource } = await opened(new FakeServer().always(kDocument));
        const sent: number[] = [];

        const outcome = await writeVersioned<Api, Doc, number>({
            resource: resource as never,
            versionOf: (document) => document.version ?? null,
            send: async (version) => {
                sent.push(version);
                return ok({ title: "two", version: 4 });
            },
            signal: signal(),
        });

        expect(sent).toEqual([3]);
        expect(outcome.kind).toBe("written");
        expect(outcome.document).toEqual({ title: "two", version: 4 });
        // Confirmed against the document the server returned, not the one the
        // caller computed.
        expect(resource.state.get().value).toEqual({ title: "two", version: 4 });
    });

    it("refuses a write with no version to carry", async () => {
        const { resource } = await opened(
            new FakeServer().always({ body: { title: "one" }, headers: kDocument.headers }),
        );
        let sends = 0;

        const outcome = await writeVersioned<Api, Doc, number>({
            resource: resource as never,
            versionOf: (document) => document.version ?? null,
            send: async () => {
                sends += 1;
                return ok({ title: "two" });
            },
            signal: signal(),
        });

        // Sending anyway is an unconditional overwrite — the lost update this
        // mechanism exists to detect, performed deliberately.
        expect(outcome.kind).toBe("no-version");
        expect(sends).toBe(0);
    });

    it("surfaces a reconciliation on a mismatch and never re-sends", async () => {
        const server = new FakeServer()
            .reply(kDocument)
            .always({ body: { title: "theirs", version: 9 }, headers: kDocument.headers });
        const { resource, counts, count } = await opened(server);
        let sends = 0;

        const outcome = await writeVersioned<Api, Doc, number>({
            resource: resource as never,
            versionOf: (document) => document.version ?? null,
            send: async () => {
                sends += 1;
                return fail(serverSaid("VERSION_MISMATCH"));
            },
            signal: signal(),
            count,
        });

        // Re-sending the same body is a lost update with extra steps.
        expect(sends).toBe(1);
        expect(outcome.kind).toBe("conflict");
        expect(outcome.current).toEqual({ title: "theirs", version: 9 });
        expect(counts).toContain("version-mismatch");
    });

    it("reports a plain CONFLICT as a failure rather than reconciling it", async () => {
        const { resource } = await opened(new FakeServer().always(kDocument));

        const outcome = await writeVersioned<Api, Doc, number>({
            resource: resource as never,
            versionOf: (document) => document.version ?? null,
            send: async () => fail(serverSaid("CONFLICT")),
            signal: signal(),
        });

        // Both are 409 and they want opposite things: one is reconciled, the
        // other is never resolved silently.
        expect(outcome.kind).toBe("failed");
    });

    it("refuses to read a version off an unconfirmed document", async () => {
        const { resource } = await opened(new FakeServer().always(kDocument));
        resource.provisional({ title: "guessed", version: 99 } as never);
        let sends = 0;

        const outcome = await writeVersioned<Api, Doc, number>({
            resource: resource as never,
            versionOf: (document) => document.version ?? null,
            send: async () => {
                sends += 1;
                return ok({ title: "two", version: 4 });
            },
            signal: signal(),
        });

        expect(outcome.kind).toBe("no-version");
        expect(sends).toBe(0);
    });
});

describe("optimistic", () => {
    it("applies locally and confirms against the server's own document", async () => {
        const { resource } = await opened(new FakeServer().always(kDocument));
        const seen: unknown[] = [];
        resource.state.subscribe((state) => seen.push(state.value));

        const outcome = await optimistic<Api, Doc>({
            resource: resource as never,
            apply: (current) => ({ ...current, title: "guessed" }),
            // Normalised on the way through, which is the case a client that
            // publishes its own guess gets wrong.
            commit: async () => ok({ title: "GUESSED", version: 4 }),
            signal: signal(),
        });

        expect(seen).toEqual([{ title: "guessed", version: 3 }, { title: "GUESSED", version: 4 }]);
        expect(outcome.kind).toBe("confirmed");
        expect(resource.state.get().stale).toBe(false);
    });

    it("keeps the unconfirmed value out of the cache", async () => {
        const { store, resource } = await opened(new FakeServer().always(kDocument));

        resource.provisional({ title: "guessed", version: 3 } as never);
        expect(resource.state.get().stale).toBe(true);

        // The provisional value reached the store and not the cache, so the
        // next thing to open this address reads the last value the server
        // confirmed rather than inheriting a guess.
        resource.release();
        const second = store.open(routeContentGet, { params: { id: "7" } });
        expect(second.state.get().value).toEqual({ title: "one", version: 3 });
        second.release();
    });

    it("restore brings back the confirmed value without a request", async () => {
        const server = new FakeServer().always(kDocument);
        const { resource } = await opened(server);

        resource.provisional({ title: "guessed", version: 3 } as never);
        resource.restore();

        expect(resource.state.get().value).toEqual({ title: "one", version: 3 });
        expect(resource.hasProvisional()).toBe(false);
        expect(server.calls).toBe(1);
    });

    it("rolls back to the last confirmed value and counts it", async () => {
        const { resource, counts, count } = await opened(new FakeServer().always(kDocument));

        const outcome = await optimistic<Api, Doc>({
            resource: resource as never,
            apply: (current) => ({ ...current, title: "guessed" }),
            commit: async () => fail({ kind: "transport", cause: "network" }),
            signal: signal(),
            count,
        });

        expect(outcome.kind).toBe("rolled-back");
        expect(resource.state.get().value).toEqual({ title: "one", version: 3 });
        // The count of times the interface told somebody something had happened
        // that had not.
        expect(counts).toContain("optimistic-rollback");
    });

    it("re-reads when the write returned no document to confirm against", async () => {
        const server = new FakeServer()
            .reply(kDocument)
            .always({ body: { title: "current", version: 4 }, headers: kDocument.headers });
        const { resource } = await opened(server);

        const outcome = await optimistic<Api, Doc>({
            resource: resource as never,
            apply: (current) => ({ ...current, title: "guessed" }),
            commit: async () => ok(null),
            signal: signal(),
        });

        // The local guess is still a guess, so what ends up on screen came from
        // the server.
        expect(outcome.kind).toBe("confirmed");
        expect(resource.state.get().value).toEqual({ title: "current", version: 4 });
    });

    it("refuses a second update over an outstanding one", async () => {
        const { resource } = await opened(new FakeServer().always(kDocument));
        resource.provisional({ title: "guessed", version: 3 } as never);

        const outcome = await optimistic<Api, Doc>({
            resource: resource as never,
            apply: (current) => ({ ...current, title: "twice" }),
            commit: async () => ok({ title: "twice", version: 4 }),
            signal: signal(),
        });

        // A function applied to a value nobody agreed to is a fabrication built
        // on a fabrication.
        expect(outcome.kind).toBe("refused");
        expect(resource.state.get().value).toEqual({ title: "guessed", version: 3 });
    });

    it("refuses when there is nothing on screen to apply to", async () => {
        const { resource } = await opened(new FakeServer().always({ status: 404, body: {} }));

        const outcome = await optimistic<Api, Doc>({
            resource: resource as never,
            apply: (current) => current,
            commit: async () => ok(null),
            signal: signal(),
        });

        expect(outcome.kind).toBe("refused");
    });
});
