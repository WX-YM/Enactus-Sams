// Every string here is written with escapes. A test for invisible characters
// that contains invisible characters is a test whose input nobody can read, and
// a reviewer who cannot see the input cannot see the bug.

import { describe, expect, it } from "vitest";

import {
    checkBidi,
    detectDirection,
    isolate,
    kArabicLetterMark,
    kFirstStrongIsolate,
    kLeftToRightIsolate,
    kLeftToRightMark,
    kPopDirectionalIsolate,
    kRightToLeftIsolate,
    kRightToLeftMark,
    stripBidiControls,
} from "../../src/core/bidi.js";

const kRlo = "\u202E"; // right-to-left override
const kLre = "\u202A"; // left-to-right embedding
const kPdi = "\u2069"; // pop directional isolate
const kFsi = "\u2068";
const kZwj = "\u200D";
const kRlm = "\u200F";
const kArabicName = "محمد"; // four Arabic letters
const kHebrewName = "דוד";

// Every control this module exports, as one table, so a character added to the
// module and not to this line fails a case rather than passing silently.
const kControls = [
    kLeftToRightIsolate,
    kRightToLeftIsolate,
    kFirstStrongIsolate,
    kPopDirectionalIsolate,
    kLeftToRightMark,
    kRightToLeftMark,
    kArabicLetterMark,
] as const;

// The seven control characters this module publishes, and the reason they are
// worth a suite of their own.
//
// They are PUBLISHED and used nowhere in the library: `isolate()` reaches for
// FSI and PDI, and the other five exist because an application has positions an
// `isolate()` cannot reach — a `title`, a label built from parts, a value beside
// a punctuation mark that would otherwise take the wrong side (`ENGINEERING_RULES.md` §8).
// So nothing here would notice one of them being the wrong code point, and the
// first thing that would is an Arabic name reordering a sentence in production.
//
// The second half is the one that actually goes wrong. The constants and the
// predicates that POLICE them are two independent copies of the same code
// points — the constants are escapes, `isMark` and `isIsolate` are numeric
// ranges — and this module's own header says what a disagreement costs: anvil
// polices the same two classes with the same code points, so a client that
// policed a different set would refuse what the server accepts or accept what it
// refuses. Nothing tied the two copies together until this file did.
describe("the control characters this module publishes", () => {
    // Spelled as code points rather than compared against another escape of the
    // same character: an assertion written `expect(kRightToLeftMark).toBe(
    // "\u200F")` is two copies of one typo, and it passes.
    it("is each the character it is named for", () => {
        expect(kLeftToRightIsolate.codePointAt(0)).toBe(0x2066);
        expect(kRightToLeftIsolate.codePointAt(0)).toBe(0x2067);
        expect(kFirstStrongIsolate.codePointAt(0)).toBe(0x2068);
        expect(kPopDirectionalIsolate.codePointAt(0)).toBe(0x2069);
        expect(kLeftToRightMark.codePointAt(0)).toBe(0x200e);
        expect(kRightToLeftMark.codePointAt(0)).toBe(0x200f);
        expect(kArabicLetterMark.codePointAt(0)).toBe(0x061c);
    });

    it("is each one code point, so a bound in code points counts it as one", () => {
        for (const control of kControls) {
            expect([...control]).toHaveLength(1);
        }
    });

    // The tie between the two copies. Every character this module hands an
    // application is a character it also refuses in an identifier — which is
    // what stops a published constant drifting out of the set the validator
    // knows about.
    it("is refused in an identifier, every one of them", () => {
        for (const control of kControls) {
            expect(checkBidi(`ali${control}ce`, "identifier").ok).toBe(false);
        }
    });

    it("is stripped by the strip, every one of them", () => {
        for (const control of kControls) {
            expect(stripBidiControls(`ali${control}ce`)).toBe("alice");
        }
    });

    // The one that would bite hardest. An unbalanced PDI inside an interpolated
    // value ends the isolate the sentence opened, and everything after it — the
    // application's own words — is then inside the user's directional context.
    it("cannot end an isolate from inside the value it isolates", () => {
        for (const control of kControls) {
            const wrapped = isolate(`ali${control}ce`);
            expect(wrapped).toBe(kFirstStrongIsolate + "alice" + kPopDirectionalIsolate);
            expect([...wrapped].filter((c) => c === kPopDirectionalIsolate)).toHaveLength(1);
        }
    });
});

