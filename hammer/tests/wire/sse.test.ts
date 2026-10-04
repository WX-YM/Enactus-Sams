// The stream: the parser, the resume, the dedupe and the handover.
//
// Driven through a fetch stand-in that hands back a real `Response` over a
// `ReadableStream` the test writes into, so the parser under test is fed the way
// a socket feeds it — in chunks that do not respect frame boundaries, which is
// the case a parser written against whole frames gets wrong.

import { describe, expect, it } from "../support/test.js";

import type { FetchLike } from "../../src/wire/client.js";
import { leadership } from "../../src/wire/leader.js";
import type { StreamClosed, StreamEvent } from "../../src/wire/sse.js";
import { kStreamRingSlots, openStream } from "../../src/wire/sse.js";
import { ChannelBus } from "../support/fake_channel.js";
import { LockRoom } from "../support/fake_locks.js";

type Connection = {
    readonly headers: Readonly<Record<string, string>>;
    readonly push: (text: string) => void;
    readonly end: () => void;
};

class StreamServer {
    readonly connections: Connection[] = [];
    private readonly statuses: number[] = [];

    // Answer the next connection with a status rather than a stream.
    refuse(...statuses: readonly number[]): this {
        this.statuses.push(...statuses);
        return this;
    }

    get last(): Connection | undefined {
        return this.connections[this.connections.length - 1];
    }

    readonly fetch: FetchLike = async (_url, init) => {
        const status = this.statuses.shift();
        if (status !== undefined) {
            return new Response(null, { status });
        }

        let controller: ReadableStreamDefaultController<Uint8Array> | null = null;
        const encoder = new TextEncoder();
        const body = new ReadableStream<Uint8Array>({
            start: (given) => {
                controller = given;
            },
        });

        const connection: Connection = {
            headers: { ...((init.headers ?? {}) as Record<string, string>) },
            push: (text) => controller?.enqueue(encoder.encode(text)),
            end: () => {
                try {
                    controller?.close();
                } catch {
                    // Already closed by an abort, which is the case this is
                    // tolerating rather than asserting.
                }
            },
        };
        this.connections.push(connection);

        init.signal?.addEventListener("abort", () => {
            controller?.error(new DOMException("aborted", "AbortError"));
        });

        return new Response(body, {
            status: 200,
            headers: { "Content-Type": "text/event-stream" },
        });
    };
}

type Host = {
    readonly server: StreamServer;
    readonly events: StreamEvent[];
    readonly closed: StreamClosed[];
    readonly delays: number[];
    readonly stream: ReturnType<typeof openStream>;
};

function open(
    over: {
        readonly server?: StreamServer;
        readonly room?: LockRoom;
        readonly bus?: ChannelBus;
        readonly events?: StreamEvent[];
        readonly ringSlots?: number;
    } = {},
): Host {
    const server = over.server ?? new StreamServer();
    const events = over.events ?? [];
    const closed: StreamClosed[] = [];
    const delays: number[] = [];

    const stream = openStream({
        address: async () => ({ ok: true, value: "https://app.example.com/inbox/stream" }),
        fetch: server.fetch,
        leadership: leadership((over.room ?? new LockRoom()).tab()),
        fanOut: (over.bus ?? new ChannelBus()).tab(),
        name: "hammer.stream.inbox",
        onEvent: (event) => events.push(event),
        onClosed: (one) => closed.push(one),
        sleep: async (ms) => {
            delays.push(ms);
        },
        unit: () => 0.5,
        ...(over.ringSlots === undefined ? {} : { ringSlots: over.ringSlots }),
    });

    return { server, events, closed, delays, stream };
}

async function settle(): Promise<void> {
    for (let i = 0; i < 12; i += 1) {
        await Promise.resolve();
    }
}

