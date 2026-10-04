// Whether a button exists. The other half — that this cannot decide whether a
// request is SENT — is asserted at the end, and it is asserted as a type,
// because a rule about what a caller can express is only kept by the compiler.

import { describe, expect, it } from "../support/test.js";

import type { ClientError, HammerError } from "../../src/core/errors.js";
import { affordsRoute, holdsAll } from "../../src/wire/affordance.js";
import { sessionView } from "../support/session.js";

const kPublic = { id: "auth.login", visibility: "public" } as const;
const kAudit = { id: "audit.list", visibility: "holder" } as const;
const kDelete = { id: "content.delete", visibility: "holder" } as const;

// The generated constants, which are numbers. No permission name appears in this
// file's idea of a permission, and none appears in a bundle that checks one.
const kPermAuditRead = 25;
const kPermFormPii = 18;

describe("an affordance for a route", () => {
    const staff = sessionView({
        bits: [kPermAuditRead],
        routes: { "audit.list": "GET /audit" },
    });

    it("is shown when the server put the route in this holder's table", () => {
        expect(affordsRoute(staff, kAudit)).toBe(true);
    });

    it("is hidden when it did not", () => {
        // The table was built by the server with the same satisfies() its
        // request filter calls, so this answer cannot disagree with the filter's.
        expect(affordsRoute(staff, kDelete)).toBe(false);
    });

    it("is hidden when there is no session", () => {
        expect(affordsRoute(null, kAudit)).toBe(false);
    });

    it("is shown for a public route, session or not", () => {
        // Reachable with no credential at all, and excluded from the table for
        // exactly that reason.
        expect(affordsRoute(null, kPublic)).toBe(true);
        expect(affordsRoute(staff, kPublic)).toBe(true);
    });

    it("is hidden for a route the server named but nothing can call", () => {
        // A control that is rendered and then fails is worse than one that was
        // never rendered.
        const broken = sessionView({ routes: { "audit.list": "ANY /audit" } });
        expect(affordsRoute(broken, kAudit)).toBe(false);
    });

    it("does not consult the permission bits", () => {
        // A holder whose bits say yes and whose table says no is a holder the
        // server will refuse. The table is the server's own answer; the bits are
        // a second implementation of it, and the one that drifts is the one
        // nobody is reading.
        const bitsOnly = sessionView({ bits: [kPermAuditRead], routes: {} });
        expect(affordsRoute(bitsOnly, kAudit)).toBe(false);
    });
});

describe("an affordance that is not a route", () => {
    it("is shown when every bit is held", () => {
        const view = sessionView({ bits: [kPermFormPii, kPermAuditRead] });
        expect(holdsAll(view, [kPermFormPii])).toBe(true);
        expect(holdsAll(view, [kPermFormPii, kPermAuditRead])).toBe(true);
    });

    it("is hidden when one is missing", () => {
        const view = sessionView({ bits: [kPermFormPii] });
        expect(holdsAll(view, [kPermFormPii, kPermAuditRead])).toBe(false);
    });

    it("asks nothing of a holder when the list is empty", () => {
        expect(holdsAll(sessionView(), [])).toBe(true);
    });

    it("is hidden when there is no session", () => {
        expect(holdsAll(null, [])).toBe(false);
        expect(holdsAll(null, [kPermFormPii])).toBe(false);
    });

    it("shows everything to a superadmin, whose bits do not say so", () => {
        // anvil keeps "is superadmin" and "holds every permission"
        // distinguishable on purpose, so a superadmin's held set is NOT
        // all-ones. A client that only counted bits would hide the whole
        // application from the one account that reaches all of it.
        const root = sessionView({ bits: [], superadmin: true });
        expect(root.permissions.isEmpty()).toBe(true);
        expect(holdsAll(root, [kPermFormPii, kPermAuditRead])).toBe(true);
    });

    it("hides rather than shows when the server does not say", () => {
        expect(sessionView({ bits: [] }).superadmin).toBe(false);
        expect(holdsAll(sessionView({ bits: [] }), [kPermFormPii])).toBe(false);
    });
});

describe("what this gate is given no way to do", () => {
    it("cannot refuse a call", () => {
        // §4.1: hide it locally, but if it is invoked, send it and let the
        // server decide. A client that refuses locally turns a stale copy into a
        // permanent denial no server-side change can clear, and the bug report
        // it produces is "the button does nothing".
        //
        // The rule is kept by the error type rather than by care: there is no
        // cause in the library meaning "denied here", so a local refusal cannot
        // be constructed, returned or reported anywhere in it.
        // @ts-expect-error no ClientError says a permission refused this.
        const denied: ClientError = { kind: "client", cause: "denied", retryAfterMs: null };
        // @ts-expect-error and no other member of the union says it either.
        const elsewhere: HammerError = { kind: "permission", cause: "forbidden" };
        expect([denied, elsewhere]).toHaveLength(2);
    });

    it("returns a boolean and nothing a request path could want", () => {
        // It answers one question. It hands back no route, no target and no
        // token, so a call site cannot accidentally build a request out of the
        // answer to "should this be visible".
        expect(typeof affordsRoute(null, kAudit)).toBe("boolean");
        expect(typeof holdsAll(null, [])).toBe("boolean");
    });
});
