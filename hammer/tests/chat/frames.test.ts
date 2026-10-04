// The socket frames against anvil's golden bytes and refusals.
//
// The bytes are copied from anvil's `tests/chat_frames_test.cc`, which says its
// golden frames are what a client's decoder is written against. A copy is the
// weaker half of the contract: the row closes when anvil prints them as a
// fixture and this suite reads the file (`docs/15-tasks.md` §Cross-repo, "chat
// golden vectors").
//
// Only the client's half is here: decoding what the server sends and encoding
// what a client sends. The server's half of each is anvil's suite.
//
// One deliberate difference, asserted by its own case: anvil round-trips counters
// up to INT64_MAX, and a JavaScript number holds integers exactly only to 2^53,
// so this decoder refuses a larger one as `frame.value` rather than reading a
// value it would get wrong.

import { describe, expect, it } from "../support/test.js";

import type { DownstreamFrame, FrameFault } from "../../src/chat/frames.js";
import {
    decodeDownstream,
    encodeUpstream,
    kInlineWakeBytes,
    kMaxDownstreamFrameBytes,
    kMaxUpstreamFrameBytes,
} from "../../src/chat/frames.js";
import { Uuid } from "../../src/core/uuid.js";

function range(from: number, count: number): number[] {
    return Array.from({ length: count }, (_, i) => from + i);
}

const kConversationBytes = range(0x10, 16);
const kUserBytes = range(0x20, 16);

function uuid(bytes: readonly number[]): Uuid {
    const parsed = Uuid.fromBytes(Uint8Array.from(bytes));
    if (!parsed.ok) {
        throw new Error("sixteen bytes are a uuid");
    }
    return parsed.value;
}

const kConversation = uuid(kConversationBytes);
const kUser = uuid(kUserBytes);

const kWakeInline = [0x01, 0x01, ...kConversationBytes, 0, 0, 0, 0, 0, 0, 0, 0x2a, 0x00, 0x02, 0x68, 0x69];
const kWakeFetch = [0x01, 0x01, ...kConversationBytes, 0, 0, 0x01, 0, 0, 0, 0, 0x01, 0x00, 0x00];
const kTyping = [0x01, 0x02, ...kConversationBytes, ...kUserBytes];
const kPresence = [0x01, 0x03, ...kUserBytes, 0x01, 0, 0, 0x01, 0x92, 0x3a, 0xbc, 0xde, 0xf0];
const kReceipt = [0x01, 0x04, ...kConversationBytes, ...kUserBytes, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 7];
const kMembership = [0x01, 0x05, ...kConversationBytes, 0, 0, 0, 0, 0, 0, 0, 3];
const kPing = [0x01, 0x06];
const kPong = [0x01, 0x07];
const kSync = [0x01, 0x08];
const kMutation = [0x01, 0x09, ...kConversationBytes, 0, 0, 0, 0, 0, 0, 0x01, 0x05];
const kClientTyping = [0x01, 0x41, ...kConversationBytes];
const kClientPong = [0x01, 0x42];
const kClientPing = [0x01, 0x43];

const kDownstreamGolden = [
    kWakeInline,
    kWakeFetch,
    kTyping,
    kPresence,
    kReceipt,
    kMembership,
    kPing,
    kPong,
    kSync,
    kMutation,
];

function decode(bytes: readonly number[]): DownstreamFrame {
    const decoded = decodeDownstream(Uint8Array.from(bytes));
    if (!decoded.ok) {
        throw new Error(`refused a golden frame as ${decoded.error}`);
    }
    return decoded.value;
}

function refused(bytes: readonly number[]): FrameFault | "accepted" {
    const decoded = decodeDownstream(Uint8Array.from(bytes));
    return decoded.ok ? "accepted" : decoded.error;
}

function changedAt(bytes: readonly number[], at: number, value: number): number[] {
    const copy = [...bytes];
    copy[at] = value;
    return copy;
}

