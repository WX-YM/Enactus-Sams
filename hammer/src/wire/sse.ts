// The live stream: one connection per session, resumed rather than restarted,
// and idempotent at the handler because the wire is at-least-once.
//
// --- why this is not `EventSource` ------------------------------------------
//
// `EventSource` reconnects by itself, on a fixed delay, with no jitter. Every
// tab of every client disconnected by one deploy therefore reconnects in the
// same second — which is the thundering herd `wire/retry.ts` spends a paragraph
// avoiding, arriving through the one API that looked like it would save work. It
// also cannot carry a header, so a resume is whatever the browser chose to send,
// and it cannot be handed an address that has been through the route builder.
//
// So the transport is `fetch` and the frames are parsed here. The parser is
// forty lines; the alternative was a reconnect policy this library does not
// control, on the one connection that stays open for hours.
//
// --- one connection, held by the leader -------------------------------------
//
// A person with twelve tabs is twelve connections, each holding a file
// descriptor for its whole life, against a ceiling anvil derives from
// `RLIMIT_NOFILE` (`notifications/sse.h`). The twelfth is not free; it is
// somebody else's.
//
// So every tab queues on one lock and exactly one of them streams. Queueing is
// right here and wrong for the refresh, and the difference is what the work IS:
// a second refresh is a rotation race, while a second CONNECTION is exactly what
// the next tab should open the moment the first is discarded. The browser
// releases a dead tab's lock, so the queue is the takeover.
//
// --- at-least-once, at both ends --------------------------------------------
//
// A reconnect replays: anvil's ring carries a per-stream sequence, and a client
// resuming from `Last-Event-ID` is sent what it missed — including, when it was
// away for longer than the ring, a gap it can only recover by re-reading. Events
// are deduplicated by id here, and the handler still has to be idempotent,
// because the dedupe window is bounded and the stream is a latency optimisation
// rather than the system of record.

import type { HammerError } from "../core/errors.js";
import type { Result } from "../core/result.js";

import type { FetchLike } from "./client.js";
import type { FanOut, Leadership } from "./leader.js";
import type { RetryPolicy, UnitRandom } from "./retry.js";
import { fullJitter, kDefaultRetryPolicy, randomUnit } from "./retry.js";
import type { Sleep } from "./schedule.js";
import { sleep as platformSleep } from "./schedule.js";

export type StreamEvent = {
    // The `id:` field, or null for a frame that carried none. It is what a
    // resume sends back and what the dedupe is keyed by.
    readonly id: string | null;

    // The `event:` field. `message` where the frame named none, which is the
    // protocol's default rather than this library's choice.
    readonly type: string;

    // The `data:` field, joined by newlines and otherwise untouched. Decoding it
    // is the application's: the shape of a notification is an application's
    // (`docs/01-seams.md` §4), and a library that parsed it would be a library
    // holding a schema nobody gave it.
    readonly data: string;
};

export type StreamClosed = {
    // Null when the application closed it.
    readonly error: HammerError | null;
};

export type StreamConfig = {
    // Where to connect, resolved per attempt rather than once: a holder route's
    // address arrives with the session, and a session refetched between two
    // attempts may carry a different one.
    readonly address: (signal: AbortSignal) => Promise<Result<string, HammerError>>;

    readonly fetch: FetchLike;
    readonly leadership: Leadership;
    readonly fanOut: FanOut;

    // The lock and the channel topic for THIS stream. One per stream route, so
    // two different streams do not take each other's turn.
    readonly name: string;

    readonly onEvent: (event: StreamEvent) => void;
    readonly onClosed?: (closed: StreamClosed) => void;
    readonly onReconnect?: () => void;

    // Where a resume starts. Null on a first connection; an application that
    // kept one is resuming across a reload, which the protocol allows and the
    // server's ring may or may not still cover.
    readonly lastEventId?: string | null;

    readonly sleep?: Sleep;
    readonly retry?: RetryPolicy;
    readonly unit?: UnitRandom;

    // How many event ids the dedupe remembers. Matched to anvil's ring, which is
    // the most one reconnect can replay in a burst.
    readonly ringSlots?: number;
};

export type Stream = {
    readonly close: () => void;
};

export const kStreamRingSlots = 64;

// A 4xx is an ANSWER: the stream will not start, and reconnecting into it is a
// client asking the same question every few seconds forever. The two exceptions
// are the ones that name a wait rather than a refusal.
function willRetry(status: number): boolean {
    if (status === 408 || status === 429) {
        return true;
    }
    return status < 400 || status >= 500;
}

