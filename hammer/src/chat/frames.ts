// The chat socket's frames: anvil's `chat/frames.h`, byte for byte.
//
// `u8 version ‖ u8 type ‖ body`, big-endian, and CANONICAL: one frame has exactly
// one encoding, and a decoder that accepted a second spelling would one day
// disagree with the server about which one was meant. So every accepted frame is
// re-encoded and compared, as anvil does. The named rules give a refusal its
// reason; the comparison is what makes the list of rules complete.
//
// Downstream types are 0x01–0x3F and upstream 0x40–0x7F, so a frame of the wrong
// direction is told apart from a type nobody defined: one is a server replaying
// what it was sent, the other is a server newer than this client.
//
// A refusal here is never answered on the socket. Nothing a correct server sends
// can be malformed, so the socket policy closes the connection, counts the
// fault and syncs over a new one (`docs/05-chat.md` §7.2). Reading on would mean
// reading frames whose boundaries are now a guess.
//
// The contract is anvil's golden bytes in `tests/chat_frames_test.cc`, not this
// comment and not anvil's header.

import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import { ServerInstant } from "../core/time.js";
import { Uuid } from "../core/uuid.js";

export const kFrameVersion = 1;

// A wake carries the message inline when the message is at most this many bytes,
// so a busy group's members do not each turn a wake into a read.
export const kInlineWakeBytes = 2048;

const kHeaderBytes = 2;
const kWakeFixedBytes = kHeaderBytes + 16 + 8 + 2;
const kTypingBytes = kHeaderBytes + 16 + 16;
const kPresenceBytes = kHeaderBytes + 16 + 1 + 8;
const kReceiptBytes = kHeaderBytes + 16 + 16 + 8 + 8;
const kMembershipBytes = kHeaderBytes + 16 + 8;
const kMutationBytes = kHeaderBytes + 16 + 8;
const kClientTypingBytes = kHeaderBytes + 16;

// The largest frame each direction carries. Anything longer is refused before a
// byte of it is read.
export const kMaxDownstreamFrameBytes = kWakeFixedBytes + kInlineWakeBytes;
export const kMaxUpstreamFrameBytes = kClientTypingBytes;

const kWake = 0x01;
const kTyping = 0x02;
const kPresence = 0x03;
const kReceipt = 0x04;
const kMembership = 0x05;
const kPing = 0x06;
const kPong = 0x07;
const kSync = 0x08;
const kMutation = 0x09;
const kClientTyping = 0x41;
const kClientPong = 0x42;
const kClientPing = 0x43;

const kDownstreamFirst = 0x01;
const kDownstreamLast = 0x3f;
const kUpstreamFirst = 0x40;
const kUpstreamLast = 0x7f;

// anvil's refusal names, unchanged, so a count of closes by reason reads the same
// on both sides.
export type FrameFault =
    // Over the direction's bound, before anything was read.
    | "frame.size"
    // Shorter than the header or the type's body, a trailing byte, or a wake whose
    // inline length disagrees with the bytes after it.
    | "frame.length"
    | "frame.version"
    | "frame.type"
    | "frame.direction"
    | "frame.inline"
    | "frame.nil"
    // A negative counter or time, an unknown presence state, read past delivered,
    // a mutation counter below one — and, on this side only, a counter past
    // 2^53, which a JavaScript number cannot hold exactly. A server reaches that
    // after nine quadrillion messages in one conversation.
    | "frame.value"
    | "frame.canonical";

export type PresenceState = "offline" | "online";

export type DownstreamFrame =
    // A message at `seq`. `inline` is the message as the history route writes it,
    // a copy of the bytes, or null when the server says to fetch it.
    | { readonly type: "wake"; readonly conversation: Uuid; readonly seq: number; readonly inline: Uint8Array | null }
    | { readonly type: "typing"; readonly conversation: Uuid; readonly user: Uuid }
    | { readonly type: "presence"; readonly user: Uuid; readonly state: PresenceState; readonly lastSeen: ServerInstant }
    | {
          readonly type: "receipt";
          readonly conversation: Uuid;
          readonly user: Uuid;
          readonly deliveredSeq: number;
          readonly readSeq: number;
      }
    | { readonly type: "membership"; readonly conversation: Uuid; readonly membershipVersion: number }
    // A message the reader may hold was edited, revoked or reacted to, and the
    // conversation's mutation counter is now at least `mutation`. Which message is
    // the catch-up's to say, because the catch-up authorises the read.
    | { readonly type: "mutation"; readonly conversation: Uuid; readonly mutation: number }
    | { readonly type: "ping" }
    | { readonly type: "pong" }
    // Wakes may have been lost: catch every conversation up from its cursors.
    | { readonly type: "sync" };

export type UpstreamFrame =
    | { readonly type: "client-typing"; readonly conversation: Uuid }
    | { readonly type: "client-pong" }
    | { readonly type: "client-ping" };

// --- reading --------------------------------------------------------------------

function readUuid(bytes: Uint8Array, at: number): Uuid {
    const parsed = Uuid.fromBytes(bytes.slice(at, at + 16));
    if (!parsed.ok) {
        // Sixteen bytes are always a uuid; a failure here is this module's bug.
        throw new Error("sixteen bytes did not make a uuid");
    }
    return parsed.value;
}

