// Where a resolved route becomes an address.
//
// The assertions that matter here are the refusals. An address that is built
// correctly is proved by one example per encoding rule; an address that is
// built WRONGLY is a request sent to somewhere else, carrying the session's
// cookies, and every one of those cases has its own test.

import { describe, expect, it } from "vitest";

import type { RouteTarget } from "../../src/wire/session_view.js";
import { buildRoute, requestPath } from "../../src/wire/route.js";

const kEmpty = { params: {}, query: {} } as const;

function target(path: string, method: RouteTarget["method"] = "GET"): RouteTarget {
    return { method, path };
}

describe("a path with no parameters", () => {
    it("is the pattern, unchanged", () => {
        expect(buildRoute(target("/login", "POST"), kEmpty)).toEqual({
            ok: true,
            value: { method: "POST", path: "/login", query: "" },
        });
    });

    it("carries the method it was resolved with", () => {
        expect(buildRoute(target("/thing", "DELETE"), kEmpty)).toMatchObject({
            ok: true,
            value: { method: "DELETE" },
        });
    });

    it("refuses a parameter the pattern does not name", () => {
        expect(buildRoute(target("/thing"), { params: { id: "a" }, query: {} })).toEqual({
            ok: false,
            error: "unknown-parameter",
        });
    });
});

describe("a path parameter", () => {
    it("is substituted", () => {
        expect(buildRoute(target("/thing/{id}"), { params: { id: "abc" }, query: {} })).toEqual({
            ok: true,
            value: { method: "GET", path: "/thing/abc", query: "" },
        });
    });

    it("is substituted more than once in one pattern", () => {
        const built = buildRoute(target("/{ns}/thing/{id}/meta"), {
            params: { ns: "images", id: "7" },
            query: {},
        });
        expect(built).toMatchObject({ ok: true, value: { path: "/images/thing/7/meta" } });
    });

    it("accepts a number, because an id is often one", () => {
        expect(buildRoute(target("/thing/{id}"), { params: { id: 42 }, query: {} })).toMatchObject({
            ok: true,
            value: { path: "/thing/42" },
        });
    });

    it("refuses a number that has already lost precision", () => {
        expect(
            buildRoute(target("/thing/{id}"), { params: { id: 2 ** 53 }, query: {} }),
        ).toEqual({ ok: false, error: "bad-number" });
    });

    it("refuses a fraction, because a segment is an identifier and not a measurement", () => {
        expect(buildRoute(target("/thing/{id}"), { params: { id: 1.5 }, query: {} })).toEqual({
            ok: false,
            error: "bad-number",
        });
    });

    it("cannot introduce a segment of its own", () => {
        const built = buildRoute(target("/thing/{id}"), {
            params: { id: "a/../b" },
            query: {},
        });
        expect(built).toMatchObject({ ok: true, value: { path: "/thing/a%2F..%2Fb" } });
    });

    it("cannot introduce a query string of its own", () => {
        const built = buildRoute(target("/thing/{id}"), {
            params: { id: "a?b=c#d" },
            query: {},
        });
        expect(built).toMatchObject({ ok: true, value: { path: "/thing/a%3Fb%3Dc%23d" } });
    });

    // The one a reviewer does not look for: `..` survives encodeURIComponent
    // untouched, and a URL parser resolves it rather than requesting it.
    it("refuses a dot segment", () => {
        expect(buildRoute(target("/thing/{id}"), { params: { id: ".." }, query: {} })).toEqual({
            ok: false,
            error: "dot-segment",
        });
        expect(buildRoute(target("/thing/{id}"), { params: { id: "." }, query: {} })).toEqual({
            ok: false,
            error: "dot-segment",
        });
    });

    it("encodes a dot segment that is only part of the value", () => {
        expect(
            buildRoute(target("/thing/{id}"), { params: { id: "..x" }, query: {} }),
        ).toMatchObject({ ok: true, value: { path: "/thing/..x" } });
    });

    it("refuses an empty value rather than collapsing the segment", () => {
        expect(buildRoute(target("/thing/{id}"), { params: { id: "" }, query: {} })).toEqual({
            ok: false,
            error: "empty-parameter",
        });
    });

    it("refuses half a surrogate pair rather than throwing a URIError", () => {
        expect(
            buildRoute(target("/thing/{id}"), { params: { id: "a\ud83d" }, query: {} }),
        ).toEqual({ ok: false, error: "unpaired-surrogate" });
    });

    it("keeps a whole surrogate pair", () => {
        const built = buildRoute(target("/thing/{id}"), {
            params: { id: "\u{1f600}" },
            query: {},
        });
        expect(built).toMatchObject({ ok: true, value: { path: "/thing/%F0%9F%98%80" } });
    });

    // Written as escapes rather than as characters: the two spellings are
    // indistinguishable in an editor, and a test whose reader cannot tell them
    // apart is a test that looks tautological and gets deleted.
    it("normalises to NFC, so a typed value and a pasted one are one address", () => {
        const composed = buildRoute(target("/thing/{id}"), {
            params: { id: "\u00e9" },
            query: {},
        });
        const decomposed = buildRoute(target("/thing/{id}"), {
            params: { id: "e\u0301" },
            query: {},
        });
        expect(composed).toMatchObject({ ok: true, value: { path: "/thing/%C3%A9" } });
        expect(decomposed).toEqual(composed);
    });

    it("reports one the caller did not supply", () => {
        expect(buildRoute(target("/thing/{id}"), kEmpty)).toEqual({
            ok: false,
            error: "missing-parameter",
        });
    });

    it("reports a parameter the pattern does not name alongside one it does", () => {
        expect(
            buildRoute(target("/thing/{id}"), { params: { id: "a", nope: "b" }, query: {} }),
        ).toEqual({ ok: false, error: "unknown-parameter" });
    });

    it("is not satisfied by an inherited property", () => {
        expect(buildRoute(target("/thing/{toString}"), kEmpty)).toEqual({
            ok: false,
            error: "missing-parameter",
        });
    });
});

