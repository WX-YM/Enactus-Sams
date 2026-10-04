// An id is sixteen bytes, and every assertion here is about keeping it that way.
//
// The failure this guards against is not a parse that rejects a good id — that
// one is loud. It is a parse that accepts a bad one: fifteen bytes and a zero
// compares equal to nothing, sorts before everything, and is indistinguishable
// from a real id in every log line it reaches.

import { describe, expect, it } from "../support/test.js";

import { Uuid, kUuidBytes } from "../../src/core/uuid.js";

const kCanonical = "01924f8c-7d3e-7a11-9b2c-0f1e2d3c4b5a";

function unwrap(text: string): Uuid {
    const parsed = Uuid.parse(text);
    if (!parsed.ok) {
        throw new Error(`expected ${text} to parse`);
    }
    return parsed.value;
}

describe("Uuid parsing", () => {
    it("round-trips the canonical form", () => {
        expect(unwrap(kCanonical).format()).toBe(kCanonical);
    });

    it("accepts upper-case hex and renders lower-case", () => {
        // anvil's parser takes either case and its formatter emits lower. Two
        // spellings of one id that compare unequal as strings is exactly why the
        // comparison here is over bytes.
        expect(unwrap(kCanonical.toUpperCase()).format()).toBe(kCanonical);
        expect(unwrap(kCanonical.toUpperCase()).equals(unwrap(kCanonical))).toBe(true);
    });

    it("carries the bytes the hex named", () => {
        const bytes = unwrap(kCanonical).bytes();
        expect(bytes.length).toBe(kUuidBytes);
        expect(bytes[0]).toBe(0x01);
        expect(bytes[6]).toBe(0x7a);
        expect(bytes[15]).toBe(0x5a);
    });

    it("fails closed on a length that is not the canonical one", () => {
        expect(Uuid.parse("")).toMatchObject({ ok: false, error: "bad-length" });
        expect(Uuid.parse(kCanonical.slice(0, 35))).toMatchObject({
            ok: false,
            error: "bad-length",
        });
        // The unhyphenated 32-character form is a real spelling elsewhere, and
        // accepting it would mean two spellings reach a dedupe key as two keys.
        expect(Uuid.parse(kCanonical.replaceAll("-", ""))).toMatchObject({
            ok: false,
            error: "bad-length",
        });
    });

    it("fails closed on a misplaced hyphen or a non-hex digit", () => {
        const misplaced = "0192-f8c74d3e-7a11-9b2c-0f1e2d3c4b5a";
        expect(misplaced.length).toBe(36);
        expect(Uuid.parse(misplaced)).toMatchObject({ ok: false, error: "bad-format" });

        const notHex = kCanonical.replace("b5a", "b5g");
        expect(Uuid.parse(notHex)).toMatchObject({ ok: false, error: "bad-format" });

        // A non-ASCII digit is the case a `code < 128` guard exists for: without
        // it the reverse table is read past its end and returns undefined, which
        // a looser check would treat as nibble zero.
        expect(Uuid.parse(kCanonical.replace("5a", "5٥"))).toMatchObject({
            ok: false,
            error: "bad-format",
        });
    });

    it("refuses a byte array of the wrong length", () => {
        expect(Uuid.fromBytes(new Uint8Array(15))).toMatchObject({
            ok: false,
            error: "bad-length",
        });
        expect(Uuid.fromBytes(new Uint8Array(kUuidBytes)).ok).toBe(true);
    });
});

describe("Uuid identity", () => {
    it("compares without rendering a string", () => {
        const a = unwrap(kCanonical);
        const b = unwrap(kCanonical);
        expect(a.equals(b)).toBe(true);
        expect(a.compare(b)).toBe(0);
        expect(a === b).toBe(false);
    });

    it("orders bytewise, which for a v7 id is chronological", () => {
        // The millisecond is the big-endian prefix, so an id minted later sorts
        // later — which is what makes a client-side sort agree with the index
        // the server paged from. A disagreement shows up at a page boundary, as
        // a row that appears twice or not at all.
        const earlier = unwrap("01924f8c-7d3e-7a11-9b2c-0f1e2d3c4b5a");
        const later = unwrap("01924f8c-7d3f-7a11-9b2c-0f1e2d3c4b5a");
        expect(earlier.compare(later)).toBe(-1);
        expect(later.compare(earlier)).toBe(1);

        const ordered = [later, earlier].sort((x, y) => x.compare(y));
        expect(ordered[0]?.equals(earlier)).toBe(true);
    });

    it("hands out a copy of its bytes", () => {
        const id = unwrap(kCanonical);
        const bytes = id.bytes();
        bytes[0] = 0xff;
        expect(id.format()).toBe(kCanonical);
    });
});

describe("Uuid generation", () => {
    it("mints a version 4 id from the platform generator", () => {
        const id = Uuid.random();
        const bytes = id.bytes();

        // Version 4 in the high nibble of byte 6, RFC 4122 variant in byte 8.
        // Asserted rather than assumed because a client-minted id is an
        // idempotency key: one that is guessable, or that carries a timestamp,
        // is a key another client can collide with.
        expect((bytes[6] ?? 0) >>> 4).toBe(4);
        expect((bytes[8] ?? 0) >>> 6).toBe(0b10);
    });

    it("does not repeat itself", () => {
        const seen = new Set<string>();
        for (let i = 0; i < 256; i += 1) {
            seen.add(Uuid.random().format());
        }
        expect(seen.size).toBe(256);
    });
});
