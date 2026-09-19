// A read, held for as long as something is looking at it.
//
// The whole of this module is three rules from `docs/00-architecture.md` §7 and
// one from `ENGINEERING_RULES.md` §7, and every one of them is a rule about what this layer
// must NOT decide:
//
//   FRESHNESS IS THE SERVER'S. A response is served without asking again only
//   for as long as `Cache-Control` said it could be, and a stale copy is served
//   only where `stale-while-revalidate` granted it. There is no default here and
//   no constant to tune: a client that picked its own thirty seconds would hold
//   a row through the permission change that was supposed to remove it.
//
//   ONE REQUEST PER ADDRESS. A screen with four components reading one resource
//   is one request. The dedupe is here as well as in `wire/client.ts` because
//   the two answer different questions — the client's is "is this exact request
//   already on the wire", and this one is "is this entry already loading",
//   which is still true while a retry is backing off and nothing is in flight.
//
//   BOUNDED, AND DROPPED WITH THE IDENTITY. `state/cache.ts` is the mechanism;
//   this is the thing that puts values into it.
//
//   NOTHING OUTLIVES WHAT CREATED IT. An entry with no watchers has its request
//   aborted and its store closed. A screen that unmounts stops paying for the
//   read it started (`ENGINEERING_RULES.md` §3.3).
//
// --- what a failure does to a value that is already on screen ---------------
//
// It does not remove it. A refresh that fails over a row somebody is reading
// leaves the row there and reports the failure beside it, because blanking the
// screen on a dropped connection is a worse answer than a stale row with an
// error on it — and the row is marked stale so that nothing downstream mistakes
// it for a confirmation.

