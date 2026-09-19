// A session payload, built the way anvil builds one.
//
// Everything a wire test needs goes through the real decode rather than around
// it: a SessionView assembled by hand in a test is a SessionView no server could
// have sent, and the assertions built on it hold for a shape that does not
// exist.
//
// That was true of this file and it was not enough. Every one of 1,145 unit
// tests went through the real decode, and all of them went through this builder
// — which built the payload the decode was WRITTEN against rather than the one
// anvil writes. A shared fixture that agrees with the implementation is two
// copies of one belief, and the first live run against the reference
// application is what told them apart (`docs/16-test-plan.md` §Phase 6).
//
// So the shape below is anvil's, key for key: `routes` from
// `append_reachable_routes` and `authority` from `append_holder_authority`.

import type { PermissionBits, SessionView } from "../../src/wire/session_view.js";
import { decodeSessionView } from "../../src/wire/session_view.js";
import { kPermissionBits, kTablesHash } from "../testapp/api/hammer.generated.js";

// The reference descriptor's own hash, so a payload built here says what a
// server talking to this generated client would say. Nothing compares the two —
// a staleness test passes the hash it wants — but a fixture holding the hash of
// a descriptor that no longer exists is one a reader will eventually believe.
//
// Read from the generated module rather than written down, which the sentence
// above had been asking for since it was written: it was a literal, the
// descriptor was refreshed, and the one assertion that compares a payload's hash
// to the client's failed with two hashes and no hint which was stale.
export const kServerHash: string = kTablesHash;

// The reference application's table, so a name a test writes is a name that
// exists. A test that invented one would be asserting against a permission no
// descriptor declares.
export const kBits: PermissionBits = kPermissionBits;

// Bit numbers to the names anvil sends, because anvil sends names. Tests are
// written in bits — a bit is what a screen checks — and this is the one place
// that has to speak both.
export function permNames(...bits: readonly number[]): readonly string[] {
    const out: string[] = [];
    for (const bit of bits) {
        const found = Object.entries(kBits).find(([, value]) => value === bit);
        if (found === undefined) {
            throw new Error(`no permission in the reference descriptor holds bit ${bit}`);
        }
        out.push(found[0]);
    }
    return out;
}

export type PayloadOptions = {
    readonly bits?: readonly number[];
    // Names, for the cases that are about a name rather than about a bit: one
    // this client has no bit for, or one that is not a string at all.
    readonly names?: readonly unknown[];
    readonly routes?: Readonly<Record<string, string>>;
    readonly superadmin?: boolean;
    readonly hash?: string;
    // Deliberately absent by default, because anvil's reference application
    // sends none: putting the descriptor hash on this response is the
    // application controller's job and no anvil writer does it.
    readonly authority?: unknown;
};

export function sessionPayload(options: PayloadOptions = {}): Record<string, unknown> {
    const payload: Record<string, unknown> = {
        routes: options.routes ?? {},
    };

    if (options.authority !== undefined) {
        payload["authority"] = options.authority;
    } else {
        const authority: Record<string, unknown> = {
            perms: options.names ?? permNames(...(options.bits ?? [])),
        };
        if (options.superadmin !== undefined) {
            authority["superadmin"] = options.superadmin;
        }
        payload["authority"] = authority;
    }

    if (options.hash !== undefined) {
        payload["hash"] = options.hash;
    }
    return payload;
}

export function sessionView(options: PayloadOptions = {}): SessionView {
    const decoded = decodeSessionView(sessionPayload(options), kBits);
    if (!decoded.ok) {
        throw new Error(`the test's own session payload does not decode: ${decoded.error}`);
    }
    return decoded.value;
}
