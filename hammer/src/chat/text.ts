// The composer's validator: anvil's `chat/text` rules, restated once.
//
// It is used in two places, and the second is the reason it has to be exact:
//
//   BEFORE A SEND, to save the round trip. A person learns that a message is too
//   long, or holds a character the server refuses, while typing it.
//
//   AFTER A DECRYPT, as the only enforcement there is. Inside an encrypted
//   conversation anvil validates nothing, because it cannot read anything, so a
//   message a modified client wrote with a U+202E in it is refused here exactly
//   as the server would have refused it in plaintext (`docs/05-chat.md` §4).
//
// So these are anvil's rules and not a client's idea of good text. Every check
// runs in the order anvil's runs, so a value with two faults is refused with the
// same reason on both sides, and every reason is the server's own wire name: a
// screen places the server's refusal where it placed its own.
//
// --- refused, never repaired -------------------------------------------------
//
// The two choices anvil makes that are easiest to undo by accident:
//
//   NFC is REQUIRED, not applied. Mention spans are code-point positions in the
//   text, and normalising can change how many code points a text has (U+0958
//   decomposes under NFC). So the composer normalises BEFORE it places a
//   mention, and this refuses a text that is not already NFC rather than
//   quietly moving every span after the change onto the wrong name.
//
//   `\n` is the only line break. `\r`, CRLF and U+2028/U+2029 are refused: each
//   is another code point the bound counts differently per platform, and in a
//   log viewer a bare `\r` overwrites the line it sits on.
//
// The bounds are parameters. Each comes from the generated `kChatLimits`, which
// is the server's own table, so there is no second copy of a number here.

import { checkBidi } from "../core/bidi.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import { codePointLength, hasLoneSurrogate, toNfc } from "../core/text.js";
import type { Uuid } from "../core/uuid.js";

// anvil's validation reasons, as anvil writes them in a VALIDATION_FAILED body.
// The subset these rules can produce.
export type ChatTextReason =
    | "REQUIRED"
    | "TOO_LONG"
    | "BAD_CHARSET"
    | "BAD_FORMAT"
    | "OUT_OF_RANGE"
    | "NOT_ALLOWED";

// --- code points ---------------------------------------------------------------