import type { HammerError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import type { CacheControl } from "../wire/cache_control.js";
import type { ApiTypes, CallOptions, CallResult, CallableRoute, Client } from "../wire/client.js";
import type { MonotonicClock } from "../wire/queue.js";
import type { PathParams, RouteQuery } from "../wire/route.js";

import type { CacheConfig, CacheKey, Identity } from "./cache.js";
import { ResourceCache, cacheKey } from "./cache.js";
import type { CountSink } from "./counts.js";
import { kNoCounts } from "./counts.js";
import type { Readable } from "./store.js";
import { Store } from "./store.js";

// The body a route answers with, and the failure it can answer with, pulled out
// of the client's own result type so that a resource is typed by the route it
// reads rather than by a second declaration of the same thing.
export type ResourceBody<A extends ApiTypes, R extends CallableRoute> = Extract<
    CallResult<A, R>,
    { readonly ok: true }
>["value"];

export type ResourceFailure<A extends ApiTypes> = HammerError<
    A["code"] | "Unknown",
    A["reason"] | "Unknown"
>;

// Every member declares every property, in one order.
//
// That is §2.3's rule — objects have exactly one shape — held while still being
// a discriminated union, which is §3.1's. A renderer reading `state.value` sees
// one hidden class whichever member it holds, so the branch that renders the
// value and the branch that renders the error do not deoptimise each other; and
// `status === "ready"` still narrows `value` to a body rather than leaving every
// call site a null check it will eventually forget.
export type ResourceState<T, E> =
    | {
          readonly status: "loading";
          readonly value: null;
          readonly error: null;
          readonly stale: false;
      }
    | {
          readonly status: "ready";
          readonly value: T;
          readonly error: null;
          readonly stale: boolean;
      }
    | {
          readonly status: "failed";
          // The last body this entry held, where there was one. A failure over a
          // value somebody is reading does not blank the screen.
          readonly value: T | null;
          readonly error: E;
          readonly stale: boolean;
      };

// The state every entry starts in, as one object rather than one per entry.
//
// Exported because `hammer/react` needs the same value for the render that
// happens BEFORE its subscription exists, and a second copy there would be a
// second identity: `useSyncExternalStore` compares snapshots with `Object.is`,
// so a freshly built literal per call is an infinite render loop rather than a
// duplicated constant.
export const kResourceLoading: ResourceState<never, never> = {
    status: "loading",
    value: null,
    error: null,
    stale: false,
};

// What goes in the cache: the body, when it was stored, and what the server said
// about storing it. The timestamp is MONOTONIC — an elapsed time this device
// measured on both ends — and never a wall-clock instant compared against a
// server one (`ENGINEERING_RULES.md` §6).
type Stored = {
    readonly body: unknown;
    readonly storedAtMs: number;
    readonly freshness: CacheControl;
};

export type Resource<T, E> = {
    // The cache key as it stands. A function rather than a field because the
    // identity is part of it: a tab that logs in re-keys every entry it is
    // holding, and a field captured before that would name an entry nobody can
    // reach.
    readonly key: () => CacheKey;

    readonly state: Readable<ResourceState<T, E>>;

    // The first state that is not `loading`, for a caller that cannot subscribe
    // and re-render its way to one. A submit handler reading the version it is
    // about to carry, a rollback confirming what it rolled back to, a route
    // guard: each of them needs the value once, at a point in a function, and
    // without this each of them writes its own subscribe-and-unsubscribe dance —
    // which is where a listener gets left attached.
    //
    // Takes a signal like every other async function here (`ENGINEERING_RULES.md` §3.1). An
    // abort resolves with whatever the entry holds NOW rather than rejecting: the
    // caller is abandoning the wait, not discovering an error, and a rejection
    // would be an exception thrown for an expected condition.
    readonly ready: (signal: AbortSignal) => Promise<ResourceState<T, E>>;

    // Ask again now, whatever the freshness says. For a pull-to-refresh and for
    // the reconciliation a `VersionMismatch` demands (`state/versioned.ts`) —
    // never for a poll, because a client that polls is a client that invented a
    // freshness the server did not grant.
    readonly refresh: () => Promise<void>;

    // A body the caller already has and the server has confirmed: the document a
    // write returned. It keeps whatever freshness the entry was last granted,
    // because a confirmation is not a new grant.
    readonly overwrite: (value: T) => void;

    // A value the server has NOT confirmed (`state/optimistic.ts`). It reaches
    // the store and never the cache, and it is marked stale, which is what makes
    // `ENGINEERING_RULES.md` §6's rule mechanical rather than remembered: an unconfirmed
    // value cannot be inherited by a second reader of this address, and nothing
    // downstream can mistake it for a confirmation.
    readonly provisional: (value: T) => void;

    // Whether a value is outstanding. A second optimistic update over the first
    // would be applying a function to a value the server has not agreed to, and
    // the result is a fabrication built on a fabrication.
    readonly hasProvisional: () => boolean;

    // Back to what the cache holds, with no write of its own. The rollback path:
    // the pre-optimistic body was the last CONFIRMED one, so restoring is
    // re-reading the entry rather than inventing a value to put back.
    readonly restore: () => void;

    // Drop the value and reload. What a rollback with nothing to roll back to
    // does (`state/optimistic.ts`).
    readonly forget: () => void;

    // Required, and required of every caller (`ENGINEERING_RULES.md` §3.3). The entry's
    // request is aborted and its store closed when the last watcher lets go; a
    // handle nobody releases is a request that outlives the screen that wanted
    // it and a store nothing will ever close.
    readonly release: () => void;
};

// One live entry. It exists while something is watching and not otherwise: the
// VALUE lives in the bounded cache, and this is only the machinery around it.
class Entry {
    readonly route: CallableRoute;
    readonly params: PathParams;
    readonly query: RouteQuery;
    readonly cacheClass: string;
    readonly store: Store<ResourceState<unknown, HammerError>>;

    key: CacheKey;
    watchers: number;
    inFlight: Promise<void> | null;
    controller: AbortController;

    // True while an unconfirmed value is on screen.
    provisional: boolean;

    constructor(parts: {
        readonly route: CallableRoute;
        readonly params: PathParams;
        readonly query: RouteQuery;
        readonly cacheClass: string;
        readonly key: CacheKey;
    }) {
        this.route = parts.route;
        this.params = parts.params;
        this.query = parts.query;
        this.cacheClass = parts.cacheClass;
        this.key = parts.key;
        this.store = new Store<ResourceState<unknown, HammerError>>(kResourceLoading);
        this.watchers = 0;
        this.inFlight = null;
        this.controller = new AbortController();
        this.provisional = false;
    }
}

export type ResourceStoreConfig<A extends ApiTypes> = {
    readonly client: Client<A>;
    readonly cache: CacheConfig;

    // Injected rather than read, so a test drives freshness deterministically
    // instead of sleeping (`ENGINEERING_RULES.md` §3.3). Monotonic: the number is only ever
    // subtracted from another reading of itself.
    readonly now?: MonotonicClock;

    readonly count?: CountSink;
};

// What a read declares besides its parameters.
type OpenOptions = {
    readonly query?: RouteQuery;

    // Which bounded class this entry's value lives in. The name is the
    // application's; what hammer insists on is that there is one and that it has
    // a ceiling (`state/cache.ts`).
    readonly class?: string;
};

const kDefaultClass = "default";

export class ResourceStore<A extends ApiTypes> {
    private readonly config: ResourceStoreConfig<A>;
    private readonly cache: ResourceCache<Stored>;
    private readonly live: Map<CacheKey, Entry>;
    private readonly now: MonotonicClock;
    private readonly count: CountSink;

    constructor(config: ResourceStoreConfig<A>) {
        this.config = config;
        this.cache = new ResourceCache<Stored>(config.cache);
        this.live = new Map();
        this.now = config.now ?? (() => performance.now());
        this.count = config.count ?? kNoCounts;
    }

    identity(): Identity {
        return this.cache.identity();
    }

    // Who this tab belongs to now. Everything held is dropped, and everything
    // still on screen is re-keyed and read again — the entry stays, because the
    // component watching it is still mounted and is entitled to the new
    // identity's answer rather than to the old one's.
    adopt(identity: Identity): void {
        if (!this.cache.adopt(identity)) {
            return;
        }
        this.count("identity-dropped");

        // Re-keyed in one pass rather than in place: the identity is part of
        // every key, so every entry moves at once and a half-rebuilt map would
        // have two keys for one entry.
        const entries = Array.from(this.live.values());
        this.live.clear();
        for (const entry of entries) {
            entry.key = this.keyFor(entry);
            this.live.set(entry.key, entry);
            entry.store.set(kResourceLoading);
            this.abort(entry);
            void this.load(entry);
        }
    }

    // A mutation says these resources are stale (`state/invalidate.ts`).
    //
    // A watched entry is read again; an unwatched one is dropped and read the
    // next time something opens it. That asymmetry is the thing that stops an
    // invalidation from being a thundering herd: what refetches is bounded by
    // what is on a screen, not by everything the tab has ever read.
    invalidate(routeIds: Iterable<string>): void {
        const wanted = new Set(routeIds);
        if (wanted.size === 0) {
            return;
        }
        this.cache.invalidateRoutes(wanted);
        this.count("invalidated");

        for (const entry of this.live.values()) {
            if (!wanted.has(entry.route.id)) {
                continue;
            }
            this.abort(entry);
            void this.load(entry);
        }
    }

    // Every entry, held and live. What a logout fans out to (`ENGINEERING_RULES.md` §4).
    clear(): void {
        this.cache.clear();
        for (const entry of this.live.values()) {
            this.abort(entry);
            entry.store.set(kResourceLoading);
        }
    }

    close(): void {
        for (const entry of this.live.values()) {
            this.abort(entry);
            entry.store.close();
        }
        this.live.clear();
        this.cache.clear();
    }

    // A read, opened. The handle is refcounted: two components reading one
    // address share one entry, one store and one request, and the entry goes
    // when the second of them releases.
    //
    // The route is constrained to one that declares no capability scope. A
    // capability is a second, deliberate act by a person (`docs/00-architecture.md`
    // §6), and a resource is a thing a screen re-reads whenever it feels stale —
    // a cache that could re-present a capability would spend a single-use token
    // on a revalidation nobody asked for.
    open<R extends CallableRoute & { readonly capability: null }>(
        route: R,
        options: OpenOptions & ParamsFor<A, R>,
    ): Resource<ResourceBody<A, R>, ResourceFailure<A>> {
        const params = (options as { readonly params?: PathParams }).params ?? {};
        const query = options.query ?? {};
        const cacheClass = options.class ?? kDefaultClass;

        const key = cacheKey({
            routeId: route.id,
            params,
            query,
            identity: this.cache.identity(),
        });

        let entry = this.live.get(key);
        if (entry === undefined) {
            entry = new Entry({ route, params, query, cacheClass, key });
            this.live.set(key, entry);
            this.adoptCached(entry);
        }
        entry.watchers += 1;

        const held = entry;
        let released = false;

        return {
            key: () => held.key,
            // The store's value type is the route's body, which this class
            // cannot express for a heterogeneous map of entries. The cast is
            // sound for the reason the cache's is: an entry's key contains its
            // route id, so every value ever written under it came from that
            // route's response.
            state: held.store as unknown as Readable<
                ResourceState<ResourceBody<A, R>, ResourceFailure<A>>
            >,
            ready: (signal) =>
                whenSettled(held.store, signal) as Promise<
                    ResourceState<ResourceBody<A, R>, ResourceFailure<A>>
                >,
            refresh: async () => {
                this.abort(held);
                await this.load(held);
            },
            overwrite: (value) => {
                this.write(held, value);
            },
            provisional: (value) => {
                held.provisional = true;
                held.store.set({ status: "ready", value, error: null, stale: true });
            },
            hasProvisional: () => held.provisional,
            restore: () => {
                held.provisional = false;
                this.adoptCached(held);
            },
            forget: () => {
                held.provisional = false;
                this.cache.delete(held.key);
                held.store.set(kResourceLoading);
                this.abort(held);
                void this.load(held);
            },
            release: () => {
                // Idempotent, for the reason `Store.subscribe`'s unsubscribe is:
                // a component that releases twice must not drop a watcher some
                // other component added.
                if (released) {
                    return;
                }
                released = true;
                held.watchers -= 1;
                if (held.watchers > 0) {
                    return;
                }
                this.abort(held);
                held.store.close();
                this.live.delete(held.key);
            },
        };
    }

    // --- what the entry does with what is already cached ---------------------

    private adoptCached(entry: Entry): void {
        const stored = this.cache.get(entry.key);
        if (stored === undefined) {
            void this.load(entry);
            return;
        }

        const ageMs = this.now() - stored.storedAtMs;
        const { freshness } = stored;
        const maxAgeMs = freshness.noCache ? 0 : (freshness.maxAgeMs ?? 0);

        if (freshness.maxAgeMs !== null && !freshness.noCache && ageMs < maxAgeMs) {
            entry.store.set({ status: "ready", value: stored.body, error: null, stale: false });
            return;
        }

        // Past its freshness. Serving it anyway is allowed only where the server
        // granted `stale-while-revalidate`, and the revalidation is not optional
        // — serving stale WITHOUT refreshing is a cache that has quietly
        // extended the freshness it was given.
        if (freshness.staleWhileRevalidateMs > 0 && ageMs < maxAgeMs + freshness.staleWhileRevalidateMs) {
            entry.store.set({ status: "ready", value: stored.body, error: null, stale: true });
            this.count("served-stale");
            void this.load(entry);
            return;
        }

        // Neither fresh nor servable: dead weight. It is dropped rather than kept
        // for a conditional request, because there is no conditional request here
        // to keep it for.
        this.cache.delete(entry.key);
        entry.store.set(kResourceLoading);
        void this.load(entry);
    }

    private write(entry: Entry, value: unknown): void {
        entry.provisional = false;
        const stored = this.cache.get(entry.key);
        this.cache.set(entry.key, {
            class: entry.cacheClass,
            routeId: entry.route.id,
            value: {
                body: value,
                // The clock is re-read rather than the old stamp kept: the value
                // is current as of now, and dating it from the last response
                // would expire a fresh document on the previous one's schedule.
                storedAtMs: this.now(),
                freshness: stored?.freshness ?? kUnservable,
            },
        });
        entry.store.set({ status: "ready", value, error: null, stale: false });
    }

    private abort(entry: Entry): void {
        entry.controller.abort();
        entry.controller = new AbortController();
        // Dropped rather than awaited. The request it names is aborted, and a
        // caller arriving now must start a new one rather than join a dead one —
        // the same reason `wire/client.ts` reports an abandoned shared request
        // synchronously.
        entry.inFlight = null;
    }

    private load(entry: Entry): Promise<void> {
        // In-flight dedupe, at the entry rather than at the address. It is still
        // true while a retry is backing off, which is exactly the window in
        // which a second component mounts and asks for the same thing.
        const running = entry.inFlight;
        if (running !== null) {
            return running;
        }

        const started = this.perform(entry);
        entry.inFlight = started;
        // Both arms rather than `finally`, for the reason `wire/client.ts` gives:
        // a `finally` builds a derived promise that rejects when the original
        // does, and nothing is attached to that one.
        const settle = (): void => {
            if (entry.inFlight === started) {
                entry.inFlight = null;
            }
        };
        void started.then(settle, settle);
        return started;
    }

    private async perform(entry: Entry): Promise<void> {
        const { signal } = entry.controller;
        let granted: CacheControl | null = null;

        // One cast, at the one boundary where a heterogeneous map of routes
        // meets a signature typed per route. `open` above is where a caller's
        // parameters are checked against the route they named; from here down
        // every entry is the same shape and the type parameter has nothing left
        // to say.
        const call = this.config.client.call.bind(this.config.client) as unknown as (
            route: CallableRoute,
            options: {
                readonly params: PathParams;
                readonly query: RouteQuery;
                readonly signal: AbortSignal;
                readonly onFreshness: (directives: CacheControl) => void;
            },
        ) => Promise<Result<unknown, HammerError>>;

        const answered = await call(entry.route, {
            params: entry.params,
            query: entry.query,
            signal,
            onFreshness: (directives) => {
                granted = directives;
            },
        });

        // The entry may have been released, re-keyed by a login or invalidated
        // while this was on the wire. Writing the answer now would resurrect a
        // store nothing is watching, or file one identity's body under another's
        // key.
        if (signal.aborted) {
            return;
        }

        if (!answered.ok) {
            const previous = entry.store.get();
            entry.store.set({
                status: "failed",
                value: previous.value,
                error: answered.error,
                stale: previous.value !== null,
            });
            return;
        }

        entry.provisional = false;
        const freshness: CacheControl = granted ?? kUnservable;
        if (!freshness.noStore) {
            this.cache.set(entry.key, {
                class: entry.cacheClass,
                routeId: entry.route.id,
                value: { body: answered.value, storedAtMs: this.now(), freshness },
            });
        }
        entry.store.set({ status: "ready", value: answered.value, error: null, stale: false });
    }

    private keyFor(entry: Entry): CacheKey {
        return cacheKey({
            routeId: entry.route.id,
            params: entry.params,
            query: entry.query,
            identity: this.cache.identity(),
        });
    }
}

// Resolves on the first state that is not `loading`.
//
// The unsubscribe is detached on every path out — the value arriving, the signal
// aborting, and the case where the entry had already settled before anybody
// asked. A listener left attached here would be one per call, on a store a
// screen holds for as long as it is open.
function whenSettled<T, E>(
    store: Readable<ResourceState<T, E>>,
    signal: AbortSignal,
): Promise<ResourceState<T, E>> {
    const held = store.get();
    if (held.status !== "loading" || signal.aborted) {
        return Promise.resolve(held);
    }

    return new Promise<ResourceState<T, E>>((resolve) => {
        const finish = (state: ResourceState<T, E>): void => {
            off();
            signal.removeEventListener("abort", onAbort);
            resolve(state);
        };
        const onAbort = (): void => {
            finish(store.get());
        };
        const off = store.subscribe((state) => {
            if (state.status !== "loading") {
                finish(state);
            }
        });
        signal.addEventListener("abort", onAbort, { once: true });
    });
}

// A response the server said nothing about. Not zero seconds and not forever:
// it may be held for as long as something is watching it, and it may not be
// served to the next thing that opens the same address. Saying nothing is not
// permission (`wire/cache_control.ts`).
const kUnservable: CacheControl = {
    noStore: false,
    noCache: true,
    isPrivate: false,
    maxAgeMs: null,
    staleWhileRevalidateMs: 0,
};

// The parameters half of the client's own call options, reused rather than
// restated: a route with no parameters takes none here too, and one with
// parameters cannot be opened without them.
type ParamsFor<A extends ApiTypes, R extends CallableRoute> = CallOptions<A, R> extends infer O
    ? O extends { readonly params: infer P }
        ? { readonly params: P }
        : { readonly params?: Record<string, never> }
    : never;