function isNil(id: Uuid): boolean {
    return id.bytes().every((octet) => octet === 0);
}

// A signed big-endian i64 as a number, or null when it is negative or past what a
// number holds exactly. Both are `frame.value`: anvil refuses the first, and the
// second cannot be re-encoded faithfully.
function readCounter(view: DataView, at: number): number | null {
    const value = view.getBigInt64(at);
    if (value < 0n || value > BigInt(Number.MAX_SAFE_INTEGER)) {
        return null;
    }
    return Number(value);
}

function decodeBody(type: number, bytes: Uint8Array): Result<DownstreamFrame, FrameFault> {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const body = kHeaderBytes;
    switch (type) {
        case kWake: {
            if (bytes.length < kWakeFixedBytes) {
                return fail("frame.length");
            }
            const inlineLength = view.getUint16(body + 24);
            // Over the cap is its own refusal even when the bytes are there,
            // because the sender broke a different rule.
            if (inlineLength > kInlineWakeBytes) {
                return fail("frame.inline");
            }
            if (bytes.length !== kWakeFixedBytes + inlineLength) {
                return fail("frame.length");
            }
            const conversation = readUuid(bytes, body);
            if (isNil(conversation)) {
                return fail("frame.nil");
            }
            const seq = readCounter(view, body + 16);
            if (seq === null) {
                return fail("frame.value");
            }
            // A copy, never a view: the socket's buffer is reused, and a wake is
            // queued and broadcast to other tabs long after this returns.
            const inline = inlineLength === 0 ? null : bytes.slice(kWakeFixedBytes);
            return ok({ type: "wake", conversation, seq, inline });
        }
        case kTyping: {
            if (bytes.length !== kTypingBytes) {
                return fail("frame.length");
            }
            const conversation = readUuid(bytes, body);
            const user = readUuid(bytes, body + 16);
            if (isNil(conversation) || isNil(user)) {
                return fail("frame.nil");
            }
            return ok({ type: "typing", conversation, user });
        }
        case kPresence: {
            if (bytes.length !== kPresenceBytes) {
                return fail("frame.length");
            }
            // A state this build does not know is one the next build means
            // something by, and reading it as either of these would be a guess.
            const state = bytes[body + 16] ?? 0xff;
            if (state > 1) {
                return fail("frame.value");
            }
            const user = readUuid(bytes, body);
            if (isNil(user)) {
                return fail("frame.nil");
            }
            const lastSeenMs = readCounter(view, body + 17);
            if (lastSeenMs === null) {
                return fail("frame.value");
            }
            const lastSeen = ServerInstant.fromServerEpochMs(lastSeenMs);
            if (!lastSeen.ok) {
                return fail("frame.value");
            }
            return ok({ type: "presence", user, state: state === 1 ? "online" : "offline", lastSeen: lastSeen.value });
        }
        case kReceipt: {
            if (bytes.length !== kReceiptBytes) {
                return fail("frame.length");
            }
            const conversation = readUuid(bytes, body);
            const user = readUuid(bytes, body + 16);
            if (isNil(conversation) || isNil(user)) {
                return fail("frame.nil");
            }
            // A message cannot be read on a device it has not reached.
            const deliveredSeq = view.getBigInt64(body + 32);
            const readSeq = view.getBigInt64(body + 40);
            if (readSeq < 0n || readSeq > deliveredSeq) {
                return fail("frame.value");
            }
            const delivered = readCounter(view, body + 32);
            const read = readCounter(view, body + 40);
            if (delivered === null || read === null) {
                return fail("frame.value");
            }
            return ok({ type: "receipt", conversation, user, deliveredSeq: delivered, readSeq: read });
        }
        case kMembership: {
            if (bytes.length !== kMembershipBytes) {
                return fail("frame.length");
            }
            const conversation = readUuid(bytes, body);
            if (isNil(conversation)) {
                return fail("frame.nil");
            }
            const membershipVersion = readCounter(view, body + 16);
            if (membershipVersion === null) {
                return fail("frame.value");
            }
            return ok({ type: "membership", conversation, membershipVersion });
        }
        case kMutation: {
            if (bytes.length !== kMutationBytes) {
                return fail("frame.length");
            }
            const conversation = readUuid(bytes, body);
            if (isNil(conversation)) {
                return fail("frame.nil");
            }
            // The counter's first value is one, so zero announces nothing.
            const mutation = readCounter(view, body + 16);
            if (mutation === null || mutation < 1) {
                return fail("frame.value");
            }
            return ok({ type: "mutation", conversation, mutation });
        }
        case kPing:
            return bytes.length === kHeaderBytes ? ok({ type: "ping" }) : fail("frame.length");
        case kPong:
            return bytes.length === kHeaderBytes ? ok({ type: "pong" }) : fail("frame.length");
        case kSync:
            return bytes.length === kHeaderBytes ? ok({ type: "sync" }) : fail("frame.length");
        default:
            return fail("frame.type");
    }
}

