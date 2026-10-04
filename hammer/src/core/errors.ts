// The failure shapes, and deliberately no message field in any of them.
//
// anvil's error responses carry a code, a request id and — for one code — a map
// of field names to reason enums. They carry no sentence, because the words
// belong to whoever knows the audience and the locale, and a library that
// invented one would ship English to every application at once.
//
// The code and reason types are PARAMETERS rather than unions declared here.
// They come from the generated descriptor module, which is the only thing that
// knows what this particular server's vocabulary is, and a union hard-coded here
// would be a second copy of it (docs/01-seams.md §2).

import type { Result } from "./result.js";

// What the server said. `code` is a literal type from the descriptor; `Unknown`
// is the member the generator always adds, because anvil's ErrorCode is
// append-only and a client that throws on a code the server added this morning
// turns a deploy into an outage in every tab that was already open.
export type ServerError<Code extends string = string, Reason extends string = string> = {
    readonly kind: "server";
    readonly code: Code;

    // The HTTP status, carried because a failure may never have reached anvil
    // at all: a proxy's 502 has no envelope and therefore no code, and without
    // the status it is indistinguishable from every other `Unknown`. The code
    // is what policy branches on; this is what says what happened when there
    // is not one.
    readonly status: number;

    // The one thing a user can report that a server-side log can be found by. It
    // is displayed; it is never interpreted.
    //
    // Null is not a defensive default — it is the common case. anvil's 404 body
    // is one constexpr string, `{"error":{"code":"NOT_FOUND"}}`, shared by a
    // stealth drop, an unmatched route and a genuinely missing object so that
    // all three are byte-identical, and Nginx serves those same bytes with no
    // request behind them to have an id.
    readonly requestId: string | null;

    // Present only for the validation code, and never echoing a submitted
    // value — that is a reflected-XSS and log-injection vector, and with
    // non-Latin input an encoding hazard too.
    //
    // A Map because the keys come off the wire, and null rather than absent
    // because an optional property is two object shapes, which deoptimises
    // every call site that has seen the other one (`CLAUDE.md` §2.3).
    readonly fields: ReadonlyMap<string, Reason> | null;
};

// The request did not produce a response. Distinguished from a server error
// because the two have opposite retry policies: a transport failure may be
// retried for an idempotent route, and a 403 never may.
export type TransportError = {
    readonly kind: "transport";
    readonly cause: "network" | "timeout" | "aborted" | "circuit-open";
};

// The client refused to send it. A local bound — the request queue's, the upload
// cap, the rate-limit budget — reached before anything left the device.
export type ClientError = {
    readonly kind: "client";
    readonly cause:
        | "queue-full"
        | "too-large"
        | "unsupported-media"
        | "budget-spent"
        // No address for this route. A MISSING ADDRESS, never a refusal: the
        // recovery is a session refetch, and reporting it as a denial would
        // rebuild in the client the oracle the server removed
        // (docs/00-architecture.md §4.2).
        | "no-route"
        // An address that cannot be used. The server named a method that is not
        // one — anvil emits `ANY` for a route description that forgot to name
        // a method, so that a client fails loudly here instead of defaulting to
        // GET and silently calling the wrong thing.
        | "unusable-route"
        // A request this client would not build: a path parameter that is empty,
        // a dot segment, half a surrogate pair, a number that will not survive
        // the round trip, or a body that will not serialise. Every one of them
        // is refused before anything is sent, and `buildRoute` is where the
        // specific reason is available to a caller that wants it.
        | "bad-parameter";

    // Milliseconds, named in the field, because a duration whose unit lives in a
    // comment is a duration that gets read as seconds. Null when waiting will
    // not help.
    readonly retryAfterMs: number | null;
};

// The bundle is older than the server it is talking to: the descriptor hash the
// session returned is not the one this client was generated from.
//
// It is surfaced and never acted on. An automatic reload discards whatever the
// user had typed, on the deploy most likely to be happening during working
// hours (docs/00-architecture.md §7.1).
export type StaleClientError = {
    readonly kind: "stale-client";
    readonly serverHash: string;
    readonly clientHash: string;
};

// A client-side password prehash that did not produce a credential
// (`docs/01-seams.md` §21). Declared here, beside the rest of the error model,
// so that `hammer/dom` can name it without importing the layer that raises it —
// and deliberately NOT a member of `HammerError`: widening that union would
// break every application that switches over it exhaustively, for a failure
// that only a login or a signup screen can meet.
export type PrehashError = {
    readonly kind: "prehash";
    readonly cause:
        // The body handed over has no identifier or no secret under the names
        // the application configured, or one of them is empty.
        | "missing-field"
        // Over the byte ceiling the server applies before it hashes. Refused here
        // so the person is told before a worker spends seconds on it.
        | "secret-too-long"
        // The salt route answered with something other than the contract shape:
        // another algorithm, a salt of the wrong length, a missing number.
        | "bad-answer"
        // A well-formed answer whose cost is outside the bounds the application
        // configured. Surfaced rather than hashed: a server answering below the
        // floor is misconfigured, and one answering far above it is asking a
        // phone for memory it does not have.
        | "out-of-bounds"
        // The device could not allocate the Argon2 memory.
        | "out-of-memory"
        // A hash is already running. The pool holds one, because one is 64 MiB.
        | "busy"
        // The worker died or reported a failure; the pool is closed.
        | "worker-failed"
        | "aborted";
};

// A request `hammer/accounts` refused to build, before anything was sent. The
// server's own bounds, published in the descriptor, applied where a person can
// be told at once — and where, under client hashing, they are the ONLY place
// they can be applied, because the server receives a credential rather than
// the password (anvil `docs/05` §13). Outside `HammerError` for PrehashError's
// reason.
export type AccountError = {
    readonly kind: "account";
    readonly cause: "secret-too-short" | "secret-too-long";
};

export type HammerError<Code extends string = string, Reason extends string = string> =
    | ServerError<Code, Reason>
    | TransportError
    | ClientError
    | StaleClientError;

export type HammerResult<T, Code extends string = string, Reason extends string = string> =
    Result<T, HammerError<Code, Reason>>;

export function isServerError<C extends string, R extends string>(
    error: HammerError<C, R>,
): error is ServerError<C, R> {
    return error.kind === "server";
}

// Whether a failure may be retried at all, before the per-code policy is
// consulted. A client-side refusal is never retried by the library: the cap it
// hit is the same cap the retry would hit, and the caller is the one who can
// change the request.
export function isRetryableKind(error: HammerError): boolean {
    return error.kind === "transport" || error.kind === "server";
}
