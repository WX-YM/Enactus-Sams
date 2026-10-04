// What a tab is allowed to remember, and for how long it is allowed to remember
// it.
//
// Three properties, and each of them is a defect that has shipped in somebody's
// client (`docs/00-architecture.md` §7):
//
//   BOUNDED      a tab stays open for days. An unbounded cache is a leak with a
//                slow fuse, and the report it produces is "it gets slow after a
//                while", which nobody can reproduce in a session.
//   PER CLASS    one ceiling over everything means a list view paging through
//                five hundred rows evicts the entries every screen reads. The
//                ceiling is per class so a chatty one cannot spend somebody
//                else's.
//   KEYED BY THE IDENTITY ALLOWED TO READ IT
//                a cache that survives a logout is one user's data rendered to
//                the next on a shared device. The identity is IN the key, so a
//                read cannot cross even before the drop happens — and the drop
//                happens too, because holding it is a disclosure waiting for an
//                XSS.
//
// **Nothing here is persisted.** No `localStorage`, no IndexedDB, no service
// worker (`tools/check-source-bans.sh` fails the build on the first two by
// name). A persisted response outlives the cookie that authorised it, which is
// private data readable after the session ended; the offline case returns as a
// per-resource opt-in that names its own eviction, not as a default somebody
// switched on because it was easy.

import type { Brand } from "../core/brand.js";
import { brand } from "../core/brand.js";
import type { PathParams, RouteQuery, QueryScalar } from "../wire/route.js";

// Who the entries belong to. Null is the anonymous tab — a real identity with
// its own entries, rather than an absence: a public list read before a login and
// the same list read after it are two different answers from the server.
export type Identity = string | null;

// A key built by `cacheKey` and by nothing else. The brand is the guarantee the
// paragraph above rests on: a string a caller assembled is a key that may have
// left the identity out, and the entry it reads is then one anybody can reach.
export type CacheKey = Brand<string, "cache-key">;

// Every component is length-prefixed rather than separated by a character
// chosen to be unlikely.
//
// Two of the components did not come from this library: the identity, which an
// application reads out of a session body, and the parameter and query values,
// which are whatever a screen put in them. A separator they can contain is a
// separator they can forge — `{ id: "a b" }` and `{ id: "a", other: "b" }` are
// one key with any character in the middle, and a forged key crosses the
// boundary the identity in it exists to draw. A length says where a value ends
// without depending on what is in it, and it cannot throw the way
// `encodeURIComponent` does on the half of a surrogate pair a screen truncated.
function field(text: string): string {
    return String(text.length) + ":" + text;
}

function scalar(value: QueryScalar): string {
    return typeof value === "string" ? value : String(value);
}

// Sorted, because `{ limit, after }` and `{ after, limit }` are one request
// written twice — the same reason `wire/route.ts` sorts a query string. A cache
// keyed by the order somebody wrote an object literal in is a cache that misses
// on every screen that spells the same read differently.
//
// The comparator returns zero for two equal names, and that is load-bearing
// rather than tidy: `Array.prototype.sort` is stable, so repeated names keep the
// order they were given, and `?tag=a&tag=b` stays a different key from
// `?tag=b&tag=a` for a server that reads them in order. A comparator that never
// returns zero throws that away silently.
//
// It sorts in place and takes ownership of the array, which is safe because both
// callers built one for this call and nothing else holds it.
function canonical(entries: (readonly [string, string])[]): string {
    entries.sort((left, right) => {
        if (left[0] === right[0]) {
            return 0;
        }
        return left[0] < right[0] ? -1 : 1;
    });
    let out = "";
    for (const [name, value] of entries) {
        out += field(name) + field(value);
    }
    return out;
}

export function cacheKey(parts: {
    readonly routeId: string;
    readonly params: PathParams;
    readonly query: RouteQuery;
    readonly identity: Identity;
}): CacheKey {
    const { routeId, params, query, identity } = parts;

    const paramEntries: (readonly [string, string])[] = [];
    for (const [name, value] of Object.entries(params)) {
        paramEntries.push([name, scalar(value)]);
    }

    const queryEntries: (readonly [string, string])[] = [];
    for (const [name, value] of Object.entries(query)) {
        // Null is "omit this one" (`wire/route.ts`), so it is omitted here too —
        // a key that recorded the absence would miss against the request that
        // left it out.
        if (value === null) {
            continue;
        }
        if (Array.isArray(value)) {
            for (const item of value as readonly QueryScalar[]) {
                queryEntries.push([name, scalar(item)]);
            }
            continue;
        }
        queryEntries.push([name, scalar(value as QueryScalar)]);
    }

    // The anonymous tab is marked rather than spelled as an empty identity: a
    // signed-in user whose id is the empty string would otherwise share the
    // anonymous tab's entries, which is the one collision this key exists to
    // make impossible.
    return brand<string, "cache-key">(
        (identity === null ? "-" : "+") +
            field(identity ?? "") +
            field(routeId) +
            canonical(paramEntries) +
            "?" +
            canonical(queryEntries),
    );
}

// A bounded map that evicts the least recently used entry.
//
// `Map` iterates in insertion order, so the oldest key is the first one the
// iterator yields and "touch" is a delete followed by a set. That is two hash
// operations against a linked list nobody has to maintain, and it is why there
// is no list here: a hand-written one is a second structure to keep in agreement
// with the map, and the copy that drifts is the one that leaks.
export class Lru<K, V> {
    private readonly entries: Map<K, V>;
    private readonly maxEntries: number;

    evictions = 0;

