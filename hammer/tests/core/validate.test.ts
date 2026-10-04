// The reasons below are the reference application's, taken from the descriptor
// in tests/testapp/. They are spelled out here rather than imported from the
// library because the library has none: the vocabulary is the descriptor's, and
// a union in hammer would be a second copy of a table that grows server-side.

import { describe, expect, it } from "../support/test.js";

import {
    all,
    boundedText,
    codePointClass,
    memberOf,
    numberInRange,
    parseIntegerIn,
} from "../../src/core/validate.js";

const kReasons = {
    required: "REQUIRED",
    tooShort: "TOO_SHORT",
    tooLong: "TOO_LONG",
    badCharset: "BAD_CHARSET",
} as const;

const kIntegerReasons = { badFormat: "BAD_FORMAT", outOfRange: "OUT_OF_RANGE" } as const;

// Escapes rather than literals, for the reason the bidi suite gives: a file
// carrying invisible characters is a file nobody can review.
const kDecomposed = "e\u0301"; // "e" + combining acute
const kComposed = "\u00E9";
const kFsi = "\u2068";
const kPdi = "\u2069";
const kRlo = "\u202E"; // right-to-left override
const kDelete = "\u007F";
const kArabicIndic29 = "\u0662\u0669";
const kExtended29 = "\u06F2\u06F9";

const prose = boundedText({
    minCodePoints: 1,
    maxCodePoints: 8,
    allowLineBreaks: true,
    textClass: "prose",
    reasons: kReasons,
});

const identifier = boundedText({
    minCodePoints: 3,
    maxCodePoints: 16,
    allowLineBreaks: false,
    textClass: "identifier",
    reasons: kReasons,
});

describe("bounded text", () => {
    it("hands back the normalised value, which is the value to send", () => {
        // The check is of the NFC form, so the caller must send the NFC form.
        // Returning a verdict rather than a value is how a form ends up sending
        // the string it checked a different spelling of.
        expect(prose(kDecomposed)).toMatchObject({ ok: true, value: kComposed });
    });

    it("counts the bound in code points", () => {
        // Eight emoji is eight code points and sixteen code units. A bound
        // written against `.length` would accept four.
        const eight = "\u{1F600}".repeat(8);
        expect(prose(eight).ok).toBe(true);
        expect(prose(eight + "\u{1F600}")).toMatchObject({ ok: false, error: "TOO_LONG" });
    });

    it("reports the reason anvil would report, in anvil's order", () => {
        // The charset check runs before the bound on both sides, so an
        // over-long value full of control characters is BAD_CHARSET on both. A
        // field that says one thing before the request and another after it is
        // a field the user cannot act on.
        expect(prose(kDelete.repeat(99))).toMatchObject({ ok: false, error: "BAD_CHARSET" });
        expect(prose("")).toMatchObject({ ok: false, error: "REQUIRED" });
        expect(prose("a".repeat(9))).toMatchObject({ ok: false, error: "TOO_LONG" });
        expect(identifier("ab")).toMatchObject({ ok: false, error: "TOO_SHORT" });
    });

    it("allows line breaks only where the rules do", () => {
        expect(prose("a\nb").ok).toBe(true);
        expect(identifier("abc\ndef")).toMatchObject({ ok: false, error: "BAD_CHARSET" });
        // DEL is refused in both: invisible, survives every "printable" filter,
        // and nothing types it on purpose.
        expect(prose("a" + kDelete + "b")).toMatchObject({ ok: false, error: "BAD_CHARSET" });
    });

    it("applies the text class to the bidi controls", () => {
        // An isolate is legitimate in a sentence and never in a username: two
        // usernames that render identically and compare unequal is the attack.
        expect(prose(kFsi + "ab" + kPdi).ok).toBe(true);
        expect(identifier(kFsi + "admin" + kPdi)).toMatchObject({
            ok: false,
            error: "BAD_CHARSET",
        });
        // An override is refused in prose as well: nothing legitimate forces
        // the direction of text that already has one.
        expect(prose("a" + kRlo + "b")).toMatchObject({ ok: false, error: "BAD_CHARSET" });
    });

    it("takes a value with no minimum", () => {
        const optional = boundedText({
            minCodePoints: 0,
            maxCodePoints: 4,
            allowLineBreaks: false,
            textClass: "prose",
            reasons: kReasons,
        });
        expect(optional("")).toMatchObject({ ok: true, value: "" });
    });
});

