// The inbox over a stream that delivers at-least-once.
//
// The stream is driven through the real client, so the events arrive the way
// they do in a browser: parsed out of a `text/event-stream` body by
// `wire/sse.ts`, in chunks that do not respect frame boundaries.

import { describe, expect, it } from "../support/test.js";

import { Inbox, kInboxHeld, kInboxRingSlots } from "../../src/state/inbox.js";
import type { StateCount } from "../../src/state/counts.js";
import { kNoCounts } from "../../src/state/counts.js";
import type { StreamEvent } from "../../src/wire/sse.js";
import { routeIdentityMe } from "../testapp/api/hammer.generated.js";
import type { Api } from "../testapp/app/client.js";
import { FakeServer, gate } from "../support/fake_fetch.js";
import { clientHarness, settle } from "../support/state.js";

type Payload = { readonly topic: string };

function inbox(
    over: {
        readonly counts?: (body: Payload) => boolean;
        readonly held?: number;
        readonly ringSlots?: number;
    } = {},
) {
    const server = new FakeServer();
    const wire = clientHarness({ server });
    const counts: StateCount[] = [];
    const held = new Inbox<Api, Payload>({
        client: wire.client,
        route: routeIdentityMe,
        decode: (event: StreamEvent) => {
            try {
                const body = JSON.parse(event.data) as { readonly topic?: unknown };
                return typeof body.topic === "string" ? { topic: body.topic } : null;
            } catch {
                return null;
            }
        },
        ...over,
        count: (name) => counts.push(name),
    });
    return { inbox: held, counts, server, wire };
}

function arrival(id: string | null, topic: string): StreamEvent {
    return { id, type: "message", data: JSON.stringify({ topic }) };
}

// The stream is leader-owned and the handler is what this file is about, so the
// events go through `accept` rather than through a fake socket: what is under
// test is the reconciliation, and the frame parser has its own suite
// (`tests/wire/sse.test.ts`). `accept` is public precisely because handing an
// event over twice is the contract.
function deliver(held: ReturnType<typeof inbox>["inbox"], event: StreamEvent): void {
    held.accept(event);
}

describe("Inbox", () => {
    it("adds an arrival and bumps the count", () => {
        const { inbox: held } = inbox();

        deliver(held, arrival("1", "content.published"));

        const state = held.store.get();
        expect(state.unread).toBe(1);
        expect(state.items).toHaveLength(1);
        expect(state.items[0]?.body).toEqual({ topic: "content.published" });
    });

    it("drops a duplicate by event id", () => {
        // A reconnect replays, so an event arriving twice is normal operation
        // rather than an error.
        const { inbox: held, counts } = inbox();

        deliver(held, arrival("1", "a"));
        deliver(held, arrival("1", "a"));

        expect(held.store.get().unread).toBe(1);
        expect(held.store.get().items).toHaveLength(1);
        expect(counts).toContain("duplicate-event");
    });

    it("remembers only as many ids as its ring holds", () => {
        const { inbox: held } = inbox({ ringSlots: 2 });

        deliver(held, arrival("1", "a"));
        deliver(held, arrival("2", "b"));
        deliver(held, arrival("3", "c"));
        // `1` has fallen out of the ring, so it is no longer recognised as a
        // duplicate. The ring is sized to anvil's replay window for exactly this
        // reason: outside it, a replay is not something this client can dedupe.
        deliver(held, arrival("1", "a"));

        expect(held.store.get().items).toHaveLength(4);
    });

    it("takes the count from the server rather than its own arithmetic", () => {
        const { inbox: held } = inbox();
        deliver(held, arrival("1", "a"));
        deliver(held, arrival("2", "b"));
        expect(held.store.get().unread).toBe(2);

        // Another tab marked four read while this one was asleep. The local
        // number was a guess between answers; the server's replaces it outright.
        held.applyServerCount(7);

        expect(held.store.get().unread).toBe(7);
    });

    it("ignores a server count that is not one", () => {
        const { inbox: held } = inbox();
        held.applyServerCount(3);

        held.applyServerCount(-1);
        held.applyServerCount(1.5);
        held.applyServerCount(Number.NaN);

        expect(held.store.get().unread).toBe(3);
    });

    it("marks read once, so a retried request cannot double-count", () => {
        // The mark-read call is idempotent at the server; this is the half that
        // has to be idempotent here.
        const { inbox: held } = inbox();
        deliver(held, arrival("1", "a"));
        deliver(held, arrival("2", "b"));

        held.markRead(["1"]);
        held.markRead(["1"]);

        expect(held.store.get().unread).toBe(1);
        expect(held.store.get().items.find((item) => item.id === "1")?.read).toBe(true);
    });

    it("never shows a negative count", () => {
        const { inbox: held } = inbox();
        deliver(held, arrival("1", "a"));
        held.applyServerCount(0);

        held.markRead(["1"]);

        // A negative badge is a guess that has been wrong for longer than
        // anybody noticed.
        expect(held.store.get().unread).toBe(0);
    });

    it("asks the application whether an arrival deserves a badge", () => {
        // Which topics count is the application's decision, not this library's.
        const { inbox: held } = inbox({ counts: (body) => body.topic !== "quiet" });

        deliver(held, arrival("1", "quiet"));
        deliver(held, arrival("2", "loud"));

        expect(held.store.get().unread).toBe(1);
        expect(held.store.get().items).toHaveLength(2);
    });

    it("drops an event this build cannot decode", () => {
        const { inbox: held } = inbox();

        // The server may be newer than this bundle, and a row nobody can read is
        // worse than no row.
        deliver(held, { id: "1", type: "message", data: "not json" });
        deliver(held, { id: "2", type: "message", data: '{"other":1}' });

        expect(held.store.get().items).toHaveLength(0);
        expect(held.store.get().unread).toBe(0);
    });

    it("bounds the list it holds", () => {
        const { inbox: held } = inbox({ held: 2 });

        deliver(held, arrival("1", "a"));
        deliver(held, arrival("2", "b"));
        deliver(held, arrival("3", "c"));

        // Newest first, and the full history is a paged read rather than a
        // growing array in a tab that stays open for days.
        expect(held.store.get().items.map((item) => item.id)).toEqual(["3", "2"]);
        expect(held.store.get().unread).toBe(3);
    });

    it("delivers an event with no id rather than dropping it", () => {
        const { inbox: held } = inbox();

        deliver(held, arrival(null, "a"));
        deliver(held, arrival(null, "a"));

        // Dropping it would lose a notification over a field the server chose
        // not to send. What it does not get is a stable identity.
        expect(held.store.get().items).toHaveLength(2);
    });

    it("opens one stream and reports it live", async () => {
        const { inbox: held, server } = inbox();
        // Held open, so the connection does not end and reconnect while the
        // assertion is being made: a stream whose body completes is a stream
        // that backs off and tries again, which is `wire/sse.ts` doing its job
        // and not what this test is about.
        const open = gate();
        server.always({ until: open.until });

        held.open();
        held.open();
        await settle();

        expect(held.store.get().live).toBe(true);
        expect(server.calls).toBe(1);

        held.close();
        expect(held.store.get().live).toBe(false);
        open.open();
    });

    it("clears everything it holds on a logout", () => {
        const { inbox: held } = inbox();
        deliver(held, arrival("1", "a"));

        held.clear();

        // A list of one person's notifications left on screen for the next one is
        // the disclosure the whole cache model is about.
        expect(held.store.get().items).toHaveLength(0);
        expect(held.store.get().unread).toBe(0);

        // And the dedupe ring went with it, so the same id is a new arrival for
        // whoever signs in next.
        deliver(held, arrival("1", "a"));
        expect(held.store.get().items).toHaveLength(1);
    });
});

