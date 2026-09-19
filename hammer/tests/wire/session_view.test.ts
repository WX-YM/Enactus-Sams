// The decode is a trust boundary, and most of this file is about the half of it
// that is not obvious. Every path here becomes the path of an authenticated
// request, so a malformed one does not merely fail — it chooses where a
// session's cookies are sent.

import { describe, expect, it } from "vitest";

import {
    decodeSessionView,
    isRoutePath,
    parseRouteTarget,
    staleClient,
} from "../../src/wire/session_view.js";
import { kBits, kServerHash, permNames, sessionPayload } from "../support/session.js";

describe("the payload a server sends", () => {
    it("decodes the shape anvil's two writers produce", () => {
        const decoded = decodeSessionView(
            sessionPayload({
                bits: [0, 25],
                hash: kServerHash,
                routes: { "content.get": "GET /content/{id}", "audit.list": "GET /audit" },
            }),
            kBits,
        );
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;

        expect(decoded.value.permissions.has(0)).toBe(true);
        expect(decoded.value.permissions.has(25)).toBe(true);
        expect(decoded.value.permissions.has(1)).toBe(false);
        expect(decoded.value.routes.get("audit.list")).toEqual({
            method: "GET",
            path: "/audit",
        });
        expect(decoded.value.superadmin).toBe(false);
        expect(decoded.value.serverHash).toBe(kServerHash);
    });

    it("maps a permission name back to the bit anvil stores it in", () => {
        // anvil sends names in BIT order rather than a mask, so this map is what
        // keeps the 16 bytes ENGINEERING_RULES.md §2.3 asks for. The names are the
        // application's table and the bits are the descriptor's.
        const names = permNames(0, 25);
        expect(names).toEqual(["ContentRead", "AuditRead"]);

        const decoded = decodeSessionView(sessionPayload({ names }), kBits);
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;
        expect(decoded.value.permissions.has(0)).toBe(true);
        expect(decoded.value.permissions.has(25)).toBe(true);
        expect(decoded.value.unknownPermissions.size).toBe(0);
    });

    it("keeps a name it has no bit for rather than failing or inventing one", () => {
        // A server newer than the bundle. Failing the decode would sign somebody
        // out on every deploy that adds a permission, and choosing a bit would
        // be inventing an authority — so the name is carried and the bit is not
        // granted, which hides more rather than less.
        const decoded = decodeSessionView(
            sessionPayload({ names: ["ContentRead", "SomethingNewer"] }),
            kBits,
        );
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;
        expect(decoded.value.permissions.has(0)).toBe(true);
        expect(decoded.value.unknownPermissions.has("SomethingNewer")).toBe(true);
    });

    it("reads no bit out of a name that is only on Object.prototype", () => {
        // The names come off the wire and the table is a plain object, so
        // `bits["toString"]` answers a function nobody put there. `Object.hasOwn`
        // is what makes that an unknown permission rather than a thrown error or
        // a granted bit.
        const decoded = decodeSessionView(
            sessionPayload({ names: ["toString", "__proto__", "constructor"] }),
            kBits,
        );
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;
        expect(decoded.value.permissions.isEmpty()).toBe(true);
        expect(decoded.value.unknownPermissions.size).toBe(3);
    });

    it("says nothing about staleness when the response carried no hash", () => {
        // anvil's reference application sends none — putting the descriptor hash
        // on this response is the application controller's job and no anvil
        // writer does it — so a client talking to it can make no claim at all.
        // Refusing the session over it would turn a hint into a sign-out.
        const decoded = decodeSessionView(sessionPayload(), kBits);
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;
        expect(decoded.value.serverHash).toBeNull();
        expect(staleClient(decoded.value, kServerHash)).toBeNull();
    });

    it("keeps the route table in a Map, not an object", () => {
        // Parsed rather than written as a literal, because the two are not the
        // same thing: `{__proto__: x}` in source sets a prototype, and
        // JSON.parse of the same text creates an own property. The wire path is
        // the second one, so it is the one worth asserting.
        const body: unknown = JSON.parse(
            `{"authority":{"superadmin":false,"perms":[]},` +
                `"routes":{"__proto__":"GET /odd","constructor":"GET /odder"}}`,
        );
        const decoded = decodeSessionView(body, kBits);
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;

        // In a plain object each of these is a lookup that answers something
        // nobody put there. In a Map they are two ordinary keys.
        expect(decoded.value.routes.get("__proto__")).toEqual({ method: "GET", path: "/odd" });
        expect(decoded.value.routes.get("toString")).toBeUndefined();
        expect(decoded.value.routes.get("content.get")).toBeUndefined();
    });
});

