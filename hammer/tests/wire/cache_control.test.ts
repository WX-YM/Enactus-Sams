// What the server said about storing a response, and what this client refuses to
// infer when it said nothing.

import { describe, expect, it } from "../support/test.js";

import { kNoDirectives, parseCacheControl } from "../../src/wire/cache_control.js";

describe("parseCacheControl", () => {
    it("grants nothing when the header is absent", () => {
        // Not zero and not forever: saying nothing is not permission, and a
        // client that read a missing header as "cache briefly" would hold a row
        // through the permission change that was meant to remove it.
        expect(parseCacheControl(null)).toEqual(kNoDirectives);
        expect(parseCacheControl("")).toEqual(kNoDirectives);
    });

    it("reads max-age as a duration in milliseconds", () => {
        expect(parseCacheControl("max-age=60").maxAgeMs).toBe(60_000);
    });

    it("reads stale-while-revalidate beside it", () => {
        const directives = parseCacheControl("max-age=30, stale-while-revalidate=120");
        expect(directives.maxAgeMs).toBe(30_000);
        expect(directives.staleWhileRevalidateMs).toBe(120_000);
    });

    it("no-store outranks a max-age sitting beside it", () => {
        const directives = parseCacheControl("max-age=600, no-store");
        expect(directives.noStore).toBe(true);
        expect(directives.maxAgeMs).toBeNull();
        expect(directives.staleWhileRevalidateMs).toBe(0);
    });

    it("must-revalidate withdraws a stale-while-revalidate grant", () => {
        // RFC 5861 tells a server not to send both. A server that does is
        // resolved toward the stricter reading: the cost of being wrong that way
        // is a round trip, and the cost of being wrong the other way is serving a
        // value the origin said was unusable.
        const directives = parseCacheControl("max-age=10, stale-while-revalidate=60, must-revalidate");
        expect(directives.staleWhileRevalidateMs).toBe(0);
    });

    it("ignores s-maxage, which was addressed to a shared cache", () => {
        // An origin commonly grants a CDN minutes and a browser seconds. Reading
        // the CDN's number here uses permission that was given to somebody else.
        const directives = parseCacheControl("s-maxage=600, max-age=5");
        expect(directives.maxAgeMs).toBe(5_000);
    });

    it("reads no-cache and private", () => {
        const directives = parseCacheControl("private, no-cache");
        expect(directives.noCache).toBe(true);
        expect(directives.isPrivate).toBe(true);
    });

    it("does not read a field name inside a quoted directive as a directive", () => {
        // `private="Authorization, X"` splits into two pieces on a naive comma
        // scan, and the second reads as a directive nobody sent.
        const directives = parseCacheControl('private="Authorization, no-store", max-age=5');
        expect(directives.isPrivate).toBe(true);
        expect(directives.noStore).toBe(false);
        expect(directives.maxAgeMs).toBe(5_000);
    });

    it("is case-insensitive and tolerates whitespace", () => {
        const directives = parseCacheControl("  MAX-AGE = 7 ,  No-Store  ");
        expect(directives.noStore).toBe(true);
    });

    it("refuses a delta-seconds that is not digits", () => {
        expect(parseCacheControl("max-age=abc").maxAgeMs).toBeNull();
        expect(parseCacheControl("max-age=-5").maxAgeMs).toBeNull();
        expect(parseCacheControl("max-age=").maxAgeMs).toBeNull();
        expect(parseCacheControl("max-age=1e3").maxAgeMs).toBeNull();
    });

    it("clamps an absurd delta-seconds rather than refusing it", () => {
        // RFC 9111 lets a cache treat an overflow as the greatest value it can
        // represent. Refusing would turn "cache this for a very long time" into
        // "cache nothing".
        expect(parseCacheControl("max-age=999999999").maxAgeMs).toBe(31_536_000_000);
    });

    it("ignores a directive it does not know", () => {
        const directives = parseCacheControl("immutable, max-age=5, no-transform");
        expect(directives.maxAgeMs).toBe(5_000);
    });
});
