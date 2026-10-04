// The composer's validator against anvil's own case table.
//
// Every case here is ported from anvil's `tests/chat_text_test.cc`, one for one
// and in its order, with the reason anvil asserts. That is a copy of a belief,
// and it is the weaker half of the contract: the row closes when anvil prints
// these cases as a fixture and this suite reads the file, as the edit recipe's
// vectors are read (`docs/15-tasks.md` §Cross-repo, "chat golden vectors").
//
// One deliberate difference. anvil's invalid-UTF-8 cases are byte sequences — a
// truncated lead, an overlong slash, an encoded surrogate — and a JavaScript
// string cannot hold the first two at all. The one a string CAN hold is a lone
// surrogate, which `fetch` would silently send as U+FFFD, so that is the case
// asserted in their place.

import { describe, expect, it } from "../support/test.js";

import type { ChatTextReason, MentionSpan } from "../../src/chat/text.js";
import {
    checkLine,
    checkLinkPreview,
    checkMentions,
    checkMessageText,
    checkProse,
    checkReaction,
    codePointToUtf16Index,
    utf16IndexToCodePoint,
} from "../../src/chat/text.js";
import { Uuid } from "../../src/core/uuid.js";

// The reference kinds' bounds, as `kChatLimits` carries them.
const kMax = 4096;
const kMaxMentions = 32;
const kReactionMax = 8;
const kPreviewLimits = { urlMaxBytes: 2048, titleMaxCodePoints: 200, descriptionMaxCodePoints: 500 };

const kBeh = "ب";

function reason(result: { readonly ok: boolean; readonly error?: unknown }): ChatTextReason | "OK" {
    return result.ok ? "OK" : (result.error as ChatTextReason);
}

function message(text: string, max: number = kMax): ChatTextReason | "OK" {
    return reason(checkMessageText(text, max));
}

function user(tag: number): Uuid {
    const bytes = new Uint8Array(16);
    bytes[15] = tag;
    const parsed = Uuid.fromBytes(bytes);
    if (!parsed.ok) {
        throw new Error("sixteen bytes are a uuid");
    }
    return parsed.value;
}

function span(tag: number, start: number, length: number): MentionSpan {
    return { user: user(tag), start, length };
}

describe("the message body", () => {
    it("accepts ordinary text in any script", () => {
        expect(message("hello")).toBe("OK");
        expect(message("مرحبا")).toBe("OK");
        expect(message("line one\nline two\n\tindented")).toBe("OK");
        expect(message("\u{1F44D} great")).toBe("OK");
    });

    it("bounds in code points, not bytes and not code units", () => {
        // 4096 Arabic letters are 8192 bytes; a byte bound would halve the
        // allowance for every Arabic writer.
        expect(message(kBeh.repeat(kMax))).toBe("OK");
        expect(message(kBeh.repeat(kMax + 1))).toBe("TOO_LONG");
        // 4096 emoji are 8192 UTF-16 code units; `.length` would refuse them.
        expect(message("\u{1F44D}".repeat(kMax))).toBe("OK");
    });

    it("takes the kind's lower bound", () => {
        expect(message("a".repeat(10), 10)).toBe("OK");
        expect(message("a".repeat(11), 10)).toBe("TOO_LONG");
        expect(message("a", 0)).toBe("TOO_LONG");
    });

    it("refuses a hostile body on its length", () => {
        expect(message("a".repeat(1 << 20))).toBe("TOO_LONG");
    });

    it("requires something that renders", () => {
        expect(message("")).toBe("REQUIRED");
        expect(message(" \n\t ")).toBe("REQUIRED");
        // No-break space, ideographic space, zero-width space and the Hangul
        // filler: each renders as nothing, which is the point of sending it.
        expect(message(" 　")).toBe("REQUIRED");
        expect(message("​ㅤ")).toBe("REQUIRED");
        expect(message(" x ")).toBe("OK");
    });

    it("refuses controls other than newline and tab", () => {
        expect(message("a\x01b")).toBe("BAD_CHARSET");
        expect(message("a\x1B[2Jb")).toBe("BAD_CHARSET");
        expect(message("a\x7F")).toBe("BAD_CHARSET");
        expect(message("a\u0085b")).toBe("BAD_CHARSET");
        expect(message("a\u009Bb")).toBe("BAD_CHARSET");
    });

    it("has one newline spelling", () => {
        expect(message("a\nb")).toBe("OK");
        expect(message("a\rb")).toBe("BAD_CHARSET");
        expect(message("a\r\nb")).toBe("BAD_CHARSET");
        expect(message("a b")).toBe("BAD_CHARSET");
        expect(message("a b")).toBe("BAD_CHARSET");
    });

    it("refuses what cannot travel as UTF-8, NUL and the non-characters", () => {
        expect(message("\uD800")).toBe("BAD_CHARSET");
        expect(message("a\uDC00")).toBe("BAD_CHARSET");
        expect(message("a\u0000b")).toBe("BAD_CHARSET");
        expect(message("￾")).toBe("BAD_CHARSET");
        expect(message("﷐")).toBe("BAD_CHARSET");
    });

    it("refuses overrides and allows isolates", () => {
        expect(message("invoice‮fdp.exe")).toBe("BAD_CHARSET");
        expect(message("a‪b")).toBe("BAD_CHARSET");
        expect(message("the word ⁧مرحبا⁩ here")).toBe("OK");
    });

    it("requires NFC rather than applying it", () => {
        expect(message("café")).toBe("OK");
        expect(message("café")).toBe("BAD_FORMAT");
        // A composition exclusion: NFC turns one code point into two, which is
        // exactly what would move a mention span.
        expect(message("क़")).toBe("BAD_FORMAT");
        expect(message("بَاب")).toBe("OK");
    });

    it("checks a title as one line and a description as prose", () => {
        expect(reason(checkLine("A title", 100))).toBe("OK");
        expect(reason(checkLine("two\nlines", 100))).toBe("BAD_CHARSET");
        expect(reason(checkLine("a\ttab", 100))).toBe("BAD_CHARSET");
        expect(reason(checkLine("", 100))).toBe("REQUIRED");
        expect(reason(checkProse("two\nlines", 1024))).toBe("OK");
    });
});