describe("a path this library will not send a cookie to", () => {
    it("refuses a protocol-relative path", () => {
        // The one malformed path that does not fail. `new URL("//evil.example",
        // origin)` is not a path on `origin` — it is a different host, and the
        // request goes there.
        expect(parseRouteTarget("GET //evil.example/audit").ok).toBe(false);
        expect(
            decodeSessionView(sessionPayload({ routes: { "a.b": "GET //evil.example/x" } }), kBits)
                .ok,
        ).toBe(false);
    });

    it("refuses an absolute URL", () => {
        expect(parseRouteTarget("GET https://evil.example/audit").ok).toBe(false);
        expect(parseRouteTarget("GET http://evil.example").ok).toBe(false);
    });

    it("refuses a backslash, which some URL parsers read as a separator", () => {
        expect(parseRouteTarget("GET /audit\\..\\admin").ok).toBe(false);
    });

    it("refuses a relative path, whitespace and a query", () => {
        expect(parseRouteTarget("GET audit").ok).toBe(false);
        expect(parseRouteTarget("GET /audit list").ok).toBe(false);
        expect(parseRouteTarget("GET /audit?x=1").ok).toBe(false);
        expect(parseRouteTarget("GET /audit#f").ok).toBe(false);
    });

    it("accepts the patterns anvil actually emits", () => {
        expect(parseRouteTarget("GET /content/{id}")).toMatchObject({ ok: true });
        expect(parseRouteTarget("DELETE /media/{ns}/{id}")).toMatchObject({ ok: true });
        expect(parseRouteTarget("POST /login")).toMatchObject({ ok: true });
    });
});

describe("a method that is not one", () => {
    it("refuses ANY rather than guessing at it", () => {
        // anvil emits ANY so a client fails loudly on a description that forgot
        // to name a method, instead of defaulting to GET and silently calling
        // the wrong thing.
        expect(parseRouteTarget("ANY /session/logout")).toMatchObject({
            ok: false,
            error: "unusable-method",
        });
        expect(parseRouteTarget("TRACE /x").ok).toBe(false);
    });

    it("costs that route and not the session", () => {
        // A real route table carries an ANY entry the day one description
        // forgets a method. Discarding the session over it logs a person out of
        // an application whose only defect is a logout route nobody can call.
        const decoded = decodeSessionView(
            sessionPayload({
                routes: { "auth.logout": "ANY /session/logout", "identity.me": "GET /me" },
            }),
            kBits,
        );
        expect(decoded.ok).toBe(true);
        if (!decoded.ok) return;
        expect(decoded.value.routes.has("auth.logout")).toBe(false);
        expect(decoded.value.unusable.has("auth.logout")).toBe(true);
        expect(decoded.value.routes.get("identity.me")).toEqual({ method: "GET", path: "/me" });
    });
});

