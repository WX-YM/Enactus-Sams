// The `hammer` entry point: the layer that imports nothing, needs no document
// and no network, and is therefore the one that stays testable with no
// environment at all (docs/00-architecture.md §2).
//
// Exports are named and explicit. A star re-export would put every module in
// every bundle that touches any of them, because a bundler cannot drop what it
// was never told is side-effect free at the granularity it needs.

export type { Brand } from "./brand.js";
export type { Ok, Fail, Result } from "./result.js";
export { ok, fail, isOk, isFail, expect } from "./result.js";
export type {
    ClientError,
    HammerError,
    HammerResult,
    ServerError,
    StaleClientError,
    TransportError,
} from "./errors.js";
export { isRetryableKind, isServerError } from "./errors.js";
export type { PermSetDecodeError } from "./perm_set.js";
export { PermSet, kPermBits, kPermBytes } from "./perm_set.js";
export type { UuidParseError } from "./uuid.js";
export { Uuid, kUuidBytes } from "./uuid.js";
export type { TextError } from "./text.js";
export {
    GraphemeMeter,
    codePointLength,
    equalsNfc,
    hasLoneSurrogate,
    toNfc,
    truncateToCodePoints,
    withinCodePointBounds,
} from "./text.js";
export type { BidiIssue, TextClass, TextDirection } from "./bidi.js";
export {
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
} from "./bidi.js";
export type { DigitShaperError } from "./digits.js";
export {
    DigitShaper,
    foldDigits,
    hasNonAsciiDigits,
    kArabicIndicZero,
    kExtendedArabicIndicZero,
} from "./digits.js";
export type { UploadFile, UploadLimits, UploadRefusal, UploadRefused } from "./upload_bounds.js";
export { checkUpload } from "./upload_bounds.js";
export type { Check, CodePointRange, IntegerRules, TextRules } from "./validate.js";
export {
    all,
    boundedText,
    codePointClass,
    memberOf,
    numberInRange,
    parseIntegerIn,
} from "./validate.js";
// `cursorFromServer` is deliberately absent, the way `brand` is: a consumer that
// can mint a cursor can mint one no server issued, and the type stops being
// evidence of anything.
export type { Cursor, PageRequest, PageRequestError } from "./cursor.js";
export { pageRequest } from "./cursor.js";
export type {
    ClassNames,
    Copy,
    FailureCopy,
    Invalidations,
} from "./tables.js";
export type { TimeParseError } from "./time.js";
export { Countdown, ServerInstant, kHourMs, kMinuteMs, kSecondMs } from "./time.js";
