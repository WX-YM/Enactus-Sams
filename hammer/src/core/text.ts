// Bounds in code points, and the reason is not pedantry.
//
// `String.length` counts UTF-16 code units: an emoji is two, every astral
// character is two, and a limit written against it silently halves the allowance
// for the scripts that need it most. anvil's bounds are code points
// (anvil `docs/03-i18n-utf8.md` §3), so the number here must be the identical
// number or the two disagree at the boundary — which a user experiences as a
// form that accepts what the server then refuses, with no error on the field.
//
// Two rules the shapes below exist to keep:
//
//   COUNT WHAT IS SENT. anvil counts the code points of the bytes it received,
//   without normalising first. So normalisation happens at the boundary, on the
//   way in, and the count is taken of the normalised value — otherwise a
//   decomposed "é" is two here and one there, or the reverse, and the limit that
//   was supposed to be the same number is two numbers.
//
//   A GRAPHEME IS A CARET CONCERN, NEVER A LIMIT. `Intl.Segmenter` answers "how
//   many characters did the user see"; code points answer "will the server take
//   this". Using the first as a bound is a bound the server does not share.

import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

export type TextError = "segmenter-unavailable" | "bad-locale-tag";

function isHighSurrogate(unit: number): boolean {
    return unit >= 0xd800 && unit <= 0xdbff;
}

function isLowSurrogate(unit: number): boolean {
    return unit >= 0xdc00 && unit <= 0xdfff;
}

// One pass, no allocation. `[...text].length` is the same answer through an
// array of one-character strings — on a long prose field that is one allocation
// per character, on every keystroke, on the device least able to afford it.
export function codePointLength(text: string): number {
    let count = 0;
    for (let i = 0; i < text.length; i += 1) {
        if (isHighSurrogate(text.charCodeAt(i)) && isLowSurrogate(text.charCodeAt(i + 1))) {
            i += 1;
        }
        count += 1;
    }
    return count;
}

// Short-circuits the moment the maximum is passed, which is the behaviour
// anvil's `within_code_point_bounds` has and for the same reason: a hostile ten
// megabyte value must not pay for a full count before being refused. Here the
// value is more often an accidental paste, and the cost lands on a keystroke.
export function withinCodePointBounds(
    text: string,
    minCodePoints: number,
    maxCodePoints: number,
): boolean {
    let count = 0;
    for (let i = 0; i < text.length; i += 1) {
        if (isHighSurrogate(text.charCodeAt(i)) && isLowSurrogate(text.charCodeAt(i + 1))) {
            i += 1;
        }
        count += 1;
        if (count > maxCodePoints) {
            return false;
        }
    }
    // The maximum is re-checked after the loop rather than only inside it: an
    // empty string never enters the loop, so a bound of zero or less would
    // otherwise be satisfied by the one value that can never be within it. A
    // nonsensical bound must fail closed, not open.
    return count >= minCodePoints && count <= maxCodePoints;
}

// NFC, and applied before comparing, hashing or sending.
//
// "é" has two encodings — one code point, or "e" plus a combining acute — and
// they are different strings that render identically. Two spellings of one value
// are two keys in every index on both sides, so a duplicate check stops
// detecting duplicates and a lookup stops finding what was stored.
export function toNfc(text: string): string {
    return text.normalize("NFC");
}

export function equalsNfc(left: string, right: string): boolean {
    return left === right || toNfc(left) === toNfc(right);
}

// Cuts at a code-point boundary and never between the halves of a surrogate
// pair. A split pair is not a shorter string: it is an unpaired surrogate, which
// the encoder turns into U+FFFD on the way out, so the user's character arrives
// at the server as a replacement character with nothing having reported an
// error.
export function truncateToCodePoints(text: string, maxCodePoints: number): string {
    if (maxCodePoints <= 0) {
        return "";
    }

    let count = 0;
    for (let i = 0; i < text.length; i += 1) {
        const wide = isHighSurrogate(text.charCodeAt(i)) && isLowSurrogate(text.charCodeAt(i + 1));
        count += 1;
        if (count > maxCodePoints) {
            return text.slice(0, i);
        }
        if (wide) {
            i += 1;
        }
    }
    return text;
}

// An unpaired surrogate cannot be encoded as UTF-8. `fetch` replaces it with
// U+FFFD rather than refusing it, so the value the server stores is not the
// value the client held and no layer between the two reports anything. It gets
// into a string by a slice at the wrong index, by a paste out of a broken
// clipboard, or by a `substring` bound written in code units.
export function hasLoneSurrogate(text: string): boolean {
    for (let i = 0; i < text.length; i += 1) {
        const unit = text.charCodeAt(i);
        if (isHighSurrogate(unit)) {
            if (!isLowSurrogate(text.charCodeAt(i + 1))) {
                return true;
            }
            i += 1;
        } else if (isLowSurrogate(unit)) {
            return true;
        }
    }
    return false;
}

// Graphemes, for the one thing they are for: where a caret may sit and what a
// backspace removes. A woman-woman-girl emoji is one thing the user sees, five
// code points and eight code units, and all three numbers are correct answers to
// different questions. It is described here rather than written: a source file
// carrying invisible joiners is a source file nobody can review.
//
// It is an owned object rather than a module-level cache keyed by locale,
// because a cache at module scope is a global mutable singleton no test can
// replace (CLAUDE.md §3.3) — and because constructing an `Intl.Segmenter` is
// expensive enough that the thing rendering a field should hold one, not create
// one per keystroke.
export class GraphemeMeter {
    private readonly segmenter: Intl.Segmenter;

    private constructor(segmenter: Intl.Segmenter) {
        this.segmenter = segmenter;
    }

    // A failure rather than a silent fall back to code points: a caret that
    // moves by code points through an emoji sequence deletes half of it, and
    // "half of it" is a different emoji. The caller decides what to do without
    // segmentation; the library does not decide for it by lying.
    static for(localeTag: string): Result<GraphemeMeter, TextError> {
        if (typeof Intl.Segmenter !== "function") {
            return fail("segmenter-unavailable");
        }
        let segmenter: Intl.Segmenter;
        try {
            segmenter = new Intl.Segmenter(localeTag, { granularity: "grapheme" });
        } catch {
            return fail("bad-locale-tag");
        }
        return ok(new GraphemeMeter(segmenter));
    }

    // **Not a limit.** The server counts code points, so a field bound checked
    // against this number is a bound the server does not share — and the two
    // disagree exactly on the input where it matters, which is every script with
    // combining marks.
    graphemeLength(text: string): number {
        let count = 0;
        for (const _ of this.segmenter.segment(text)) {
            count += 1;
        }
        return count;
    }

    // Every index a caret may occupy, in code units, so a renderer can map a
    // selection the platform gave it in code units onto whole characters.
    caretBoundaries(text: string): readonly number[] {
        const boundaries: number[] = [];
        for (const segment of this.segmenter.segment(text)) {
            boundaries.push(segment.index);
        }
        boundaries.push(text.length);
        return boundaries;
    }
}