describe("a payload that is not one", () => {
    it("fails closed on an authority block that is not one", () => {
        // Absent is not "this holder holds nothing" — it is a response that did
        // not come from anvil's writer, and treating it as an empty set hands a
        // holder a screen with every non-route affordance missing and nothing to
        // explain it.
        for (const authority of [undefined, null, [], "x", 3]) {
            expect(decodeSessionView({ routes: {}, authority }, kBits).ok).toBe(false);
        }
        expect(decodeSessionView({ routes: {} }, kBits).ok).toBe(false);
    });

    it("fails closed on a permission list it cannot read", () => {
        for (const perms of [undefined, null, "ContentRead", 3, {}]) {
            expect(decodeSessionView(sessionPayload({ authority: { perms } }), kBits).ok).toBe(
                false,
            );
        }
        // A name that is not a string is a malformed response rather than a
        // permission nobody knows, and the two must not be conflated: the first
        // means the body is not what it claims, and the second is an ordinary
        // consequence of deploying the server first.
        expect(decodeSessionView(sessionPayload({ names: ["ContentRead", 7] }), kBits).ok).toBe(
            false,
        );
    });

    it("fails on a hash that is not a sha256", () => {
        expect(decodeSessionView(sessionPayload({ hash: "1e867d6c" }), kBits).ok).toBe(false);
        expect(
            decodeSessionView(sessionPayload({ hash: kServerHash.toUpperCase() }), kBits).ok,
        ).toBe(false);
    });

    it("fails on a route table that is not an object", () => {
        const authority = { superadmin: false, perms: [] };
        expect(decodeSessionView({ authority, routes: [] }, kBits).ok).toBe(false);
        expect(decodeSessionView({ authority }, kBits).ok).toBe(false);
    });

    it("refuses a table too large to be one", () => {
        const routes: Record<string, string> = {};
        for (let i = 0; i < 4097; i += 1) {
            routes[`r${i}`] = "GET /x";
        }
        expect(decodeSessionView(sessionPayload({ routes }), kBits).ok).toBe(false);
    });

    it("fails on anything that is not an object at all", () => {
        for (const body of [null, [], "x", 3]) {
            expect(decodeSessionView(body, kBits).ok).toBe(false);
        }
    });
});

describe("a bundle older than the server", () => {
    it("is reported and not acted on", () => {
        const view = decodeSessionView(sessionPayload({ hash: kServerHash }), kBits);
        expect(view.ok).toBe(true);
        if (!view.ok) return;

        expect(staleClient(view.value, kServerHash)).toBeNull();
        expect(staleClient(view.value, `${"0".repeat(63)}1`)).toEqual({
            kind: "stale-client",
            serverHash: kServerHash,
            clientHash: `${"0".repeat(63)}1`,
        });
    });
});

// The predicate the decode above is built on, called directly.
//
// It is PUBLISHED, so an application that has a path from somewhere other than a
// session — a route it is about to declare, a value it is about to log — can ask
// the same question the decode asks, rather than writing a second regular
// expression that is nearly the same. Every case above reaches it through
// `parseRouteTarget`, which cannot express a path with no method in front of it;
// these are the ones only a direct call can state.
describe("the path predicate an application can call", () => {
    it("takes a route pattern and the braces a parameter is written in", () => {
        for (const path of ["/", "/me", "/content/{id}", "/a-b_c.d~e", "/media/{ns}/{id}"]) {
            expect(isRoutePath(path)).toBe(true);
        }
    });

    it("refuses anything that could name a host", () => {
        // `:` is absent from the pattern, so a scheme cannot be spelled; `//`
        // is refused before the pattern runs, because resolving one against an
        // origin succeeds — somewhere else.
        for (const path of ["//evil.example", "https://evil.example/x", "/\\evil.example"]) {
            expect(isRoutePath(path)).toBe(false);
        }
    });

    it("refuses a double separator anywhere, not only at the front", () => {
        // Wider than the reason the code gives, and deliberately so: `/a//b` is
        // not a different host, but it is not a pattern anvil emits either, and
        // a normalising proxy in front of it may or may not collapse it. A path
        // whose meaning depends on what is in front of the server is not one to
        // send a cookie to.
        expect(isRoutePath("/a//b")).toBe(false);
    });

    it("refuses a path that is not one", () => {
        for (const path of ["", "me", "./me", "/me?q=1", "/me#top", "/me ", "/%2e%2e/etc"]) {
            expect(isRoutePath(path)).toBe(false);
        }
    });
});
