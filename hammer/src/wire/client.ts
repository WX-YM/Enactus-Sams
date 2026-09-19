// The pipeline, in the order `docs/00-architecture.md` §4 states it.
//
// The order is the point: each stage is cheaper than the next, so a call that
// cannot succeed is abandoned before it costs a round trip. Stages 1-8 complete
// with ZERO network, which is this library's version of anvil's thesis — the
// cheapest round trip is the one that is not made.
//
//    1  route resolution        compiled table for a public route, the session's
//                               holder-scoped table otherwise
//    2  the affordance check    DELIBERATELY ABSENT here, see below
//    3  schema validation       not yet: anvil has no request binder to emit one
//    4  the local budget        a token bucket per declared bucket
//    5  the capability          discharged at the TYPE level, so there is no
//                               run-time stage to forget
//    6  in-flight dedupe        a duplicate joins the first rather than being sent
//    7  the idempotency key     minted for anything retryable and not idempotent
//    8  queue admission         bounded in-flight; a full queue sheds here
//    9  fetch, credentials: "include"
//   10  the envelope decode     exactly one place, and it is not here
//   11  the retry decision      per status and per method, Retry-After honoured
//
// --- stage 2 is not here, and that is the design ----------------------------
//
// The permission pre-check governs AFFORDANCES and is given no way to refuse a
// call (`wire/affordance.ts`). A client that checked it here would turn a stale
// permission copy into a permanent denial no server-side change can clear, and
// the bug report it produces is "the button does nothing". So a call that a
// local copy says will fail is SENT, and a 403 in return is what tells this
// client its copy is stale — which is what makes the staleness self-healing.
//
// --- what this module must never do -----------------------------------------
//
// It builds no URL by concatenation, decodes no envelope of its own, holds no
// token and logs no URL, body or header. Each of those is a rule with a script
// behind it (`tools/check-wire-discipline.sh`, `tools/check-source-bans.sh`),
// because each is a rule that is obvious until the week somebody is busy.