// The UTF-8 length of a string with no lone surrogate, without encoding it.
// anvil bounds some values in bytes before it decodes them, and a refusal has to
// fire at the same point here for the reasons to agree.
function utf8Length(text: string): number {
    let bytes = 0;
    for (let i = 0; i < text.length; i += 1) {
        const unit = text.charCodeAt(i);
        if (unit < 0x80) {
            bytes += 1;
        } else if (unit < 0x800) {
            bytes += 2;
        } else if (unit >= 0xd800 && unit <= 0xdbff) {
            // A pair: four bytes for the two units together.
            bytes += 4;
            i += 1;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

// C0, DEL and C1. C1 by code point: U+0085 is a line break to some renderers and
// invisible to others.
function isControl(cp: number): boolean {
    return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f);
}

function isLineOrParagraphSeparator(cp: number): boolean {
    return cp === 0x2028 || cp === 0x2029;
}

// anvil's `i18n::validate` beyond structure: NUL terminates a C string on the
// other side, and the non-characters have no interchange meaning. Exactly these
// and no others, because that is the set anvil refuses.
function isNonCharacter(cp: number): boolean {
    return cp === 0xfffe || cp === 0xffff || (cp >= 0xfdd0 && cp <= 0xfdef);
}

// "Renders as nothing": White_Space and Default_Ignorable_Code_Point, which
// covers the zero-width characters, the bidi marks, the variation selectors and
// the Hangul fillers — the usual way to post a message that looks empty past a
// naive whitespace check. ASCII is answered without the regex.
const kBlank = /^[\p{White_Space}\p{Default_Ignorable_Code_Point}]$/u;

function isBlank(character: string, cp: number): boolean {
    if (cp < 0x80) {
        return cp === 0x20 || cp === 0x09 || cp === 0x0a;
    }
    return kBlank.test(character);
}

// A string anvil would accept as UTF-8 at all. A JavaScript string cannot hold
// an overlong or out-of-range sequence, so what is left is a lone surrogate —
// which `fetch` would silently replace with U+FFFD — NUL and the non-characters.
function isEncodable(text: string): boolean {
    if (hasLoneSurrogate(text)) {
        return false;
    }
    for (const character of text) {
        const cp = character.codePointAt(0) ?? 0;
        if (cp === 0 || isNonCharacter(cp)) {
            return false;
        }
    }
    return true;
}

// A mention's span is in code points and the DOM's selection is in UTF-16 code
// units. This is the one conversion between the two, in each direction, so a
// span is never computed in one unit and applied in the other.
export function codePointToUtf16Index(text: string, codePoints: number): number {
    let index = 0;
    for (let seen = 0; seen < codePoints && index < text.length; seen += 1) {
        const unit = text.charCodeAt(index);
        index += unit >= 0xd800 && unit <= 0xdbff && index + 1 < text.length ? 2 : 1;
    }
    return index;
}

export function utf16IndexToCodePoint(text: string, index: number): number {
    return codePointLength(text.slice(0, index));
}

// --- prose -----------------------------------------------------------------------

// The character rules every free-text part of a message shares, in anvil's
// order. `text` is non-empty.
function checkProseRules(
    text: string,
    maxCodePoints: number,
    allowLineBreaks: boolean,
): Result<void, ChatTextReason> {
    // A code point is at most four bytes, so anything longer is over the bound
    // without decoding it.
    if (utf8Length(text) > maxCodePoints * 4) {
        return fail("TOO_LONG");
    }
    if (!isEncodable(text)) {
        return fail("BAD_CHARSET");
    }
    if (codePointLength(text) > maxCodePoints) {
        return fail("TOO_LONG");
    }

    let visible = false;
    for (const character of text) {
        const cp = character.codePointAt(0) ?? 0;
        if (isControl(cp) && !(allowLineBreaks && (cp === 0x0a || cp === 0x09))) {
            return fail("BAD_CHARSET");
        }
        if (isLineOrParagraphSeparator(cp)) {
            return fail("BAD_CHARSET");
        }
        if (!visible && !isBlank(character, cp)) {
            visible = true;
        }
    }
    if (!visible) {
        return fail("REQUIRED");
    }

    // The house prose policy, not a second one: overrides and embeddings are
    // refused, isolates are allowed because a mixed Arabic and English sentence
    // needs them.
    if (!checkBidi(text, "prose").ok) {
        return fail("BAD_CHARSET");
    }
    if (toNfc(text) !== text) {
        return fail("BAD_FORMAT");
    }
    return ok();
}

// A message body, under its kind's bound.
export function checkMessageText(text: string, maxCodePoints: number): Result<void, ChatTextReason> {
    if (text.length === 0) {
        return fail("REQUIRED");
    }
    return checkProseRules(text, maxCodePoints, true);
}

// One line of prose: a conversation's title, an attachment's file name.
export function checkLine(text: string, maxCodePoints: number): Result<void, ChatTextReason> {
    if (text.length === 0) {
        return fail("REQUIRED");
    }
    return checkProseRules(text, maxCodePoints, false);
}

// Prose that may span lines: a conversation's description.
export function checkProse(text: string, maxCodePoints: number): Result<void, ChatTextReason> {
    if (text.length === 0) {
        return fail("REQUIRED");
    }
    return checkProseRules(text, maxCodePoints, true);
}

// --- mentions ----------------------------------------------------------------------

// The id is the authority: display names are neither unique nor stable, so
// nothing is ever parsed out of `@name`. `start` and `length` are in code
// points. anvil's wire calls the first one `offset`; that word is reserved in
// this library for the pagination it refuses to express
// (`tools/check-wire-discipline.sh`), so the one encoder that writes a span
// spells it there and nowhere else.
export type MentionSpan = {
    readonly user: Uuid;
    readonly start: number;
    readonly length: number;
};

function isNil(user: Uuid): boolean {
    return user.bytes().every((octet) => octet === 0);
}

// `text` must already have passed `checkMessageText`. In offset order, not
// overlapping, each inside the text. The same person may be mentioned twice,
// and whether each is a member is the server's question, not this one's.
export function checkMentions(
    text: string,
    mentions: readonly MentionSpan[],
    maxMentions: number,
): Result<void, ChatTextReason> {
    if (mentions.length > maxMentions) {
        return fail("TOO_LONG");
    }
    if (mentions.length === 0) {
        return ok();
    }
    const total = codePointLength(text);
    let previousEnd = 0;
    for (const mention of mentions) {
        if (isNil(mention.user)) {
            return fail("REQUIRED");
        }
        if (mention.length === 0) {
            return fail("BAD_FORMAT");
        }
        if (mention.start < previousEnd) {
            return fail("BAD_FORMAT");
        }
        const end = mention.start + mention.length;
        if (end > total) {
            return fail("OUT_OF_RANGE");
        }
        previousEnd = end;
    }
    return ok();
}

// --- link preview --------------------------------------------------------------------

// Every part of a preview is the client's, because the server never fetches one
// and hammer does not either: fetching a URL a person typed discloses it, and in
// an encrypted conversation that is the disclosure the encryption exists to
// prevent.
export type LinkPreview = {
    readonly url: string;
    readonly title: string;
    readonly description: string;
};

export type LinkPreviewLimits = {
    readonly urlMaxBytes: number;
    readonly titleMaxCodePoints: number;
    readonly descriptionMaxCodePoints: number;
};

// The field names anvil reports a preview's refusal under.
export type LinkPreviewField = "preview.url" | "preview.title" | "preview.description";

export type LinkPreviewRefusal = {
    readonly field: LinkPreviewField;
    readonly reason: ChatTextReason;
};

function isAsciiLetter(code: number): boolean {
    return (code >= 0x61 && code <= 0x7a) || (code >= 0x41 && code <= 0x5a);
}

function isAsciiDigit(code: number): boolean {
    return code >= 0x30 && code <= 0x39;
}

// anvil's `check_host` for a link: no credentials in the authority, no IP
// literal however it is spelled, ASCII letters, digits, hyphens and dots only,
// and a dot somewhere, because a name with no TLD is not a public target.
function checkHost(authority: string): Result<void, ChatTextReason> {
    if (authority.length === 0) {
        return fail("BAD_FORMAT");
    }
    // https://www.bank.example@evil.example/ reads as one origin and goes to
    // another.
    if (authority.includes("@")) {
        return fail("NOT_ALLOWED");
    }
    if (authority.startsWith("[")) {
        return fail("NOT_ALLOWED");
    }
    const colon = authority.indexOf(":");
    const name = colon < 0 ? authority : authority.slice(0, colon);
    if (name.length === 0) {
        return fail("BAD_FORMAT");
    }
    let literal = true;
    for (let i = 0; i < name.length; i += 1) {
        const code = name.charCodeAt(i);
        if (code > 0x7f) {
            return fail("BAD_CHARSET");
        }
        const digit = isAsciiDigit(code);
        if (!isAsciiLetter(code) && !digit && code !== 0x2d && code !== 0x2e) {
            return fail("BAD_FORMAT");
        }
        // All digits, dots and x is an IPv4 literal in decimal, octal or hex.
        if (!digit && code !== 0x2e && code !== 0x78 && code !== 0x58) {
            literal = false;
        }
    }
    if (literal) {
        return fail("NOT_ALLOWED");
    }
    if (!name.includes(".")) {
        return fail("BAD_FORMAT");
    }
    return ok();
}

const kHttps = "https://";

function checkPreviewUrl(url: string, maxBytes: number): Result<void, ChatTextReason> {
    if (url.length === 0) {
        return fail("REQUIRED");
    }
    if (utf8Length(url) > maxBytes) {
        return fail("TOO_LONG");
    }
    // An encoded URL is ASCII with no space and no control. A raw one is how a
    // value survives one parser and means something else to the next, and a
    // byte past ASCII is a host that was never IDNA-encoded, where a homoglyph
    // domain hides. The bound is therefore in bytes, the one exception here.
    for (let i = 0; i < url.length; i += 1) {
        const code = url.charCodeAt(i);
        if (code <= 0x20 || code >= 0x7f) {
            return fail("BAD_CHARSET");
        }
    }
    if (url.slice(0, kHttps.length).toLowerCase() !== kHttps) {
        return fail("NOT_ALLOWED");
    }
    const rest = url.slice(kHttps.length);
    const end = rest.search(/[/?#]/);
    return checkHost(end < 0 ? rest : rest.slice(0, end));
}

// The first part that fails, as anvil names it. A title and a description are
// optional, and an empty one is accepted; a title is one line.
export function checkLinkPreview(
    preview: LinkPreview,
    limits: LinkPreviewLimits,
): Result<void, LinkPreviewRefusal> {
    const url = checkPreviewUrl(preview.url, limits.urlMaxBytes);
    if (!url.ok) {
        return fail({ field: "preview.url", reason: url.error });
    }
    if (preview.title.length !== 0) {
        const title = checkProseRules(preview.title, limits.titleMaxCodePoints, false);
        if (!title.ok) {
            return fail({ field: "preview.title", reason: title.error });
        }
    }
    if (preview.description.length !== 0) {
        const description = checkProseRules(preview.description, limits.descriptionMaxCodePoints, true);
        if (!description.ok) {
            return fail({ field: "preview.description", reason: description.error });
        }
    }
    return ok();
}

// --- reactions ------------------------------------------------------------------------

const kZeroWidthJoiner = 0x200d;

// The identifier class's refusals, minus the zero-width joiner, which is how
// every family, profession and skin-tone emoji is spelled.
function isRefusedInReaction(cp: number): boolean {
    return (
        cp === 0x200b ||
        cp === 0x200c ||
        cp === 0xfeff ||
        cp === 0x200e ||
        cp === 0x200f ||
        cp === 0x061c ||
        (cp >= 0x2066 && cp <= 0x2069)
    );
}

// Exactly one extended grapheme cluster, in NFC, of at most `maxCodePoints`.
// Whether it is "an emoji" is not decided here: an application with a palette
// checks its own list. A joiner may not open or close a reaction, where it
// joins nothing and only makes a second spelling of the same picture — and
// reactions are counted per distinct value, so two spellings split a tally.
//
// No segmenter is a refusal, as it is on the server: a reaction that cannot be
// shown to be one cluster is not accepted as one.
export function checkReaction(reaction: string, maxCodePoints: number): Result<void, ChatTextReason> {
    if (reaction.length === 0) {
        return fail("REQUIRED");
    }
    // Before anything is decoded: an oversized value costs one comparison.
    if (utf8Length(reaction) > maxCodePoints * 4) {
        return fail("TOO_LONG");
    }
    if (!isEncodable(reaction)) {
        return fail("BAD_CHARSET");
    }
    if (codePointLength(reaction) > maxCodePoints) {
        return fail("TOO_LONG");
    }

    let visible = false;
    let first = true;
    let previous = 0;
    for (const character of reaction) {
        const cp = character.codePointAt(0) ?? 0;
        if (isControl(cp) || isLineOrParagraphSeparator(cp) || isRefusedInReaction(cp)) {
            return fail("BAD_CHARSET");
        }
        if (cp === kZeroWidthJoiner && first) {
            return fail("BAD_CHARSET");
        }
        if (!visible && !isBlank(character, cp)) {
            visible = true;
        }
        first = false;
        previous = cp;
    }
    if (previous === kZeroWidthJoiner) {
        return fail("BAD_CHARSET");
    }
    if (!visible) {
        return fail("REQUIRED");
    }
    if (!checkBidi(reaction, "prose").ok) {
        return fail("BAD_CHARSET");
    }
    if (toNfc(reaction) !== reaction) {
        return fail("BAD_FORMAT");
    }
    if (typeof Intl.Segmenter !== "function") {
        return fail("BAD_FORMAT");
    }
    const clusters = new Intl.Segmenter(undefined, { granularity: "grapheme" }).segment(reaction)[Symbol.iterator]();
    clusters.next();
    return clusters.next().done === true ? ok() : fail("BAD_FORMAT");
}