describe("a pattern that is not one", () => {
    it("refuses an unbalanced brace", () => {
        expect(buildRoute(target("/thing/{id"), { params: { id: "a" }, query: {} })).toEqual({
            ok: false,
            error: "bad-pattern",
        });
    });

    it("refuses an empty parameter name", () => {
        expect(buildRoute(target("/thing/{}"), kEmpty)).toEqual({
            ok: false,
            error: "bad-pattern",
        });
    });

    // A `RouteTarget` is a public type with no brand, so a hand-written one
    // reaches here. `//evil.example/x` resolved against an origin is not a path
    // on that origin — it is a different host, and it is the one malformed path
    // that succeeds.
    it("refuses a protocol-relative path", () => {
        expect(buildRoute(target("//evil.example/x"), kEmpty)).toEqual({
            ok: false,
            error: "bad-pattern",
        });
    });

    it("refuses a path that is not rooted", () => {
        expect(buildRoute(target("thing"), kEmpty)).toEqual({
            ok: false,
            error: "bad-pattern",
        });
    });

    it("refuses an absolute URL", () => {
        expect(buildRoute(target("https://evil.example/x"), kEmpty)).toEqual({
            ok: false,
            error: "bad-pattern",
        });
    });
});

describe("the query", () => {
    it("is empty when nothing was given", () => {
        expect(buildRoute(target("/thing"), kEmpty)).toMatchObject({
            ok: true,
            value: { query: "" },
        });
    });

    it("encodes a value", () => {
        const built = buildRoute(target("/thing"), {
            params: {},
            query: { q: "a b&c=d" },
        });
        expect(built).toMatchObject({ ok: true, value: { query: "q=a%20b%26c%3Dd" } });
    });

    // `URLSearchParams` spells a space `+`, which only a decoder that knows it
    // is reading a form reads back as one.
    it("spells a space %20 rather than +", () => {
        const built = buildRoute(target("/thing"), { params: {}, query: { q: "a b" } });
        expect(built).toMatchObject({ ok: true, value: { query: "q=a%20b" } });
    });

    it("keeps a literal plus distinct from a space", () => {
        const built = buildRoute(target("/thing"), { params: {}, query: { q: "a+b" } });
        expect(built).toMatchObject({ ok: true, value: { query: "q=a%2Bb" } });
    });

    it("sorts its keys, so one request has one address", () => {
        const one = buildRoute(target("/thing"), {
            params: {},
            query: { limit: 50, after: "abc" },
        });
        const other = buildRoute(target("/thing"), {
            params: {},
            query: { after: "abc", limit: 50 },
        });
        expect(one).toMatchObject({ ok: true, value: { query: "after=abc&limit=50" } });
        expect(other).toEqual(one);
    });

    it("keeps the order of repeated values for one key", () => {
        const built = buildRoute(target("/thing"), {
            params: {},
            query: { tag: ["b", "a"] },
        });
        expect(built).toMatchObject({ ok: true, value: { query: "tag=b&tag=a" } });
    });

    it("omits a null", () => {
        const built = buildRoute(target("/thing"), {
            params: {},
            query: { after: null, limit: 10 },
        });
        expect(built).toMatchObject({ ok: true, value: { query: "limit=10" } });
    });

    it("keeps an empty string, which is a value and not an absence", () => {
        const built = buildRoute(target("/thing"), { params: {}, query: { q: "" } });
        expect(built).toMatchObject({ ok: true, value: { query: "q=" } });
    });

    it("spells a boolean", () => {
        const built = buildRoute(target("/thing"), {
            params: {},
            query: { draft: true, archived: false },
        });
        expect(built).toMatchObject({
            ok: true,
            value: { query: "archived=false&draft=true" },
        });
    });

    it("accepts a fraction, because a query value is not an identifier", () => {
        const built = buildRoute(target("/thing"), { params: {}, query: { lat: 51.5 } });
        expect(built).toMatchObject({ ok: true, value: { query: "lat=51.5" } });
    });

    it("refuses a number that is not one", () => {
        expect(
            buildRoute(target("/thing"), { params: {}, query: { limit: Number.NaN } }),
        ).toEqual({ ok: false, error: "bad-number" });
        expect(
            buildRoute(target("/thing"), { params: {}, query: { limit: Infinity } }),
        ).toEqual({ ok: false, error: "bad-number" });
    });

    // URLSearchParams does not throw on this one. It substitutes U+FFFD and
    // sends a value that is not the one the caller passed.
    it("refuses half a surrogate pair rather than sending a replacement character", () => {
        expect(
            buildRoute(target("/thing"), { params: {}, query: { q: "a\ud83d" } }),
        ).toEqual({ ok: false, error: "unpaired-surrogate" });
    });

    it("refuses half a surrogate pair inside a list", () => {
        expect(
            buildRoute(target("/thing"), { params: {}, query: { tag: ["ok", "\udc00"] } }),
        ).toEqual({ ok: false, error: "unpaired-surrogate" });
    });

    it("normalises a value to NFC", () => {
        const composed = buildRoute(target("/thing"), { params: {}, query: { q: "\u00e9" } });
        const decomposed = buildRoute(target("/thing"), {
            params: {},
            query: { q: "e\u0301" },
        });
        expect(composed).toMatchObject({ ok: true, value: { query: "q=%C3%A9" } });
        expect(decomposed).toEqual(composed);
    });
});

describe("the joined address", () => {
    it("is the path alone when there is no query", () => {
        const built = buildRoute(target("/thing"), kEmpty);
        expect(built.ok && requestPath(built.value)).toBe("/thing");
    });

    it("joins the two with a question mark", () => {
        const built = buildRoute(target("/thing/{id}"), {
            params: { id: "7" },
            query: { limit: 10 },
        });
        expect(built.ok && requestPath(built.value)).toBe("/thing/7?limit=10");
    });

    // Everything this module produces is an absolute-path reference, so
    // resolving one against the API origin cannot reach a different host. That
    // is the property the refusals above exist to keep, stated once against the
    // platform's own parser rather than against the regular expression.
    it("resolves against an origin without leaving it", () => {
        for (const value of ["a/../b", "..x", "%2e%2e", "a?b", "\u{1f600}"]) {
            const built = buildRoute(target("/thing/{id}"), {
                params: { id: value },
                query: {},
            });
            expect(built.ok).toBe(true);
            if (!built.ok) continue;
            const url = new URL(requestPath(built.value), "https://api.example");
            expect(url.origin).toBe("https://api.example");
            expect(url.pathname.startsWith("/thing/")).toBe(true);
        }
    });
});
