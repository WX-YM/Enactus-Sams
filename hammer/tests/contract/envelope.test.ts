// What anvil writes, decoded by what hammer generated.
//
// Every other suite in this repository asserts hammer against hammer: a fetch
// stand-in answers what the test wrote, and the decode agrees with it because
// the test is the one who made it up. This suite is the one place the other
// participant is present — its tables, recorded out of its own headers
// (`tools/record-envelopes.sh`), and its descriptor, emitted by its own binary.
//
// What can actually disagree is narrow and expensive:
//
//   A CODE anvil APPENDS. `ErrorCode` is append-only server-side and the union
//   here is generated from a descriptor. A client whose vocabulary is missing a
//   code decodes it to `Unknown` — which is correct behaviour and a silent
//   downgrade of every surface that had a sentence for it.
//
//   A STATUS THAT MOVED. The status is the only thing the retry policy and the
//   breaker read on a code they do not know, so a table that disagrees is a
//   client retrying something anvil considers final.
//
//   THE STEALTH BODY. A denied request on a stealth route and a genuinely
//   missing object answer with byte-identical bodies, which is a property of the
//   bytes and not of the design document.
//
// --- what is NOT recorded here, and why -------------------------------------
//
// A response per code, captured from a running server. There is nothing to
// capture from: anvil is a library plus test executables, with no runnable
// reference application (`docs/15-tasks.md` §Cross-repo), and its listener tests
// start Drogon in-process. So the fixture is recorded from the headers that
// decide those bytes, which is the same provenance the descriptor fixture has
// and one step short of the plan's "captured from a real server".
//
// And one thing that is recorded as ABSENT: `request_id` and `fields` appear in
// anvil's own `docs/00-architecture.md` §8 and in no writer anywhere in its
// source. The decode handles both, because the documented shape is the contract
// a client is written against; that nothing produces one yet is a cross-repo row
// rather than a reason to drop the handling.

import { readFileSync } from "node:fs";

import { describe, expect, it } from "vitest";

import { decodeEnvelope, errorVocabulary } from "../../src/wire/envelope.js";
import type { ErrorCode, ValidationReason } from "../testapp/api/hammer.generated.js";

import {
    kErrorCodeHttpStatus,
    kErrorCodeMaxValue,
    kErrorCodeOk,
    kErrorCodeValues,
    kStealthHiddenErrorCodes,
    kValidationReasonValues,
} from "../testapp/api/hammer.generated.js";

type RecordedCode = {
    readonly name: string;
    readonly value: number;
    readonly status: number;
    readonly stealthHidden: boolean;
    readonly carriesFieldDetail: boolean;
    readonly body: string | null;
    readonly contentType: string;
    readonly cacheControl: string;
};

type Recorded = {
    readonly recordedFrom: string;
    readonly codes: readonly RecordedCode[];
    readonly reasons: readonly { readonly name: string; readonly value: number }[];
    readonly stealth: {
        readonly status: number;
        readonly body: string;
        readonly contentType: string;
        readonly cacheControl: string;
        readonly contentTypeOptions: string;
    };
};

const kRecorded = JSON.parse(
    readFileSync(new URL("../fixtures/envelopes/anvil.json", import.meta.url), "utf8"),
) as Recorded;

const kDescriptor = JSON.parse(
    readFileSync(new URL("../testapp/hammer.descriptor.json", import.meta.url), "utf8"),
) as {
    readonly tables: {
        readonly error_codes: {
            readonly max: number;
            readonly codes: readonly {
                readonly name: string;
                readonly value: number;
                readonly http: number;
                readonly stealth_hidden: boolean;
            }[];
        };
        readonly validation_reasons: readonly { readonly name: string; readonly value: number }[];
    };
};

const kVocabulary = errorVocabulary<ErrorCode, ValidationReason>({
    codes: kErrorCodeValues,
    reasons: kValidationReasonValues,
});

// The failures. `OK` is the success member — value zero, no error body, and not
// something an error surface can reach.
const kFailures = kRecorded.codes.filter((code) => code.name !== kErrorCodeOk);

describe("anvil's error table and this client's", () => {
    it("recorded something to compare", () => {
        expect(kRecorded.codes.length).toBeGreaterThan(1);
        expect(kRecorded.reasons.length).toBeGreaterThan(1);
    });

    // The descriptor is emitted by one anvil binary and the recording is taken
    // from the headers another one compiles. They are two paths out of one
    // enum, and a disagreement means one of them was produced from a different
    // checkout — which is the staleness `check-descriptor.sh` cannot see.
    it("agrees with the descriptor the generator read, code for code", () => {
        const descriptor = new Map(
            kDescriptor.tables.error_codes.codes.map((code) => [code.name, code]),
        );

        expect(kDescriptor.tables.error_codes.max).toBe(kErrorCodeMaxValue);
        expect(descriptor.size).toBe(kRecorded.codes.length);

        for (const code of kRecorded.codes) {
            const declared = descriptor.get(code.name);
            expect(declared, code.name).toBeDefined();
            expect(declared?.value, code.name).toBe(code.value);
            expect(declared?.http, code.name).toBe(code.status);
            expect(declared?.stealth_hidden, code.name).toBe(code.stealthHidden);
        }
    });

    // The one that matters at run time. A code anvil can write and this bundle
    // cannot name decodes to `Unknown`: correct, and a downgrade of every
    // surface that had a sentence for it.
    it("names every code anvil can write, with the status anvil gives it", () => {
        for (const code of kRecorded.codes) {
            // Indexed by the TABLE's key type rather than by `ErrorCode`: the
            // union carries hammer's own `Unknown` member, which is a name
            // anvil does not have and never sends.
            const name = code.name as keyof typeof kErrorCodeValues;
            expect(Object.keys(kErrorCodeValues), code.name).toContain(code.name);
            expect(kErrorCodeValues[name], code.name).toBe(code.value);
            expect(kErrorCodeHttpStatus[name], code.name).toBe(code.status);
        }
    });

    // The 404 oracle, from the client's side. hammer emits the list so an
    // application can know which codes it will never see distinguished from a
    // 404 on a stealth route; a list that disagreed would describe a server
    // behaviour that does not happen.
    it("names exactly the codes anvil rewrites to a 404 on a stealth route", () => {
        const anvils = kRecorded.codes.filter((code) => code.stealthHidden).map((c) => c.name);
        expect([...kStealthHiddenErrorCodes].sort()).toEqual([...anvils].sort());
    });

    it("names every validation reason anvil can write", () => {
        const declared = new Map(
            kDescriptor.tables.validation_reasons.map((reason) => [reason.name, reason.value]),
        );
        for (const reason of kRecorded.reasons) {
            const name = reason.name as keyof typeof kValidationReasonValues;
            expect(Object.keys(kValidationReasonValues), reason.name).toContain(reason.name);
            expect(kValidationReasonValues[name], reason.name).toBe(reason.value);
            expect(declared.get(reason.name), reason.name).toBe(reason.value);
        }
    });
});

