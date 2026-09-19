// The freshness the server granted, and nothing this client decided for itself.
//
// `docs/00-architecture.md` §7 states the rule in one line — "hammer revalidates
// while serving stale where the server permitted it, and invents no freshness of
// its own" — and this is the whole of the mechanism behind it. A client that
// picked its own thirty seconds would serve a stale row on a resource the server
// marked `no-store`, which is how a permission change takes effect on one screen
// and not on the one beside it.
//
// The default is therefore NOT "cache briefly". It is `maxAgeMs: null`, which
// every caller reads as "no freshness was granted": a response with no
// `Cache-Control` is one the server said nothing about, and saying nothing is not
// permission.
//
// --- what is deliberately not read here -------------------------------------
//
// `s-maxage` is for a SHARED cache, and this is a private one — one tab, one
// identity, one heap. A client honouring it would use the number a CDN was given
// in place of the number it was given itself, and the two differ precisely where
// it matters: an origin commonly grants a CDN minutes and a browser seconds.
//
// `Age`, `Expires` and `Date` are not read either, and that is §6's rule rather
// than an omission: `Expires` is an absolute instant, so using it means comparing
// a server timestamp with the device clock, which is user-settable and routinely
// minutes out. Every number below is a DURATION.

// A scanner rather than a regular expression, for the reason `core/validate.ts`
// gives: a header is attacker-adjacent input on a hot path, and a pattern with a
// nested quantifier over one is a frozen main thread. This is one pass, no
// backtracking, and no intermediate array of directives.
const kMaxDeltaSeconds = 31_536_000;

export type CacheControl = {
    // Nothing may be written down at all. It outranks every other directive
    // here, including a `max-age` sitting beside it.
    readonly noStore: boolean;

    // It may be held, but not served without asking the server first. Held
    // separately from `maxAgeMs: 0` because the two reach a revalidating cache
    // the same way and a non-revalidating one differently, and collapsing them
    // would make that a property of this parser rather than of the caller.
    readonly noCache: boolean;

    // Scoped to one identity. hammer's cache is memory-only and keyed by the
    // identity allowed to read it (`state/cache.ts`), so this is already true of
    // every entry; it is surfaced so that a consumer who later adds persistence
    // has to answer for it rather than discover it.
    readonly isPrivate: boolean;

    // How long the response may be served without asking. Null when the server
    // named no duration — which is not zero and not forever, but "this cache was
    // given no permission".
    readonly maxAgeMs: number | null;

    // How long past expiry a stale copy may be served WHILE a revalidation runs.
    // Zero unless the server said so: stale-while-revalidate is the one directive
    // that lets a client show a value it knows is out of date, so it exists only
    // where it was granted (RFC 5861).
    readonly staleWhileRevalidateMs: number;
};

export const kNoDirectives: CacheControl = {
    noStore: false,
    noCache: false,
    isPrivate: false,
    maxAgeMs: null,
    staleWhileRevalidateMs: 0,
};

// `delta-seconds`, per RFC 9111: digits, and an unreasonably large value is
// clamped rather than refused — the RFC says a cache may treat an overflow as
// the greatest value it can represent, and refusing would turn "cache this for a
// very long time" into "cache nothing".
function deltaSecondsMs(value: string): number | null {
    if (value.length === 0 || value.length > 12) {
        return null;
    }
    let seconds = 0;
    for (let i = 0; i < value.length; i += 1) {
        const digit = value.charCodeAt(i) - 0x30;
        if (digit < 0 || digit > 9) {
            return null;
        }
        seconds = seconds * 10 + digit;
    }
    return Math.min(seconds, kMaxDeltaSeconds) * 1000;
}

export function parseCacheControl(header: string | null): CacheControl {
    if (header === null || header.length === 0) {
        return kNoDirectives;
    }

    let noStore = false;
    let noCache = false;
    let isPrivate = false;
    let mustRevalidate = false;
    let maxAgeMs: number | null = null;
    let staleWhileRevalidateMs = 0;

    let at = 0;
    while (at < header.length) {
        // One directive: up to the next comma that is not inside a quoted
        // string. The quoting matters for `no-cache="Set-Cookie"` and
        // `private="Authorization, X"`, where a naive split on commas reads the
        // second field name as a directive of its own.
        let end = at;
        let quoted = false;
        while (end < header.length) {
            const ch = header[end];
            if (ch === '"') {
                quoted = !quoted;
            } else if (ch === "," && !quoted) {
                break;
            }
            end += 1;
        }

        const directive = header.slice(at, end).trim();
        at = end + 1;
        if (directive.length === 0) {
            continue;
        }

        const equals = directive.indexOf("=");
        const name = (equals < 0 ? directive : directive.slice(0, equals)).trim().toLowerCase();
        let value = equals < 0 ? "" : directive.slice(equals + 1).trim();
        if (value.length >= 2 && value.startsWith('"') && value.endsWith('"')) {
            value = value.slice(1, -1);
        }

        switch (name) {
            case "no-store":
                noStore = true;
                break;
            case "no-cache":
                noCache = true;
                break;
            case "private":
                isPrivate = true;
                break;
            case "must-revalidate":
            case "proxy-revalidate":
                mustRevalidate = true;
                break;
            case "max-age":
                maxAgeMs = deltaSecondsMs(value);
                break;
            case "stale-while-revalidate":
                staleWhileRevalidateMs = deltaSecondsMs(value) ?? 0;
                break;
            default:
                break;
        }
    }

    // `must-revalidate` says a stale copy may not be served without validating
    // it, and stale-while-revalidate says one may. RFC 5861 tells a server not to
    // send both; a server that does is resolved toward the stricter of the two,
    // because the cost of being wrong in that direction is a round trip and the
    // cost of being wrong in the other is a value the origin said was unusable.
    if (mustRevalidate) {
        staleWhileRevalidateMs = 0;
    }

    return {
        noStore,
        noCache,
        isPrivate,
        maxAgeMs: noStore ? null : maxAgeMs,
        staleWhileRevalidateMs: noStore ? 0 : staleWhileRevalidateMs,
    };
}
