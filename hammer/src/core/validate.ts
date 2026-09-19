// Client-side validation is a round-trip saver and never a control.
//
// The server validates the same input against the same bounds, because the
// server is the only participant an attacker does not own. What generating both
// from one descriptor buys is that the two AGREE — and agreement is not only
// about accept versus refuse. The checks below run in anvil's order
// (anvil `input/fields.cc`) so that the same input produces the same REASON: a
// field that says one thing before the request and a different thing after it is
// a field the user cannot act on.
//
// The reason type is a parameter throughout. The names belong to the descriptor
// (`docs/01-seams.md` §13), and a union spelled here would be a second copy of a
// table that is append-only on the server.
//
// **There is no regular-expression combinator.** A pattern from a descriptor is
// a pattern an application wrote, and a nested quantifier over user input is a
// frozen main thread on a keystroke — on the device least able to absorb it, and
// for input the user is still typing. Character classes are expressed as code
// point ranges and matched by a linear scan, which cannot backtrack because
// there is nothing to backtrack over.

import type { TextClass } from "./bidi.js";
import { checkBidi } from "./bidi.js";
import { foldDigits } from "./digits.js";
import type { Result } from "./result.js";
import { fail, ok } from "./result.js";
import { codePointLength, toNfc, withinCodePointBounds } from "./text.js";

// A check returns the value to use, not merely a verdict: normalisation and
// folding happen inside it, and the value that was checked is the value that
// must be sent. Returning a bare boolean is how a form ends up sending the
// unnormalised string it checked the normalised form of.
export type Check<T, Reason extends string> = (value: T) => Result<T, Reason>;

export type TextRules<Reason extends string> = {
    readonly minCodePoints: number;
    readonly maxCodePoints: number;
    readonly allowLineBreaks: boolean;
    readonly textClass: TextClass;
    readonly reasons: {
        readonly required: Reason;
        readonly tooShort: Reason;
        readonly tooLong: Reason;
        readonly badCharset: Reason;
    };
};

// C0 controls other than the ones that legitimately appear in prose, and DEL —
// invisible, survives every "printable" filter, and nothing types it on purpose.
function hasForbiddenControl(text: string, allowLineBreaks: boolean): boolean {
    for (let i = 0; i < text.length; i += 1) {
        const unit = text.charCodeAt(i);
        if (unit === 0x7f) {
            return true;
        }
        if (unit >= 0x20) {
            continue;
        }
        if (allowLineBreaks && (unit === 0x0a || unit === 0x09 || unit === 0x0d)) {
            continue;
        }
        return true;
    }
    return false;
}

// Normalises, then checks, then hands back the normalised value.
//
// The order of the checks is anvil's, and so is the order of the two length
// reasons: the bound check short-circuits past the maximum, so the count is only
// taken again when it is already known to be outside the range.
export function boundedText<Reason extends string>(rules: TextRules<Reason>): Check<string, Reason> {
    const { minCodePoints, maxCodePoints, allowLineBreaks, textClass, reasons } = rules;

    return (value: string): Result<string, Reason> => {
        // Before anything counts it: the count has to be of the value that will
        // be sent, or the client and the server are counting two strings.
        const text = toNfc(value);

        if (text.length === 0 && minCodePoints > 0) {
            return fail(reasons.required);
        }
        if (hasForbiddenControl(text, allowLineBreaks)) {
            return fail(reasons.badCharset);
        }
        if (!withinCodePointBounds(text, minCodePoints, maxCodePoints)) {
            return fail(
                codePointLength(text) < minCodePoints ? reasons.tooShort : reasons.tooLong,
            );
        }

        const bidi = checkBidi(text, textClass);
        if (!bidi.ok) {
            return fail(reasons.badCharset);
        }

        return ok(text);
    };
}

// Inclusive code point ranges, as [low, high] pairs. A linear scan over the
// input against a handful of ranges: no compilation, no backtracking, and a cost
// that is the length of the input times a constant a reader can see.
export type CodePointRange = readonly [number, number];

export function codePointClass<Reason extends string>(
    ranges: readonly CodePointRange[],
    reason: Reason,
): Check<string, Reason> {
    return (value: string): Result<string, Reason> => {
        for (const character of value) {
            const cp = character.codePointAt(0) ?? 0;
            let allowed = false;
            for (const range of ranges) {
                if (cp >= range[0] && cp <= range[1]) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                return fail(reason);
            }
        }
        return ok(value);
    };
}

// The set is built once, when the check is built, rather than per call: a
// membership test on a hot path is a hash lookup, and an array of names scanned
// per keystroke is the shape this library avoids everywhere else.
export function memberOf<Value extends string, Reason extends string>(
    values: readonly Value[],
    reason: Reason,
): Check<Value, Reason> {
    const allowed = new Set<string>(values);
    return (value: Value): Result<Value, Reason> => (allowed.has(value) ? ok(value) : fail(reason));
}

export type IntegerRules<Reason extends string> = {
    readonly min: number;
    readonly max: number;
    readonly reasons: {
        readonly badFormat: Reason;
        readonly outOfRange: Reason;
    };
};

// The longest int64 is twenty characters with its sign, so anything longer
// cannot be in range: the length is a refusal rather than a truncation.
const kMaxIntegerCharacters = 20;

// Folds first, for the same reason anvil does: an identifier typed on an Arabic
// keyboard is digits, and a validator that refuses them refuses that user for
// typing their own language.
export function parseIntegerIn<Reason extends string>(
    rules: IntegerRules<Reason>,
): (text: string) => Result<number, Reason> {
    const { min, max, reasons } = rules;

    return (text: string): Result<number, Reason> => {
        const digits = foldDigits(text);
        if (digits.length === 0 || digits.length > kMaxIntegerCharacters) {
            return fail(reasons.badFormat);
        }

        let index = 0;
        let negative = false;
        if (digits.charAt(0) === "-" || digits.charAt(0) === "+") {
            negative = digits.charAt(0) === "-";
            index = 1;
        }
        if (index === digits.length) {
            return fail(reasons.badFormat);
        }

        let magnitude = 0;
        for (; index < digits.length; index += 1) {
            const unit = digits.charCodeAt(index);
            if (unit < 0x30 || unit > 0x39) {
                // Trailing junk, never a silently truncated prefix: "12abc" is a
                // format error and not twelve.
                return fail(reasons.badFormat);
            }
            magnitude = magnitude * 10 + (unit - 0x30);
        }

        const value = negative ? -magnitude : magnitude;

        // A value the client cannot hold exactly is refused rather than rounded.
        // Rounding an id addresses a different row, and nothing downstream can
        // tell that it happened.
        if (!Number.isSafeInteger(value)) {
            return fail(reasons.outOfRange);
        }
        if (value < min || value > max) {
            return fail(reasons.outOfRange);
        }
        return ok(value);
    };
}

export function numberInRange<Reason extends string>(
    min: number,
    max: number,
    reason: Reason,
): Check<number, Reason> {
    return (value: number): Result<number, Reason> =>
        Number.isFinite(value) && value >= min && value <= max ? ok(value) : fail(reason);
}

// Short-circuits at the first failure, and threads the value through: each check
// sees what the one before it produced, so a normalisation in the first is
// visible to the bound in the second.
export function all<T, Reason extends string>(
    ...checks: readonly Check<T, Reason>[]
): Check<T, Reason> {
    return (value: T): Result<T, Reason> => {
        let current = value;
        for (const check of checks) {
            const result = check(current);
            if (!result.ok) {
                return result;
            }
            current = result.value;
        }
        return ok(current);
    };
}
