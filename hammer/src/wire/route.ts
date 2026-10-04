// Where a resolved route becomes an address.
//
// Two rules, and `tools/check-wire-discipline.sh` fails the build on either
// being broken: every path parameter goes through `encodeURIComponent`, every
// query value through `URLSearchParams` (`CLAUDE.md` §5). There is no function
// here that takes a URL and no `fetch(url)` anywhere in the public surface,
// because a concatenated URL is an injection and an untyped route in one line.
//
// --- what interpolation does not do ----------------------------------------
//
// `encodeURIComponent` is necessary and it is not sufficient, and the two
// things it leaves alone are the two worth naming.
//
// It leaves `.` and `..` exactly as they are, because both are unreserved. A
// parameter holding `..` is therefore a DOT SEGMENT by the time a URL parser
// sees it, and `/thing/../elsewhere` is not a request for a document named
// `..`: it is a request for `/elsewhere`, resolved before a byte leaves the
// device, against an origin that carries the session's cookies. It is the same
// class of defect as the protocol-relative path `session_view.ts` refuses, and
// it arrives from the other direction — not from the server's table, but from
// whatever the screen put in the parameter.
//
// It THROWS on a lone surrogate. `URIError`, from a value that is most often a
// string somebody truncated in the middle of an emoji, on a path whose whole
// design is that failure is in the return type (`CLAUDE.md` §3.1). The
// surrogate is found first and reported rather than thrown; the query side
// needs the same check for the opposite reason, because `URLSearchParams` does
// not throw there — it substitutes U+FFFD and sends a value that is not the one
// the caller passed.
//
// --- why the query is sorted -----------------------------------------------
//
// `{ limit, after }` and `{ after, limit }` are one request written twice, and
// the address is what the in-flight dedupe and the resource cache are keyed by
// (`CLAUDE.md` §7). Sorting the keys costs a comparison sort over a handful of
// names and makes "one request per (route, params) in flight" a property of
// this function rather than of the order somebody wrote an object literal in.

import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import { hasLoneSurrogate, toNfc } from "../core/text.js";

import type { HttpMethod, RouteTarget } from "./session_view.js";
import { isRoutePath } from "./session_view.js";

export type QueryScalar = string | number | boolean;

// `null` is "omit this one", spelled rather than left out, for the reason
// `PageRequest.after` is null rather than absent: at a call site an omitted key
// and a key holding a value look identical, and the one that silently means
// "send nothing" is the one that ships.
export type QueryValue = QueryScalar | readonly QueryScalar[] | null;

export type PathParams = Readonly<Record<string, string | number>>;
export type RouteQuery = Readonly<Record<string, QueryValue>>;

// One object rather than two positional records. Two arguments of the same
// shape sitting next to each other is a swap nothing catches — a path parameter
// silently becoming a query value produces a request for the wrong document,
// with a 200 on it.
export type RouteRequest = {
    readonly params: PathParams;
    readonly query: RouteQuery;
};

export type RouteBuildError =
    // The pattern is not one: an unbalanced brace, or a path this library will
    // not send a cookie to. Reachable because `RouteTarget` is a public type and
    // a caller can spell one by hand.
    | "bad-pattern"
    // A `{name}` the caller did not supply.
    | "missing-parameter"
    // A parameter the pattern does not name. Refused rather than ignored: it is
    // a typo, and a typo dropped in silence is a request for something else.
    | "unknown-parameter"
    // An empty value, which collapses a segment and changes the shape of the
    // path rather than filling it in.
    | "empty-parameter"
    // `.` or `..`, which a URL parser resolves rather than requests.
    | "dot-segment"
    // Half a surrogate pair: `encodeURIComponent` throws on one and
    // `URLSearchParams` silently corrupts it.
    | "unpaired-surrogate"
    // A number that will not survive the round trip. In a path it has to be a
    // safe integer — past 2^53 the value is already not the identifier the
    // caller meant — and in a query it has to be finite, because `String(NaN)`
    // is a validation failure at the server and a silent one in the browser.
    | "bad-number";

export type BuiltRoute = {
    readonly method: HttpMethod;

    // Absolute-path reference, parameters substituted and encoded.
    readonly path: string;

    // The query string with no leading `?`, empty when there is none. Separate
    // from the path because the two are separate in every place they are next
    // used: a cache key wants them apart, a log line drops the second
    // (`CLAUDE.md` §5), and `Request` takes them joined.
    readonly query: string;
};

