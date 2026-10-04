// Where this client is allowed to send a session's cookies.
//
// The deployment invariant is same-ORIGIN, not same-site
// (`docs/00-architecture.md` §8.4), and the two failures it is refusing are
// different sizes:
//
//   CROSS-ORIGIN is a cost. Every mutating request pays a CORS preflight —
//   one extra round trip, 60–200 ms on mobile, on exactly the requests a
//   person is waiting for. It is allowed and it is recorded, because a cost
//   nobody measured is a cost nobody removes.
//
//   CROSS-SITE is not a cost, it is a broken session. anvil's credentials are
//   `__Host-` cookies with `SameSite=Lax`, and a Lax cookie is not sent on a
//   cross-site subresource request at all. Every call would be anonymous, the
//   first one would 401, the refresh would 401 too, and the user would be
//   signed out by a deployment mistake that looks like an authentication bug.
//   So it is refused at construction, where the message can still say what is
//   wrong.
//
// --- hammer ships no public suffix list ------------------------------------
//
// "Same site" means "one registrable domain", and deciding that for two
// arbitrary hosts needs the Public Suffix List: several thousand rules, revised
// monthly, and wrong in the dangerous direction the moment a copy goes stale —
// a stale list calls `a.github.io` and `b.github.io` one site, which is exactly
// the pair it exists to separate. A library with a zero-dependency rule
// (`CLAUDE.md` §5) does not carry that table and does not carry a guess.
//
// What it carries instead is the one comparison that needs no list — the
// origins are equal — and, for a deployment that genuinely splits them, a
// CHECKED CLAIM: the application names the registrable domain it believes the
// two share, and both hosts are required to lie within it. That converts
// "trust me" into something a wrong answer fails loudly on, and it puts the
// claim in the one place that knows the deployment.
//
// --- why the page origin is a parameter ------------------------------------
//
// `location.origin` is a document's, and `src/wire/` may not name a document
// (`tools/check-layering.sh`). It is also what a test has to vary in order to
// drive any of this at all, which is the same reason the fetch, the clock and
// the lock manager are injected rather than read (`CLAUDE.md` §3.3).

import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

import type { BuiltRoute } from "./route.js";
import { requestPath } from "./route.js";

export type ApiOriginError =
    // Not parseable as an absolute URL at all.
    | "not-a-url"
    // Parseable, but it is not an origin: it carries a path, a query, a
    // fragment or embedded credentials. Refused rather than trimmed, because
    // trimming accepts a configuration that says something other than what its
    // author meant.
    | "not-an-origin"
    // A scheme that is not `http` or `https`. There is no cookie jar behind a
    // `file:` or an extension scheme for this library to be driving.
    | "insecure"
    // `http:` on a host that is not loopback. `__Host-` cookies require
    // `Secure`, so the session this client exists to drive cannot exist there.
    | "not-https"
    // Same host, different scheme. Site comparison is SCHEMEFUL: `http://x` and
    // `https://x` are two sites to a modern browser, and a cookie set on one is
    // not sent to the other.
    | "cross-scheme"
    // The hosts differ and no shared registrable domain was declared, or the one
    // declared does not contain both of them.
    | "cross-site"
    // The declared site is not a domain this check can use: an address, a single
    // label, an empty string, or something with a scheme or a path in it.
    | "bad-site";

export type ApiOrigin = {
    // Scheme, host and port, with no trailing slash — what `URL.origin` returns
    // and what a request is built against.
    readonly origin: string;

    // True when every mutating request costs a CORS preflight. Carried rather
    // than recomputed so the observability record can state it: a deployment
    // pays this on every write and it is invisible in a waterfall unless
    // somebody is looking for the `OPTIONS`.
    readonly crossOrigin: boolean;
};

export type ApiOriginRequest = {
    // Where the document was served from: `location.origin`, passed in.
    readonly pageOrigin: string;

    readonly apiOrigin: string;

    // The registrable domain the application asserts both hosts share, or null
    // for a deployment that does not need one. Null rather than absent, for the
    // reason every other nullable field here is (`CLAUDE.md` §2.3).
    readonly site: string | null;
};