describe("code point classes", () => {
    const hexadecimal = codePointClass(
        [
            [0x30, 0x39],
            [0x61, 0x66],
        ],
        "BAD_FORMAT",
    );

    it("accepts what is in range and refuses what is not", () => {
        expect(hexadecimal("0a9f").ok).toBe(true);
        expect(hexadecimal("0A9F")).toMatchObject({ ok: false, error: "BAD_FORMAT" });
        expect(hexadecimal("")).toMatchObject({ ok: true });
    });

    it("compares whole code points, not code units", () => {
        // The astral character is one code point far outside the range. A scan
        // over code units would test each half of the pair separately, and both
        // halves sit in a range no class ever names.
        expect(hexadecimal("\u{1F600}")).toMatchObject({ ok: false, error: "BAD_FORMAT" });
    });
});

describe("membership", () => {
    const member = memberOf(["alpha", "beta"], "NOT_ALLOWED");

    it("answers from a set built once", () => {
        expect(member("alpha")).toMatchObject({ ok: true, value: "alpha" });
        expect(member("gamma" as "alpha")).toMatchObject({ ok: false, error: "NOT_ALLOWED" });
    });
});

describe("integers", () => {
    const bounded = parseIntegerIn({ min: 1, max: 100, reasons: kIntegerReasons });

    it("folds before parsing", () => {
        // Twenty-nine, typed on an Arabic keyboard. Refusing it refuses the
        // user rather than the input.
        expect(bounded(kArabicIndic29)).toMatchObject({ ok: true, value: 29 });
        expect(bounded(kExtended29)).toMatchObject({ ok: true, value: 29 });
    });

    it("refuses trailing junk rather than truncating it", () => {
        expect(bounded("12abc")).toMatchObject({ ok: false, error: "BAD_FORMAT" });
        expect(bounded("")).toMatchObject({ ok: false, error: "BAD_FORMAT" });
        expect(bounded("-")).toMatchObject({ ok: false, error: "BAD_FORMAT" });
        expect(bounded(" 12")).toMatchObject({ ok: false, error: "BAD_FORMAT" });
    });

    it("refuses what it cannot hold exactly", () => {
        // Beyond 2^53 a JS number rounds. A rounded id addresses a different
        // row, and nothing downstream can tell that it happened.
        const wide = parseIntegerIn({
            min: 0,
            max: Number.MAX_SAFE_INTEGER,
            reasons: kIntegerReasons,
        });
        expect(wide("9007199254740993")).toMatchObject({ ok: false, error: "OUT_OF_RANGE" });
        expect(wide("9007199254740991")).toMatchObject({ ok: true, value: 9007199254740991 });
        expect(bounded("99999999999999999999999")).toMatchObject({
            ok: false,
            error: "BAD_FORMAT",
        });
    });

    it("applies the range", () => {
        expect(bounded("0")).toMatchObject({ ok: false, error: "OUT_OF_RANGE" });
        expect(bounded("101")).toMatchObject({ ok: false, error: "OUT_OF_RANGE" });
        expect(bounded("-5")).toMatchObject({ ok: false, error: "OUT_OF_RANGE" });
        expect(bounded("+7")).toMatchObject({ ok: true, value: 7 });
    });

    it("bounds a number that arrived as a number", () => {
        const ratio = numberInRange(0, 1, "OUT_OF_RANGE");
        expect(ratio(0.5).ok).toBe(true);
        expect(ratio(1.5)).toMatchObject({ ok: false, error: "OUT_OF_RANGE" });
        expect(ratio(Number.NaN)).toMatchObject({ ok: false, error: "OUT_OF_RANGE" });
    });
});

describe("composition", () => {
    it("threads the value through and stops at the first failure", () => {
        const slug = all(
            boundedText({
                minCodePoints: 1,
                maxCodePoints: 6,
                allowLineBreaks: false,
                textClass: "identifier",
                reasons: kReasons,
            }),
            codePointClass(
                [
                    [0x61, 0x7a],
                    [0x2d, 0x2d],
                ],
                "BAD_FORMAT",
            ),
        );

        expect(slug("ab-cd")).toMatchObject({ ok: true, value: "ab-cd" });
        // The bound fails first, so the class never sees the value: the reason
        // reported is the reason for the first thing that was wrong with it.
        expect(slug("ABCDEFGH")).toMatchObject({ ok: false, error: "TOO_LONG" });
        expect(slug("ABC")).toMatchObject({ ok: false, error: "BAD_FORMAT" });
    });

    it("sees what the check before it produced", () => {
        // The class runs against the NFC form, because the bound normalised it.
        // Against the decomposed form the combining acute is a code point no
        // Latin range allows, and the field would refuse a value the server
        // accepts.
        const latin = all(
            boundedText({
                minCodePoints: 1,
                maxCodePoints: 8,
                allowLineBreaks: false,
                textClass: "identifier",
                reasons: kReasons,
            }),
            codePointClass(
                [
                    [0x61, 0x7a],
                    [0x00e0, 0x00ff],
                ],
                "BAD_FORMAT",
            ),
        );
        expect(latin(kDecomposed)).toMatchObject({ ok: true, value: kComposed });
    });
});