describe("the frames", () => {
    it("delivers an event with its id, type and data", async () => {
        const host = open();
        await settle();

        host.server.last?.push("id: 7\nevent: notification\ndata: {\"n\":1}\n\n");
        await settle();

        expect(host.events).toEqual([{ id: "7", type: "notification", data: '{"n":1}' }]);
        host.stream.close();
    });

    // A socket does not respect frame boundaries, which is the case a parser
    // written against whole frames gets wrong.
    it("delivers an event split across chunks", async () => {
        const host = open();
        await settle();

        host.server.last?.push("id: 7\nev");
        await settle();
        host.server.last?.push("ent: ping\ndata: x");
        await settle();
        host.server.last?.push("\n\n");
        await settle();

        expect(host.events).toEqual([{ id: "7", type: "ping", data: "x" }]);
        host.stream.close();
    });

    it("joins a multi-line data field with newlines", async () => {
        const host = open();
        await settle();
        host.server.last?.push("data: one\ndata: two\n\n");
        await settle();

        expect(host.events[0]?.data).toBe("one\ntwo");
        host.stream.close();
    });

    it("names an event with no type the protocol's default", async () => {
        const host = open();
        await settle();
        host.server.last?.push("data: x\n\n");
        await settle();

        expect(host.events[0]?.type).toBe("message");
        host.stream.close();
    });

    // Its whole job is to stop a proxy reaping a connection that has been quiet.
    it("delivers nothing for a keep-alive comment", async () => {
        const host = open();
        await settle();
        host.server.last?.push(": keep-alive\n\n");
        await settle();

        expect(host.events).toEqual([]);
        host.stream.close();
    });

    it("accepts the carriage returns a proxy may introduce", async () => {
        const host = open();
        await settle();
        host.server.last?.push("data: x\r\n\r\n");
        await settle();

        expect(host.events).toEqual([{ id: null, type: "message", data: "x" }]);
        host.stream.close();
    });
});

describe("resuming", () => {
    it("sends no Last-Event-ID on a first connection", async () => {
        const host = open();
        await settle();
        expect(host.server.connections[0]?.headers["Last-Event-ID"]).toBeUndefined();
        host.stream.close();
    });

    // A reconnect replays from where the client got to, which is what makes a
    // dropped connection a latency event rather than a data-loss one.
    it("resumes from the last id it saw", async () => {
        const host = open();
        await settle();

        host.server.last?.push("id: 41\ndata: x\n\n");
        await settle();
        host.server.last?.end();
        await settle();

        expect(host.server.connections).toHaveLength(2);
        expect(host.server.connections[1]?.headers["Last-Event-ID"]).toBe("41");
        host.stream.close();
    });

    // A frame with no data dispatches nothing and still moves the resume point,
    // which is how a server advances a client past events it has no business
    // seeing.
    it("advances the resume point on a frame that carries no data", async () => {
        const host = open();
        await settle();

        host.server.last?.push("id: 99\n\n");
        await settle();
        host.server.last?.end();
        await settle();

        expect(host.events).toEqual([]);
        expect(host.server.connections[1]?.headers["Last-Event-ID"]).toBe("99");
        host.stream.close();
    });

    it("waits a jittered backoff between attempts", async () => {
        const host = open();
        await settle();
        host.server.last?.end();
        await settle();

        expect(host.delays).toHaveLength(1);
        expect(host.delays[0]).toBeGreaterThan(0);
        host.stream.close();
    });

    // The server naming a reconnect delay is the same instruction Retry-After
    // is, and it is honoured the same way.
    it("honours a retry field the server sent", async () => {
        const host = open();
        await settle();

        host.server.last?.push("retry: 4500\ndata: x\n\n");
        await settle();
        host.server.last?.end();
        await settle();

        expect(host.delays).toEqual([4500]);
        host.stream.close();
    });
});

describe("at-least-once", () => {
    // A reconnect replays. An event handled twice must be a no-op rather than a
    // second notification.
    it("drops an event it has already handled", async () => {
        const host = open();
        await settle();

        host.server.last?.push("id: 7\ndata: x\n\n");
        await settle();
        host.server.last?.end();
        await settle();
        host.server.last?.push("id: 7\ndata: x\n\nid: 8\ndata: y\n\n");
        await settle();

        expect(host.events.map((event) => event.id)).toEqual(["7", "8"]);
        host.stream.close();
    });

    // The window is bounded on purpose: an unbounded set on a connection open
    // for days is a leak with a slow fuse, and the handler has to be idempotent
    // anyway.
    it("remembers a bounded number of ids and no more", async () => {
        const host = open({ ringSlots: 2 });
        await settle();

        host.server.last?.push("id: 1\ndata: a\n\nid: 2\ndata: b\n\nid: 3\ndata: c\n\n");
        await settle();
        host.server.last?.push("id: 1\ndata: a\n\n");
        await settle();

        expect(host.events.map((event) => event.id)).toEqual(["1", "2", "3", "1"]);
        host.stream.close();
    });
});

