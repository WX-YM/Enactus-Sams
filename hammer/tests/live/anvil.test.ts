// The runs that only a live server can make, against `anvil_reference_server`.
//
// It is excluded from `npm run check` — `npm run test:live` is the only thing
// that runs it, and it fails rather than skips when `HAMMER_LIVE_ORIGIN` is
// absent, because a live suite that quietly passes with no server is a suite
// whose green means nothing.
//
// --- what the first execution found ------------------------------------------
//
// `decodeSessionView` was written against a payload anvil does not send, and
// 1,145 unit tests passed over it (`tests/live/harness.ts`). That is the entire
// justification for this file existing.
//
// --- what still cannot be written, and why ----------------------------------
//
// The phase-6 row asks for an upload, an SSE delivery and a versioned write.
// The reference application has no upload route, no stream route reachable over
// HTTP — `live.feed` is a WebSocket upgrade — and no write at all past
// `auth.login`, `auth.refresh` and `auth.logout`. So `wire/upload.ts` and
// `wire/sse.ts` are still driven by the unit suites and by nothing else, and the
// versioned case below asserts the half that IS reachable: a write with no
// version read is refused rather than sent. Saying so here is the honest version
// of rows that would otherwise be quietly dropped.
//
// --- the shape of what is left ----------------------------------------------
//
// Every case is about a claim this library makes that a unit suite can only
// assert against its own stand-in:
//
//   the credential   is a cookie hammer cannot read, and the jar proves the
//                    absence rather than the presence;
//   the table        is scoped per holder by the server, which needs two holders
//                    to be visible as a scoping at all;
//   a 404            is not a permission error, on a stealth route that is
//                    byte-identical to one that does not exist;
//   the envelope     carries the code and the request id anvil's writer produces;
//   the response     is the shape the descriptor declared and the binder wrote.

import { afterAll, beforeAll, describe, expect, it } from "vitest";

import { writeVersioned } from "../../src/state/versioned.js";
import {
    kPermAuditRead,
    kPermContentRead,
    routeAuditList,
    routeContentDelete,
    routeContentGet,
    routeIdentityMe,
    routeMediaList,
} from "../testapp/api/hammer.generated.js";
import { signIn, signOut } from "../testapp/app/state.js";
import type { LiveRun } from "./harness.js";
import { liveCredentials, liveRun, liveSuperadmin, signal } from "./harness.js";

let run: LiveRun;

beforeAll(() => {
    run = liveRun();
});

afterAll(() => {
    run?.close();
});