// Bounded, evicting, and keyed by the only thing that identifies an event across
// a reconnect. An unbounded set on a connection that is open for days is a leak
// with a slow fuse (`ENGINEERING_RULES.md` §2.3).
class SeenIds {
    private readonly slots: number;
    private readonly order: string[];
    private readonly ids: Set<string>;

    constructor(slots: number) {
        this.slots = slots;
        this.order = [];
        this.ids = new Set();
    }

    // False when this id has already been handled.
    admit(id: string | null): boolean {
        if (id === null) {
            // A frame with no id cannot be deduplicated and cannot be resumed
            // from. anvil's carry one; a keep-alive comment is not an event.
            return true;
        }
        if (this.ids.has(id)) {
            return false;
        }
        this.ids.add(id);
        this.order.push(id);
        if (this.order.length > this.slots) {
            const evicted = this.order.shift();
            if (evicted !== undefined) {
                this.ids.delete(evicted);
            }
        }
        return true;
    }
}

// The frame parser.
//
// The wire format is lines: `field: value`, a blank line dispatching whatever
// has accumulated, a leading colon marking a comment. Three details are easy to
// miss and each is in the specification for a reason:
//
//   A `data:` field ACCUMULATES across lines, joined by newlines.
//   A frame with no data dispatches NOTHING and still updates the resume point,
//   which is how a server advances a client past events it has no business
//   seeing.
//   One optional space after the colon belongs to the field, not to the value.
class FrameParser {
    private pending = "";
    private data: string[] = [];
    private type = "";
    private id: string | null = null;

    // Set by a `retry:` field. A server naming a reconnect delay is the same
    // instruction `Retry-After` is, and it is honoured the same way.
    retryMs: number | null = null;

    lastEventId: string | null = null;

    push(chunk: string, dispatch: (event: StreamEvent) => void): void {
        this.pending += chunk;

        for (;;) {
            const at = this.pending.search(/\r\n|\n|\r/);
            if (at < 0) {
                return;
            }
            const line = this.pending.slice(0, at);
            const skip = this.pending.startsWith("\r\n", at) ? 2 : 1;
            this.pending = this.pending.slice(at + skip);
            this.line(line, dispatch);
        }
    }

    private line(line: string, dispatch: (event: StreamEvent) => void): void {
        if (line.length === 0) {
            this.dispatch(dispatch);
            return;
        }
        if (line.startsWith(":")) {
            // A comment, which is what a keep-alive is. It resets no state and
            // dispatches nothing; its whole job is to stop a proxy reaping a
            // connection that has been quiet.
            return;
        }

        const colon = line.indexOf(":");
        const field = colon < 0 ? line : line.slice(0, colon);
        let value = colon < 0 ? "" : line.slice(colon + 1);
        if (value.startsWith(" ")) {
            value = value.slice(1);
        }

        switch (field) {
            case "event":
                this.type = value;
                break;
            case "data":
                this.data.push(value);
                break;
            case "id":
                // A NUL is the one value the specification says to ignore rather
                // than to store, and the reason is downstream: the id comes back
                // in a header, and a header value cannot carry one.
                if (!value.includes("\u0000")) {
                    this.id = value;
                    this.lastEventId = value;
                }
                break;
            case "retry": {
                const ms = Number(value);
                if (Number.isInteger(ms) && ms >= 0) {
                    this.retryMs = ms;
                }
                break;
            }
            default:
                break;
        }
    }

    private dispatch(dispatch: (event: StreamEvent) => void): void {
        if (this.data.length === 0) {
            this.type = "";
            this.id = null;
            return;
        }
        dispatch({
            id: this.id,
            type: this.type.length === 0 ? "message" : this.type,
            data: this.data.join("\n"),
        });
        this.data = [];
        this.type = "";
        this.id = null;
    }
}

type Broadcast = {
    readonly kind: "hammer.stream";
    readonly name: string;
    readonly event: StreamEvent;
};

function decodeBroadcast(message: unknown, name: string): StreamEvent | null {
    if (typeof message !== "object" || message === null) {
        return null;
    }
    const shaped = message as Partial<Broadcast>;
    if (shaped.kind !== "hammer.stream" || shaped.name !== name) {
        return null;
    }
    const event = shaped.event;
    if (typeof event !== "object" || event === null) {
        return null;
    }
    const { id, type, data } = event as Partial<StreamEvent>;
    if (typeof type !== "string" || typeof data !== "string") {
        return null;
    }
    return { id: typeof id === "string" ? id : null, type, data };
}

