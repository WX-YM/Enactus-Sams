// Notifications, over a stream that delivers at-least-once.
//
// --- the count comes from the server ------------------------------------------
//
// A client that counted its own unread notifications drifts, and it drifts in a
// way nobody can debug: a reconnect replays, a second tab marks one read, a
// device was asleep for the arrivals that happened while it was. So the server's
// number is authoritative and every server response overwrites whatever this
// store had worked out for itself.
//
// The stream still adjusts it, and that is not a contradiction — it is what
// makes a badge appear the moment something arrives rather than at the next
// poll. The rule is which one wins: a local adjustment is a GUESS between server
// answers, and the next answer replaces it outright rather than being reconciled
// against it. A count that drifts is the defect people notice first and report
// last (`docs/16-test-plan.md`).
//
// --- every handler is idempotent ---------------------------------------------
//
// SSE resumes with `Last-Event-ID` and a reconnect replays, so an event arriving
// twice is normal operation and not an error (`docs/01-seams.md` §9). The dedupe
// is by event id over a bounded ring — bounded because a tab stays open for
// days, and sized to anvil's own replay ring because that is the most one
// reconnect can deliver in a burst.
//
// --- what this module does not decide -----------------------------------------
//
// Which topics deserve a badge, what "read" means in this product, and the word
// for "notifications" in any language. All three are the application's
// (`ENGINEERING_RULES.md` §9). What arrives here is an event with an id and a payload whose
// shape the application decodes, and what leaves is a count and a list.

import type { ApiTypes, CallableRoute, Client } from "../wire/client.js";
import type { PathParams, RouteQuery } from "../wire/route.js";
import type { Stream, StreamEvent } from "../wire/sse.js";

import type { CountSink } from "./counts.js";
import { kNoCounts } from "./counts.js";
import { Lru } from "./cache.js";
import type { Readable } from "./store.js";
import { Store } from "./store.js";

// Matched to anvil's replay ring: the most one reconnect can deliver at once,
// which is the window a duplicate can arrive within.
export const kInboxRingSlots = 256;

// How many notifications are held. A bound rather than a growing list, for the
// reason every cache here is bounded: a tab open for days is a leak with a slow
// fuse. The list is a view of the most recent, and the full history is a paged
// read — which is `state/paginate.ts`'s job and not this one's.
export const kInboxHeld = 100;

export type Notification<T> = {
    // The stream's event id, which is what the dedupe is keyed by and what a
    // resume sends back.
    readonly id: string;
    readonly type: string;

    // The decoded payload. The shape is the application's, because a
    // notification's shape is (`docs/01-seams.md` §4).
    readonly body: T;

    // Whether this one has been read. Optimistic on a mark-read and corrected by
    // the next server answer, like the count.
    readonly read: boolean;
};

export type InboxState<T> = {
    // Authoritative as of the last server answer, adjusted by arrivals since.
    readonly unread: number;

    // Most recent first, which is the order a list renders and the order a ring
    // makes cheap.
    readonly items: readonly Notification<T>[];

    // Whether the stream is connected in SOME tab. It is a hint for an
    // interface, never a guarantee: the leader may be a tab nobody is looking at
    // (`wire/leader.ts`).
    readonly live: boolean;
};

export type InboxConfig<A extends ApiTypes, T> = {
    readonly client: Client<A>;

    // The route the stream is opened on, and whatever it needs to be addressed.
    readonly route: CallableRoute;
    readonly params?: PathParams;
    readonly query?: RouteQuery;

    // How to read one event's payload. Returns null for an event this build does
    // not understand, which is normal mid-deploy — the server may be newer than
    // this bundle (`docs/00-architecture.md` §7.1) — and is dropped rather than
    // rendered as a blank row.
    readonly decode: (event: StreamEvent) => T | null;

    // Whether an arrival counts toward the badge. The application's, because
    // which topics deserve one is (`ENGINEERING_RULES.md` §9).
    readonly counts?: (body: T) => boolean;

    readonly ringSlots?: number;
    readonly held?: number;
    readonly count?: CountSink;
};

export class Inbox<A extends ApiTypes, T> {
    private readonly config: InboxConfig<A, T>;
    private readonly state: Store<InboxState<T>>;
    private readonly seen: Lru<string, true>;
    private readonly countSink: CountSink;
    private readonly held: number;

    private stream: Stream | null;
    private lastEventId: string | null;

    constructor(config: InboxConfig<A, T>) {
        this.config = config;
        this.countSink = config.count ?? kNoCounts;
        this.held = config.held ?? kInboxHeld;
        this.seen = new Lru<string, true>(config.ringSlots ?? kInboxRingSlots);
        this.state = new Store<InboxState<T>>({ unread: 0, items: [], live: false });
        this.stream = null;
        this.lastEventId = null;
    }

    get store(): Readable<InboxState<T>> {
        return this.state;
    }

