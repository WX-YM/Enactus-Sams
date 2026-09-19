// Whether a button exists. Never whether a request is sent.
//
// The session payload carries the permission set and the holder's route table,
// so a client can answer "may this person do X" with no round trip. That answer
// governs AFFORDANCES — whether a control is rendered, whether a menu entry is
// there at all (`docs/00-architecture.md` §4.1).
//
// It must never refuse a call somebody has managed to make. The local copy is a
// copy, and anvil's `perm_epoch` exists precisely because a copy goes stale: a
// grant made thirty seconds ago is not in it. A client that refuses locally
// turns a stale copy into a permanent denial no server-side change can clear,
// and the bug report it produces is "the button does nothing".
//
// So: hide it locally, and if it is invoked anyway, send it and let the server
// decide. A 403 in return means the copy is stale and is the signal to refetch
// the session, which is what makes the staleness self-healing rather than
// sticky.
//
// **This library provides no way to do the other thing.** `ClientError` has no
// cause meaning "denied", so a local permission refusal cannot be constructed,
// returned or reported anywhere in hammer. That is the rule as a type rather
// than as a review comment.

import type { SessionView } from "./session_view.js";

// The part of a generated route `const` this reads. Structural, so no table
// from any application reaches this layer.
export type AffordableRoute = {
    readonly id: string;
    readonly visibility: "public" | "holder";
};

// Whether this holder reaches this route at all.
//
// Answered from the ROUTE TABLE rather than from the permission bits, and the
// difference is not a shortcut — it is the only version that agrees with the
// server. anvil builds that table with `satisfies()`, the same function its
// request filter calls, superadmin short-circuit included; a bitset check here
// would be a second implementation of "may this holder reach this route", and
// the one that drifts is the one nobody is reading. It would also hide every
// guarded control from a superadmin, whose held set is deliberately NOT
// all-ones so that "is superadmin" and "holds everything" stay distinguishable
// in an audit log.
export function affordsRoute(view: SessionView | null, route: AffordableRoute): boolean {
    if (route.visibility === "public") {
        // Reachable by definition, and excluded from the table for that reason:
        // its path is already in the bundle, and sending it again is bytes on
        // every session response.
        return true;
    }
    if (view === null) {
        return false;
    }
    // Present but uncallable counts as absent here. A control that is rendered
    // and then fails is worse than one that was never rendered, and the fix for
    // it is in the server's route description rather than in this person's day.
    if (view.unusable.has(route.id)) {
        return false;
    }
    return view.routes.has(route.id);
}

// Whether this holder carries every one of these permission bits.
//
// For an affordance that is not a route — a column of personal data, a bulk
// action, a tab in a staff screen. Where the affordance IS a route, use
// `affordsRoute`: it is exact, and this is not.
//
// The bits are the generated `kPerm…` constants, which are numbers. There is no
// permission name in this call and none in a bundle that makes it.
export function holdsAll(view: SessionView | null, bits: readonly number[]): boolean {
    if (view === null) {
        return false;
    }
    // The short-circuit anvil's `satisfies()` makes, and the reason the session
    // payload has to say so: a superadmin's held set does not contain every bit,
    // so a client that only counted bits would hide the whole application from
    // the one account that reaches all of it.
    if (view.superadmin) {
        return true;
    }
    for (const bit of bits) {
        if (!view.permissions.has(bit)) {
            return false;
        }
    }
    return true;
}