describe("direction detection", () => {
    it("takes the direction of the first letter", () => {
        expect(detectDirection("Ahmed")).toBe("ltr");
        expect(detectDirection(kArabicName)).toBe("rtl");
        expect(detectDirection(kHebrewName)).toBe("rtl");
        expect(detectDirection("\u{1E900}")).toBe("rtl"); // Adlam, an astral RTL script
    });

    it("skips digits and punctuation, which have no direction of their own", () => {
        // A phone number is not a left-to-right sentence. Deciding from the
        // first character rather than the first letter renders an Arabic
        // contact's number in the wrong order.
        expect(detectDirection(`+1 (555) 0100 ${kArabicName}`)).toBe("rtl");
        expect(detectDirection("123 Ahmed")).toBe("ltr");
    });

    it("reports neutral rather than guessing", () => {
        // The caller must not turn this into "ltr": a bare number inherits the
        // direction of its surroundings, and forcing one is how it ends up
        // rendered backwards in an Arabic form.
        expect(detectDirection("")).toBe("neutral");
        expect(detectDirection("12:45")).toBe("neutral");
        expect(detectDirection("٠١")).toBe("neutral"); // Arabic-Indic digits
    });

    it("treats CJK as left-to-right", () => {
        expect(detectDirection("中文")).toBe("ltr");
    });
});

describe("isolation", () => {
    it("wraps interpolated text", () => {
        expect(isolate(kArabicName)).toBe(`${kFsi}${kArabicName}${kPdi}`);
        expect(kFirstStrongIsolate).toBe(kFsi);
        expect(kPopDirectionalIsolate).toBe(kPdi);
    });

    it("strips a control that would end the isolate early", () => {
        // This is the case that makes the strip load-bearing rather than
        // decorative: an unbalanced PDI inside the value closes the isolate the
        // library opened, and the rest of the application's sentence renders
        // inside the user's directional context.
        const hostile = `${kArabicName}${kPdi}`;
        const wrapped = isolate(hostile);
        expect(wrapped).toBe(`${kFsi}${kArabicName}${kPdi}`);
        expect(wrapped.indexOf(kPdi)).toBe(wrapped.length - 1);
    });

    it("strips an override, which reorders everything after it", () => {
        const spoofed = `exe.${kRlo}gnp.evil`;
        expect(stripBidiControls(spoofed)).toBe("exe.gnp.evil");
        expect(isolate(spoofed)).toBe(`${kFsi}exe.gnp.evil${kPdi}`);
    });

    it("leaves ordinary text alone", () => {
        expect(stripBidiControls("Ahmed")).toBe("Ahmed");
        expect(stripBidiControls("")).toBe("");
    });
});

describe("the two text classes", () => {
    it("refuses an override in both", () => {
        // Nothing legitimate forces the direction of text that already has one.
        expect(checkBidi(`a${kRlo}b`, "prose")).toMatchObject({ ok: false, error: "override" });
        expect(checkBidi(`a${kRlo}b`, "identifier")).toMatchObject({
            ok: false,
            error: "override",
        });
        expect(checkBidi(`a${kLre}b`, "prose")).toMatchObject({ ok: false, error: "override" });
    });

    it("allows an isolate in prose and refuses one in an identifier", () => {
        // A mixed English/Arabic sentence genuinely needs isolates. A username
        // does not, and two usernames that render identically and compare
        // unequal is the whole attack.
        expect(checkBidi(`${kFsi}${kArabicName}${kPdi} replied`, "prose").ok).toBe(true);
        expect(checkBidi(`${kFsi}admin${kPdi}`, "identifier")).toMatchObject({
            ok: false,
            error: "override",
        });
    });

    it("allows a joiner in prose and refuses one in an identifier", () => {
        // ZWJ is orthographically meaningful in Persian and in emoji sequences;
        // in an identifier it is an invisible character that forks one value
        // into two.
        expect(checkBidi(`a${kZwj}b`, "prose").ok).toBe(true);
        expect(checkBidi(`a${kZwj}b`, "identifier")).toMatchObject({
            ok: false,
            error: "zero-width",
        });
    });

    it("allows a directional mark in prose and refuses one in an identifier", () => {
        expect(checkBidi(`a${kRlm}b`, "prose").ok).toBe(true);
        expect(checkBidi(`a${kRlm}b`, "identifier")).toMatchObject({ ok: false, error: "mark" });
        expect(checkBidi("\u061C", "identifier")).toMatchObject({ ok: false, error: "mark" });
    });

    it("accepts text with no controls in it at all", () => {
        expect(checkBidi(kArabicName, "identifier").ok).toBe(true);
        expect(checkBidi("\u{1F600}", "identifier").ok).toBe(true);
        expect(checkBidi("", "identifier").ok).toBe(true);
    });
});
