// The two directions are the whole file: what is folded on the way in, and what
// is shaped on the way out. A value that crosses in the wrong direction is a
// validation failure at the server with nothing in the browser to explain it.

import { describe, expect, it } from "../support/test.js";

import {
    DigitShaper,
    foldDigits,
    hasNonAsciiDigits,
    kArabicIndicZero,
    kExtendedArabicIndicZero,
} from "../../src/core/digits.js";

const kArabicIndic = "٢٩٨٠"; // U+0660..U+0669
const kExtended = "۲۹۸۰"; // U+06F0..U+06F9, Persian and Urdu
const kDevanagari = "२९८०"; // U+0966..U+096F, deliberately NOT folded

describe("the two ranges this module folds", () => {
    // Published, so an application can build its own input filter against the
    // same two ranges rather than a third one. Asserted against the code points
    // rather than against the strings above, because the strings are where a
    // typo would be invisible: the literals in this file and the constants in
    // that one are the same claim written twice.
    it("starts where Unicode says each block starts", () => {
        expect(kArabicIndicZero).toBe(0x0660);
        expect(kExtendedArabicIndicZero).toBe(0x06f0);
    });

    it("spans the ten digits the fixtures above are drawn from", () => {
        for (const [zero, sample] of [
            [kArabicIndicZero, kArabicIndic],
            [kExtendedArabicIndicZero, kExtended],
        ] as const) {
            for (const digit of sample) {
                const cp = digit.codePointAt(0) ?? 0;
                expect(cp).toBeGreaterThanOrEqual(zero);
                expect(cp).toBeLessThanOrEqual(zero + 9);
            }
        }
    });

    // The range that is deliberately NOT folded, stated against the constants
    // so that widening one of them fails here rather than in anvil.
    it("does not reach Devanagari, which anvil refuses as a format error", () => {
        const cp = (kDevanagari.codePointAt(0) ?? 0);
        expect(cp).toBeGreaterThan(kExtendedArabicIndicZero + 9);
    });
});

describe("folding on the way in", () => {
    it("folds both Arabic-Indic ranges to ASCII", () => {
        // A user typing an identifier on an Arabic keyboard produces these. A
        // validator that refuses them refuses that user for typing their own
        // language, and it is not a security control to do so.
        expect(foldDigits(kArabicIndic)).toBe("2980");
        expect(foldDigits(kExtended)).toBe("2980");
        expect(foldDigits(`+20 ${kArabicIndic}`)).toBe("+20 2980");
    });

    it("leaves every other script's digits alone", () => {
        // anvil folds exactly these two ranges and refuses the rest as a format
        // error. Folding a third here would accept a value the server then
        // refuses, which is the one failure the client cannot explain.
        expect(foldDigits(kDevanagari)).toBe(kDevanagari);
    });

    it("returns the input untouched when there is nothing to fold", () => {
        const ascii = "2980";
        expect(foldDigits(ascii)).toBe(ascii);
        expect(hasNonAsciiDigits(ascii)).toBe(false);
        expect(hasNonAsciiDigits(kArabicIndic)).toBe(true);
        expect(hasNonAsciiDigits(kDevanagari)).toBe(false);
    });

    it("does not disturb text around the digits", () => {
        expect(foldDigits(`محمد ${kArabicIndic}`)).toBe("محمد 2980");
        expect(foldDigits("\u{1F600}")).toBe("\u{1F600}");
        expect(foldDigits("")).toBe("");
    });
});

describe("shaping on the way out", () => {
    it("renders the locale's digits", () => {
        const shaper = DigitShaper.for("ar-EG");
        expect(shaper.ok).toBe(true);
        if (!shaper.ok) return;

        expect(shaper.value.isAscii()).toBe(false);
        expect(shaper.value.shape("2980")).toBe(kArabicIndic);
    });

    it("is the identity where the locale's digits are the wire's", () => {
        const shaper = DigitShaper.for("en");
        if (!shaper.ok) return;

        const text = "2980";
        expect(shaper.value.isAscii()).toBe(true);
        expect(shaper.value.shape(text)).toBe(text);
    });

    it("round-trips through the fold", () => {
        // The invariant that keeps a shaped value out of a comparison: what is
        // displayed folds back to exactly what was sent.
        const shaper = DigitShaper.for("ar-EG");
        if (!shaper.ok) return;

        for (const value of ["0", "2980", "1234567890"]) {
            expect(foldDigits(shaper.value.shape(value))).toBe(value);
        }
    });

    it("shapes only digits", () => {
        const shaper = DigitShaper.for("ar-EG");
        if (!shaper.ok) return;

        // Every ASCII digit in the string, including the ones inside what a
        // caller thinks of as one token: shaping is applied to whatever it is
        // handed, and deciding which digits are part of the number is the
        // caller's job, not a rule a library can guess.
        expect(shaper.value.shape("+20 2980 ext.")).toBe(`+\u0662\u0660 ${kArabicIndic} ext.`);
        expect(shaper.value.shape("\u{1F600}")).toBe("\u{1F600}");
    });

    it("reports a tag it cannot use rather than throwing out of a render", () => {
        // Every tag comes from the descriptor, so this is a generator failure
        // that reached run time. A throw here takes the screen with it; a
        // failure lets the caller render unshaped.
        expect(DigitShaper.for("not a tag")).toMatchObject({
            ok: false,
            error: "bad-locale-tag",
        });
    });
});