// A label, a dot, and at least one more label. Deliberately narrow: this is a
// claim about a deployment, and a claim that cannot be spelled wrongly is worth
// more than one that accepts everything.
const kDomain = /^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$/;

function parse(text: string): URL | null {
    try {
        return new URL(text);
    } catch {
        // The one thing `new URL` does that this module must not: throw for an
        // input that came from a configuration file (`CLAUDE.md` §3.1).
        return null;
    }
}

// A secure context without `https`. The browsers treat these as secure, so a
// development deployment on one is not a misconfiguration to refuse.
function isLoopback(hostname: string): boolean {
    return (
        hostname === "localhost" ||
        hostname.endsWith(".localhost") ||
        hostname === "127.0.0.1" ||
        hostname === "[::1]"
    );
}

function asOrigin(text: string): Result<URL, ApiOriginError> {
    const url = parse(text);
    if (url === null) {
        return fail("not-a-url");
    }
    if (url.protocol !== "https:" && url.protocol !== "http:") {
        return fail("insecure");
    }
    if (
        url.pathname !== "/" ||
        url.search.length !== 0 ||
        url.hash.length !== 0 ||
        url.username.length !== 0 ||
        url.password.length !== 0
    ) {
        return fail("not-an-origin");
    }
    if (url.protocol === "http:" && !isLoopback(url.hostname)) {
        return fail("not-https");
    }
    return ok(url);
}

// Within the declared registrable domain: the domain itself, or a subdomain of
// it. A suffix compare would call `evilexample.com` a subdomain of
// `example.com`, which is the whole class of bug this shape exists to avoid.
function within(hostname: string, site: string): boolean {
    return hostname === site || hostname.endsWith("." + site);
}

// The origin, or the reason this client will not be built.
//
// Called by `createClient`, which turns a failure here into a throw: a
// misconfigured client is programmer error and is the one thing a throw is for
// (`CLAUDE.md` §3.1). It is a `Result` here so that the decision can be tested
// without constructing one, and so the reason is a value rather than a string.
export function defineApiOrigin(request: ApiOriginRequest): Result<ApiOrigin, ApiOriginError> {
    const api = asOrigin(request.apiOrigin);
    if (!api.ok) {
        return api;
    }

    const page = parse(request.pageOrigin);
    if (page === null) {
        return fail("not-a-url");
    }

    if (page.origin === api.value.origin) {
        return ok({ origin: api.value.origin, crossOrigin: false });
    }

    if (page.protocol !== api.value.protocol) {
        return fail("cross-scheme");
    }

    // Same host, different port. Cookies are not port-scoped — the browser
    // treats `x.test` and `x.test:8443` as one host for every cookie it holds —
    // so the session survives, and what is left is the preflight.
    if (page.hostname === api.value.hostname) {
        return ok({ origin: api.value.origin, crossOrigin: true });
    }

    const site = request.site;
    if (site === null) {
        return fail("cross-site");
    }
    if (!kDomain.test(site)) {
        return fail("bad-site");
    }
    if (!within(page.hostname, site) || !within(api.value.hostname, site)) {
        return fail("cross-site");
    }

    return ok({ origin: api.value.origin, crossOrigin: true });
}

// The address a request is actually made to, and the only place an origin and a
// path are joined.
//
// Both halves are already checked — the origin by this module, the path by the
// route builder and by `isRoutePath` before that — which is what makes the join
// a concatenation rather than a parse. `new URL(path, origin)` would be the
// obvious spelling and is the wrong one: it RESOLVES, so a path that slipped
// through as `//elsewhere.example/x` would silently become a request to another
// host, which is the defect `session_view.ts` refuses at the other end.
export function requestUrl(origin: ApiOrigin, built: BuiltRoute): string {
    return origin.origin + requestPath(built);
}