describe("a live anvil", () => {
    // The property every unit test passes without: there is no token in a
    // variable anywhere, because the credential is a cookie this client cannot
    // read. What a Node run can see is the absence — the jar has entries and
    // hammer never asked for them.
    it("signs in without the client ever seeing a credential", async () => {
        const answered = await signIn(run.state, liveCredentials(), signal());

        expect(answered.ok).toBe(true);
        expect(run.jar.count).toBeGreaterThan(0);

        // The session is read through the same store an application reads it
        // through, and the holder route table arrives with it — which is the
        // only way any of the calls below have an address at all.
        const view = await run.state.session.load(signal());
        expect(view).not.toBeNull();
        expect(view?.routes.size).toBeGreaterThan(0);
    });

    // A holder route, whose path was never in the bundle. If this fails with
    // `unusable-route` the server sent a table this client cannot use; if it
    // fails with a 404 the object is missing OR forbidden, and the client is not
    // entitled to know which.
    it("calls a route whose address arrived with the session", async () => {
        const answered = await run.state.api.call(routeContentGet, {
            params: { id: process.env["HAMMER_LIVE_CONTENT_ID"] ?? "1" },
            signal: signal(),
        });

        if (!answered.ok) {
            // A 404 is not a permission error, and this suite must not report it
            // as one (`ENGINEERING_RULES.md` §5).
            expect(answered.error.kind).toBe("server");
        }
        expect(answered.ok || answered.error.kind === "server").toBe(true);
    });

    // The table is a PROJECTION, and that is only visible as one from the side
    // that was not given the rest of it.
    //
    // anvil builds this with the same `satisfies()` its request filter calls, so
    // the table cannot drift from the decision — and what arrives here is this
    // holder's map rather than the whole one. The editor holds `ContentRead` and
    // nothing else, so `content.get` has an address and `media.list` and
    // `audit.list` have none, path and all.
    it("is handed this holder's routes and not the whole table", async () => {
        const view = run.state.session.current();
        expect(view).not.toBeNull();
        if (view === null) return;

        expect(view.routes.has("content.get")).toBe(true);
        expect(view.routes.get("content.get")?.path).toBe("/content/{id}");

        // Not "present and refused" — ABSENT. A path this holder cannot reach is
        // not in the response at all, which is the whole of docs/01-seams.md
        // §4.1 observed from the client side.
        expect(view.routes.has("media.list")).toBe(false);
        expect(view.routes.has("audit.list")).toBe(false);

        expect(view.permissions.has(kPermContentRead)).toBe(true);
        expect(view.permissions.has(kPermAuditRead)).toBe(false);
        expect(view.superadmin).toBe(false);

        // Every name the server sent mapped to a bit this client knows. A name
        // here would mean the server is newer than this bundle, which is worth
        // failing a contract run over even though it must never fail a session.
        expect([...view.unknownPermissions]).toEqual([]);

        // And the affordance answer, which is what a screen actually asks. It
        // reads the table rather than counting bits, because the server built
        // the table with the function that makes the decision.
        expect(run.state.session.affords(routeContentGet)).toBe(true);
        expect(run.state.session.affords(routeAuditList)).toBe(false);
        expect(run.state.session.affords(routeMediaList)).toBe(false);
    });

    // A stealth route, from a holder who does not reach it.
    //
    // The client cannot call it at all: the address is not in the table, so the
    // failure is local. What matters is the SHAPE of that failure — it is a
    // missing address and never a denial, because a client that said "forbidden"
    // where the server would have said 404 rebuilds the oracle anvil spent a
    // whole design removing, in the one place the server cannot reach.
    it("reports a route it has no address for as absent, never as forbidden", async () => {
        const answered = await run.state.api.call(routeAuditList, {
            query: { limit: 10 },
            signal: signal(),
        });

        expect(answered.ok).toBe(false);
        if (answered.ok) return;
        expect(answered.error.kind).toBe("client");
        if (answered.error.kind !== "client") return;
        // `ClientError` has no cause meaning "denied", so this is the type
        // making the rule rather than this assertion making it.
        expect(answered.error.cause).toBe("no-route");
    });

    // The envelope, as anvil's one writer produces it.
    //
    // `request_id` is the half that was missing for the whole of phase 6: the
    // architecture document published `{"error":{"code","request_id","fields"}}`
    // from phase 0 and no writer in anvil produced the second key. It does now,
    // and this is the only place that can say so — a contract suite asserts what
    // was recorded, and a recording is a copy of what somebody believed.
    it("decodes the error envelope anvil writes, request id and all", async () => {
        // A public route with no capability to spend. The server answers 404
        // through `append_error_body`, which is the same writer the access
        // filter uses.
        const answered = await run.state.api.call(routeContentGet, {
            params: { id: "does-not-exist" },
            signal: signal(),
        });

        expect(answered.ok).toBe(false);
        if (answered.ok || answered.error.kind !== "server") return;
        expect(answered.error.code).toBe("NOT_FOUND");
        expect(answered.error.status).toBe(404);

        // Null, and it is not a defensive default. anvil's 404 body is ONE
        // constexpr string shared by a stealth drop, an unmatched route and a
        // genuinely missing object, so that all three are byte-identical — and a
        // shared constant cannot carry a per-request id. Asserting the null is
        // asserting the stealth property from the client side.
        expect(answered.error.requestId).toBeNull();
        expect(answered.error.fields).toBeNull();
    });

    // The response shape the descriptor declares, against the handler that was
    // compiled against it.
    //
    // This is the response-binder seam end to end: anvil's `static_assert`s hold
    // the handler to `kMeResponse`, the emitter writes that schema into the
    // descriptor, the generator turns it into `RouteResponses["identity.me"]`,
    // and this call is typed by it. Nothing in the chain is a copy of anything
    // else in it.
    it("answers a described route with the shape it was described with", async () => {
        const answered = await run.state.api.call(routeIdentityMe, { signal: signal() });

        expect(answered.ok).toBe(true);
        if (!answered.ok) return;

        const me = answered.value;
        // Typed, not narrowed: the fields below are `UuidText`, `string` and
        // `readonly string[] | null` because the descriptor said so. A field
        // spelled wrong here is a compile error.
        expect(me.id).toMatch(/^[0-9a-f-]{36}$/);
        expect(me.session_id).toMatch(/^[0-9a-f-]{36}$/);
        expect(me.locale.length).toBeGreaterThan(0);
        // Staff, so a list rather than the null a superadmin gets.
        expect(me.permissions).toEqual(["ContentRead"]);
    });

    // A read-modify-write with no version read, against a real server that
    // really does answer 404.
    //
    // The RECONCILIATION half of this row cannot be driven here and the reason
    // is at the top of this file: the reference application has no write route
    // and nothing behind `content.get`, so there is no document to read a
    // version from and no `VERSION_MISMATCH` to be told about. What CAN be
    // driven is the half that is this library's own and that a unit suite
    // asserts against a resource the test filled: that a write with no version
    // is REFUSED rather than sent.
    //
    // It is worth a live case rather than only a unit one because the failure it
    // prevents is silent. A version nothing read is an unconditional overwrite —
    // the lost update the whole mechanism exists to detect, sent deliberately —
    // and a `send` that is never called is the only observable difference.
    it("refuses a versioned write it has no version for, and sends nothing", async () => {
        const id = process.env["HAMMER_LIVE_CONTENT_ID"] ?? "1";
        const resource = run.state.resources.open(routeContentGet, {
            params: { id },
            class: "document",
        });

        try {
            // A 404 is not a permission error and is not reported as one: the
            // object is missing OR forbidden and this client is not entitled to
            // know which (`ENGINEERING_RULES.md` §5).
            const ready = await resource.ready(signal());
            expect(ready.status).toBe("failed");

            let sent = 0;
            const outcome = await writeVersioned({
                resource,
                versionOf: (document) => document.version,
                send: async (version, inner) => {
                    sent += 1;
                    return await run.state.api.call(routeContentGet, {
                        params: { id },
                        body: { title: "live", version },
                        signal: inner,
                    });
                },
                signal: signal(),
            });

            expect(outcome.kind).toBe("no-version");
            expect(sent).toBe(0);
        } finally {
            resource.release();
        }
    });

    // A capability is a second, deliberate act by a person, single-use at the
    // server. The client's part is not retrying it: a replay must be refused by
    // anvil and must not be re-sent by this client on its own.
    it("spends a capability once and does not replay it", async () => {
        const id = process.env["HAMMER_LIVE_CONTENT_ID"] ?? "1";
        const minted = await run.state.api.mint<typeof routeContentGet, "ContentDelete">(
            routeContentGet,
            { params: { id }, signal: signal() },
            (body) => (body as { readonly capability?: unknown }).capability,
        );

        if (!minted.ok) {
            // No capability was issued, so there is nothing to spend. The
            // property that matters here is the compiler's and has already been
            // discharged: `routeContentDelete` cannot be called without a
            // `Capability<"ContentDelete">`, and the only producer of one is
            // this mint — so a run that stops here has asserted it by being
            // unable to continue.
            return;
        }

        const first = await run.state.api.call(routeContentDelete, {
            params: { id },
            capability: minted.value,
            signal: signal(),
        });
        expect(first.ok || first.error.kind === "server").toBe(true);

        const replay = await run.state.api.call(routeContentDelete, {
            params: { id },
            capability: minted.value,
            signal: signal(),
        });
        expect(replay.ok).toBe(false);
    });
});

