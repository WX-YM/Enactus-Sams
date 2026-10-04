// The one place a response becomes a value.
//
// The vocabulary is built from the reference application's generated tables
// rather than from a list written here, because the pairing between the names
// the decode knows and the unions the call site spells is the thing being
// asserted: a hand-written set would agree with itself and with nothing else.

import { describe, expect, it } from "../support/test.js";

import type { ErrorCode, ValidationReason } from "../testapp/api/hammer.generated.js";
import {
    kErrorCodeValues,
    kValidationReasonValues,
} from "../testapp/api/hammer.generated.js";
import { decodeEnvelope, errorVocabulary } from "../../src/wire/envelope.js";

const kVocabulary = errorVocabulary<ErrorCode, ValidationReason>({
    codes: kErrorCodeValues,
    reasons: kValidationReasonValues,
});

function decode(status: number, body: unknown) {
    return decodeEnvelope({ status, body }, kVocabulary);
}

function envelope(error: Readonly<Record<string, unknown>>): unknown {
    return { error };
}

describe("the vocabulary", () => {
    it("holds every failure name the server has", () => {
        expect(kVocabulary.codes.has("VALIDATION_FAILED")).toBe(true);
        expect(kVocabulary.reasons.has("BAD_FORMAT")).toBe(true);
    });

    it("leaves out the success member, which no error surface can reach", () => {
        expect(kVocabulary.codes.has("OK")).toBe(false);
        expect(kVocabulary.reasons.has("OK")).toBe(false);
    });
});

describe("a response that succeeded", () => {
    it("is the body, undecorated", () => {
        expect(decode(200, { id: "abc" })).toEqual({ ok: true, value: { id: "abc" } });
    });

    it("keeps a body that is a JSON null distinct from a body that is absent", () => {
        expect(decode(200, null)).toEqual({ ok: true, value: null });
    });

    it("accepts no body on a status that carries none", () => {
        for (const status of [204, 205, 304]) {
            expect(decode(status, undefined)).toEqual({ ok: true, value: null });
        }
    });

    // A 200 whose body did not parse is a response this client cannot use, and
    // succeeding with nothing would move the failure to whichever line narrows
    // it next.
    it("fails on a body that should have been there", () => {
        expect(decode(200, undefined)).toEqual({
            ok: false,
            error: {
                kind: "server",
                code: "Unknown",
                status: 200,
                requestId: null,
                fields: null,
            },
        });
    });
});

describe("a failure anvil sent", () => {
    it("decodes the code, the request id and nothing else", () => {
        expect(decode(429, envelope({ code: "RATE_LIMITED", request_id: "01JABC" }))).toEqual({
            ok: false,
            error: {
                kind: "server",
                code: "RATE_LIMITED",
                status: 429,
                requestId: "01JABC",
                fields: null,
            },
        });
    });

    // anvil's 404 body is one constexpr string shared by a stealth drop, an
    // unmatched route and a genuinely missing object, so that all three are
    // byte-identical. It has no request id, and its own suite asserts so.
    it("decodes the stealth 404, which carries no request id at all", () => {
        expect(decode(404, envelope({ code: "NOT_FOUND" }))).toEqual({
            ok: false,
            error: {
                kind: "server",
                code: "NOT_FOUND",
                status: 404,
                requestId: null,
                fields: null,
            },
        });
    });

    it("maps the fields of a validation failure", () => {
        const decoded = decode(
            400,
            envelope({
                code: "VALIDATION_FAILED",
                request_id: "01JABC",
                fields: { email: "BAD_FORMAT", name: "TOO_LONG" },
            }),
        );
        expect(decoded.ok).toBe(false);
        if (decoded.ok) return;
        expect(decoded.error.code).toBe("VALIDATION_FAILED");
        expect(decoded.error.fields?.get("email")).toBe("BAD_FORMAT");
        expect(decoded.error.fields?.get("name")).toBe("TOO_LONG");
    });

    it("keeps an empty field map distinct from no field map", () => {
        const decoded = decode(400, envelope({ code: "VALIDATION_FAILED", fields: {} }));
        expect(decoded.ok).toBe(false);
        if (decoded.ok) return;
        expect(decoded.error.fields?.size).toBe(0);
    });

    // A key named `__proto__` in a plain object is a lookup that answers
    // something nobody put there, and these keys come off the wire. The body is
    // built with JSON.parse rather than as a literal, because a literal sets
    // the prototype and never creates the own property a parsed response does —
    // which is the whole reason the hazard is easy to miss.
    it("puts the fields in a Map, so a key cannot name a prototype", () => {
        const body: unknown = JSON.parse(
            '{"error":{"code":"VALIDATION_FAILED","fields":{"__proto__":"REQUIRED"}}}',
        );
        const decoded = decode(400, body);
        expect(decoded.ok).toBe(false);
        if (decoded.ok) return;
        expect(decoded.error.fields?.get("__proto__")).toBe("REQUIRED");
        expect(decoded.error.fields?.get("toString")).toBeUndefined();
    });
});

