// Sixteen bytes, because a UUID is sixteen bytes.
//
// The form a client usually holds is the 36-character hyphenated string, and it
// is the wrong one to hold: 36 UTF-16 code units is 72 bytes of heap against 16,
// a comparison is a string compare rather than a two-word loop, and a list view
// holding a thousand ids pays both a thousand times. The string is a rendering,
// produced where a human or a log will read it and nowhere else.
//
// Byte order is the wire's, which matters more than it looks: anvil stores an id
// as 16 raw bytes and its UUIDv7 prefix is big-endian precisely so that a
// bytewise comparison is a chronological one. `compare` here is that same
// bytewise order, so a client-side sort of a page agrees with the index the
// server paged it from.

import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

export const kUuidBytes = 16;

// 8-4-4-4-12 with four hyphens.
const kFormattedLength = 36;

const kHexDigits = "0123456789abcdef";

// A 128-entry reverse table rather than indexOf per character, for the reason
// perm_set.ts gives: a scan of a 16-character string per nibble turns a parse
// that should be invisible into something measurable on a list response.
const kHexReverse: Int8Array = (() => {
    const table = new Int8Array(128).fill(-1);
    for (let i = 0; i < 10; i += 1) {
        table[0x30 + i] = i;
    }
    for (let i = 0; i < 6; i += 1) {
        table[0x61 + i] = 10 + i;
        table[0x41 + i] = 10 + i;
    }
    return table;
})();

export type UuidParseError = "bad-length" | "bad-format";

export class Uuid {
    // Private and never handed out; `bytes()` returns a copy. A Uint8Array is
    // never deeply frozen, so the only way to keep an id immutable is to keep
    // the array unreachable.
    private readonly octets: Uint8Array;

    private constructor(octets: Uint8Array) {
        this.octets = octets;
    }

    // The only generator, and it is deliberately the platform's.
    //
    // A hand-rolled v4 over crypto.getRandomValues is four lines and one of them
    // is the version nibble somebody eventually gets wrong; a fallback to a
    // weaker source when the platform's is absent is a guessable idempotency key
    // and a guessable client id, silently. An environment with no
    // crypto.randomUUID is not a secure context, which means the session cookies
    // this library exists to drive do not work there either — that is a
    // misconfigured client, and it is the one thing a throw is for
    // (ENGINEERING_RULES.md §3.1).
    static random(): Uuid {
        const parsed = Uuid.parse(crypto.randomUUID());
        if (!parsed.ok) {
            throw new Error("crypto.randomUUID returned a value that is not a UUID");
        }
        return parsed.value;
    }

    // The canonical hyphenated form only, in either case. Anything else is a
    // value from somewhere other than the server, and it fails closed: there is
    // no partially parsed id, because a partially parsed id is an authority
    // check against fifteen bytes and one zero.
    static parse(text: string): Result<Uuid, UuidParseError> {
        if (text.length !== kFormattedLength) {
            return fail("bad-length");
        }

        const octets = new Uint8Array(kUuidBytes);
        let written = 0;
        let high = -1;

        for (let i = 0; i < kFormattedLength; i += 1) {
            const code = text.charCodeAt(i);

            if (i === 8 || i === 13 || i === 18 || i === 23) {
                if (code !== 0x2d) {
                    return fail("bad-format");
                }
                continue;
            }

            const nibble = code < 128 ? kHexReverse[code] : -1;
            if (nibble === undefined || nibble < 0) {
                return fail("bad-format");
            }

            if (high < 0) {
                high = nibble;
            } else {
                octets[written] = (high << 4) | nibble;
                written += 1;
                high = -1;
            }
        }

        return ok(new Uuid(octets));
    }

    static fromBytes(bytes: Uint8Array): Result<Uuid, UuidParseError> {
        if (bytes.length !== kUuidBytes) {
            return fail("bad-length");
        }
        return ok(new Uuid(Uint8Array.from(bytes)));
    }

    // Sixteen byte comparisons and no allocation. This is the operation the
    // representation exists for: it runs once per row per render, against a
    // selection, a key or a dedupe set.
    equals(other: Uuid): boolean {
        for (let i = 0; i < kUuidBytes; i += 1) {
            if (this.octets[i] !== other.octets[i]) {
                return false;
            }
        }
        return true;
    }

    // Bytewise, which for a UUIDv7 is chronological — the timestamp is the
    // big-endian prefix. A client-side sort of a page therefore agrees with the
    // index the server paged it from, and a disagreement at a page boundary is a
    // row that appears twice or never.
    compare(other: Uuid): number {
        for (let i = 0; i < kUuidBytes; i += 1) {
            const mine = this.octets[i] ?? 0;
            const theirs = other.octets[i] ?? 0;
            if (mine !== theirs) {
                return mine < theirs ? -1 : 1;
            }
        }
        return 0;
    }

    // For a human and for a log line, and never as a key: two ids that are equal
    // are equal as bytes, and rendering both to compare them is 72 bytes of
    // garbage per comparison.
    format(): string {
        let out = "";
        for (let i = 0; i < kUuidBytes; i += 1) {
            if (i === 4 || i === 6 || i === 8 || i === 10) {
                out += "-";
            }
            const byte = this.octets[i] ?? 0;
            out += kHexDigits.charAt(byte >>> 4);
            out += kHexDigits.charAt(byte & 0x0f);
        }
        return out;
    }

    bytes(): Uint8Array {
        return Uint8Array.from(this.octets);
    }
}
