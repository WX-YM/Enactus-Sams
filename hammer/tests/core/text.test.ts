// The bound in this file is the same number anvil uses, and the tests exist to
// keep it the same number.
//
// Every assertion below has the same failure on the other side of it: a form
// that accepts what the server refuses, or refuses what it would have taken,
// with no error rendered on the field because the client believed it had
// already checked.

import { describe, expect, it } from "../support/test.js";

import {
    GraphemeMeter,
    codePointLength,
    equalsNfc,
    hasLoneSurrogate,
    toNfc,
    truncateToCodePoints,
    withinCodePointBounds,
} from "../../src/core/text.js";
import { Prng } from "../support/prng.js";

// One character each from the cases a bound written in code units gets wrong.
// Written as escapes rather than as literals: a decomposed sequence in a source
// file is one editor away from being normalised, and the test that proves the
// two forms differ would then be comparing a string with itself.
const kAstral: string = "\u{1F600}"; // one code point, two code units
const kCombining: string = "e\u0301"; // "e" + combining acute: two code points
const kComposed: string = "\u00E9"; // the same glyph as one code point
const kFamily: string = "\u{1F469}\u200D\u{1F469}\u200D\u{1F467}"; // one grapheme, five code points

describe("code-point counting", () => {
    it("counts an astral character once", () => {
        // `.length` says two. A 20-character bound written against it accepts
        // ten emoji and no error says why.
        expect(kAstral.length).toBe(2);
        expect(codePointLength(kAstral)).toBe(1);
    });

    it("counts a combining sequence as its code points", () => {
        expect(codePointLength(kCombining)).toBe(2);
        expect(codePointLength(kComposed)).toBe(1);
        expect(codePointLength(kFamily)).toBe(5);
    });

    it("counts an empty string as nothing", () => {
        expect(codePointLength("")).toBe(0);
    });

    it("agrees with a bound check that short-circuits", () => {
        const text = `${kAstral}${kAstral}${kAstral}`;
        expect(withinCodePointBounds(text, 0, 3)).toBe(true);
        expect(withinCodePointBounds(text, 0, 2)).toBe(false);
        expect(withinCodePointBounds(text, 4, 10)).toBe(false);
        expect(withinCodePointBounds("", 1, 10)).toBe(false);
        expect(withinCodePointBounds("", 0, 10)).toBe(true);
    });
});

describe("truncation", () => {
    it("never splits a surrogate pair", () => {
        // `slice(0, 3)` on two emoji returns one emoji and half of another. The
        // half is a lone surrogate, which the encoder replaces with U+FFFD on
        // the way out — so the server stores a character the user never typed.
        const pair = `${kAstral}${kAstral}`;
        expect(pair.slice(0, 3).length).toBe(3);
        expect(hasLoneSurrogate(pair.slice(0, 3))).toBe(true);

        const cut = truncateToCodePoints(pair, 1);
        expect(cut).toBe(kAstral);
        expect(hasLoneSurrogate(cut)).toBe(false);
    });

    it("returns the input when it already fits", () => {
        const text = "abc";
        expect(truncateToCodePoints(text, 3)).toBe(text);
        expect(truncateToCodePoints(text, 99)).toBe(text);
    });

    it("cuts at zero", () => {
        expect(truncateToCodePoints(kAstral, 0)).toBe("");
        expect(truncateToCodePoints(kAstral, -1)).toBe("");
    });

    it("cuts by code points, not by glyphs", () => {
        // Documented rather than incidental: truncation is a limit concern, and
        // a limit is code points. A caller that wants whole glyphs measures with
        // a GraphemeMeter first and decides for itself.
        expect(truncateToCodePoints(kCombining, 1)).toBe("e");
    });
});

describe("normalisation", () => {
    it("makes two spellings of one glyph compare equal", () => {
        expect(kCombining === kComposed).toBe(false);
        expect(equalsNfc(kCombining, kComposed)).toBe(true);
        expect(toNfc(kCombining)).toBe(kComposed);
    });

    it("changes the count, which is why it happens before the count", () => {
        // The whole reason normalisation is at the boundary: anvil counts the
        // code points of what it received. Normalising after the check means the
        // number checked is not the number sent.
        expect(codePointLength(kCombining)).toBe(2);
        expect(codePointLength(toNfc(kCombining))).toBe(1);
    });
});

