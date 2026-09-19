// The one place a response becomes a value.
//
// `tools/check-wire-discipline.sh` fails the build on a second one, and the
// reason is not tidiness: a call site that reaches into `json.error.code`
// itself is a call site that will miss the next code anvil appends, and the
// miss is silent — a screen that renders nothing for a failure that happened.
//
// --- what the server actually sends ----------------------------------------
//
// anvil answers every failure with one shape and no sentence:
//
//     { "error": { "code": "VALIDATION_FAILED",
//                  "request_id": "01J…",
//                  "fields": { "email": "BAD_FORMAT" } } }
//
// Three things about that shape are easy to get wrong from the documentation
// alone, and all three were read out of `include/anvil/http/errors.h` rather
// than assumed.
//
//   THE 404 CARRIES NO REQUEST ID. `kNotFoundBody` is a constexpr string —
//   `{"error":{"code":"NOT_FOUND"}}` — used for a stealth drop, an unmatched
//   route and a genuinely missing object alike, so that all three are
//   byte-identical, and Nginx's `error_page` serves exactly those bytes with no
//   request behind it to have an id. anvil's own suite asserts the absence. So
//   `requestId` is nullable here, and an error surface that assumed a string
//   would render the word `undefined` on the most common failure there is.
//
//   `fields` APPEARS ONLY FOR ONE CODE, and never echoes a submitted value —
//   that is a reflected-XSS and log-injection vector, and an encoding hazard on
//   non-Latin input besides. Nothing here puts one back.
//
//   THE FAILURE MAY NEVER HAVE REACHED anvil AT ALL. A proxy's 502 is an HTML
//   page; a gateway timeout may be empty. There is no envelope in either, and
//   the decode still has to produce something a caller can handle — which is
//   what `Unknown` is for, and why the status is carried alongside the code.
//   Without it a 502 from the proxy and a 200 whose body did not parse are one
//   indistinguishable failure with two different right answers.
//
// --- a code this bundle has never heard of ---------------------------------
//
// `ErrorCode` is append-only server-side. A deploy can answer a tab that has
// been open since this morning with a code its bundle predates, and a decode
// that threw would turn that deploy into an outage in every one of them. So the
// known names arrive as a vocabulary and anything outside it decodes to
// `Unknown`, which is a member the application owes a sentence for exactly
// because it is the one that will be reached.
//
// The same is true of a validation reason, and the answer is the same. Dropping
// the entry instead would be a form that refuses to submit with nothing marked
// on it.
//
// --- what this module does not decide --------------------------------------
//
// Whether to retry, whether to refresh, whether to open the circuit: those read
// the code and the method and belong to the policy that owns them. And nothing
// here knows that a 404 on a stealth route is not evidence of anything — that
// is the application's rendering, and `kStealthHiddenErrorCodes` is emitted for
// it.

