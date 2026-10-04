// The bounded, identity-keyed, evicting cache.
//
// Three of the four assertions `docs/16-test-plan.md` lists for this phase are
// here, and the fourth — that nothing is persisted — is in
// `tests/state/persistence.test.ts`, because asserting an absence needs the
// storage APIs to exist and fail rather than to be missing from the environment.

import { describe, expect, it } from "../support/test.js";

import { Lru, ResourceCache, cacheKey } from "../../src/state/cache.js";

const kClasses = { small: 2, large: 8 };

function cache(): ResourceCache<string> {
    return new ResourceCache<string>({ classes: kClasses, defaultMaxEntries: 4 });
}

function key(routeId: string, params: Record<string, string> = {}, identity: string | null = null) {
    return cacheKey({ routeId, params, query: {}, identity });
}

describe("Lru", () => {
    it("evicts the least recently used entry at the ceiling", () => {
        const lru = new Lru<string, number>(2);
        lru.set("a", 1);
        lru.set("b", 2);
        lru.set("c", 3);

        expect(lru.peek("a")).toBeUndefined();
        expect(lru.peek("b")).toBe(2);
        expect(lru.peek("c")).toBe(3);
        expect(lru.evictions).toBe(1);
    });

    it("a read makes an entry recent, so the other one goes first", () => {
        const lru = new Lru<string, number>(2);
        lru.set("a", 1);
        lru.set("b", 2);
        lru.get("a");
        lru.set("c", 3);

        expect(lru.peek("a")).toBe(1);
        expect(lru.peek("b")).toBeUndefined();
    });

    it("an overwrite counts as a use", () => {
        // Without this a key rewritten on every poll keeps its original position
        // and is evicted while it is the one thing being read.
        const lru = new Lru<string, number>(2);
        lru.set("a", 1);
        lru.set("b", 2);
        lru.set("a", 9);
        lru.set("c", 3);

        expect(lru.peek("a")).toBe(9);
        expect(lru.peek("b")).toBeUndefined();
    });

    it("peek does not reorder, so a sweep does not reverse the eviction order", () => {
        const lru = new Lru<string, number>(2);
        lru.set("a", 1);
        lru.set("b", 2);
        lru.peek("a");
        lru.set("c", 3);

        expect(lru.peek("a")).toBeUndefined();
    });

    it("refuses a ceiling that cannot hold an entry", () => {
        expect(() => new Lru<string, number>(0)).toThrow();
        expect(() => new Lru<string, number>(1.5)).toThrow();
    });
});

describe("cacheKey", () => {
    it("is the same key however the parameters were written", () => {
        const left = cacheKey({
            routeId: "content.get",
            params: { id: "7", ns: "content" },
            query: { limit: 10, after: null },
            identity: "u1",
        });
        const right = cacheKey({
            routeId: "content.get",
            params: { ns: "content", id: "7" },
            query: { after: null, limit: 10 },
            identity: "u1",
        });

        expect(left).toBe(right);
    });

    it("a null query value is omitted, not recorded as an absence", () => {
        const withNull = cacheKey({
            routeId: "media.list",
            params: {},
            query: { after: null },
            identity: null,
        });
        const without = cacheKey({ routeId: "media.list", params: {}, query: {}, identity: null });

        expect(withNull).toBe(without);
    });

    it("keeps the order of repeated query values", () => {
        // `?tag=a&tag=b` and `?tag=b&tag=a` are two requests to a server that
        // reads them in order, so they are two keys.
        const first = cacheKey({
            routeId: "media.list",
            params: {},
            query: { tag: ["a", "b"] },
            identity: null,
        });
        const second = cacheKey({
            routeId: "media.list",
            params: {},
            query: { tag: ["b", "a"] },
            identity: null,
        });

        expect(first).not.toBe(second);
    });

    it("cannot be forged by a parameter that contains the separator", () => {
        // The components are length-prefixed for exactly this: a value holding
        // whatever character separates them would otherwise produce a key that
        // reads as a different request's.
        const one = cacheKey({
            routeId: "content.get",
            params: { id: "a", extra: "b" },
            query: {},
            identity: null,
        });
        const two = cacheKey({
            routeId: "content.get",
            params: { id: "a 8:extra1:b" },
            query: {},
            identity: null,
        });

        expect(one).not.toBe(two);
    });

    it("an empty identity is not the anonymous tab", () => {
        const anonymous = cacheKey({ routeId: "x", params: {}, query: {}, identity: null });
        const named = cacheKey({ routeId: "x", params: {}, query: {}, identity: "" });

        expect(anonymous).not.toBe(named);
    });

    it("two identities never share an entry", () => {
        const mine = cacheKey({ routeId: "identity.me", params: {}, query: {}, identity: "u1" });
        const theirs = cacheKey({ routeId: "identity.me", params: {}, query: {}, identity: "u2" });

        expect(mine).not.toBe(theirs);
    });
});

