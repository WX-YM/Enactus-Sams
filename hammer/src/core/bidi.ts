// Direction is a property of the text, and getting it wrong is not cosmetic.
//
// An RTL name interpolated into a Latin sentence reorders the sentence around
// it: the words move, and the result is not a slightly ugly line — it is a
// different sentence, in which a trailing "deleted" can arrive before the name
// it was about. `dir="auto"` on the element holding the text is the first
// answer, and these are for the places an attribute cannot reach: a `title`, a
// label built from parts, anything that is one text node by the time it renders.
//
// The second half is the control characters. U+202E reverses everything after
// it, so `exe.<U+202E>gnp.evil` renders as `evil.png` and remains an
// executable — the same trick spoofs a display name, a note title and a row in
// a moderation queue, where the value approved and the value seen are not the
// same value. anvil polices them on the way in with the same two classes and the
// same code points (anvil `i18n/bidi.h`); a client that policed a different set
// would refuse what the server accepts, or accept what it refuses.

import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

// The isolates. FSI is the one to reach for by default: it asks the renderer to
// derive the direction from the content, so a name whose direction the
// application does not know still isolates correctly.
// Written as escapes, never as the characters themselves: a source file
// containing an invisible control is a source file nobody can review, which is
// the defect these exist to prevent.
export const kFirstStrongIsolate = "\u2068";
export const kPopDirectionalIsolate = "\u2069";
export const kLeftToRightIsolate = "\u2066";
export const kRightToLeftIsolate = "\u2067";

// The marks, for a position where a whole isolate cannot go — between a value
// and a punctuation mark that would otherwise take the wrong side.
export const kLeftToRightMark = "\u200E";
export const kRightToLeftMark = "\u200F";
export const kArabicLetterMark = "\u061C";

export type TextDirection = "ltr" | "rtl" | "neutral";

// The two classes anvil polices by, because the answer genuinely differs:
//
//   identifier  a filename, a username, a slug, a key. Compared, indexed, and
//               displayed in lists where a spoofed value is indistinguishable
//               from a real one. No control of any kind, including the
//               zero-width joiners, which produce two values that render
//               identically and compare unequal.
//   prose       a note body, a label, section copy. Isolates are genuinely
//               required to render a mixed English/Arabic sentence, and the
//               joiners are orthographically meaningful in Persian. Overrides
//               are not, in either class: nothing legitimate needs to force the
//               direction of text that already has one.
export type TextClass = "identifier" | "prose";

export type BidiIssue = "override" | "mark" | "zero-width";

function isZeroWidth(cp: number): boolean {
    return (cp >= 0x200b && cp <= 0x200d) || cp === 0xfeff;
}

function isMark(cp: number): boolean {
    return cp === 0x200e || cp === 0x200f || cp === 0x061c;
}

// LRE, RLE, PDF, LRO, RLO.
function isEmbeddingOrOverride(cp: number): boolean {
    return cp >= 0x202a && cp <= 0x202e;
}

// LRI, RLI, FSI, PDI.
function isIsolate(cp: number): boolean {
    return cp >= 0x2066 && cp <= 0x2069;
}

export function checkBidi(text: string, textClass: TextClass): Result<void, BidiIssue> {
    const identifier = textClass === "identifier";

    for (const character of text) {
        const cp = character.codePointAt(0) ?? 0;

        if (isEmbeddingOrOverride(cp)) {
            return fail("override");
        }
        if (isIsolate(cp)) {
            if (identifier) {
                return fail("override");
            }
            continue;
        }
        if (isMark(cp) && identifier) {
            return fail("mark");
        }
        if (isZeroWidth(cp) && identifier) {
            return fail("zero-width");
        }
    }

    return ok();
}

// Removes every bidi control and zero-width character. Used on the way into an
// isolate rather than as a validator: a value that contains one of these is
// refused by `checkBidi`, and this is for text already accepted as prose that is
// about to be concatenated into a sentence it must not reorder.
export function stripBidiControls(text: string): string {
    let out = "";
    for (const character of text) {
        const cp = character.codePointAt(0) ?? 0;
        if (isEmbeddingOrOverride(cp) || isIsolate(cp) || isMark(cp) || isZeroWidth(cp)) {
            continue;
        }
        out += character;
    }
    return out;
}

// Wraps interpolated text so it cannot reorder the sentence around it.
//
// The strip is not belt and braces. An unbalanced PDI inside the value closes
// the isolate this function opened, and everything after it — the rest of the
// application's sentence — is then inside the user's directional context. The
// isolate is only an isolate if what it contains cannot end it.
export function isolate(text: string): string {
    return kFirstStrongIsolate + stripBidiControls(text) + kPopDirectionalIsolate;
}

// The strong right-to-left blocks: Hebrew through Arabic Extended-A, the two
// presentation-form ranges, and the historic and Adlam planes. Consulted only
// for a character already known to be a letter, so the ranges do not have to
// exclude the marks and signs inside them.
function isRightToLeftLetter(cp: number): boolean {
    return (
        (cp >= 0x0590 && cp <= 0x08ff) ||
        (cp >= 0xfb1d && cp <= 0xfdff) ||
        (cp >= 0xfe70 && cp <= 0xfeff) ||
        (cp >= 0x10800 && cp <= 0x10fff) ||
        (cp >= 0x1e800 && cp <= 0x1efff)
    );
}

// A single character class with no quantifier, so there is nothing for the
// engine to backtrack over: it is one linear scan for the first letter, which is
// what the bidi algorithm's first rule asks for. Digits and punctuation are
// skipped because they take their direction from what surrounds them — a phone
// number is not a left-to-right sentence.
const kFirstLetter = /\p{L}/u;

// What to put in `dir`. "neutral" is the honest answer for a string with no
// letters in it at all, and it is the one a caller must not silently turn into
// "ltr": a bare number inherits the direction of its surroundings, and forcing
// it is how a phone number ends up rendered backwards in an Arabic form.
export function detectDirection(text: string): TextDirection {
    const strong = kFirstLetter.exec(text);
    if (strong === null) {
        return "neutral";
    }
    const cp = strong[0].codePointAt(0) ?? 0;
    return isRightToLeftLetter(cp) ? "rtl" : "ltr";
}