// A second session, because signing out ends the one every case above shares.
describe("signing out", () => {
    it("ends the session at the server and not only in this tab", async () => {
        const ending = liveRun();
        try {
            expect((await signIn(ending.state, liveCredentials(), signal())).ok).toBe(true);
            expect(await ending.state.session.load(signal())).not.toBeNull();

            // The route first, then the local clear — see `signOut`. This is
            // the whole reason the reference consumer has that function: a
            // sign-out that only clears locally leaves a session anvil is still
            // holding, and every unit test of the store passes while it does.
            const answered = await signOut(ending.state, signal());
            expect(answered.ok).toBe(true);

            // The store is emptied locally the moment logout is called, which is
            // the part a unit suite asserts. What needs a server is the other
            // half: the session is gone at anvil too, so a read with the same
            // jar — cookies cleared by the response — is anonymous rather than
            // active.
            const after = await ending.state.session.load(signal());
            expect(after).toBeNull();
            expect(ending.state.session.identity()).toBeNull();
            expect(ending.state.session.store.get().status).toBe("anonymous");

            // The cookies the server cleared are gone from the jar. This is the
            // weakest form of the claim and it is stated as such: a jar is not a
            // browser, and `__Host-` scoping and `HttpOnly` are enforcement no
            // jar performs. `tests/browser/credentials.test.ts` is where that
            // half is asserted.
            expect(ending.jar.names().filter((name) => name.startsWith("__Host-"))).toEqual([]);
        } finally {
            ending.close();
        }
    });
});

