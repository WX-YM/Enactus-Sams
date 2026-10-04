// Whether to try again, decided once, here.
//
// Retry policy belongs to the library and never to a call site (`CLAUDE.md`
// §6): a call site that decides for itself is a call site that will retry a
// payment, invent a backoff against a server that named one, or loop on a 403
// until the account is locked. This module is a pure function of what happened,
// so the whole policy is one file somebody can read in a sitting.
//
// --- it branches on the HTTP STATUS, not on the error code ------------------
//
// The obvious implementation is a table keyed by anvil's error codes, and it is
// worse in two ways that only show up in production.
//
// The first is that the codes are the DESCRIPTOR's vocabulary
// (`docs/01-seams.md` §2), and a copy of them in here would be the copy nobody
// updates — the thing `wire/envelope.ts` takes a vocabulary parameter to avoid.
//
// The second is the one that actually costs a user something: A FAILURE MAY
// NEVER HAVE REACHED anvil AT ALL. A proxy's 502 is an HTML page and a gateway
// timeout may be empty, so both decode to `Unknown` with no code to branch on —
// and they are precisely the failures where retrying is right. Branching on the
// status gets those cases correct for free, and it is why the decoded error
// carries the status beside the code.
//
// Where the status is genuinely ambiguous, the tie is broken by what THIS
// CLIENT knows about the call it made — whether a capability was presented,
// whether a key is on the wire — rather than by a second look at the body.
//
// --- what "safe to repeat" means --------------------------------------------
//
// Two conditions, and neither is about the verb:
//
//   REPEATABLE. The descriptor's `idempotent` flag, or an idempotency key on
//   the wire. The key is what converts at-least-once on the wire into
//   at-most-once at the server, which is the bargain `wire/idempotency.ts`
//   exists for.
//
//   NOT BURNING A GRANT. A call presenting a single-use capability is never
//   auto-retried, whatever else is true. The server consumed the token whether
//   or not the response arrived, so the retry is answered with a capability
//   failure and reports failure for an operation that succeeded
//   (`docs/01-seams.md` §6).

import type { HammerError } from "../core/errors.js";

export type RetryPolicy = {
    // Total attempts, the first one included. Three is two retries.
    readonly maxAttempts: number;

    // The first backoff, doubled per attempt and then jittered.
    readonly baseDelayMs: number;

    // The ceiling the doubling is clamped to. A backoff that keeps doubling
    // outlives the screen that wanted the answer.
    readonly maxDelayMs: number;
};

export const kDefaultRetryPolicy: RetryPolicy = {
    maxAttempts: 3,
    baseDelayMs: 200,
    maxDelayMs: 8000,
};

export type RetryFacts = {
    readonly error: HammerError;

    // The attempt that just failed, counting from one.
    readonly attempt: number;

    // The route's descriptor flag.
    readonly idempotent: boolean;

    // An idempotency key is on the wire for this call.
    readonly carriesKey: boolean;

    // A single-use capability was presented and is now spent.
    readonly burnsCapability: boolean;

    // What the server named, in milliseconds, or null when it named nothing.
    readonly retryAfterMs: number | null;

    // A credential refresh has already been performed and replayed for this
    // call. A second `401` after one is a rejection rather than a race
    // (`docs/00-architecture.md` §5).
    readonly replayed: boolean;
};

export type RetryAction =
    // Report the failure. Everything that is not below is this.
    | { readonly kind: "give-up" }
    // Wait, then send the same request again with the same idempotency key.
    | { readonly kind: "retry"; readonly delayMs: number }
    // One leader-owned credential refresh, then replay this call once.
    | { readonly kind: "refresh" }
    // Never retried, and the local permission copy disagreed with the server:
    // refetch the session so the affordances stop lying, and report the
    // failure (`docs/00-architecture.md` §4.1).
    | { readonly kind: "stale-session" };

const kGiveUp: RetryAction = { kind: "give-up" };
const kRefresh: RetryAction = { kind: "refresh" };
const kStaleSession: RetryAction = { kind: "stale-session" };

// A number in [0, 1), from the platform's CSPRNG.
//
// `Math.random` is banned for this and `tools/check-source-bans.sh` is what
// enforces it, because the reason is not obvious enough to survive review:
// jitter from a predictable source is jitter that SYNCHRONISES. Every client
// seeded the same way — which is what a deterministic generator started at page
// load amounts to — backs off by the same amount and retries in the same
// millisecond, which is the thundering herd the jitter was added to prevent.
export type UnitRandom = () => number;

export const randomUnit: UnitRandom = () => {
    const draw = new Uint32Array(1);
    crypto.getRandomValues(draw);
    return (draw[0] ?? 0) / 0x1_0000_0000;
};

// Full jitter: anywhere in [0, backoff), rather than backoff plus a wobble.
//
// The variants that keep a floor — "backoff/2 plus a random half" — retain the
// property that made the herd: every client that failed together is still in
// the same narrow band, just a wider one. Full jitter spreads them across the
// whole interval, and the cost is that one unlucky client retries almost
// immediately, which the attempt cap already bounds.
export function fullJitter(
    attempt: number,
    policy: RetryPolicy,
    unit: UnitRandom = randomUnit,
): number {
    const doubled = policy.baseDelayMs * 2 ** Math.max(0, attempt - 1);
    return Math.floor(unit() * Math.min(policy.maxDelayMs, doubled));
}

function repeatable(facts: RetryFacts): boolean {
    if (facts.burnsCapability) {
        return false;
    }
    return facts.idempotent || facts.carriesKey;
}

