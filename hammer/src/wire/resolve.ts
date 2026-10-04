// Where a route's address comes from, and what happens when there is not one.
//
// Two tiers, and which one a route uses is decided by its visibility and by
// nothing else (`docs/00-architecture.md` §4.2):
//
//   PUBLIC   the method and path are compiled in. They name a route anybody may
//            call, so a bundle carrying them discloses nothing.
//   HOLDER   the method and path arrive with the session, filtered by the server
//            to what this holder's permissions reach. They are in no bundle, no
//            chunk and no source map.
//
// --- the recovery, and why it is shaped like this --------------------------
//
// A route id with no entry is a MISSING ADDRESS, not a refusal. It is the one
// place §4.1's "send it and let the server decide" cannot apply, because there
// is nothing to send it to.
//
// So the recovery is the mechanism that already exists for a stale copy:
// refetch the session, look again, and if the entry is still absent report the
// same shape as everything else that is absent. Exactly one refetch — a second
// is a client that hammers its own session endpoint every time somebody clicks
// a button they do not have.
//
// **Never a permission-flavoured error**, and that is not a matter of care here:
// `ClientError` has no cause meaning "denied", so a permission refusal cannot be
// returned from this function at all. The type is the rule. A client that said
// "forbidden" where the server would have said 404 would rebuild the oracle
// anvil spent a whole design removing, in the one place the server cannot reach.

import type { ClientError, TransportError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

import type { RouteTarget, SessionView } from "./session_view.js";
import { isRoutePath } from "./session_view.js";

// The shape a generated route `const` already has. Structural, so the generated
// module needs no import from this layer and this layer needs no import from a
// generated module — which is what keeps `hammer/wire` free of any application's
// table (`CLAUDE.md` §1).
export type ResolvableRoute = {
    readonly id: string;
    readonly visibility: "public" | "holder";
    readonly method: string | null;
    readonly path: string | null;
};

// Injected rather than owned. The store that holds a session is the thing that
// will dispose it (`CLAUDE.md` §3.3), and a resolver with a store of its own is
// a second opinion about who is signed in.
export type SessionSource = {
    // What this tab holds now, or null before the first fetch and after a
    // logout. Never throws and never fetches: resolution of a public route must
    // not depend on there being a session at all.
    readonly current: () => SessionView | null;

    // The one recovery §4.2 allows, called at most once per resolution. Returns
    // null when there is no session to be had.
    readonly refetch: (signal: AbortSignal) => Promise<SessionView | null>;
};

// Narrower than `HammerError` on purpose. A `ServerError` cannot come out of a
// function that sends nothing, and neither can a denial.
export type ResolveError = ClientError | TransportError;

const kNoRoute: ClientError = { kind: "client", cause: "no-route", retryAfterMs: null };
const kUnusable: ClientError = { kind: "client", cause: "unusable-route", retryAfterMs: null };
const kAborted: TransportError = { kind: "transport", cause: "aborted" };

const kMethods: readonly RouteTarget["method"][] = [
    "GET",
    "HEAD",
    "POST",
    "PUT",
    "PATCH",
    "DELETE",
];

// The compiled tier, re-checked. The generator validated this path before it
// wrote it, so the only thing that reaches here malformed is a route object
// somebody wrote by hand — and one place producing a `RouteTarget` is one place
// to read when asking what this library is willing to send a cookie to.
function compiledTarget(route: ResolvableRoute): Result<RouteTarget, ResolveError> {
    const { method, path } = route;
    if (method === null || path === null) {
        return fail(kNoRoute);
    }
    const known = kMethods.find((candidate) => candidate === method);
    if (known === undefined) {
        return fail(kUnusable);
    }
    if (!isRoutePath(path)) {
        return fail(kNoRoute);
    }
    return ok({ method: known, path });
}

function fromSession(
    view: SessionView | null,
    id: string,
): Result<RouteTarget, ResolveError> | null {
    if (view === null) {
        return null;
    }
    const target = view.routes.get(id);
    if (target !== undefined) {
        return ok(target);
    }
    // Named by the server and uncallable, which is a different fact from absent
    // and is worth reporting as one: retrying it, refetching for it or hiding it
    // are three different responses and only one of them is right.
    if (view.unusable.has(id)) {
        return fail(kUnusable);
    }
    return null;
}

export async function resolveRoute(
    route: ResolvableRoute,
    source: SessionSource,
    signal: AbortSignal,
): Promise<Result<RouteTarget, ResolveError>> {
    if (signal.aborted) {
        return fail(kAborted);
    }

    if (route.visibility === "public") {
        return compiledTarget(route);
    }

    const held = fromSession(source.current(), route.id);
    if (held !== null) {
        return held;
    }

    // One refetch. The local table is a copy and anvil's `perm_epoch` exists
    // because a copy goes stale, so an absent entry is as likely to be a grant
    // made thirty seconds ago as a route this holder will never reach.
    const refreshed = await source.refetch(signal);
    if (signal.aborted) {
        return fail(kAborted);
    }

    const second = fromSession(refreshed, route.id);
    if (second !== null) {
        return second;
    }

    return fail(kNoRoute);
}