// The shared opening, in the order the grammar promises: the bound before
// anything, the header's length before the header, the version before the type.
function checkHeader(bytes: Uint8Array, bound: number): Result<void, FrameFault> {
    if (bytes.length > bound) {
        return fail("frame.size");
    }
    if (bytes.length < kHeaderBytes) {
        return fail("frame.length");
    }
    if (bytes[0] !== kFrameVersion) {
        return fail("frame.version");
    }
    return ok();
}

function sameBytes(left: Uint8Array, right: Uint8Array): boolean {
    if (left.length !== right.length) {
        return false;
    }
    for (let i = 0; i < left.length; i += 1) {
        if (left[i] !== right[i]) {
            return false;
        }
    }
    return true;
}

export function decodeDownstream(bytes: Uint8Array): Result<DownstreamFrame, FrameFault> {
    const header = checkHeader(bytes, kMaxDownstreamFrameBytes);
    if (!header.ok) {
        return header;
    }
    const type = bytes[1] ?? 0;
    if (type >= kUpstreamFirst && type <= kUpstreamLast) {
        return fail("frame.direction");
    }
    if (type < kDownstreamFirst || type > kDownstreamLast) {
        return fail("frame.type");
    }
    const frame = decodeBody(type, bytes);
    if (!frame.ok) {
        return frame;
    }
    if (!sameBytes(encodeDownstream(frame.value), bytes)) {
        return fail("frame.canonical");
    }
    return frame;
}

// --- writing --------------------------------------------------------------------

class Writer {
    private readonly bytes: Uint8Array;
    private readonly view: DataView;
    private at = 0;

    constructor(size: number) {
        this.bytes = new Uint8Array(size);
        this.view = new DataView(this.bytes.buffer);
    }

    header(type: number): void {
        this.bytes[this.at] = kFrameVersion;
        this.bytes[this.at + 1] = type;
        this.at += 2;
    }

    raw(value: Uint8Array): void {
        this.bytes.set(value, this.at);
        this.at += value.length;
    }

    u8(value: number): void {
        this.bytes[this.at] = value;
        this.at += 1;
    }

    u16(value: number): void {
        this.view.setUint16(this.at, value);
        this.at += 2;
    }

    i64(value: number): void {
        this.view.setBigInt64(this.at, BigInt(value));
        this.at += 8;
    }

    done(): Uint8Array {
        return this.bytes;
    }
}

// Downstream frames are the server's to write. This exists so that every frame
// this module accepts is shown to be the one encoding of what it read.
function encodeDownstream(frame: DownstreamFrame): Uint8Array {
    switch (frame.type) {
        case "wake": {
            const inline = frame.inline ?? new Uint8Array(0);
            const w = new Writer(kWakeFixedBytes + inline.length);
            w.header(kWake);
            w.raw(frame.conversation.bytes());
            w.i64(frame.seq);
            w.u16(inline.length);
            w.raw(inline);
            return w.done();
        }
        case "typing": {
            const w = new Writer(kTypingBytes);
            w.header(kTyping);
            w.raw(frame.conversation.bytes());
            w.raw(frame.user.bytes());
            return w.done();
        }
        case "presence": {
            const w = new Writer(kPresenceBytes);
            w.header(kPresence);
            w.raw(frame.user.bytes());
            w.u8(frame.state === "online" ? 1 : 0);
            // The re-encode is the one place the instant becomes a number again,
            // and it goes straight back to bytes.
            w.i64(frame.lastSeen.toDate().getTime());
            return w.done();
        }
        case "receipt": {
            const w = new Writer(kReceiptBytes);
            w.header(kReceipt);
            w.raw(frame.conversation.bytes());
            w.raw(frame.user.bytes());
            w.i64(frame.deliveredSeq);
            w.i64(frame.readSeq);
            return w.done();
        }
        case "membership": {
            const w = new Writer(kMembershipBytes);
            w.header(kMembership);
            w.raw(frame.conversation.bytes());
            w.i64(frame.membershipVersion);
            return w.done();
        }
        case "mutation": {
            const w = new Writer(kMutationBytes);
            w.header(kMutation);
            w.raw(frame.conversation.bytes());
            w.i64(frame.mutation);
            return w.done();
        }
        case "ping":
            return headerOnly(kPing);
        case "pong":
            return headerOnly(kPong);
        case "sync":
            return headerOnly(kSync);
    }
}

function headerOnly(type: number): Uint8Array {
    const w = new Writer(kHeaderBytes);
    w.header(type);
    return w.done();
}

// The three frames a client sends. A nil conversation is programmer error: every
// conversation id this library holds came from a decoded answer.
export function encodeUpstream(frame: UpstreamFrame): Uint8Array {
    switch (frame.type) {
        case "client-typing": {
            if (isNil(frame.conversation)) {
                throw new Error("a typing frame names a conversation");
            }
            const w = new Writer(kClientTypingBytes);
            w.header(kClientTyping);
            w.raw(frame.conversation.bytes());
            return w.done();
        }
        case "client-pong":
            return headerOnly(kClientPong);
        case "client-ping":
            return headerOnly(kClientPing);
    }
}