describe("graphemes are a caret concern", () => {
    it("counts what the user sees, which is not what the server counts", () => {
        const meter = GraphemeMeter.for("en");
        expect(meter.ok).toBe(true);
        if (!meter.ok) return;

        expect(meter.value.graphemeLength(kFamily)).toBe(1);
        expect(codePointLength(kFamily)).toBe(5);
        expect(kFamily.length).toBe(8);
    });

    it("reports where a caret may sit", () => {
        const meter = GraphemeMeter.for("en");
        if (!meter.ok) return;

        // Code-unit indices, because that is the currency a selection API deals
        // in: after "a", after the whole family, and the end.
        expect(meter.value.caretBoundaries(`a${kFamily}`)).toEqual([0, 1, 9]);
    });

    it("reports the absence of a segmenter rather than falling back", () => {
        // A fall back to code points moves a caret into the middle of an emoji
        // sequence, and a backspace there deletes a character the user never
        // typed. The caller is told; it is not told a number that is wrong.
        const original = Intl.Segmenter;
        try {
            Reflect.deleteProperty(Intl, "Segmenter");
            expect(GraphemeMeter.for("en")).toMatchObject({
                ok: false,
                error: "segmenter-unavailable",
            });
        } finally {
            Object.defineProperty(Intl, "Segmenter", {
                value: original,
                configurable: true,
                writable: true,
            });
        }
    });
});

// --- properties -------------------------------------------------------------
//
// The cases above are the ones somebody thought of. These are the ones nobody
// did: a thousand strings drawn from the alphabets where a code-unit bound and a
// code-point bound disagree, against a reference implementation that is correct
// and too slow to ship.

const kAlphabet: readonly string[] = [
    "a",
    "Z",
    "0",
    " ",
    "\u00E9",
    "e\u0301",
    "\u0627",
    "\u0628",
    "\u0663", // an Arabic-Indic digit
    "\u4E2D",
    "\uD55C",
    "\u{1F600}",
    "\u{1F469}\u200D\u{1F467}",
    "\u{1D11E}", // an astral non-emoji
    "\u0640", // tatweel: a joiner that is its own code point
];

function randomText(prng: Prng, maxParts: number): string {
    const parts = prng.below(maxParts);
    let out = "";
    for (let i = 0; i < parts; i += 1) {
        out += prng.pick(kAlphabet);
    }
    return out;
}

describe("code-point properties", () => {
    it("counts what the reference implementation counts", () => {
        for (let seed = 1; seed <= 200; seed += 1) {
            const prng = new Prng(seed);
            const text = randomText(prng, 24);
            // The reference allocates an array of one-character strings, which
            // is exactly what the shipped implementation avoids.
            expect(codePointLength(text), `seed ${seed}`).toBe([...text].length);
        }
    });

    it("agrees with its own short-circuiting bound check", () => {
        for (let seed = 1; seed <= 200; seed += 1) {
            const prng = new Prng(seed);
            const text = randomText(prng, 24);
            const count = codePointLength(text);
            for (const max of [0, 1, count - 1, count, count + 1]) {
                expect(withinCodePointBounds(text, 0, max), `seed ${seed} max ${max}`).toBe(
                    count <= max,
                );
            }
        }
    });

    it("truncates to a prefix of the right length and never to a broken one", () => {
        for (let seed = 1; seed <= 200; seed += 1) {
            const prng = new Prng(seed);
            const text = randomText(prng, 24);
            const count = codePointLength(text);
            const limit = prng.below(count + 2);
            const cut = truncateToCodePoints(text, limit);

            expect(text.startsWith(cut), `seed ${seed}`).toBe(true);
            expect(codePointLength(cut), `seed ${seed}`).toBe(Math.min(limit, count));
            expect(hasLoneSurrogate(cut), `seed ${seed}`).toBe(false);
        }
    });

    it("normalises idempotently", () => {
        for (let seed = 1; seed <= 200; seed += 1) {
            const prng = new Prng(seed);
            const text = randomText(prng, 24);
            const once = toNfc(text);
            expect(toNfc(once), `seed ${seed}`).toBe(once);
            expect(equalsNfc(text, once), `seed ${seed}`).toBe(true);
        }
    });
});