export function openStream(config: StreamConfig): Stream {
    const sleep = config.sleep ?? platformSleep;
    const retry = config.retry ?? kDefaultRetryPolicy;
    const unit = config.unit ?? randomUnit;
    const seen = new SeenIds(config.ringSlots ?? kStreamRingSlots);
    const lifetime = new AbortController();

    let lastEventId = config.lastEventId ?? null;
    let serverRetryMs: number | null = null;
    let closed = false;

    // Every tab handles every event through one dedupe, leader and follower
    // alike. A follower that skipped it would double-handle the burst that
    // arrives when a leader hands over mid-replay.
    const handle = (event: StreamEvent): void => {
        if (!seen.admit(event.id)) {
            return;
        }
        config.onEvent(event);
    };

    const detach = config.fanOut.listen((message) => {
        const event = decodeBroadcast(message, config.name);
        if (event !== null) {
            handle(event);
        }
    });

    const finish = (error: HammerError | null): void => {
        if (closed) {
            return;
        }
        closed = true;
        detach();
        lifetime.abort();
        config.onClosed?.({ error });
    };

    // One connection, from open to end. Returns the failure that ended it, or
    // null when it ended cleanly — which for a stream is still a reconnect,
    // because a server closing an idle connection is routine.
    const connect = async (): Promise<HammerError | null> => {
        const address = await config.address(lifetime.signal);
        if (!address.ok) {
            return address.error;
        }

        const headers: Record<string, string> = { Accept: "text/event-stream" };
        if (lastEventId !== null) {
            // The resume point, as a header rather than a query value: it is not
            // part of the address. The same stream resumed twice is one
            // resource, and a URL that changed per attempt would defeat every
            // cache and every log line that groups by route.
            headers["Last-Event-ID"] = lastEventId;
        }

        let response: Response;
        try {
            response = await config.fetch(address.value, {
                method: "GET",
                headers,
                signal: lifetime.signal,
                credentials: "include",
                redirect: "error",
            });
        } catch {
            return lifetime.signal.aborted
                ? { kind: "transport", cause: "aborted" }
                : { kind: "transport", cause: "network" };
        }

        if (!response.ok) {
            return {
                kind: "server",
                code: "Unknown",
                status: response.status,
                requestId: null,
                fields: null,
            };
        }

        const body = response.body;
        if (body === null) {
            return { kind: "transport", cause: "network" };
        }

        const parser = new FrameParser();
        const reader = body.getReader();
        const decoder = new TextDecoder();

        try {
            for (;;) {
                const chunk = await reader.read();
                if (chunk.done) {
                    return null;
                }
                parser.push(decoder.decode(chunk.value, { stream: true }), (event) => {
                    handle(event);
                    // The leader holds the only connection, so every other tab's
                    // copy of this event comes from here. The message carries the
                    // event and no credential — there is none to carry.
                    config.fanOut.post({
                        kind: "hammer.stream",
                        name: config.name,
                        event,
                    } satisfies Broadcast);
                });
                if (parser.lastEventId !== null) {
                    lastEventId = parser.lastEventId;
                }
                if (parser.retryMs !== null) {
                    serverRetryMs = parser.retryMs;
                }
            }
        } catch {
            return lifetime.signal.aborted
                ? { kind: "transport", cause: "aborted" }
                : { kind: "transport", cause: "network" };
        } finally {
            void reader.cancel().catch(() => {
                // Cancelling a reader whose socket is already gone throws, and
                // there is nothing to do about it: the connection this was
                // releasing is the thing that ended.
            });
        }
    };

    const lead = async (): Promise<void> => {
        let attempt = 0;
        for (;;) {
            if (closed || lifetime.signal.aborted) {
                return;
            }

            const ended = await connect();
            if (closed || lifetime.signal.aborted) {
                return;
            }

            if (ended !== null && ended.kind === "server" && !willRetry(ended.status)) {
                finish(ended);
                return;
            }

            attempt += 1;
            // The server's number where it named one, a jittered backoff
            // otherwise — the same bargain `Retry-After` gets, for the same
            // reason.
            const waitMs = serverRetryMs ?? fullJitter(attempt, retry, unit);
            await sleep(waitMs, lifetime.signal);
            config.onReconnect?.();
        }
    };

    // Every tab queues. One streams, the rest wait, and the browser releasing a
    // discarded tab's lock is what makes the takeover automatic.
    void config.leadership.exclusive(config.name, lead).catch(() => {
        finish({ kind: "transport", cause: "network" });
    });

    return {
        close: () => {
            finish(null);
        },
    };
}