    // Opens the stream. Leader-owned across every tab on this session, with the
    // followers hearing the events over the channel — a person with twelve tabs
    // must not be twelve connections against a ceiling anvil derives from
    // `RLIMIT_NOFILE` (`wire/sse.ts`).
    //
    // The handler below therefore runs in EVERY tab, including the ones that are
    // not connected, which is the other half of why it has to be idempotent.
    open(): void {
        if (this.stream !== null) {
            return;
        }
        this.stream = this.config.client.stream(this.config.route, {
            ...(this.config.params === undefined ? {} : { params: this.config.params }),
            ...(this.config.query === undefined ? {} : { query: this.config.query }),
            lastEventId: this.lastEventId,
            onEvent: (event) => {
                this.accept(event);
            },
            onClosed: () => {
                // The reason is not read here. A stream that ended is a stream
                // that ended: the retry policy already decided whether it was
                // worth reconnecting, and a surface that rendered the cause
                // would be rendering a transport failure at somebody who asked
                // about their notifications.
                this.closed();
            },
        });
        this.state.set({ ...this.state.get(), live: true });
    }

    // The server's own number, from whatever response carries it. It REPLACES
    // the local count rather than being reconciled against it: the local one was
    // a guess between answers, and a client that tried to merge the two would be
    // inventing a third number neither participant holds.
    applyServerCount(unread: number): void {
        if (!Number.isInteger(unread) || unread < 0) {
            return;
        }
        this.state.set({ ...this.state.get(), unread });
    }

    // Marked read locally, ahead of the call. The call itself is the
    // application's — it is one idempotent, versioned request, which is
    // `state/versioned.ts`'s shape — and this is the part that has to be safe to
    // repeat: marking a read notification read again is a no-op here, so a
    // retried request cannot double-count.
    markRead(ids: readonly string[]): void {
        const wanted = new Set(ids);
        if (wanted.size === 0) {
            return;
        }

        const held = this.state.get();
        let cleared = 0;
        const items: Notification<T>[] = [];
        for (const item of held.items) {
            if (wanted.has(item.id) && !item.read) {
                cleared += 1;
                items.push({ id: item.id, type: item.type, body: item.body, read: true });
                continue;
            }
            items.push(item);
        }
        if (cleared === 0) {
            return;
        }

        // Never below zero. The count is the server's and this is a guess: a
        // negative badge is a guess that has been wrong for longer than anybody
        // noticed.
        this.state.set({
            unread: Math.max(0, held.unread - cleared),
            items,
            live: held.live,
        });
    }

    close(): void {
        this.stream?.close();
        this.stream = null;
        this.state.set({ ...this.state.get(), live: false });
    }

    // Everything this tab holds, dropped. What a logout fans out to: a list of
    // one person's notifications left on screen for the next one is the
    // disclosure the whole cache model is about.
    clear(): void {
        this.seen.clear();
        this.lastEventId = null;
        this.state.set({ unread: 0, items: [], live: this.state.get().live });
    }

    // One event, handled idempotently. Public rather than private because that
    // IS the contract: the stream is at-least-once, so this is safe to call
    // twice with the same event, and a caller with an event from somewhere else
    // — a replay an application stored, a second transport — is entitled to hand
    // it over rather than reach around the dedupe.
    accept(event: StreamEvent): void {
        if (event.id !== null) {
            // Resumed from here on the next reconnect, whether or not the event
            // was a duplicate: the server's cursor advances regardless of what
            // this client already had.
            this.lastEventId = event.id;

            if (this.seen.get(event.id) !== undefined) {
                // A reconnect replayed it. Normal operation, and the reason
                // every handler here is idempotent (`ENGINEERING_RULES.md` §6).
                this.countSink("duplicate-event");
                return;
            }
            this.seen.set(event.id, true);
        }

        const body = this.config.decode(event);
        if (body === null) {
            // An event this bundle does not understand. Dropped rather than
            // rendered as a blank row — the server may be newer than this build,
            // and a row nobody can read is worse than no row.
            return;
        }

        const held = this.state.get();
        const notification: Notification<T> = {
            // An event with no id cannot be deduped and cannot be resumed from.
            // It is still delivered, because dropping it would lose a
            // notification over a field the server chose not to send; what it
            // does not get is a stable identity, so the id is the one thing a
            // caller must not treat as unique here.
            id: event.id ?? "",
            type: event.type,
            body,
            read: false,
        };

        // Newest first, bounded. One slice rather than an unshift-and-pop pair,
        // because a list rendered from this is re-rendered on every arrival and
        // the identity of the array is what the store compares.
        const items = [notification, ...held.items].slice(0, this.held);
        const bumps = this.config.counts === undefined || this.config.counts(body);

        this.state.set({
            unread: bumps ? held.unread + 1 : held.unread,
            items,
            live: held.live,
        });
    }

    private closed(): void {
        this.stream = null;
        this.state.set({ ...this.state.get(), live: false });
    }
}
