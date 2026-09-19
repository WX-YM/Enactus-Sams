// Digits shape for display and never on the wire.
//
// A user typing a number on an Arabic keyboard produces U+0660..U+0669 or, for
// Persian and Urdu, U+06F0..U+06F9. Those characters are semantically digits and
// are not ASCII, so a numeric field that has not folded them refuses a value the
// user typed correctly in their own language — and refuses it silently, because
// nothing in the browser says which character was the problem.
//
// The two directions are not symmetrical, and conflating them is the defect this
// module exists to prevent:
//
//   IN   fold to ASCII before validating and before sending. anvil folds exactly
//        these two ranges (anvil `i18n/digits.h`) and rejects anything else as a
//        format error. Folding a third script here would accept a value the
//        server refuses, which is a form with no error on the field.
//   OUT  shape for display only, derived from the locale tag through `Intl`. A
//        shaped digit that goes back on the wire is a validation failure at the
//        server, and a shaped digit in a comparison is a value that matches
//        nothing.

import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

export const kArabicIndicZero = 0x0660;
export const kExtendedArabicIndicZero = 0x06f0;

const kAsciiZero = 0x30;
const kAsciiNine = 0x39;

export type DigitShaperError = "bad-locale-tag";

function foldedCodePoint(cp: number): number {
    if (cp >= kArabicIndicZero && cp <= kArabicIndicZero + 9) {
        return kAsciiZero + (cp - kArabicIndicZero);
    }
    if (cp >= kExtendedArabicIndicZero && cp <= kExtendedArabicIndicZero + 9) {
        return kAsciiZero + (cp - kExtendedArabicIndicZero);
    }
    return cp;
}

// So a caller can skip the allocation in the overwhelmingly common case where
// there is nothing to fold. Both ranges are in the basic plane, so a code-unit
// scan sees every one of them and never needs to decode a pair.
export function hasNonAsciiDigits(text: string): boolean {
    for (let i = 0; i < text.length; i += 1) {
        const unit = text.charCodeAt(i);
        if (
            (unit >= kArabicIndicZero && unit <= kArabicIndicZero + 9) ||
            (unit >= kExtendedArabicIndicZero && unit <= kExtendedArabicIndicZero + 9)
        ) {
            return true;
        }
    }
    return false;
}

// Unconditional, never gated on the locale: the keyboard and the locale the
// application is rendering in are not the same fact, and a user switching
// keyboards mid-field is ordinary. Everything that is not one of the two folded
// ranges is left exactly as it was, so a digit from a third script reaches the
// server and is refused there — by the same rule, with the same reason, as it
// would have been here.
export function foldDigits(text: string): string {
    if (!hasNonAsciiDigits(text)) {
        return text;
    }

    let out = "";
    for (let i = 0; i < text.length; i += 1) {
        out += String.fromCharCode(foldedCodePoint(text.charCodeAt(i)));
    }
    return out;
}

// Display shaping, owned by whatever is rendering.
//
// The ten digits are derived from the tag through `Intl` rather than from a
// table of numbering systems: a second table is a second thing to disagree with
// the one the platform already has, and the platform's is the one that gets
// updated. They are derived once, at construction, because a numbering system is
// not always a contiguous run of code points — an offset from its zero is right
// for the Arabic ranges and wrong for at least one system in CLDR, and a shaper
// that is wrong for one locale is wrong invisibly.
export class DigitShaper {
    private readonly digits: readonly string[];
    private readonly ascii: boolean;

    private constructor(digits: readonly string[], ascii: boolean) {
        this.digits = digits;
        this.ascii = ascii;
    }

    static for(localeTag: string): Result<DigitShaper, DigitShaperError> {
        let format: Intl.NumberFormat;
        try {
            format = new Intl.NumberFormat(localeTag, { useGrouping: false });
        } catch {
            // Every tag hammer sees comes from the descriptor, so this is a
            // generator failure that reached run time rather than a condition
            // the caller can recover from — but a throw out of a render path
            // takes the screen with it, and the caller can at least render
            // unshaped.
            return fail("bad-locale-tag");
        }

        const digits: string[] = [];
        let ascii = true;
        for (let digit = 0; digit <= 9; digit += 1) {
            const shaped = format.format(digit);
            digits.push(shaped);
            if (shaped !== String.fromCharCode(kAsciiZero + digit)) {
                ascii = false;
            }
        }
        return ok(new DigitShaper(digits, ascii));
    }

    // ASCII digits in, the locale's digits out. Input that is already shaped is
    // left alone rather than folded and re-shaped: shaping is the last thing
    // that happens to a value, after every comparison and before it is read.
    shape(text: string): string {
        if (this.ascii) {
            return text;
        }

        let out = "";
        for (let i = 0; i < text.length; i += 1) {
            const unit = text.charCodeAt(i);
            if (unit >= kAsciiZero && unit <= kAsciiNine) {
                out += this.digits[unit - kAsciiZero] ?? String.fromCharCode(unit);
            } else {
                out += text.charAt(i);
            }
        }
        return out;
    }

    // True where the locale's digits are the wire's digits, so a caller can skip
    // the pass entirely.
    isAscii(): boolean {
        return this.ascii;
    }
}