describe("the envelope anvil writes", () => {
    // Every recorded body, through the one decode. The bytes are anvil's own
    // assembly; the parse is what a browser does to them.
    it("decodes to the code it names, for every failure anvil can write", () => {
        for (const code of kFailures) {
            const body: unknown = JSON.parse(code.body ?? "");
            const decoded = decodeEnvelope({ status: code.status, body }, kVocabulary);

            expect(decoded.ok, code.name).toBe(false);
            if (decoded.ok) {
                continue;
            }
            expect(decoded.error.code, code.name).toBe(code.name);
            expect(decoded.error.status, code.name).toBe(code.status);

            // anvil's filter writes neither, and a surface that assumed a
            // string would render the word `undefined` on the most common
            // failure there is.
            expect(decoded.error.requestId, code.name).toBeNull();
            expect(decoded.error.fields, code.name).toBeNull();
        }
    });

    // The property is about the BYTES: a probe must not be able to tell a
    // forbidden object from a missing one, and the two paths that answer it are
    // in different translation units.
    it("answers a stealth drop with the same bytes as a genuine 404", () => {
        const genuine = kRecorded.codes.find((code) => code.name === "NOT_FOUND");
        expect(genuine?.body).toBe(kRecorded.stealth.body);
        expect(genuine?.status).toBe(kRecorded.stealth.status);
        expect(genuine?.contentType).toBe(kRecorded.stealth.contentType);
        expect(genuine?.cacheControl).toBe(kRecorded.stealth.cacheControl);

        const decoded = decodeEnvelope(
            { status: kRecorded.stealth.status, body: JSON.parse(kRecorded.stealth.body) },
            kVocabulary,
        );
        expect(decoded.ok).toBe(false);
        if (!decoded.ok) {
            expect(decoded.error.code).toBe("NOT_FOUND");
            expect(decoded.error.requestId).toBeNull();
        }
    });

    // The shape anvil's own documentation publishes and its source does not yet
    // write. A client is written against the contract, so the decode handles it;
    // the row asking anvil to ship a writer is in `docs/15-tasks.md`.
    it("reads the request id and the field map the documented envelope carries", () => {
        const documented = {
            error: {
                code: "VALIDATION_FAILED",
                request_id: "01JABCDEF0123456789XYZ",
                fields: { email: "BAD_FORMAT", name: "REQUIRED" },
            },
        };

        const decoded = decodeEnvelope({ status: 400, body: documented }, kVocabulary);
        expect(decoded.ok).toBe(false);
        if (decoded.ok) {
            return;
        }
        expect(decoded.error.code).toBe("VALIDATION_FAILED");
        expect(decoded.error.requestId).toBe("01JABCDEF0123456789XYZ");
        expect(decoded.error.fields?.get("email")).toBe("BAD_FORMAT");
        expect(decoded.error.fields?.get("name")).toBe("REQUIRED");

        // And the code that carries it is the only one anvil says carries one.
        const carries = kRecorded.codes.filter((code) => code.carriesFieldDetail);
        expect(carries.map((code) => code.name)).toEqual(["VALIDATION_FAILED"]);
    });

    // A deploy answers a tab that has been open since this morning with a code
    // its bundle predates. A decode that threw would turn that deploy into an
    // outage in every one of them.
    it("decodes a code appended after this bundle was built", () => {
        const ahead = { error: { code: "QUOTA_EXHAUSTED", request_id: "01JZZZ" } };
        const decoded = decodeEnvelope({ status: 402, body: ahead }, kVocabulary);

        expect(decoded.ok).toBe(false);
        if (!decoded.ok) {
            expect(decoded.error.code).toBe("Unknown");
            // The status survives, because it is all a policy has left to read.
            expect(decoded.error.status).toBe(402);
        }
    });

    // A proxy's 502 is an HTML page and a gateway timeout may be empty. Neither
    // has an envelope, and the decode still has to produce something a caller
    // can render.
    it("decodes a failure that never reached anvil at all", () => {
        for (const body of [undefined, "<html>502</html>", null, []]) {
            const decoded = decodeEnvelope({ status: 502, body }, kVocabulary);
            expect(decoded.ok).toBe(false);
            if (!decoded.ok) {
                expect(decoded.error.code).toBe("Unknown");
                expect(decoded.error.status).toBe(502);
            }
        }
    });
});