// A path parameter. Numbers are accepted because an id is often one, and are
// held to a safe integer for the reason `bad-number` gives.
function encodeSegment(value: string | number): Result<string, RouteBuildError> {
    if (typeof value === "number") {
        if (!Number.isSafeInteger(value)) {
            return fail("bad-number");
        }
        return ok(String(value));
    }

    // Normalised on the way out, not compared on the way back: two visually
    // identical strings that differ by composition are two different keys to the
    // server's index, and the one that was typed rather than pasted is the one
    // that finds nothing (`CLAUDE.md` §8).
    const text = toNfc(value);
    if (text.length === 0) {
        return fail("empty-parameter");
    }
    if (text === "." || text === "..") {
        return fail("dot-segment");
    }
    if (hasLoneSurrogate(text)) {
        return fail("unpaired-surrogate");
    }
    return ok(encodeURIComponent(text));
}

function substitute(pattern: string, params: PathParams): Result<string, RouteBuildError> {
    let out = "";
    let cursor = 0;

    // The names the pattern actually used, so that "a parameter nobody asked
    // for" is decided by comparing two sets rather than two counts — a pattern
    // naming one parameter twice is otherwise reported as a typo.
    const named: string[] = [];

    for (;;) {
        const open = pattern.indexOf("{", cursor);
        if (open === -1) {
            out += pattern.slice(cursor);
            break;
        }

        const close = pattern.indexOf("}", open + 1);
        if (close === -1) {
            return fail("bad-pattern");
        }

        const name = pattern.slice(open + 1, close);
        if (name.length === 0 || name.includes("{")) {
            return fail("bad-pattern");
        }

        const supplied = Object.hasOwn(params, name) ? params[name] : undefined;
        if (supplied === undefined) {
            return fail("missing-parameter");
        }

        const segment = encodeSegment(supplied);
        if (!segment.ok) {
            return segment;
        }

        out += pattern.slice(cursor, open);
        out += segment.value;
        if (!named.includes(name)) {
            named.push(name);
        }
        cursor = close + 1;
    }

    if (named.length !== Object.keys(params).length) {
        return fail("unknown-parameter");
    }
    return ok(out);
}

function isScalarList(value: QueryValue): value is readonly QueryScalar[] {
    return Array.isArray(value);
}

function encodeScalar(value: QueryScalar): Result<string, RouteBuildError> {
    if (typeof value === "number") {
        if (!Number.isFinite(value)) {
            return fail("bad-number");
        }
        return ok(String(value));
    }
    if (typeof value === "boolean") {
        return ok(value ? "true" : "false");
    }

    const text = toNfc(value);
    if (hasLoneSurrogate(text)) {
        return fail("unpaired-surrogate");
    }
    return ok(text);
}

function buildQuery(query: RouteQuery): Result<string, RouteBuildError> {
    const keys = Object.keys(query).sort();
    if (keys.length === 0) {
        return ok("");
    }

    const search = new URLSearchParams();
    for (const key of keys) {
        const value = query[key];
        if (value === undefined || value === null) {
            continue;
        }

        // A list keeps the order it was given. The ORDER of repeated values for
        // one key is meaningful to whoever reads them; the order of the keys
        // themselves is not, which is why only the latter is sorted.
        const scalars = isScalarList(value) ? value : [value];
        for (const scalar of scalars) {
            const encoded = encodeScalar(scalar);
            if (!encoded.ok) {
                return encoded;
            }
            search.append(key, encoded.value);
        }
    }

    // `URLSearchParams` spells a space `+`, which is the form-urlencoded rule
    // and is decoded as a space only by a decoder that knows it is reading a
    // form. anvil has no query parser yet, so there is no agreement to honour
    // and the safe emission is the one every decoder reads the same way: a
    // literal `+` is already `%2B` by the time `toString` returns, so every `+`
    // left in the result is a space.
    return ok(search.toString().replaceAll("+", "%20"));
}

// The address, or the reason there is not one.
//
// The target comes from `resolveRoute`, which is the only thing that produces
// one from a session or a generated `const`. The pattern is re-checked anyway:
// `RouteTarget` is a public type with no brand, so a caller can write one by
// hand, and the one malformed path that does not fail is the one that succeeds
// somewhere else.
export function buildRoute(
    target: RouteTarget,
    request: RouteRequest,
): Result<BuiltRoute, RouteBuildError> {
    if (!isRoutePath(target.path)) {
        return fail("bad-pattern");
    }

    const path = substitute(target.path, request.params);
    if (!path.ok) {
        return path;
    }

    const query = buildQuery(request.query);
    if (!query.ok) {
        return query;
    }

    return ok({ method: target.method, path: path.value, query: query.value });
}

// The two halves joined, for the one caller that needs them joined. Kept out of
// `BuiltRoute` so that nothing which only wants the path has to carry a string
// with a query string in it past a log line.
export function requestPath(built: BuiltRoute): string {
    if (built.query.length === 0) {
        return built.path;
    }
    return built.path + "?" + built.query;
}