import type { HammerError, HammerResult, TransportError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

import type { AffordableRoute } from "./affordance.js";
import { affordsRoute } from "./affordance.js";
import type { BreakerConfig } from "./breaker.js";
import { CircuitBreaker } from "./breaker.js";
import type { Capability } from "./capability.js";
import { burnsOnUse, capabilityToken, kCapabilityHeader, mintCapability } from "./capability.js";
import type { CredentialCounts } from "./credentials.js";
import { Credentials, kDefaultLeaderWaitMs } from "./credentials.js";
import type { ErrorVocabulary } from "./envelope.js";
import { decodeEnvelope, errorVocabulary } from "./envelope.js";
import { carriesIdempotencyKey, kIdempotencyHeader, mintIdempotencyKey } from "./idempotency.js";
import type { ExclusiveLocks, FanOut, Leadership } from "./leader.js";
import { leadership, noFanOut } from "./leader.js";
import type { ApiOrigin, ApiOriginRequest } from "./origin.js";
import { defineApiOrigin, requestUrl } from "./origin.js";
import type { MonotonicClock, QueueConfig } from "./queue.js";
import { RequestQueue, kDefaultQueue } from "./queue.js";
import type { RateLimitTable } from "./rate_limit.js";
import { RateLimiter } from "./rate_limit.js";
import type { SessionSource } from "./resolve.js";
import { resolveRoute } from "./resolve.js";
import type { Stream, StreamClosed, StreamEvent } from "./sse.js";
import { openStream } from "./sse.js";
import type { UploadLimits, UploadOptions } from "./upload.js";
import { checkUpload, uploadBody, uploadContentType } from "./upload.js";
import type { PathParams, RouteQuery } from "./route.js";
import { buildRoute } from "./route.js";
import type { CacheControl } from "./cache_control.js";
import { parseCacheControl } from "./cache_control.js";
import type { RetryPolicy } from "./retry.js";
import { kDefaultRetryPolicy, parseRetryAfter, retryAction } from "./retry.js";
import type { Sleep } from "./schedule.js";
import { sleep as platformSleep } from "./schedule.js";

// The part of a generated route `const` a call reads. Structural, so no table
// from any application reaches this layer (`ENGINEERING_RULES.md` §1).
export type CallableRoute = {
    readonly id: string;
    readonly visibility: "public" | "holder";
    readonly method: string | null;
    readonly path: string | null;
    readonly capability: string | null;
    readonly rateLimit: string | null;
    readonly idempotent: boolean;
};

// What one application's generated module knows, as one type parameter rather
// than four. The application declares it once, from the generated names, and
// every call is then typed by the route it names.
//
// `hash` is the descriptor these came from, as a LITERAL type. It is what makes
// `ApiTables` below pair with this rather than merely sit beside it: the tables
// carry the hash as a value and this type demands the same one, so one
// application's tables read under another's unions is a compile error at
// `createClient`. Two generated modules made from the same descriptor pair
// freely, because they are the same tables.
export type ApiTypes = {
    readonly params: Readonly<Record<string, PathParams>>;
    readonly responses: Readonly<Record<string, unknown>>;
    readonly code: string;
    readonly reason: string;
    readonly hash: string;
};

type ParamsOf<A extends ApiTypes, R extends CallableRoute> = R["id"] extends keyof A["params"]
    ? A["params"][R["id"]]
    : PathParams;

type ResultOf<A extends ApiTypes, R extends CallableRoute> = R["id"] extends keyof A["responses"]
    ? A["responses"][R["id"]]
    : unknown;

export type CallResult<A extends ApiTypes, R extends CallableRoute> = HammerResult<
    ResultOf<A, R>,
    A["code"] | "Unknown",
    A["reason"] | "Unknown"
>;

// A route with no parameters takes none, and one with parameters cannot be
// called without them. Both halves are the compiler's rather than a reviewer's.
export type ParamsOption<A extends ApiTypes, R extends CallableRoute> = Record<
    string,
    never
> extends ParamsOf<A, R>
    ? { readonly params?: Record<string, never> }
    : { readonly params: ParamsOf<A, R> };

// The capability requirement, as a type. A route that declares a scope cannot
// be called without a `Capability` for THAT scope, and one that declares none
// will not accept a property that is not there to pass.
export type CapabilityOption<R extends CallableRoute> = R["capability"] extends string
    ? { readonly capability: Capability<R["capability"]> }
    : { readonly capability?: never };

type CommonOptions = {
    readonly query?: RouteQuery;

    // Required, on every call, with no default (`ENGINEERING_RULES.md` §3.1). A request
    // nothing can cancel is a request that outlives the screen that wanted it,
    // and a default would be the thing every call site quietly accepted.
    readonly signal: AbortSignal;

    // The freshness the server granted this response, for a caller that caches
    // it (`state/resource.ts`). A callback rather than a second return value
    // because the answer belongs to one attempt and the result belongs to the
    // call: a request that was retried, refreshed and replayed has one body and
    // several responses, and only the last of them said anything about storing
    // it.
    //
    // Called once, on the attempt that produced the body, and never on a
    // failure — there is nothing to cache. A caller that joins an in-flight
    // request rather than making one is not called at all, which is why the
    // layer that caches also dedupes: the request that RAN is the one with the
    // header.
    readonly onFreshness?: (directives: CacheControl) => void;
};

export type CallOptions<A extends ApiTypes, R extends CallableRoute> = CommonOptions & {
    // Encoded as JSON. Absent for a request that has no body.
    readonly body?: unknown;
} & ParamsOption<A, R> &
    CapabilityOption<R>;

// The body of ONE attempt.
//
// A transport retry, a backoff retry and the replay after a credential refresh
// are three attempts at one request, and each of them needs a body of its own.
// A `ReadableStream` is CONSUMED by the attempt that sends it: handing the same
// one to `fetch` a second time throws, this client cannot tell that throw from a
// dropped connection, and the call spends its whole attempt budget re-sending a
// stream that is already empty — reporting a network failure for a network that
// was working (`docs/15-tasks.md` §The first consumer).
//
// It is also called inside the one function that sends a request, so nothing is
// built for an attempt that is not made: a shed queue, an open circuit and an
// aborted signal now read no byte of the file at all.
export type RequestBody = () => BodyInit;

// What `send` accepts: a body that survives being sent twice, or a factory for
// one that does not.
//
// The one-shot case is EXCLUDED from the direct form rather than warned about in
// a comment. A `ReadableStream` passed by value is a call that retries itself
// into its own attempt cap, and the compiler is the cheap place to find that
// (`ENGINEERING_RULES.md` §1). Everything else a `BodyInit` can be — a `File`, a `Blob`,
// bytes, a string, `FormData`, `URLSearchParams` — is re-readable, so it is
// passed as itself and sent again as itself.
export type SendBody = Exclude<BodyInit, ReadableStream> | RequestBody;

export type SendOptions<A extends ApiTypes, R extends CallableRoute> = CommonOptions & {
    // A body the caller has already shaped: a `File` for an upload, bytes for
    // anything else. It is passed to `fetch` as it arrives and is never read
    // here — a `File` is a handle, not bytes, until something reads it
    // (`ENGINEERING_RULES.md` §2.2).
    readonly body: SendBody | null;
    readonly contentType: string | null;
} & ParamsOption<A, R> &
    CapabilityOption<R>;

// Narrow enough that a test supplies a function, wide enough that the
// platform's `fetch` is assignable to it.
export type FetchLike = (url: string, init: RequestInit) => Promise<Response>;

// One structured record per request (`docs/00-architecture.md` §9). No URL, no
// body, no header — the same redaction rule as the server, applied where the
// data is most casually available.
export type RequestRecord = {
    readonly routeId: string;
    readonly status: number | null;
    readonly requestId: string | null;
    readonly durationMs: number;
    readonly queueWaitMs: number;
    readonly retries: number;
    readonly transport: TransportError["cause"] | null;
};

// Counted rather than logged per event, for the reason anvil gives: under the
// load that makes one of these fire, a line per occurrence is itself the
// problem. The names are hammer's mechanisms; an application maps them into its
// own namespace, because this library declares no metric name in one.
export type ClientCount =
    | "request-shed"
    | "replay"
    | "circuit-open"
    | "stale-session"
    | "stream-reconnect";

export type Telemetry = {
    readonly request: (record: RequestRecord) => void;
    readonly count: (name: ClientCount) => void;
};

const kNoTelemetry: Telemetry = { request: () => {}, count: () => {} };

// One descriptor's tables, as one member: `kApiTables` in the generated module.
//
// Every field here is generated, and they are generated TOGETHER — an object
// literal over the four `const`s beside it — so the set arrives from the one
// file whose hash is checked against the server's (`docs/01-seams.md` §16).
// Passed as four separate members they were four chances to hand a client a
// table from a different descriptor, a hand-written stand-in, or last month's
// regeneration, and nothing in the type system was looking at any of it.
//
// The vocabulary is built HERE rather than by the application, from the value
// maps the module already exports. `errorVocabulary` stays exported for a
// consumer that decodes an envelope itself, but a client no longer makes one
// spell it: the two type parameters it took were the application restating what
// `A` already says, and a restatement is a place for the two to disagree.
export type ApiTables<A extends ApiTypes> = {
    // The wire names, as anvil stores them. Both maps carry the success member
    // as well as the failures; the decode takes the failures, and the numbers
    // are here because anvil keeps them in audit rows read back long after the
    // deploy that wrote them (`wire/envelope.ts`).
    readonly codes: Readonly<Record<string, number>>;
    readonly reasons: Readonly<Record<string, number>>;

    // Values, not names — nothing at this layer knows what a bucket or a scope
    // is called (`ENGINEERING_RULES.md` §1).
    readonly rateLimits: RateLimitTable;
    readonly singleUse: Readonly<Record<string, boolean>>;
    readonly bodyMaxBytes: number;

    // The descriptor this set came from, and the only member nothing decodes
    // with. It is here to be CHECKED: `A["hash"]` is the literal the generated
    // `Api` carries, so a tables object from a different descriptor does not
    // satisfy this and `createClient` will not compile.
    //
    // This replaces a phantom `unique symbol`, which was the obvious way and
    // bought nothing. The generated module imports nothing and so cannot spell a
    // symbol hammer declares, which forced the member to be optional — and an
    // optional phantom constrains no object literal at all. What actually stops
    // one application's tables being read under another's unions has to be a
    // value the generator can write down, and the descriptor hash already was
    // one (`docs/01-seams.md` §16).
    readonly hash: A["hash"];
};

export type ClientConfig<A extends ApiTypes> = {
    // `kApiTables` from the generated module.
    readonly api: ApiTables<A>;

    // Where the document was served from and where the API is, UNVALIDATED.
    // The check is the constructor's, because a client is the thing that would
    // otherwise send every request anonymously and a deployment that gets this
    // wrong has no other symptom (`wire/origin.ts`).
    readonly origin: ApiOriginRequest;

    readonly fetch: FetchLike;
    readonly session: SessionSource;

    // The route a refresh is made to. Public, and neither the client nor the
    // generator can know which one it is: the descriptor carries the route and
    // nothing that marks it as the refresh, so the application names it.
    readonly refreshRoute: CallableRoute;

    readonly queue?: QueueConfig;
    readonly retry?: RetryPolicy;
    readonly breaker?: BreakerConfig;

    // Platform singletons, injected (`ENGINEERING_RULES.md` §3.3). Null for the lock
    // manager means "this platform has none", which is a degraded election
    // rather than an error.
    readonly locks?: ExclusiveLocks | null;
    readonly fanOut?: FanOut;
    readonly now?: MonotonicClock;
    readonly sleep?: Sleep;

    readonly telemetry?: Telemetry;

    // What this tab drops when the session ends: caches, streams, anything
    // keyed by an identity that no longer exists.
    readonly onLogout?: () => void;

    readonly leaderWaitMs?: number;
};

const kNetworkFailure: TransportError = { kind: "transport", cause: "network" };
const kAborted: TransportError = { kind: "transport", cause: "aborted" };

function clientError(
    cause: "too-large" | "bad-parameter",
): HammerError<string, string> {
    return { kind: "client", cause, retryAfterMs: null };
}

// A `BodyInit` cannot be a function, so the two arms of `SendBody` separate
// without a discriminant anybody has to remember to set.
function bodyFor(body: SendBody): BodyInit {
    return typeof body === "function" ? body() : body;
}

// The statuses that carry no body by definition, so nothing tries to read one.
const kBodyless: readonly number[] = [204, 205, 304];

type Attempted = {
    // Every shape a single attempt can end in, the local refusals included: a
    // shed queue and a spent budget are as much a reason not to try again as a
    // 404 is, and the policy that decides that reads one union.
    readonly outcome: Result<unknown, HammerError<string, string>>;
    readonly status: number | null;
    readonly retryAfterMs: number | null;
    readonly queueWaitMs: number;
};

// A request several callers are waiting on.
//
// The shared request runs under a signal of its own, and it is aborted only
// when EVERY joiner has gone. Handing it the first caller's signal would let one
// screen unmounting cancel a request the other four are still waiting for,
// which is a dedupe that makes things worse under exactly the conditions that
// produced the duplicate.
class Shared<T> {
    readonly promise: Promise<T>;
    private readonly controller: AbortController;
    private readonly onAbandoned: () => void;
    private joined: number;
    private left: number;

    constructor(run: (signal: AbortSignal) => Promise<T>, onAbandoned: () => void) {
        this.controller = new AbortController();
        this.onAbandoned = onAbandoned;
        this.joined = 0;
        this.left = 0;
        this.promise = run(this.controller.signal);
    }

    join(signal: AbortSignal, aborted: T): Promise<T> {
        this.joined += 1;
        return new Promise<T>((resolve) => {
            const onAbort = (): void => {
                this.left += 1;
                if (this.left === this.joined) {
                    // Everybody who was waiting has gone. The request is
                    // abandoned rather than left running, and the client is told
                    // SYNCHRONOUSLY so that a caller arriving a microtask later
                    // starts a new request instead of joining a dead one.
                    this.onAbandoned();
                    this.controller.abort();
                }
                resolve(aborted);
            };
            if (signal.aborted) {
                onAbort();
                return;
            }
            signal.addEventListener("abort", onAbort, { once: true });
            void this.promise.then(
                (value) => {
                    signal.removeEventListener("abort", onAbort);
                    resolve(value);
                },
                () => {
                    signal.removeEventListener("abort", onAbort);
                    resolve(aborted);
                },
            );
        });
    }
}

export class Client<A extends ApiTypes> {
    private readonly config: ClientConfig<A>;
    private readonly queue: RequestQueue;
    private readonly limiter: RateLimiter;
    private readonly breaker: CircuitBreaker;
    private readonly telemetry: Telemetry;
    private readonly now: MonotonicClock;
    private readonly sleep: Sleep;
    private readonly lifetime: AbortController;
    private readonly leader: Leadership;
    private readonly fanOut: FanOut;

    // Keyed by the address, because two calls to one route with different
    // parameters are two requests and two calls with the same ones are one.
    private readonly inFlight: Map<string, Shared<CallResult<A, CallableRoute>>>;

    private readonly vocabulary: ErrorVocabulary<A["code"], A["reason"]>;

    readonly credentials: Credentials;

    // The validated origin, and the reason it is readable: `crossOrigin` is a
    // cost a deployment pays on every mutating request and one nothing else can
    // observe — an `OPTIONS` is invisible in a waterfall unless somebody is
    // looking for it. A cost nobody measured is a cost nobody removes.
    readonly origin: ApiOrigin;

    constructor(config: ClientConfig<A>) {
        // A misconfigured client is the one thing a throw is for (`ENGINEERING_RULES.md`
        // §3.1). The alternative is not a degraded client: `SameSite=Lax`
        // cookies are not sent on a cross-site subresource request, so every
        // call would be anonymous, the first would 401, the refresh would 401
        // too, and the user would be signed out by a deployment mistake that
        // reads as an authentication bug. `defineApiOrigin` stays exported so
        // the decision can be made — and tested — without constructing one of
        // these.
        const origin = defineApiOrigin(config.origin);
        if (!origin.ok) {
            throw new Error(`the API origin is unusable: ${origin.error}`);
        }
        this.origin = origin.value;

        this.config = config;
        this.vocabulary = errorVocabulary<A["code"], A["reason"]>(config.api);
        this.queue = new RequestQueue(config.queue ?? kDefaultQueue, config.now);
        this.limiter = new RateLimiter(config.api.rateLimits);
        this.breaker = new CircuitBreaker(config.breaker);
        this.telemetry = config.telemetry ?? kNoTelemetry;
        this.now = config.now ?? (() => performance.now());
        this.sleep = config.sleep ?? platformSleep;
        this.lifetime = new AbortController();
        this.inFlight = new Map();
        // One election and one channel for this tab, shared by the refresh and
        // by every stream: two lock managers over one platform would be two
        // opinions about which tab is the leader.
        this.leader = leadership(config.locks ?? null);
        this.fanOut = config.fanOut ?? noFanOut;

        this.credentials = new Credentials({
            leadership: this.leader,
            fanOut: this.fanOut,
            refresh: (signal) => this.refresh(signal),
            onLogout: config.onLogout ?? (() => {}),
            leaderWaitMs: config.leaderWaitMs ?? kDefaultLeaderWaitMs,
            sleep: this.sleep,
        });
    }

    // Whether a control should exist. Answered from the session's own table,
    // never from the bits, and it governs rendering and nothing else — this
    // class provides no way to refuse a call locally, which is the rule as a
    // type rather than as a review comment (`wire/affordance.ts`).
    affords(route: AffordableRoute): boolean {
        return affordsRoute(this.config.session.current(), route);
    }

    counts(): CredentialCounts {
        return this.credentials.counts();
    }

    // The session is over. Fans out to every tab, which drops its caches and
    // closes its streams.
    logout(): void {
        this.credentials.logout();
    }

    close(): void {
        this.credentials.close();
        this.lifetime.abort();
    }

    async call<R extends CallableRoute>(
        route: R,
        options: CallOptions<A, R>,
    ): Promise<CallResult<A, R>> {
        const body = (options as { readonly body?: unknown }).body;
        if (body === undefined) {
            return await this.perform(route, options, null, null);
        }

        let encoded: Uint8Array<ArrayBuffer>;
        try {
            encoded = new TextEncoder().encode(JSON.stringify(body));
        } catch {
            // A body that will not serialise — a cycle, a BigInt — is programmer
            // error, but throwing it from here is a rejected promise on a path
            // whose whole design is that failure is in the return type.
            return fail(clientError("bad-parameter")) as CallResult<A, R>;
        }

        // The descriptor's own cap, checked before a byte is sent. It is not the
        // enforcement — anvil enforces during the read — it is the round trip
        // that is saved.
        if (encoded.byteLength > this.config.api.bodyMaxBytes) {
            return fail(clientError("too-large")) as CallResult<A, R>;
        }

        return await this.perform(route, options, encoded, "application/json; charset=utf-8");
    }

    // The same pipeline for a body the caller has already shaped. `wire/upload.ts`
    // is what this exists for: a `File` goes to `fetch` as itself, so the bytes
    // never enter the JS heap.
    async send<R extends CallableRoute>(
        route: R,
        options: SendOptions<A, R>,
    ): Promise<CallResult<A, R>> {
        return await this.perform(route, options, options.body, options.contentType);
    }

    // The live stream for a route, opened by whichever tab wins the election and
    // fanned out to the rest (`wire/sse.ts`).
    //
    // The handler runs in EVERY tab, including the ones that are not connected,
    // and it runs at-least-once: a reconnect replays. It must be idempotent,
    // which is the same property anvil's own inbox relies on.
    stream<R extends CallableRoute>(
        route: R,
        options: {
            readonly params?: PathParams;
            readonly query?: RouteQuery;
            readonly onEvent: (event: StreamEvent) => void;
            readonly onClosed?: (closed: StreamClosed) => void;
            readonly lastEventId?: string | null;
        },
    ): Stream {
        return openStream({
            // Resolved per attempt: a holder route's address arrives with the
            // session, and the session may have been refetched since the last
            // connection.
            address: async (signal) => {
                const target = await resolveRoute(route, this.config.session, signal);
                if (!target.ok) {
                    return fail(target.error);
                }
                const built = buildRoute(target.value, {
                    params: options.params ?? {},
                    query: options.query ?? {},
                });
                if (!built.ok) {
                    return fail(clientError("bad-parameter"));
                }
                return ok(requestUrl(this.origin, built.value));
            },
            fetch: this.config.fetch,
            leadership: this.leader,
            fanOut: this.fanOut,
            name: "hammer.stream." + route.id,
            onEvent: options.onEvent,
            ...(options.onClosed === undefined ? {} : { onClosed: options.onClosed }),
            onReconnect: () => {
                this.telemetry.count("stream-reconnect");
            },
            lastEventId: options.lastEventId ?? null,
            sleep: this.sleep,
            retry: this.config.retry ?? kDefaultRetryPolicy,
        });
    }

    // An upload, through the same pipeline as every other request and with the
    // bytes never entering the JS heap (`wire/upload.ts`).
    //
    // The size and the type are refused before the first byte, which saves an
    // upload that was going to be refused after the last one — on a connection
    // where forty megabytes is minutes. It is not the enforcement: anvil
    // enforces during the read, because a client-side check is one an attacker
    // skips.
    async upload<R extends CallableRoute>(
        route: R,
        limits: UploadLimits,
        options: UploadOptions<A, R>,
    ): Promise<CallResult<A, R>> {
        const allowed = checkUpload(options.file, limits);
        if (!allowed.ok) {
            return fail(allowed.error) as CallResult<A, R>;
        }
        // Per attempt, and not once for the call: with a progress handler the
        // body is a stream, and a stream is spent by the attempt that sends it.
        // The counter starts at zero again with it, which is the honest number —
        // the bytes really are being sent a second time, and a counter carried
        // across attempts would report an upload as half done while the server
        // had received none of it.
        return await this.perform(
            route,
            options,
            () => uploadBody(options.file, options.onProgress),
            uploadContentType(options.file),
        );
    }

    // A capability, from the call that mints one and from nowhere else.
    //
    // The token is somewhere in a response whose shape is the application's
    // (`docs/01-seams.md` §4), so the application says where — and because that
    // reader only ever runs on a body this client received, a `Capability`
    // still cannot be produced without the server having minted one.
    async mint<R extends CallableRoute, S extends string>(
        route: R,
        options: CallOptions<A, R>,
        readToken: (body: ResultOf<A, R>) => unknown,
    ): Promise<HammerResult<Capability<S>, A["code"] | "Unknown", A["reason"] | "Unknown">> {
        const answered = await this.call(route, options);
        if (!answered.ok) {
            return answered;
        }
        const minted = mintCapability<S>(readToken(answered.value));
        if (!minted.ok) {
            return fail(clientError("bad-parameter")) as HammerResult<
                Capability<S>,
                A["code"] | "Unknown",
                A["reason"] | "Unknown"
            >;
        }
        return ok(minted.value);
    }

    private async perform<R extends CallableRoute>(
        route: R,
        options: CommonOptions & { readonly params?: PathParams; readonly capability?: unknown },
        body: SendBody | null,
        contentType: string | null,
        // True for the refresh request itself. A 401 on THAT route is the
        // session ending rather than a credential to refresh, and a client that
        // did not know the difference would ask the refresh machinery to
        // recover the refresh — which is a deadlock in this tab and a loop
        // against the server.
        isRefresh = false,
    ): Promise<CallResult<A, R>> {
        const { signal } = options;
        if (signal.aborted) {
            return fail(kAborted) as CallResult<A, R>;
        }

        // 1. The address, or the reason there is not one. A missing entry
        //    refetches the session once and then reports not-found — never a
        //    permission-flavoured error (`docs/00-architecture.md` §4.2).
        const target = await resolveRoute(route, this.config.session, signal);
        if (!target.ok) {
            return fail(target.error) as CallResult<A, R>;
        }

        const built = buildRoute(target.value, {
            params: options.params ?? {},
            query: options.query ?? {},
        });
        if (!built.ok) {
            return fail(clientError("bad-parameter")) as CallResult<A, R>;
        }

        const url = requestUrl(this.origin, built.value);
        const capability =
            typeof options.capability === "string" ? (options.capability as Capability<string>) : null;

        // 6. One request per address in flight. Only for a route that is safe to
        //    repeat: two identical POSTs are two writes somebody asked for, and
        //    merging them would be this client deciding one of them did not
        //    happen. What protects a mutation from its own duplicate is the
        //    idempotency key, which is a different mechanism for a different
        //    problem (`ENGINEERING_RULES.md` §7).
        const shareable = route.idempotent && body === null;
        const address = built.value.method + " " + url;

        if (shareable) {
            const running = this.inFlight.get(address);
            if (running !== undefined) {
                return (await running.join(signal, fail(kAborted) as CallResult<A, CallableRoute>)) as CallResult<A, R>;
            }
        }

        const run = (shared: AbortSignal): Promise<CallResult<A, R>> =>
            this.attempts(route, {
                url,
                method: built.value.method,
                body,
                contentType,
                capability,
                signal: shared,
                isRefresh,
                onFreshness: options.onFreshness ?? null,
            });

        if (!shareable) {
            return await run(signal);
        }

        const shared = new Shared<CallResult<A, CallableRoute>>(
            (inner) => run(inner) as Promise<CallResult<A, CallableRoute>>,
            () => {
                this.inFlight.delete(address);
            },
        );
        this.inFlight.set(address, shared);
        // Both arms, rather than `finally`: a `finally` builds a DERIVED promise
        // that rejects when the original does, and nothing is attached to that
        // one. Every task body catches (`docs/00-architecture.md` §3), and the
        // shape of not doing so is an unhandled rejection in a tab nobody is
        // looking at.
        const forget = (): void => {
            this.inFlight.delete(address);
        };
        void shared.promise.then(forget, forget);
        return (await shared.join(signal, fail(kAborted) as CallResult<A, CallableRoute>)) as CallResult<A, R>;
    }

    private async attempts<R extends CallableRoute>(
        route: R,
        request: {
            readonly url: string;
            readonly method: string;
            readonly body: SendBody | null;
            readonly contentType: string | null;
            readonly capability: Capability<string> | null;
            readonly signal: AbortSignal;
            readonly isRefresh: boolean;
            readonly onFreshness: ((directives: CacheControl) => void) | null;
        },
    ): Promise<CallResult<A, R>> {
        // 7. One key for the whole call, reused by every attempt at it — a
        //    transport retry, a backoff retry and the replay after a refresh are
        //    attempts at one request (`wire/idempotency.ts`).
        const key = carriesIdempotencyKey(route) ? mintIdempotencyKey() : null;
        const burnsCapability = request.capability !== null && burnsOnUse(this.config.api.singleUse, route.capability);

        const startedAtMs = this.now();
        let attempt = 1;
        let retries = 0;
        // The refresh route starts as though it had already replayed: there is
        // no second credential behind the one it is renewing.
        let replayed = request.isRefresh;
        let queueWaitMs = 0;
        let lastRequestId: string | null = null;

        for (;;) {
            const generation = this.credentials.generation();
            const attempted = await this.attemptOnce(route, request, key);
            queueWaitMs += attempted.queueWaitMs;

            if (attempted.outcome.ok) {
                this.telemetry.request({
                    routeId: route.id,
                    status: attempted.status,
                    requestId: null,
                    durationMs: this.now() - startedAtMs,
                    queueWaitMs,
                    retries,
                    transport: null,
                });
                return ok(attempted.outcome.value) as CallResult<A, R>;
            }

            const error = attempted.outcome.error;
            if (error.kind === "server") {
                lastRequestId = error.requestId;
                if (error.status === 429) {
                    // The whole bucket waits out the window the server named,
                    // not just this call.
                    this.limiter.holdBack(
                        route.rateLimit,
                        attempted.retryAfterMs ?? 0,
                        this.now(),
                    );
                }
            }

            const action = retryAction(
                {
                    error,
                    attempt,
                    idempotent: route.idempotent,
                    carriesKey: key !== null,
                    burnsCapability,
                    retryAfterMs: attempted.retryAfterMs,
                    replayed,
                },
                this.config.retry ?? kDefaultRetryPolicy,
            );

            if (action.kind === "retry") {
                await this.sleep(action.delayMs, request.signal);
                if (request.signal.aborted) {
                    return fail(kAborted) as CallResult<A, R>;
                }
                attempt += 1;
                retries += 1;
                continue;
            }

            if (action.kind === "refresh") {
                const replay = await this.replayAfterRefresh(generation, request.signal);
                if (replay) {
                    this.telemetry.count("replay");
                    replayed = true;
                    attempt += 1;
                    retries += 1;
                    continue;
                }
            }

            if (action.kind === "stale-session") {
                // The server disagreed with the local permission copy, which is
                // what anvil's `perm_epoch` exists because of. Refetched so the
                // affordances stop lying; the failure is still the answer.
                this.telemetry.count("stale-session");
                void this.config.session.refetch(this.lifetime.signal);
            }

            this.telemetry.request({
                routeId: route.id,
                status: attempted.status,
                requestId: lastRequestId,
                durationMs: this.now() - startedAtMs,
                queueWaitMs,
                retries,
                transport: error.kind === "transport" ? error.cause : null,
            });
            return fail(error) as CallResult<A, R>;
        }
    }

    // Whether the request may be sent again: either because the credential has
    // already changed under this request, or because this tab's refresh
    // succeeded.
    private async replayAfterRefresh(generation: number, signal: AbortSignal): Promise<boolean> {
        // A request that left before a refresh and came back after one is
        // evidence that the OLD credential was expired, which is already known.
        // Replaying it costs one request; refreshing again costs a rotation.
        if (this.credentials.generation() !== generation) {
            return true;
        }
        const outcome = await this.credentials.refreshOnce(signal);
        return outcome === "refreshed";
    }

    private async attemptOnce<R extends CallableRoute>(
        route: R,
        request: {
            readonly url: string;
            readonly method: string;
            readonly body: SendBody | null;
            readonly contentType: string | null;
            readonly capability: Capability<string> | null;
            readonly signal: AbortSignal;
            readonly onFreshness: ((directives: CacheControl) => void) | null;
        },
        key: string | null,
    ): Promise<Attempted> {
        const nothingSent = (error: HammerError<string, string>): Attempted => ({
            outcome: fail(error),
            status: null,
            retryAfterMs: null,
            queueWaitMs: 0,
        });

        // The circuit first: it is the cheapest refusal there is, and the whole
        // point of it is to be cheaper than the timeout it replaces.
        const admitted = this.breaker.admit(this.now());
        if (!admitted.ok) {
            this.telemetry.count("circuit-open");
            return nothingSent(admitted.error);
        }

        // 4. The local budget.
        const budget = this.limiter.admit(route.rateLimit, this.now());
        if (!budget.ok) {
            return { outcome: budget, status: null, retryAfterMs: null, queueWaitMs: 0 };
        }

        // 8. Queue admission. A full queue sheds here rather than growing.
        const ran = await this.queue.run(async (queueWaitMs) => {
            const sent = await this.send1(request, key);
            return { sent, queueWaitMs };
        }, request.signal);

        if (!ran.ok) {
            if (ran.error.kind === "client") {
                this.telemetry.count("request-shed");
            }
            return { outcome: fail(ran.error), status: null, retryAfterMs: null, queueWaitMs: 0 };
        }

        const { sent, queueWaitMs } = ran.value;
        return { ...sent, queueWaitMs };
    }

    // 9, 10. The one `fetch` in this library, and the one decode site.
    private async send1(
        request: {
            readonly url: string;
            readonly method: string;
            readonly body: SendBody | null;
            readonly contentType: string | null;
            readonly capability: Capability<string> | null;
            readonly signal: AbortSignal;
            readonly onFreshness: ((directives: CacheControl) => void) | null;
        },
        key: string | null,
    ): Promise<Omit<Attempted, "queueWaitMs">> {
        const headers: Record<string, string> = { Accept: "application/json" };
        if (request.contentType !== null) {
            headers["Content-Type"] = request.contentType;
        }
        if (key !== null) {
            headers[kIdempotencyHeader] = key;
        }
        if (request.capability !== null) {
            headers[kCapabilityHeader] = capabilityToken(request.capability);
        }

        // The body for THIS attempt, built here because here is where an attempt
        // is made. A re-readable body hands back the same object; a one-shot one
        // builds a second (`RequestBody`).
        const outgoing = request.body === null ? null : bodyFor(request.body);

        // A streamed request body requires `duplex: "half"` by the Fetch
        // standard, and the platform's TypeScript types do not carry the key.
        // Attached here rather than spelled in the literal below, which is the
        // honest form of "the platform knows this and the types do not" — and
        // attached in the ONE place a request is built, so an upload does not
        // have to know it.
        const duplex =
            outgoing instanceof ReadableStream ? ({ duplex: "half" } as RequestInit) : {};

        let response: Response;
        try {
            response = await this.config.fetch(request.url, {
                ...duplex,
                method: request.method,
                headers,
                body: outgoing,
                signal: request.signal,
                // The cookies are the session. This is the only place in the
                // library that says so, which is why a second `credentials:`
                // anywhere else is a build failure.
                credentials: "include",
                // A redirect is an instruction to send this request somewhere
                // else, and "somewhere else" is precisely what every path check
                // in this library exists to bound. An API route does not answer
                // with one; a misconfiguration does.
                redirect: "error",
            });
        } catch {
            // `fetch` rejects for a network failure, a refused redirect and an
            // abort alike, and the signal is the only thing that separates the
            // last one.
            const cause = request.signal.aborted ? kAborted : kNetworkFailure;
            this.breaker.failed(this.now());
            return { outcome: fail(cause), status: null, retryAfterMs: null };
        }

        const status = response.status;
        const retryAfterMs = parseRetryAfter(
            response.headers.get("Retry-After"),
            response.headers.get("Date"),
        );

        // A 503 is the origin saying it cannot answer, which is the one server
        // reply that counts toward the circuit. Everything else — including a
        // 500 — is an answer, and a client that opened the circuit on one would
        // take out every route in the application because one handler threw.
        if (status === 503) {
            this.breaker.failed(this.now());
        } else {
            this.breaker.succeeded();
        }

        let body: unknown;
        if (!kBodyless.includes(status)) {
            try {
                body = await response.json();
            } catch {
                // A proxy's HTML error page, a truncated body, a gateway that
                // sent nothing. The decode answers for all three.
                body = undefined;
            }
        }

        const decoded = decodeEnvelope({ status, body }, this.vocabulary);

        // The freshness the SERVER granted, handed to whoever is going to store
        // the body. Read here because this is the one place a `Response` exists
        // — a layer above would have to be given the header, and a header given
        // to a layer above is a header that gets logged (`ENGINEERING_RULES.md` §5).
        if (decoded.ok && request.onFreshness !== null) {
            request.onFreshness(parseCacheControl(response.headers.get("Cache-Control")));
        }

        return {
            outcome: decoded as Result<unknown, HammerError<string, string>>,
            status,
            retryAfterMs,
        };
    }

    // The refresh request, made through the same pipeline as everything else so
    // that it is queued, bounded and observable like any other call — and made
    // with `refreshRoute`, because the client cannot know which route that is.
    private async refresh(signal: AbortSignal): Promise<boolean> {
        const answered = await this.perform(
            this.config.refreshRoute,
            { signal, params: {} },
            null,
            null,
            true,
        );
        return answered.ok;
    }
}

export function createClient<A extends ApiTypes>(config: ClientConfig<A>): Client<A> {
    return new Client<A>(config);
}