describe("the tabs that are not connected", () => {
    it("receives the leader's events without opening a connection", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const leader = open({ room, bus });
        await settle();

        const followerEvents: StreamEvent[] = [];
        const follower = open({ room, bus, events: followerEvents, server: leader.server });
        await settle();

        leader.server.last?.push("id: 7\ndata: x\n\n");
        await settle();

        expect(followerEvents).toEqual([{ id: "7", type: "message", data: "x" }]);
        // One connection for the session, not one per tab: the twelfth is
        // somebody else's file descriptor.
        expect(leader.server.connections).toHaveLength(1);

        leader.stream.close();
        follower.stream.close();
    });

    // The browser releases a discarded tab's lock, so the queue is the takeover.
    it("takes the stream over when the leader releases the lock", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new StreamServer();
        const leader = open({ room, bus, server });
        await settle();

        const follower = open({ room, bus, server, events: [] });
        await settle();
        expect(server.connections).toHaveLength(1);

        leader.stream.close();
        await settle();

        expect(server.connections).toHaveLength(2);
        follower.stream.close();
    });

    it("ignores a broadcast that belongs to another stream", async () => {
        const bus = new ChannelBus();
        const other = bus.tab();
        const host = open({ bus });
        await settle();

        other.post({
            kind: "hammer.stream",
            name: "hammer.stream.something-else",
            event: { id: "1", type: "message", data: "x" },
        });
        other.post({ kind: "not-hammer's", name: "hammer.stream.inbox" });
        await settle();

        expect(host.events).toEqual([]);
        host.stream.close();
    });
});

describe("a connection the server refuses", () => {
    // A 4xx is an answer: the stream will not start, and reconnecting into it is
    // a client asking the same question every few seconds forever.
    it("closes rather than reconnecting on a 404", async () => {
        const server = new StreamServer().refuse(404);
        const host = open({ server });
        await settle();

        expect(host.closed).toEqual([
            { error: { kind: "server", code: "Unknown", status: 404, requestId: null, fields: null } },
        ]);
        expect(server.connections).toHaveLength(0);
    });

    it("closes on a 401, which the next ordinary call is what recovers", async () => {
        const server = new StreamServer().refuse(401);
        const host = open({ server });
        await settle();

        expect(host.closed[0]?.error).toMatchObject({ status: 401 });
    });

    it("reconnects through a 503", async () => {
        const server = new StreamServer().refuse(503);
        const host = open({ server });
        await settle();

        expect(host.closed).toEqual([]);
        expect(server.connections).toHaveLength(1);
        host.stream.close();
    });
});

describe("closing", () => {
    it("reports the close and stops reconnecting", async () => {
        const host = open();
        await settle();

        host.stream.close();
        await settle();

        expect(host.closed).toEqual([{ error: null }]);
        const attempts = host.server.connections.length;
        await settle();
        expect(host.server.connections).toHaveLength(attempts);
    });

    it("is idempotent, because a close arrives from three directions", async () => {
        const host = open();
        await settle();

        host.stream.close();
        host.stream.close();
        await settle();

        expect(host.closed).toHaveLength(1);
    });

    it("stops delivering another tab's events once it is closed", async () => {
        const bus = new ChannelBus();
        const other = bus.tab();
        const host = open({ bus });
        await settle();

        host.stream.close();
        other.post({
            kind: "hammer.stream",
            name: "hammer.stream.inbox",
            event: { id: "1", type: "message", data: "x" },
        });
        await settle();

        expect(host.events).toEqual([]);
    });
});

describe("the dedupe ring this module ships", () => {
    // Bounded, and the bound is the whole point: a tab stays open for days, and
    // a set of every event id it has ever seen is a leak with a slow fuse
    // (`CLAUDE.md` §2.3). Sixty-four is the reconnect replay window — enough
    // that a resume does not re-deliver, small enough that the memory is a
    // rounding error.
    it("is sixty-four slots, and is published so a caller can widen it", () => {
        expect(kStreamRingSlots).toBe(64);
        expect(Number.isInteger(kStreamRingSlots)).toBe(true);
        expect(kStreamRingSlots).toBeGreaterThan(0);
    });
});