    constructor(maxEntries: number) {
        // A ceiling below one is a cache that cannot hold the entry it was just
        // asked to hold: a configuration nobody means, and a read that always
        // misses. It is a throw because it is programmer error (`CLAUDE.md` §3.1).
        if (!Number.isInteger(maxEntries) || maxEntries < 1) {
            throw new Error("cache ceiling must be a positive integer");
        }
        this.entries = new Map();
        this.maxEntries = maxEntries;
    }

    get size(): number {
        return this.entries.size;
    }

    get(key: K): V | undefined {
        const held = this.entries.get(key);
        if (held === undefined) {
            return undefined;
        }
        this.entries.delete(key);
        this.entries.set(key, held);
        return held;
    }

    // Reads without making the entry recent. For a sweep that has to look at
    // every entry — an invalidation, an expiry pass — where touching each one in
    // turn reverses the eviction order and evicts whatever was actually in use.
    peek(key: K): V | undefined {
        return this.entries.get(key);
    }

    set(key: K, value: V): void {
        // Deleted first even when it is already there, so an overwrite counts as
        // a use. Without it, a key rewritten on every poll keeps its original
        // position and is evicted while it is the one thing being read.
        this.entries.delete(key);
        this.entries.set(key, value);

        while (this.entries.size > this.maxEntries) {
            const oldest = this.entries.keys().next();
            if (oldest.done === true) {
                return;
            }
            this.entries.delete(oldest.value);
            this.evictions += 1;
        }
    }

    delete(key: K): boolean {
        return this.entries.delete(key);
    }

    clear(): void {
        this.entries.clear();
    }

    [Symbol.iterator](): IterableIterator<[K, V]> {
        return this.entries[Symbol.iterator]();
    }
}

// What a stored value carries besides itself. The route id is here rather than
// only inside the key because an invalidation names a route, and the key is
// opaque by construction — parsing one back out would make the separator above
// part of the contract.
export type CacheEntry<V> = {
    readonly routeId: string;
    readonly value: V;
};

// The class a route's entries live in, and the ceiling that class gets. The
// names are the application's: hammer has no opinion about what a resource class
// is called, only that one exists and that it is bounded (`CLAUDE.md` §1).
export type CacheClasses = Readonly<Record<string, number>>;

export type CacheConfig = {
    readonly classes: CacheClasses;

    // For a route whose class the application did not name. A ceiling rather
    // than "unbounded", because the failure mode of a forgotten class has to be
    // a small cache and never an unbounded one.
    readonly defaultMaxEntries: number;
};

export class ResourceCache<V> {
    private readonly config: CacheConfig;
    private readonly byClass: Map<string, Lru<CacheKey, CacheEntry<V>>>;
    private who: Identity;

    constructor(config: CacheConfig) {
        this.config = config;
        this.byClass = new Map();
        this.who = null;
    }

    identity(): Identity {
        return this.who;
    }

    // The session says who this tab belongs to now. Everything held is dropped
    // when that changes, in BOTH directions: a login inherits the anonymous
    // tab's entries and a logout leaves the signed-in ones behind, and only one
    // of those two is obviously wrong.
    //
    // Returns whether it dropped anything, so a caller can count it rather than
    // infer it.
    adopt(identity: Identity): boolean {
        if (this.who === identity) {
            return false;
        }
        this.who = identity;
        this.clear();
        return true;
    }

    get(key: CacheKey): V | undefined {
        for (const lru of this.byClass.values()) {
            const held = lru.get(key);
            if (held !== undefined) {
                return held.value;
            }
        }
        return undefined;
    }

    set(
        key: CacheKey,
        entry: { readonly class: string; readonly routeId: string; readonly value: V },
    ): void {
        this.lru(entry.class).set(key, { routeId: entry.routeId, value: entry.value });
    }

    delete(key: CacheKey): void {
        for (const lru of this.byClass.values()) {
            lru.delete(key);
        }
    }

    // Every entry of every route named, whatever its parameters were. A mutation
    // invalidates a RESOURCE and not one address of it: a write that changed row
    // seven makes the page containing row seven stale, and the client has no way
    // to know which page that was (`docs/00-architecture.md` §7).
    //
    // A sweep rather than an index, because the sweep is bounded by the same
    // ceilings the top of this file is about, and an index is a second structure
    // to keep in agreement with the map.
    invalidateRoutes(routeIds: Iterable<string>): number {
        const wanted = new Set(routeIds);
        if (wanted.size === 0) {
            return 0;
        }

        let dropped = 0;
        for (const lru of this.byClass.values()) {
            // Collected before deleting. Removing from a `Map` while iterating
            // it is defined behaviour, but the entry being removed is the one
            // the iterator is standing on, and a reader should not have to know
            // that to believe this loop.
            const doomed: CacheKey[] = [];
            for (const [key, entry] of lru) {
                if (wanted.has(entry.routeId)) {
                    doomed.push(key);
                }
            }
            for (const key of doomed) {
                lru.delete(key);
                dropped += 1;
            }
        }
        return dropped;
    }

    clear(): void {
        for (const lru of this.byClass.values()) {
            lru.clear();
        }
    }

    size(): number {
        let total = 0;
        for (const lru of this.byClass.values()) {
            total += lru.size;
        }
        return total;
    }

    private lru(name: string): Lru<CacheKey, CacheEntry<V>> {
        const held = this.byClass.get(name);
        if (held !== undefined) {
            return held;
        }
        // `hasOwnProperty` rather than a bare read: the class names are the
        // application's, and a class called `constructor` would otherwise
        // resolve to a function and a ceiling that is not a number.
        const ceiling = Object.prototype.hasOwnProperty.call(this.config.classes, name)
            ? this.config.classes[name]
            : undefined;
        const made = new Lru<CacheKey, CacheEntry<V>>(ceiling ?? this.config.defaultMaxEntries);
        this.byClass.set(name, made);
        return made;
    }
}