describe("mentions", () => {
    it("accepts sorted spans inside the text, counted in code points", () => {
        // "مرحبا أحمد و سارة"
        const text =
            "مرحبا أحمد و سارة";
        expect(message(text)).toBe("OK");
        expect(reason(checkMentions(text, [span(1, 6, 4), span(2, 13, 4)], kMaxMentions))).toBe("OK");
        expect(reason(checkMentions(text, [], kMaxMentions))).toBe("OK");
    });

    it("lets one person be mentioned twice, and adjacent spans touch", () => {
        expect(reason(checkMentions("ab cd", [span(1, 0, 2), span(1, 3, 2)], kMaxMentions))).toBe("OK");
        expect(reason(checkMentions("abcd", [span(1, 0, 2), span(2, 2, 2)], kMaxMentions))).toBe("OK");
    });

    it("refuses a span past the end, and a wrapping one", () => {
        expect(reason(checkMentions("abcde", [span(1, 3, 2)], kMaxMentions))).toBe("OK");
        expect(reason(checkMentions("abcde", [span(1, 4, 2)], kMaxMentions))).toBe("OUT_OF_RANGE");
        expect(reason(checkMentions(kBeh + kBeh, [span(1, 0, 3)], kMaxMentions))).toBe("OUT_OF_RANGE");
        expect(reason(checkMentions("abc", [span(1, 0xffffffff, 2)], kMaxMentions))).toBe("OUT_OF_RANGE");
    });

    it("refuses overlapping, unsorted and empty spans, and the nil user", () => {
        expect(reason(checkMentions("abcdef", [span(1, 0, 3), span(2, 2, 2)], kMaxMentions))).toBe(
            "BAD_FORMAT",
        );
        expect(reason(checkMentions("abcdef", [span(1, 3, 2), span(2, 0, 2)], kMaxMentions))).toBe(
            "BAD_FORMAT",
        );
        expect(reason(checkMentions("abc", [span(1, 1, 0)], kMaxMentions))).toBe("BAD_FORMAT");
        expect(reason(checkMentions("abc", [span(0, 0, 1)], kMaxMentions))).toBe("REQUIRED");
    });

    it("allows at most the descriptor's count", () => {
        const text = "a".repeat(64);
        const spans = Array.from({ length: kMaxMentions }, (_, i) => span(1, i, 1));
        expect(reason(checkMentions(text, spans, kMaxMentions))).toBe("OK");
        expect(reason(checkMentions(text, [...spans, span(1, 40, 1)], kMaxMentions))).toBe("TOO_LONG");
    });

    it("converts between code points and the DOM's code units in one place", () => {
        // An emoji is two code units and one code point, so a span placed after
        // one in the wrong unit lands one character late.
        const text = "\u{1F44D} @sara";
        expect(codePointToUtf16Index(text, 2)).toBe(3);
        expect(utf16IndexToCodePoint(text, 3)).toBe(2);
        expect(codePointToUtf16Index(text, utf16IndexToCodePoint(text, text.length))).toBe(text.length);
    });
});