import type { ServerError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

// anvil's enums put the success member at zero. It is not a failure any error
// surface can reach, so the generated unions leave it out and so does the
// vocabulary built from them.
const kSuccessValue = 0;

// hammer's own member, PascalCase against anvil's SCREAMING_SNAKE so that no
// enumerator the server appends can ever collide with it.
const kUnknown = "Unknown";

// A request id is displayed verbatim and copied into a support channel, and it
// is the one server value this library hands onward without a decision. So it
// has to look like an id: a ULID, a UUID, or something of that shape. A control
// character in a field that is echoed into a log line is log injection, and a
// value of unbounded length is a screen nobody can read.
const kRequestId = /^[A-Za-z0-9_-]{1,64}$/;

// A validation response names the fields of one form. The cap is not that
// number — it is the point past which the response has stopped being a field
// map, and a map that large is not one to render any of.
const kMaxFields = 256;

// The statuses that carry no body by definition. A 2xx that is not one of these
// and arrives with nothing readable is a response this client cannot use, and
// saying so is better than succeeding with a value the caller then narrows into
// a crash somewhere else.
const kNoContentStatuses: readonly number[] = [204, 205, 304];

const kFailureStatus = 400;

declare const kVocabularyTypes: unique symbol;

// The wire names this client knows, paired with the unions that spell them.
//
// The two halves cannot be derived from each other — TypeScript erases the
// union and the set is a run-time value — so they are carried together and
// stated once, where the generated module is in scope. The phantom member is
// the same trick `core/brand.ts` uses: declared, never defined, erased, and
// worth its line because it stops a decode being handed one client's names
// under another client's types.
export type ErrorVocabulary<Code extends string, Reason extends string> = {
    readonly codes: ReadonlySet<string>;
    readonly reasons: ReadonlySet<string>;
    readonly [kVocabularyTypes]?: readonly [Code, Reason];
};

// Built from the generated value maps, which already hold every wire name the
// server has. Taking the maps rather than a list of names means an application
// adds nothing and forgets nothing: the names are the keys it already imports.
export function errorVocabulary<Code extends string, Reason extends string>(tables: {
    readonly codes: Readonly<Record<string, number>>;
    readonly reasons: Readonly<Record<string, number>>;
}): ErrorVocabulary<Code, Reason> {
    return { codes: failureNames(tables.codes), reasons: failureNames(tables.reasons) };
}

function failureNames(table: Readonly<Record<string, number>>): ReadonlySet<string> {
    const names = new Set<string>();
    for (const [name, value] of Object.entries(table)) {
        if (value !== kSuccessValue) {
            names.add(name);
        }
    }
    return names;
}

export type ResponseFacts = {
    readonly status: number;

    // The parsed body, or `undefined` when there was none and when it did not
    // parse. The two are one case here on purpose: both mean this response
    // carries nothing the decode can read, and the status is what separates the
    // ones that are allowed to.
    readonly body: unknown;
};

function asObject(value: unknown): Readonly<Record<string, unknown>> | null {
    if (typeof value !== "object" || value === null || Array.isArray(value)) {
        return null;
    }
    return value as Readonly<Record<string, unknown>>;
}

function decodeRequestId(value: unknown): string | null {
    if (typeof value !== "string" || !kRequestId.test(value)) {
        return null;
    }
    return value;
}

// A reason outside the vocabulary is `Unknown`, because the enum is append-only.
// A value that is not a string at all is a malformed response, and a response
// that is malformed in one place is not one to keep the rest of — so the whole
// map goes rather than the entry, the way `session_view.ts` drops a session
// rather than half of one.
function decodeFields<Reason extends string>(
    value: unknown,
    known: ReadonlySet<string>,
): ReadonlyMap<string, Reason | "Unknown"> | null {
    const raw = asObject(value);
    if (raw === null) {
        return null;
    }

    const entries = Object.entries(raw);
    if (entries.length > kMaxFields) {
        return null;
    }

    // A Map rather than an object because the keys come off the wire, and a key
    // named `__proto__` in a plain object is a lookup that answers something
    // nobody put there.
    const fields = new Map<string, Reason | "Unknown">();
    for (const [field, reason] of entries) {
        if (typeof reason !== "string") {
            return null;
        }
        fields.set(field, known.has(reason) ? (reason as Reason) : kUnknown);
    }
    return fields;
}

function unknownFailure<Code extends string, Reason extends string>(
    status: number,
): ServerError<Code | "Unknown", Reason | "Unknown"> {
    return { kind: "server", code: kUnknown, status, requestId: null, fields: null };
}

// The decode. It never throws: every shape that is not the one anvil documents
// resolves to a failure a caller can render, because the alternative is a
// rejected promise on the day the server is already having.
export function decodeEnvelope<Code extends string, Reason extends string>(
    response: ResponseFacts,
    vocabulary: ErrorVocabulary<Code, Reason>,
): Result<unknown, ServerError<Code | "Unknown", Reason | "Unknown">> {
    const { status, body } = response;

    if (status < kFailureStatus) {
        if (kNoContentStatuses.includes(status)) {
            return ok(null);
        }
        if (body === undefined) {
            return fail(unknownFailure(status));
        }
        return ok(body);
    }

    const envelope = asObject(asObject(body)?.["error"]);
    if (envelope === null) {
        return fail(unknownFailure(status));
    }

    const rawCode = envelope["code"];
    const code =
        typeof rawCode === "string" && vocabulary.codes.has(rawCode)
            ? (rawCode as Code)
            : kUnknown;

    return fail({
        kind: "server",
        code,
        status,
        requestId: decodeRequestId(envelope["request_id"]),
        fields: decodeFields<Reason>(envelope["fields"], vocabulary.reasons),
    });
}