function filled(bytes: readonly number[], from: number, to: number, value: number): number[] {
    return bytes.map((byte, i) => (i >= from && i < to ? value : byte));
}

describe("the golden downstream frames", () => {
    it("decodes a wake carrying its message, as a copy", () => {
        const source = Uint8Array.from(kWakeInline);
        const decoded = decodeDownstream(source);
        expect(decoded.ok).toBe(true);
        if (!decoded.ok || decoded.value.type !== "wake") return;
        expect(decoded.value.conversation.equals(kConversation)).toBe(true);
        expect(decoded.value.seq).toBe(42);
        expect(Array.from(decoded.value.inline ?? [])).toEqual([0x68, 0x69]);
        // A copy, never a view: the socket's buffer is reused, and a wake is
        // queued and broadcast long after the frame arrived.
        source[kWakeInline.length - 1] = 0x00;
        expect(Array.from(decoded.value.inline ?? [])).toEqual([0x68, 0x69]);
    });

    it("decodes a wake that says fetch it", () => {
        const wake = decode(kWakeFetch);
        expect(wake.type === "wake" && wake.seq).toBe(2 ** 40 + 1);
        expect(wake.type === "wake" && wake.inline).toBe(null);
    });

    it("decodes typing, presence, a receipt, membership and a mutation", () => {
        const typing = decode(kTyping);
        expect(typing.type === "typing" && typing.user.equals(kUser)).toBe(true);

        const presence = decode(kPresence);
        expect(presence.type === "presence" && presence.state).toBe("online");
        expect(presence.type === "presence" && presence.lastSeen.toDate().getTime()).toBe(0x0192_3abc_def0);

        const receipt = decode(kReceipt);
        expect(receipt.type === "receipt" && [receipt.deliveredSeq, receipt.readSeq]).toEqual([9, 7]);

        const membership = decode(kMembership);
        expect(membership.type === "membership" && membership.membershipVersion).toBe(3);

        const mutation = decode(kMutation);
        expect(mutation.type === "mutation" && mutation.mutation).toBe(261);
    });

    it("decodes the three header-only frames", () => {
        expect(decode(kPing).type).toBe("ping");
        expect(decode(kPong).type).toBe("pong");
        expect(decode(kSync).type).toBe("sync");
    });

    it("bounds the downstream frame at a full inline wake", () => {
        const full = [...kWakeFetch.slice(0, 26), 0x08, 0x00, ...new Array<number>(kInlineWakeBytes).fill(0x41)];
        expect(full.length).toBe(kMaxDownstreamFrameBytes);
        expect(refused(full)).toBe("accepted");
    });
});

describe("the golden upstream frames", () => {
    it("encodes typing, pong and ping as anvil's bytes", () => {
        expect(Array.from(encodeUpstream({ type: "client-typing", conversation: kConversation }))).toEqual(kClientTyping);
        expect(Array.from(encodeUpstream({ type: "client-pong" }))).toEqual(kClientPong);
        expect(Array.from(encodeUpstream({ type: "client-ping" }))).toEqual(kClientPing);
        expect(kClientTyping.length).toBe(kMaxUpstreamFrameBytes);
    });

    it("refuses to name the nil conversation, as programmer error", () => {
        const nil = uuid(new Array<number>(16).fill(0));
        expect(() => encodeUpstream({ type: "client-typing", conversation: nil })).toThrow();
    });
});

