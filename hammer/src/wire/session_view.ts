// What a session response tells this tab, decoded once.
//
// It carries the two things a client cannot be given at build time: the 128-bit
// permission set, and the paths of the routes this holder reaches. The second is
// the whole of `docs/01-seams.md` §4.1 — a privileged route's path is in no
// bundle, no chunk and no source map, because a lazily loaded chunk is a public
// URL and splitting one was never access control. It arrives here instead,
// filtered by the server to what this holder's permissions actually reach, which
// is better than any split: the filtering is done by the only participant an
// attacker does not own, and it is per holder rather than per audience, so a
// compromised account yields that account's map rather than the whole one.
//
// --- this decode is a trust boundary, and it is the interesting one ---------
//
// Every path below becomes the path of an authenticated request. A response
// that is malformed, truncated, corrupted by an intermediary or served by
// something that is not the server therefore reaches further than most: it
// chooses where the session's cookies are sent. So the paths are validated
// rather than trusted, and the check that matters is the one nobody writes —
// `//evil.example` is a protocol-relative URL, and `new URL("//evil.example",
// origin)` is not a path on `origin` at all. It is a different host.
//
// Nothing here is persisted. The payload is memory-only and dies with the tab
// (`docs/00-architecture.md` §7): it is scoped to one person, and writing it
// anywhere a second person could read is the defect the whole cache model
// exists to avoid.
//
// --- the envelope, and the one this file used to expect ---------------------
//
// This decode was written against a payload carrying `perms` as 24 base64url
// characters, `hash`, `superadmin` and `routes`, all at the root. anvil ships
// something else, and the first live run against the reference application is
// what found it — 1,145 unit tests passed over it, because every one of them
// built its payload with the same builder this file was written against.
//
//     {"routes":{"content.get":"GET /content/{id}"},
//      "authority":{"superadmin":false,"perms":["ContentRead"]}}
//
// Two keys, written by `append_reachable_routes` and `append_holder_authority`,
// which are the two functions anvil publishes for this response. `perms` is a
// list of NAMES in bit order rather than a mask, and the reasons are anvil's
// own: `for_each_name` skips the reserved gaps between an application's blocks,
// so a bit nothing names never reaches a client as a control that authorises
// nothing; and a superadmin's set is deliberately not all-ones, so sending
// `~PermSet{}` would put the very conflation `core/types.h` keeps apart onto
// the wire for every client to un-conflate.
//
// So the names are mapped back to bits HERE, through a table the application
// supplies from its generated module, and what this library holds is still the
// 16 bytes `ENGINEERING_RULES.md` §2.3 asks for. The alternative — keeping the names and
// comparing strings on every check — is the thing that rule exists to prevent.
//
// The table is the application's and not hammer's for the usual reason, and it
// has a bundle consequence worth stating: a chunk that decodes a session ships
// every permission NAME, because nothing can map a name to a bit without the
// map. A chunk that does not decode one ships none, which is why the table is a
// separate generated `const` rather than a member of `kApiTables`.

