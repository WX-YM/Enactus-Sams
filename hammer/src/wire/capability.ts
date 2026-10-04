// A grant the server issued, carried to the one call that redeems it.
//
// anvil's destructive routes require a capability minted by an EARLIER,
// SEPARATE request and consumed exactly once (`docs/01-seams.md` §6). The scope
// exists so that a destructive action is a second, deliberate act — usually a
// confirmation somebody gave — and the requirement is discharged here by the
// type system rather than by a reviewer: a route declaring a scope takes a
// `Capability<Scope>` in its call options, and there is no way to spell one
// without having made the call that produced it.
//
// --- what the brand is and is not -------------------------------------------
//
// It is not a security control. The token is checked by the server, against a
// digest it stored, bound to a user, a scope and a subject; a forged one matches
// nothing. What the brand buys is the other half — a requirement that cannot be
// FORGOTTEN. The compiler refuses the call, at every call site, on the day the
// server starts requiring one, which is the same trick `Cursor` uses to make an
// offset unspellable.
//
// So the factory is module-private in the sense that matters: it is exported to
// the library and is NOT re-exported from `hammer/wire` (see `src/wire/index.ts`,
// and `cursorFromServer` in the core entry point for the same omission). The
// only thing an application can reach is the client's mint call, which produces
// one from a response the server sent.
//
// --- why it travels in a header ---------------------------------------------
//
// anvil hands its preview capability over in a query string exactly once and
// exchanges it for a path-scoped cookie with a `303` (anvil `docs/19` §6). That
// is a deliberate one-shot, and everything about it says why a URL is the wrong
// place for the general case: a URL is in the address bar, the history, the
// `Referer` and every analytics payload ever built from `location.href`
// (`CLAUDE.md` §5). A header is in none of them, and it is dropped by the
// redacting logger along with every other header.
//
// The name carries no `X-` prefix, per RFC 6648: the prefix was deprecated
// precisely because a header that gets standardised has to be renamed, and a
// rename of a request header is a breaking change for every deployment that
// reads it.

import type { Brand } from "../core/brand.js";
import { brand } from "../core/brand.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

declare const kScopeTag: unique symbol;

// A string, at run time, and nothing more: the brand and the scope are both
// erased, so a capability costs one string and no wrapper object.
//
// The scope rides as an optional phantom for the reason `ErrorVocabulary` uses
// one — it is declared, never defined, and erased — and it is what keeps
// `Capability<"A">` out of a parameter that asked for `Capability<"B">`.
export type Capability<Scope extends string> = Brand<string, "capability"> & {
    readonly [kScopeTag]?: Scope;
};

// The header this token is presented in. Spelled once, here, so that no call
// site spells it and no two spell it differently — the same reason anvil keeps
// `Retry-After` in one constant.
export const kCapabilityHeader = "Capability";

export type CapabilityError =
    // Nothing to present. Reached by a mint call whose response carried no
    // token, which is a server that did not mint one rather than a token that
    // is empty.
    | "no-token"
    // Past the bound below.
    | "too-long"
    // A character that cannot be in a header value: a control character, a
    // newline, anything outside printable ASCII. `fetch` throws on one, and a
    // throw is the wrong report for a value that came off a wire.
    | "not-header-safe";

// A bound rather than a guess at the token's shape. The token is opaque — it is
// a server's, and hammer reads nothing in it — so its CONTENT needs no opinion
// beyond "a header can carry this". Its LENGTH does: a header value of
// unbounded length is a request the server refuses at its own header cap, which
// is a confusing failure for an operation that never had a chance.
const kMaxTokenChars = 512;

// Printable ASCII, which is a strict subset of what a header value may hold.
// Narrow on purpose: nothing anvil mints is outside it, and every character
// this refuses is one that has a second meaning somewhere in the chain.
const kHeaderSafe = /^[\x20-\x7E]+$/;

// The only producer. Exported to the library, never from an entry point.
export function mintCapability<Scope extends string>(
    token: unknown,
): Result<Capability<Scope>, CapabilityError> {
    if (typeof token !== "string" || token.length === 0) {
        return fail("no-token");
    }
    if (token.length > kMaxTokenChars) {
        return fail("too-long");
    }
    if (!kHeaderSafe.test(token)) {
        return fail("not-header-safe");
    }
    return ok(brand<string, "capability">(token) as Capability<Scope>);
}

// The token, for the one line that writes the header. Exported to the library
// and not from an entry point: a public accessor is a capability in a log line
// and in an analytics payload, which is the thing the header was chosen to
// avoid.
export function capabilityToken(capability: Capability<string>): string {
    return capability as string;
}

// Whether redeeming this scope burns the token, read from the generated table
// the application supplies.
//
// It decides one thing and it is not a detail: a call carrying a single-use
// capability is NEVER auto-retried. The server consumed the token whether or
// not the response arrived, so a retry presents a token that is already spent,
// is answered with a capability failure, and reports a failure for an operation
// that succeeded. The recovery is a re-read, not a retry
// (`docs/01-seams.md` §6).
export function burnsOnUse(
    singleUse: Readonly<Record<string, boolean>>,
    scope: string | null,
): boolean {
    if (scope === null) {
        return false;
    }
    // Absent means yes. A scope the table has not heard of is a table that is
    // older than the server, and the direction to be wrong in is the one that
    // retries nothing.
    return singleUse[scope] !== false;
}