describe("the link preview", () => {
    function refused(url: string, title = "", description = ""): string {
        const result = checkLinkPreview({ url, title, description }, kPreviewLimits);
        return result.ok ? "OK" : `${result.error.field} ${result.error.reason}`;
    }

    it("accepts an absolute https preview", () => {
        expect(refused("https://example.com/a?b=c#d", "A title", "First line.\nSecond line.")).toBe("OK");
        expect(refused("HTTPS://Example.COM/")).toBe("OK");
        expect(refused("https://xn--mgbh0fb.example/")).toBe("OK");
    });

    it("allows only absolute https", () => {
        expect(refused("")).toBe("preview.url REQUIRED");
        expect(refused("javascript:alert(1)")).toBe("preview.url NOT_ALLOWED");
        expect(refused("http://example.com/")).toBe("preview.url NOT_ALLOWED");
        expect(refused("/relative/page")).toBe("preview.url NOT_ALLOWED");
        expect(refused("#fragment")).toBe("preview.url NOT_ALLOWED");
        expect(refused("mailto:a@example.com")).toBe("preview.url NOT_ALLOWED");
        expect(refused("data:text/html,x")).toBe("preview.url NOT_ALLOWED");
    });

    it("judges the authority as any link's", () => {
        expect(refused("https://bank.example@evil.example/")).toBe("preview.url NOT_ALLOWED");
        expect(refused("https://192.0.2.1/")).toBe("preview.url NOT_ALLOWED");
        expect(refused("https://intranet/")).toBe("preview.url BAD_FORMAT");
    });

    it("takes encoded ASCII and bounds it in bytes", () => {
        expect(refused("https://pаypal.example/")).toBe("preview.url BAD_CHARSET");
        expect(refused("https://example.com/a b")).toBe("preview.url BAD_CHARSET");
        expect(refused("https://example.com/\r\nSet-Cookie:x")).toBe("preview.url BAD_CHARSET");
        const atBound = `https://example.com/${"a".repeat(2048 - 20)}`;
        expect(atBound.length).toBe(2048);
        expect(refused(atBound)).toBe("OK");
        expect(refused(`${atBound}a`)).toBe("preview.url TOO_LONG");
    });

    it("takes a title as one bounded line", () => {
        const url = "https://example.com/";
        expect(refused(url, kBeh.repeat(200))).toBe("OK");
        expect(refused(url, kBeh.repeat(201))).toBe("preview.title TOO_LONG");
        expect(refused(url, "two\nlines")).toBe("preview.title BAD_CHARSET");
        expect(refused(url, "a\ttab")).toBe("preview.title BAD_CHARSET");
        expect(refused(url, "gnp.‮exe")).toBe("preview.title BAD_CHARSET");
        expect(refused(url, "   ")).toBe("preview.title REQUIRED");
    });

    it("takes a description as bounded prose", () => {
        const url = "https://example.com/";
        expect(refused(url, "", kBeh.repeat(500))).toBe("OK");
        expect(refused(url, "", kBeh.repeat(501))).toBe("preview.description TOO_LONG");
        expect(refused(url, "", "a\x01")).toBe("preview.description BAD_CHARSET");
        expect(refused(url, "", "café")).toBe("preview.description BAD_FORMAT");
    });
});

describe("a reaction", () => {
    function react(text: string): ChatTextReason | "OK" {
        return reason(checkReaction(text, kReactionMax));
    }

    it("accepts one cluster of any kind", () => {
        expect(react("\u{1F44D}")).toBe("OK");
        expect(react("\u{1F44D}\u{1F3FD}")).toBe("OK");
        expect(react("❤️")).toBe("OK");
        expect(react("1️⃣")).toBe("OK");
        expect(react("\u{1F1EA}\u{1F1EC}")).toBe("OK");
        expect(react("a")).toBe("OK");
        expect(react("بَ")).toBe("OK");
    });

    it("takes a joiner family as one reaction", () => {
        expect(react("\u{1F468}‍\u{1F469}‍\u{1F467}‍\u{1F466}")).toBe("OK");
    });

    it("refuses more than one cluster", () => {
        expect(react("\u{1F44D}\u{1F44D}")).toBe("BAD_FORMAT");
        expect(react("ab")).toBe("BAD_FORMAT");
        expect(react("\u{1F1EA}\u{1F1EC}\u{1F1EA}")).toBe("BAD_FORMAT");
        expect(react("a‍b")).toBe("BAD_FORMAT");
    });

    it("bounds in code points and in bytes", () => {
        expect(react(`a${"́".repeat(8)}`)).toBe("TOO_LONG");
        expect(react(`a${"́".repeat(7)}`)).not.toBe("TOO_LONG");
        expect(react("a".repeat(33))).toBe("TOO_LONG");
    });

    it("requires something that renders", () => {
        expect(react("")).toBe("REQUIRED");
        expect(react(" ")).toBe("REQUIRED");
        expect(react("ㅤ")).toBe("REQUIRED");
    });

    it("takes the identifier policy, except an interior joiner", () => {
        expect(react("\x01")).toBe("BAD_CHARSET");
        expect(react("\n")).toBe("BAD_CHARSET");
        expect(react("‮\u{1F44D}")).toBe("BAD_CHARSET");
        expect(react("⁧\u{1F44D}")).toBe("BAD_CHARSET");
        expect(react("‎")).toBe("BAD_CHARSET");
        expect(react("​")).toBe("BAD_CHARSET");
        expect(react("a‌")).toBe("BAD_CHARSET");
        expect(react("‍")).toBe("BAD_CHARSET");
        expect(react("\u{1F44D}‍")).toBe("BAD_CHARSET");
        expect(react("‍\u{1F44D}")).toBe("BAD_CHARSET");
    });

    it("refuses what cannot travel and what is not NFC", () => {
        expect(react("\uD83D")).toBe("BAD_CHARSET");
        expect(react("é")).toBe("BAD_FORMAT");
        expect(react("é")).toBe("OK");
    });
});