describe("refusals, in anvil's order", () => {
    it("refuses a version other than one", () => {
        for (const version of [0x00, 0x02, 0xff]) {
            expect(refused(changedAt(kTyping, 0, version))).toBe("frame.version");
        }
    });

    it("refuses a type nobody defined, and names the other direction's", () => {
        for (const type of [0x00, 0x0a, 0x3f, 0x80, 0xff]) {
            expect(refused([0x01, type])).toBe("frame.type");
        }
        expect(refused(kClientTyping)).toBe("frame.direction");
        expect(refused(kClientPong)).toBe("frame.direction");
        expect(refused(kClientPing)).toBe("frame.direction");
    });

    it("refuses a trailing byte and every short prefix", () => {
        for (const golden of kDownstreamGolden) {
            expect(refused([...golden, 0x00])).toBe("frame.length");
            for (let n = 0; n < golden.length; n += 1) {
                expect(refused(golden.slice(0, n))).toBe("frame.length");
            }
        }
    });

    it("refuses a wake whose inline length disagrees with its body, or is over the cap", () => {
        expect(refused(changedAt(kWakeInline, 27, 0x03))).toBe("frame.length");
        expect(refused(changedAt(kWakeInline, 27, 0x01))).toBe("frame.length");
        // A claim of 2049 within the bound is its own refusal; carried in full
        // it is over the bound, which is checked first.
        const claim = changedAt(changedAt(kWakeFetch, 26, 0x08), 27, 0x01);
        expect(refused(claim)).toBe("frame.inline");
        expect(refused([...claim, ...new Array<number>(kInlineWakeBytes + 1).fill(0x41)])).toBe("frame.size");
    });

    it("refuses a presence state it does not know, and a negative last seen", () => {
        expect(refused(changedAt(kPresence, 18, 0x02))).toBe("frame.value");
        expect(refused(changedAt(kPresence, 18, 0xff))).toBe("frame.value");
        expect(refused(changedAt(kPresence, 19, 0x80))).toBe("frame.value");
    });

    it("refuses read past delivered, and negative counters", () => {
        expect(refused(changedAt(kReceipt, 49, 0x0a))).toBe("frame.value");
        expect(refused(changedAt(kWakeFetch, 18, 0x80))).toBe("frame.value");
        expect(refused(changedAt(kMembership, 18, 0xff))).toBe("frame.value");
        expect(refused(filled(kReceipt, 34, kReceipt.length, 0xff))).toBe("frame.value");
    });

    it("refuses a mutation counter below one", () => {
        expect(refused(filled(kMutation, 18, kMutation.length, 0x00))).toBe("frame.value");
        expect(refused(changedAt(kMutation, 18, 0x80))).toBe("frame.value");
    });

    it("refuses a nil conversation and a nil user", () => {
        for (const golden of [kWakeInline, kTyping, kReceipt, kMembership, kMutation]) {
            expect(refused(filled(golden, 2, 18, 0x00))).toBe("frame.nil");
        }
        expect(refused(filled(kTyping, 18, kTyping.length, 0x00))).toBe("frame.nil");
        expect(refused(filled(kPresence, 2, 18, 0x00))).toBe("frame.nil");
    });

    it("refuses a frame over the bound before its header is read", () => {
        expect(refused(new Array<number>(kMaxDownstreamFrameBytes + 1).fill(0xff))).toBe("frame.size");
    });

    it("refuses a counter past 2^53 on this side, which anvil would accept", () => {
        // INT64_MAX: a valid frame to anvil, and a number this decoder cannot
        // hold exactly, so reading it would hand the store a different seq.
        const max = [...kMutation.slice(0, 18), 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff];
        expect(refused(max)).toBe("frame.value");
        const atSafe = [...kMutation.slice(0, 18), 0x00, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff];
        expect(refused(atSafe)).toBe("accepted");
    });
});

describe("single-byte changes", () => {
    it("are refused by a named rule, never by the canonical backstop", () => {
        // The decoder re-encodes every frame it accepts and refuses a mismatch as
        // `frame.canonical`. anvil's header says that backstop firing means a rule
        // is missing, so across every one-byte change to every golden frame, each
        // refusal must name the rule that caught it.
        for (const golden of kDownstreamGolden) {
            for (let at = 0; at < golden.length; at += 1) {
                for (const value of [0x00, 0x01, 0x7f, 0x80, 0xff]) {
                    expect(refused(changedAt(golden, at, value))).not.toBe("frame.canonical");
                }
            }
        }
    });
});