describe("a code this bundle predates", () => {
    // The enum is append-only server-side. A decode that threw would turn a
    // deploy into an outage in every tab that was already open.
    it("decodes to the member the application owes a sentence for", () => {
        expect(decode(418, envelope({ code: "TEAPOT_UNAVAILABLE" }))).toMatchObject({
            ok: false,
            error: { code: "Unknown", status: 418 },
        });
    });

    it("keeps the request id of a code it cannot name", () => {
        expect(decode(418, envelope({ code: "TEAPOT", request_id: "01JX" }))).toMatchObject({
            ok: false,
            error: { code: "Unknown", requestId: "01JX" },
        });
    });

    it("does the same for a reason, rather than dropping the field", () => {
        const decoded = decode(
            400,
            envelope({ code: "VALIDATION_FAILED", fields: { email: "SOMETHING_NEW" } }),
        );
        expect(decoded.ok).toBe(false);
        if (decoded.ok) return;
        // Dropping the entry instead would be a form that refuses to submit
        // with nothing marked on it.
        expect(decoded.error.fields?.get("email")).toBe("Unknown");
    });
});

describe("a failure that never reached anvil", () => {
    it("decodes a proxy's HTML page to Unknown, carrying the status", () => {
        expect(decode(502, undefined)).toEqual({
            ok: false,
            error: {
                kind: "server",
                code: "Unknown",
                status: 502,
                requestId: null,
                fields: null,
            },
        });
    });

    it("decodes a body that is not an envelope", () => {
        for (const body of [null, [], "gateway timeout", 0, { detail: "nope" }]) {
            expect(decode(504, body)).toMatchObject({
                ok: false,
                error: { code: "Unknown", status: 504 },
            });
        }
    });

    it("decodes an error member that is not an object", () => {
        expect(decode(500, { error: "INTERNAL" })).toMatchObject({
            ok: false,
            error: { code: "Unknown", status: 500 },
        });
    });
});

describe("a malformed envelope", () => {
    it("never throws, whatever it is handed", () => {
        const bodies: readonly unknown[] = [
            undefined,
            null,
            0,
            "",
            [],
            { error: null },
            { error: [] },
            { error: {} },
            { error: { code: 7 } },
            { error: { code: "NOT_FOUND", request_id: 7 } },
            { error: { code: "VALIDATION_FAILED", fields: [] } },
            { error: { code: "VALIDATION_FAILED", fields: { email: 7 } } },
        ];
        for (const body of bodies) {
            expect(() => decode(400, body)).not.toThrow();
        }
    });

    it("drops a request id that is not one", () => {
        // It is displayed verbatim and copied into a support channel, and it
        // reaches a log line: a control character in it is log injection in the
        // one field that is meant to be safe to echo.
        for (const value of ["", "01J ABC", "01J\nABC", "a".repeat(65), 7, null, {}]) {
            expect(
                decode(500, envelope({ code: "INTERNAL", request_id: value })),
            ).toMatchObject({ ok: false, error: { requestId: null } });
        }
    });

    it("keeps a request id that looks like one", () => {
        for (const value of ["01JABCDEF", "a-b_c", "01J8Z9QK4T7V2WXYZ0ABCDEF12"]) {
            expect(
                decode(500, envelope({ code: "INTERNAL", request_id: value })),
            ).toMatchObject({ ok: false, error: { requestId: value } });
        }
    });

    // A response that is malformed in one place is not one to keep the rest of,
    // which is the rule `session_view.ts` already makes about a session.
    it("drops the whole field map when one value is not a string", () => {
        const decoded = decode(
            400,
            envelope({ code: "VALIDATION_FAILED", fields: { email: "REQUIRED", name: 7 } }),
        );
        expect(decoded).toMatchObject({ ok: false, error: { fields: null } });
    });

    it("drops a field map that has stopped being one", () => {
        const many: Record<string, string> = {};
        for (let i = 0; i < 257; i += 1) {
            many[`f${i}`] = "REQUIRED";
        }
        expect(
            decode(400, envelope({ code: "VALIDATION_FAILED", fields: many })),
        ).toMatchObject({ ok: false, error: { fields: null } });
    });

    it("keeps a field map at the cap", () => {
        const many: Record<string, string> = {};
        for (let i = 0; i < 256; i += 1) {
            many[`f${i}`] = "REQUIRED";
        }
        const decoded = decode(400, envelope({ code: "VALIDATION_FAILED", fields: many }));
        expect(decoded.ok).toBe(false);
        if (decoded.ok) return;
        expect(decoded.error.fields?.size).toBe(256);
    });
});

describe("the decoded code is a literal type", () => {
    it("narrows to the generated union rather than to string", () => {
        const decoded = decode(403, envelope({ code: "FORBIDDEN" }));
        if (decoded.ok) {
            throw new Error("the fixture is a failure");
        }
        // The assignment is the assertion: it does not compile unless the
        // vocabulary's type parameters reached the decode. A `string` code
        // would be the whole mechanism quietly not working.
        const code: ErrorCode = decoded.error.code;
        const fields: ReadonlyMap<string, ValidationReason> | null = decoded.error.fields;
        expect(code).toBe("FORBIDDEN");
        expect(fields).toBeNull();
    });
});