// The delay to wait before the next attempt: exactly what the server named, or
// a jittered backoff when it named nothing.
//
// `Retry-After` is honoured EXACTLY and is never jittered. A client that adds
// its own spread to a number the server chose has decided it knows better than
// the only participant that can see the window, and the spread it adds is the
// half that retries back INTO the outage it was told to wait out. The variance
// a herd needs is already there: the windows themselves started at different
// times.
function delayFor(facts: RetryFacts, policy: RetryPolicy, unit: UnitRandom): number {
    if (facts.retryAfterMs !== null) {
        return facts.retryAfterMs;
    }
    return fullJitter(facts.attempt, policy, unit);
}

export function retryAction(
    facts: RetryFacts,
    policy: RetryPolicy = kDefaultRetryPolicy,
    unit: UnitRandom = randomUnit,
): RetryAction {
    const { error } = facts;

    // A local refusal is never retried by the library: the cap it hit is the
    // cap the retry would hit, and the caller is the one who can change the
    // request (`core/errors.ts`). A stale client is not a failure of the
    // request at all.
    if (error.kind === "client" || error.kind === "stale-client") {
        return kGiveUp;
    }

    if (error.kind === "transport") {
        // An abort is the screen saying it no longer wants the answer, and an
        // open circuit is this client already having decided. Retrying either
        // is arguing with a decision that was just made.
        if (error.cause === "aborted" || error.cause === "circuit-open") {
            return kGiveUp;
        }
        if (!repeatable(facts) || facts.attempt >= policy.maxAttempts) {
            return kGiveUp;
        }
        return { kind: "retry", delayMs: delayFor(facts, policy, unit) };
    }

    const retryIfSafe = (): RetryAction => {
        if (!repeatable(facts) || facts.attempt >= policy.maxAttempts) {
            return kGiveUp;
        }
        return { kind: "retry", delayMs: delayFor(facts, policy, unit) };
    };

    switch (error.status) {
        // The access credential expired. Not a login prompt: that is what the
        // refresh is for, and a client that presented one before the refresh
        // had failed would ask people to sign in during a deploy.
        case 401:
            return facts.replayed ? kGiveUp : kRefresh;

        // Denied. Never retried — the server will say the same thing — but the
        // local permission copy is now known to disagree with the server's, and
        // anvil's `perm_epoch` exists because a copy goes stale.
        //
        // Unless a capability was presented, in which case the likelier reading
        // is a token that is spent or expired rather than a permission that
        // changed, and refetching the session would be a request that answers a
        // question nobody asked. anvil maps a capability failure to 403 as well,
        // which is why the status alone cannot separate them.
        case 403:
            return facts.burnsCapability ? kGiveUp : kStaleSession;

        // A proxy gave up waiting for the request. anvil does not send it; a
        // load balancer does, and it means the request may never have run.
        case 408:
            return retryIfSafe();

        // The server named a window. Honoured exactly, and the whole bucket is
        // held back by the caller (`wire/rate_limit.ts`).
        case 429:
            return retryIfSafe();

        // The handler ran and threw. The one case where an idempotency key is
        // NOT enough on its own: the key protects a repeat only if the
        // application wired that route to anvil's store, and a 500 is the
        // failure mode where the work may already have happened. So it is
        // retried for a naturally idempotent route and for nothing else, which
        // is the rule `docs/00-architecture.md` §6 states.
        case 500:
            if (!facts.idempotent || facts.burnsCapability) {
                return kGiveUp;
            }
            return retryIfSafe();

        // The origin could not answer: a shed queue, a bad gateway, a gateway
        // timeout. The request most likely never ran, which is what makes these
        // the retryable ones.
        case 502:
        case 503:
        case 504:
            return retryIfSafe();

        // Everything else is an ANSWER — not found, conflict, version mismatch,
        // validation, payload too large, unsupported media, a capability that is
        // required or spent, insufficient storage. The server read the request
        // and disagreed with it, and the same request gets the same answer. The
        // recovery for each belongs to the screen, and for a version mismatch it
        // is a re-read rather than a retry, which is a lost update with extra
        // steps.
        default:
            return kGiveUp;
    }
}

// --- Retry-After ------------------------------------------------------------

// Whole seconds, which is what anvil sends: `retry_after_seconds()` rounds up,
// never returns zero, and clamps to the rule's own window.
const kDeltaSeconds = /^[0-9]{1,7}$/;

// A ceiling on what this client will honour. A proxy or a misconfiguration can
// name an hour, and a screen that has silently decided to do nothing for an hour
// is indistinguishable from one that is broken.
const kMaxRetryAfterMs = 120_000;

// `Retry-After`, as a duration.
//
// The HTTP-date form is the interesting half. Comparing it to `Date.now()` is
// exactly the mistake `core/time.ts` exists to make unspellable — the device
// clock is user-settable and routinely minutes out, so the difference between
// the two is wrong by an amount nothing on the device can measure. But the
// response carries the server's own `Date`, and the difference between two
// values the SERVER produced is a duration its clock measured on both ends.
// That is the only honest reading of it, and where the header is absent the
// only honest answer is that there is no duration here — the policy's backoff
// then applies, which is what would have happened anyway.
export function parseRetryAfter(retryAfter: string | null, serverDate: string | null): number | null {
    if (retryAfter === null) {
        return null;
    }

    const value = retryAfter.trim();
    if (kDeltaSeconds.test(value)) {
        return Math.min(kMaxRetryAfterMs, Number(value) * 1000);
    }

    if (serverDate === null) {
        return null;
    }
    const until = Date.parse(value);
    const from = Date.parse(serverDate);
    if (Number.isNaN(until) || Number.isNaN(from)) {
        return null;
    }
    const durationMs = until - from;
    if (durationMs <= 0) {
        return null;
    }
    return Math.min(kMaxRetryAfterMs, durationMs);
}