import { PermSet } from "../core/perm_set.js";
import type { StaleClientError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

// The methods anvil's `method_name()` can emit, less `ANY`. HEAD is here because
// the descriptor can express it even though anvil's own table does not, and a
// method a generated client can spell must be one a resolved route can carry.
export type HttpMethod = "GET" | "HEAD" | "POST" | "PUT" | "PATCH" | "DELETE";

const kMethods: readonly HttpMethod[] = ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE"];

// A route pattern and nothing that is not one: the unreserved characters, the
// separator, and the braces a parameter is written in. A scheme cannot be
// spelled because `:` is absent, and a backslash cannot because some URL
// parsers read one as a separator and others do not.
const kRoutePattern = /^\/[A-Za-z0-9\-._~/{}]*$/;

const kSha256 = /^[0-9a-f]{64}$/;

// A holder reaches what their permissions reach, which is bounded by the
// server's route table. The cap is not that number — it is the point past which
// a response has stopped being a route table, and a tab that allocates from a
// response it has not finished checking is a tab somebody can grow.
const kMaxRoutes = 4096;

export type RouteTarget = {
    readonly method: HttpMethod;
    readonly path: string;
};

export type SessionView = {
    readonly permissions: PermSet;

    // Keyed by route id. A Map rather than an object because the keys come off
    // the wire, and a key named `__proto__` in a plain object is a lookup that
    // answers something nobody put there.
    readonly routes: ReadonlyMap<string, RouteTarget>;

    // anvil distinguishes "is superadmin" from "holds every permission" on
    // purpose, so the two stay apart in an audit log and no bit-fiddling
    // accident can synthesise the first (`accesscontrol/decision.h`). The route
    // table above already accounts for it — the server built it with the same
    // `satisfies()` the request filter calls — so this is only consulted by a
    // permission check that is not about a route. False when the server does not
    // say, which hides more rather than less.
    readonly superadmin: boolean;

    // Ids the server named with a method that is not one. They are kept rather
    // than dropped so that resolving one fails loudly and specifically, the way
    // anvil's `method_name()` says it must — and kept OUT of `routes` so that
    // nothing can reach a target there is no method for.
    //
    // They do not fail the decode. A real route table carries an `ANY` entry the
    // day one description forgets a method, and discarding the session over it
    // would log a person out of an application whose only defect is a logout
    // route nobody can call.
    readonly unusable: ReadonlySet<string>;

    // Permission names the server sent that this client has no bit for, which is
    // a server newer than the bundle. Kept for the same reason `unusable` is and
    // dropped from the set for the same reason: failing the decode would sign
    // somebody out on every deploy that adds a permission, and inventing a bit
    // for a name is inventing an authority.
    //
    // A screen that cares can say "this build is missing something" rather than
    // silently hiding a control. `stale` on the session store is the other half
    // of that answer and usually the one to render.
    readonly unknownPermissions: ReadonlySet<string>;

    // The descriptor hash the server was built from. Surfaced, never acted on.
    //
    // Null where the response carried none. anvil publishes no writer for it —
    // putting it on the session response is the application controller's job,
    // using the constant its generated module exports — and the reference
    // application does not, so a client talking to it can make no staleness
    // claim at all. Null rather than an empty string, because "not told" and
    // "told nothing" are different and only one of them is a malformed payload.
    readonly serverHash: string | null;
};

export type SessionDecodeError =
    | "not-an-object"
    | "bad-authority"
    | "bad-permissions"
    | "bad-routes"
    | "too-many-routes"
    | "bad-route-target"
    | "unusable-method"
    | "bad-hash";

// A permission name to the bit anvil stores it in, from the application's
// generated module. Every name the server can send is a key.
//
// A plain object rather than a `Map`, because the generated module is an object
// literal with no call in it (`ENGINEERING_RULES.md` §2.1) and a `Map` needs a `new` at
// module scope. The keys are read with `Object.hasOwn` rather than by lookup:
// the names come off the wire, and `bits["toString"]` on a plain object answers
// a function nobody put there.
export type PermissionBits = Readonly<Record<string, number>>;

// Splits `"POST /content/{id}"` — the shape `append_reachable_routes()` writes,
// one string rather than an object because a client needs both halves and never
// one, and two-field objects are three bytes of punctuation per route on a
// response sent to every signed-in tab.
//
// `ANY` is refused rather than guessed at. It is not a method: it is a
// declaration that every method shares one policy, and anvil emits it precisely
// so that a client fails loudly on a description that forgot to name one instead
// of defaulting to GET and silently calling the wrong thing.
export function parseRouteTarget(value: string): Result<RouteTarget, SessionDecodeError> {
    const space = value.indexOf(" ");
    if (space <= 0) {
        return fail("bad-route-target");
    }

    const method = value.slice(0, space);
    const path = value.slice(space + 1);

    const known = kMethods.find((candidate) => candidate === method);
    if (known === undefined) {
        return fail(method === "ANY" ? "unusable-method" : "bad-route-target");
    }
    if (!isRoutePath(path)) {
        return fail("bad-route-target");
    }
    return ok({ method: known, path });
}

// A path this library is willing to send a session's cookies to.
export function isRoutePath(path: string): boolean {
    // A leading pair of separators is a protocol-relative URL, and resolving one
    // against an origin produces a request to somebody else's host. It is the
    // one malformed path that does not fail — it succeeds, somewhere else.
    if (path.includes("//")) {
        return false;
    }
    return kRoutePattern.test(path);
}

function asObject(value: unknown): Readonly<Record<string, unknown>> | null {
    if (typeof value !== "object" || value === null || Array.isArray(value)) {
        return null;
    }
    return value as Readonly<Record<string, unknown>>;
}

// Fails closed. A malformed payload is no session rather than a partial one: a
// tab that believes it knows which routes a person reaches, having dropped the
// half it could not read, is a tab whose blank screen nobody can explain. The
// one exception is a route whose METHOD is unusable, which is that route's
// defect and is carried as such.
export function decodeSessionView(
    body: unknown,
    bits: PermissionBits,
): Result<SessionView, SessionDecodeError> {
    const root = asObject(body);
    if (root === null) {
        return fail("not-an-object");
    }

    // `authority` is the key `append_holder_authority` writes and it is
    // required, because its absence is not "this holder holds nothing" — it is a
    // response that did not come from the writer, and treating it as an empty
    // set would hand every holder a screen with every non-route affordance
    // missing and nothing to explain it.
    const authority = asObject(root["authority"]);
    if (authority === null) {
        return fail("bad-authority");
    }

    const names = authority["perms"];
    if (!Array.isArray(names)) {
        return fail("bad-permissions");
    }
    const held: number[] = [];
    const unknownPermissions = new Set<string>();
    for (const name of names) {
        // A name that is not a string is a malformed response rather than a
        // permission nobody knows, and the two must not be conflated: the first
        // means the body is not what it claims and the second is an ordinary
        // consequence of deploying the server first.
        if (typeof name !== "string") {
            return fail("bad-permissions");
        }
        if (!Object.hasOwn(bits, name)) {
            unknownPermissions.add(name);
            continue;
        }
        const bit = bits[name];
        if (typeof bit !== "number") {
            unknownPermissions.add(name);
            continue;
        }
        held.push(bit);
    }
    // `of` ignores a bit outside the 128, which is the right answer for a table
    // an application supplied: a bit nothing can store is a bit nothing holds.
    const permissions = PermSet.of(...held);

    // Absent is allowed and means "no claim". anvil publishes no writer that
    // puts the descriptor hash on this response, so a client talking to the
    // reference application has nothing to compare against — and refusing the
    // session over it would make a staleness HINT into a sign-out.
    const rawHash = root["hash"];
    if (rawHash !== undefined && (typeof rawHash !== "string" || !kSha256.test(rawHash))) {
        return fail("bad-hash");
    }
    const hash = rawHash === undefined ? null : rawHash;

    const rawRoutes = asObject(root["routes"]);
    if (rawRoutes === null) {
        return fail("bad-routes");
    }

    const entries = Object.entries(rawRoutes);
    if (entries.length > kMaxRoutes) {
        return fail("too-many-routes");
    }

    const routes = new Map<string, RouteTarget>();
    const unusable = new Set<string>();
    for (const [id, value] of entries) {
        if (typeof value !== "string") {
            return fail("bad-route-target");
        }
        const target = parseRouteTarget(value);
        if (!target.ok) {
            // A method that is not one is this route's problem. Anything else is
            // a malformed response, and a response that is malformed in one
            // place is not one to keep the rest of.
            if (target.error === "unusable-method") {
                unusable.add(id);
                continue;
            }
            return fail(target.error);
        }
        routes.set(id, target.value);
    }

    // Absent means false. A server that has not been taught to say so hides an
    // affordance it could have shown, which is the direction to be wrong in.
    const superadmin = authority["superadmin"] === true;

    return ok({
        permissions,
        routes,
        unusable,
        unknownPermissions,
        superadmin,
        serverHash: hash,
    });
}

// The bundle is older than the server it is talking to. It is returned rather
// than acted on: an automatic reload discards whatever the user had typed, on
// the deploy most likely to be happening during working hours
// (`docs/00-architecture.md` §7.1).
export function staleClient(view: SessionView, clientHash: string): StaleClientError | null {
    // No hash is no claim. A response that did not carry one says nothing about
    // which tables the server was built from, and reporting every such session
    // as stale would put a "this page is out of date" banner on every screen of
    // an application whose controller has not been taught to send it.
    if (view.serverHash === null || view.serverHash === clientHash) {
        return null;
    }
    return { kind: "stale-client", serverHash: view.serverHash, clientHash };
}
