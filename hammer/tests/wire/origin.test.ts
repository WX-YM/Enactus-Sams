// What this client is willing to send a session's cookies to.
//
// The cases that matter are the ones where the deployment looks fine and the
// session is already gone: a cross-site API answers every request anonymously,
// because a `SameSite=Lax` cookie is not sent on a cross-site subresource
// request at all. There is no runtime symptom that says so — every call simply
// 401s — which is why the refusal is at construction.

import { describe, expect, it } from "vitest";

import type { ApiOrigin } from "../../src/wire/origin.js";
import { defineApiOrigin, requestUrl } from "../../src/wire/origin.js";

const kPage = "https://app.example.com";

function define(apiOrigin: string, site: string | null = null) {
    return defineApiOrigin({ pageOrigin: kPage, apiOrigin, site });
}

describe("the same origin", () => {
    it("is accepted and costs no preflight", () => {
        expect(define(kPage)).toEqual({
            ok: true,
            value: { origin: kPage, crossOrigin: false },
        });
    });

    it("is accepted spelled with a trailing slash, which is the same origin", () => {
        expect(define(kPage + "/")).toMatchObject({ ok: true, value: { crossOrigin: false } });
    });

    it("is accepted on an explicit default port", () => {
        expect(define("https://app.example.com:443")).toMatchObject({
            ok: true,
            value: { origin: kPage },
        });
    });
});

describe("a cross-origin API", () => {
    it("is accepted on a different port and records the preflight", () => {
        expect(define("https://app.example.com:8443")).toEqual({
            ok: true,
            value: { origin: "https://app.example.com:8443", crossOrigin: true },
        });
    });

    it("is accepted on a declared site and records the preflight", () => {
        expect(define("https://api.example.com", "example.com")).toEqual({
            ok: true,
            value: { origin: "https://api.example.com", crossOrigin: true },
        });
    });

    it("accepts the site itself as either host", () => {
        expect(
            defineApiOrigin({
                pageOrigin: "https://example.com",
                apiOrigin: "https://api.example.com",
                site: "example.com",
            }),
        ).toMatchObject({ ok: true });
    });
});

describe("a cross-site API", () => {
    it("is refused when no site is declared", () => {
        expect(define("https://api.other.com")).toEqual({ ok: false, error: "cross-site" });
    });

    it("is refused when the declared site does not hold the page", () => {
        expect(define("https://api.example.com", "other.com")).toEqual({
            ok: false,
            error: "cross-site",
        });
    });

    // The suffix compare that looks right and is not: `evilexample.com` ends
    // with `example.com` and is a different registrable domain.
    it("is refused for a host that merely ends with the site", () => {
        expect(
            defineApiOrigin({
                pageOrigin: kPage,
                apiOrigin: "https://api.evilexample.com",
                site: "example.com",
            }),
        ).toEqual({ ok: false, error: "cross-site" });
    });

    it("refuses a single-label site, which names no registrable domain", () => {
        expect(define("https://api.example.com", "com")).toEqual({ ok: false, error: "bad-site" });
    });

    it("refuses a site that is a URL rather than a domain", () => {
        expect(define("https://api.example.com", "https://example.com")).toEqual({
            ok: false,
            error: "bad-site",
        });
    });

    // Schemeful same-site: a cookie set on the https origin is not sent to the
    // http one, so this is a broken session rather than a preflight.
    it("refuses the same host on a different scheme", () => {
        expect(
            defineApiOrigin({
                pageOrigin: "http://localhost:5173",
                apiOrigin: "https://localhost:5173",
                site: null,
            }),
        ).toEqual({ ok: false, error: "cross-scheme" });
    });
});

describe("what is not an origin", () => {
    it("refuses a string that is not a URL", () => {
        expect(define("api.example.com")).toEqual({ ok: false, error: "not-a-url" });
    });

    it("refuses one carrying a path", () => {
        expect(define("https://app.example.com/api")).toEqual({ ok: false, error: "not-an-origin" });
    });

    it("refuses one carrying a query", () => {
        expect(define("https://app.example.com/?v=1")).toEqual({ ok: false, error: "not-an-origin" });
    });

    it("refuses one carrying embedded credentials", () => {
        expect(define("https://user:pw@app.example.com")).toEqual({
            ok: false,
            error: "not-an-origin",
        });
    });

    it("refuses a scheme that has no cookie jar behind it", () => {
        expect(define("ftp://app.example.com")).toEqual({ ok: false, error: "insecure" });
    });

    // `__Host-` cookies require Secure. A plain-http API is a session that
    // cannot exist, which is worth failing on rather than discovering at login.
    it("refuses plain http on a routable host", () => {
        expect(define("http://api.example.com")).toEqual({ ok: false, error: "not-https" });
    });

    it("accepts plain http on loopback, which the browsers treat as secure", () => {
        expect(
            defineApiOrigin({
                pageOrigin: "http://localhost:5173",
                apiOrigin: "http://localhost:8080",
                site: null,
            }),
        ).toEqual({ ok: true, value: { origin: "http://localhost:8080", crossOrigin: true } });
    });
});

describe("the address a request is made to", () => {
    const origin: ApiOrigin = { origin: "https://app.example.com", crossOrigin: false };

    it("joins the origin and the path", () => {
        expect(requestUrl(origin, { method: "GET", path: "/content/7", query: "" })).toBe(
            "https://app.example.com/content/7",
        );
    });

    it("joins the query when there is one", () => {
        expect(requestUrl(origin, { method: "GET", path: "/media", query: "limit=20" })).toBe(
            "https://app.example.com/media?limit=20",
        );
    });
});