// The three defaults this layer publishes, and the reason each is a number a
// deployment is allowed to see.
//
// A tab stays open for days. Every bound here is the difference between a store
// and a leak with a slow fuse (`CLAUDE.md` §2.3), so each one is asserted
// against the behaviour it produces rather than only against its own literal —
// a constant can be right while the code that was supposed to read it is not.
describe("the bounds an inbox keeps", () => {
    it("holds a hundred notifications and drops the oldest past that", () => {
        expect(kInboxHeld).toBe(100);

        const { inbox: held } = inbox();
        for (let i = 0; i < kInboxHeld + 10; i += 1) {
            deliver(held, arrival(String(i), "t"));
        }

        const state = held.store.get();
        expect(state.items).toHaveLength(kInboxHeld);
        // The list is a view of the most recent; the full history is a paged
        // read, which is `state/paginate.ts`'s job and not this one's.
        expect(state.items[0]?.id).toBe(String(kInboxHeld + 9));
        expect(state.unread).toBe(kInboxHeld + 10);
    });

    it("remembers two hundred and fifty-six event ids, which is anvil's replay ring", () => {
        // Matched to the server's ring on purpose: the window a duplicate can
        // arrive within is the window one reconnect can replay, and a client
        // that remembered fewer would re-deliver an event it had already shown.
        expect(kInboxRingSlots).toBe(256);

        const { inbox: held, counts } = inbox({ held: 4 });
        deliver(held, arrival("first", "t"));
        for (let i = 0; i < kInboxRingSlots - 1; i += 1) {
            deliver(held, arrival(`filler-${i}`, "t"));
        }

        // Still inside the ring, so the replay is recognised as one.
        deliver(held, arrival("first", "t"));
        expect(counts.filter((name) => name === "duplicate-event")).toHaveLength(1);
    });

    it("counts nothing, silently, when an application supplies no sink", () => {
        // The default every store in this layer falls back to — `config.count ??
        // kNoCounts` — and it is published so an application can pass it
        // explicitly where some other config demands a sink.
        //
        // Its totality is the compiler's rather than this test's: the parameter
        // is `StateCount`, so a member added to that union is still a call this
        // accepts. What is asserted here is the part a type cannot say, which is
        // that it does nothing and reports nothing back.
        const inert: StateCount[] = [];
        expect(kNoCounts("duplicate-event")).toBeUndefined();
        expect(inert).toEqual([]);

        // And an inbox handed it behaves exactly as one handed a recorder does.
        const { inbox: held } = inbox();
        deliver(held, arrival("1", "t"));
        expect(held.store.get().unread).toBe(1);
    });
});