describe("ResourceCache", () => {
    it("bounds each class separately, so a chatty one cannot evict another's", () => {
        const held = cache();
        for (let i = 0; i < 6; i += 1) {
            held.set(key("media.list", { page: String(i) }), {
                class: "small",
                routeId: "media.list",
                value: String(i),
            });
        }
        held.set(key("identity.me"), { class: "large", routeId: "identity.me", value: "me" });

        expect(held.get(key("identity.me"))).toBe("me");
        expect(held.get(key("media.list", { page: "0" }))).toBeUndefined();
        expect(held.get(key("media.list", { page: "5" }))).toBe("5");
    });

    it("falls back to the default ceiling for a class nobody named", () => {
        const held = cache();
        for (let i = 0; i < 5; i += 1) {
            held.set(key("content.get", { id: String(i) }), {
                class: "unnamed",
                routeId: "content.get",
                value: String(i),
            });
        }

        // A forgotten class is a SMALL cache and never an unbounded one.
        expect(held.size()).toBe(4);
    });

    it("drops everything when the identity changes, in both directions", () => {
        const held = cache();
        held.set(key("identity.me"), { class: "large", routeId: "identity.me", value: "anon" });

        expect(held.adopt("u1")).toBe(true);
        expect(held.size()).toBe(0);

        held.set(key("identity.me", {}, "u1"), {
            class: "large",
            routeId: "identity.me",
            value: "mine",
        });

        // A logout leaves the signed-in entries behind if nothing drops them.
        expect(held.adopt(null)).toBe(true);
        expect(held.size()).toBe(0);
    });

    it("adopting the identity it already has drops nothing", () => {
        const held = cache();
        held.set(key("identity.me"), { class: "large", routeId: "identity.me", value: "anon" });

        expect(held.adopt(null)).toBe(false);
        expect(held.get(key("identity.me"))).toBe("anon");
    });

    it("invalidates every entry of a route, whatever its parameters were", () => {
        const held = cache();
        held.set(key("media.list", { page: "1" }), {
            class: "large",
            routeId: "media.list",
            value: "one",
        });
        held.set(key("media.list", { page: "2" }), {
            class: "large",
            routeId: "media.list",
            value: "two",
        });
        held.set(key("identity.me"), { class: "large", routeId: "identity.me", value: "me" });

        expect(held.invalidateRoutes(["media.list"])).toBe(2);
        expect(held.get(key("media.list", { page: "1" }))).toBeUndefined();
        expect(held.get(key("identity.me"))).toBe("me");
    });

    it("invalidating nothing touches nothing", () => {
        const held = cache();
        held.set(key("identity.me"), { class: "large", routeId: "identity.me", value: "me" });

        expect(held.invalidateRoutes([])).toBe(0);
        expect(held.get(key("identity.me"))).toBe("me");
    });

    it("a class named after an Object property is still a class", () => {
        const held = new ResourceCache<string>({
            classes: { constructor: 1 },
            defaultMaxEntries: 4,
        });
        held.set(key("a"), { class: "constructor", routeId: "a", value: "1" });
        held.set(key("b"), { class: "constructor", routeId: "b", value: "2" });

        expect(held.size()).toBe(1);
        expect(held.get(key("b"))).toBe("2");
    });
});