// The account whose permission set is deliberately not all-ones.
//
// `satisfies()` short-circuits on user type, so a superadmin reaches everything
// while holding zero bits — and a client that rendered non-route affordances by
// counting bits would hide the whole application from exactly that account. This
// is the defect `append_holder_authority` was added to close, observed from the
// side that was suffering it.
describe("a superadmin", () => {
    const who = liveSuperadmin();

    it.runIf(who !== null)("reaches every route while holding no permission bit", async () => {
        if (who === null) return;
        const root = liveRun();
        try {
            expect((await signIn(root.state, who, signal())).ok).toBe(true);
            const view = await root.state.session.load(signal());
            expect(view).not.toBeNull();
            if (view === null) return;

            // Zero bits. Not a mistake and not an empty response: anvil declines
            // to send `~PermSet{}` precisely so that "is superadmin" and "holds
            // every permission" stay distinguishable in an audit log.
            expect(view.permissions.isEmpty()).toBe(true);
            expect(view.superadmin).toBe(true);
            // And `holds` answers TRUE for a bit this account does not have,
            // because it reads the flag rather than counting: that short-circuit
            // is the one `satisfies()` makes server-side, and a client without
            // it hides every non-route affordance from the account that reaches
            // all of them.
            expect(root.state.session.holds([kPermAuditRead])).toBe(true);

            // And the route table is complete anyway, because the SERVER built
            // it with `satisfies()` rather than by counting the bits it sent.
            // The two answers disagree, and that is the point: the table is the
            // one a route affordance uses.
            expect(view.routes.has("audit.list")).toBe(true);
            expect(view.routes.has("media.list")).toBe(true);
            expect(root.state.session.affords(routeAuditList)).toBe(true);
        } finally {
            root.close();
        }
    });

    it.runIf(who !== null)("is answered with null permissions rather than an empty list", async () => {
        if (who === null) return;
        const root = liveRun();
        try {
            expect((await signIn(root.state, who, signal())).ok).toBe(true);
            const answered = await root.state.api.call(routeIdentityMe, { signal: signal() });
            expect(answered.ok).toBe(true);
            if (!answered.ok) return;

            // The reason the field is declared nullable in the descriptor. An
            // empty list would read as "holds no permissions", which is the
            // opposite of true for this account — and a client typed from a
            // schema that did not carry `nullable` would have crashed here on a
            // field it was told it could trust.
            expect(answered.value.permissions).toBeNull();
        } finally {
            root.close();
        }
    });
});
