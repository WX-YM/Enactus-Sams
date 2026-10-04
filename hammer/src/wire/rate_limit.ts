// The local copy of the server's budget, spent before a round trip rather than
// after one.
//
// It is advisory and it is NOT the limit — the server's is
// (`docs/01-seams.md` §11). What it exists for is narrower and more specific
// than "rate limiting": an interface must not spend a person's budget on
// retries THIS LIBRARY decided to make, and then present them with a lockout
// they did not cause. A screen that fires ten requests, gets a 429 on one and
// retries the other nine into the same window is how a rate limit becomes a
// lockout, and it is a client-side defect end to end.
//
// --- why a local refusal is sound here and is not elsewhere -----------------
//
// The permission pre-check may never refuse a call (`wire/affordance.ts`),
// because the local copy can be STALE in the direction that denies something
// the server would have allowed. This is the opposite: the bucket is per tab,
// so it can only ever have counted FEWER events than the server has — other
// tabs, other devices and the same user's phone all spend the same budget and
// none of them are in here. A bucket that reads empty locally is therefore one
// the server would also have refused, and the refusal costs a round trip
// instead of making one.
//
// --- a 429 holds back the bucket, not the call ------------------------------
//
// anvil derives its `Retry-After` from the BUCKET — keyed by identity and rule
// rather than by route, so two routes that must be indistinguishable produce
// the same number as well as the same status (`http/retry_after.h`). The client
// side of that is to hold the whole bucket for the duration the server named:
// retrying the other nine calls while one waits is the lockout again, arrived at
// from the other direction.

import type { ClientError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

// The shape of a row in the generated `kRateLimits`, structurally. The table is
// the application's and arrives through the descriptor; nothing here knows a
// bucket name (`CLAUDE.md` §1).
export type BucketSpec = {
    readonly windowMs: number;
    readonly maxEvents: number;
};

export type RateLimitTable = Readonly<Record<string, BucketSpec>>;

// One shape, every field declared where it is created, and nothing added later:
// a shape change deoptimises every call site that has already seen the old one
// (`CLAUDE.md` §2.3).
type Bucket = {
    // Fractional on purpose. An integer count would refill in steps of a whole
    // event, so a bucket of five events per hour would admit nothing for
    // twelve minutes after the first one and then admit two at once.
    tokens: number;
    refilledAtMs: number;

    // When a `429` said to stop. Held for the whole bucket, and separate from
    // the token count because the two expire independently: the tokens refill
    // on their own schedule and this one is the server's instruction.
    heldUntilMs: number;
};

function bucketAt(spec: BucketSpec, nowMs: number): Bucket {
    return { tokens: spec.maxEvents, refilledAtMs: nowMs, heldUntilMs: 0 };
}

function refill(bucket: Bucket, spec: BucketSpec, nowMs: number): void {
    const elapsed = nowMs - bucket.refilledAtMs;
    if (elapsed <= 0) {
        return;
    }
    const gained = (elapsed * spec.maxEvents) / spec.windowMs;
    bucket.tokens = Math.min(spec.maxEvents, bucket.tokens + gained);
    bucket.refilledAtMs = nowMs;
}

function spent(retryAfterMs: number): ClientError {
    return { kind: "client", cause: "budget-spent", retryAfterMs };
}

export class RateLimiter {
    private readonly table: RateLimitTable;

    // Keyed by bucket name, and bounded by the descriptor: an entry can only
    // exist for a name the table declares, so this cannot grow with traffic.
    private readonly buckets: Map<string, Bucket>;

    constructor(table: RateLimitTable) {
        this.table = table;
        this.buckets = new Map();
    }

    // Whether a call against this bucket may be sent now, consuming one event
    // when it may.
    //
    // The clock is a parameter and it is the MONOTONIC one. A bucket driven by
    // the wall clock refills by an hour when the operating system corrects the
    // device's time, and empties for an hour when it corrects it the other way
    // (`CLAUDE.md` §6).
    admit(bucket: string | null, monotonicNowMs: number): Result<void, ClientError> {
        if (bucket === null) {
            return ok();
        }
        const spec = this.table[bucket];
        if (spec === undefined) {
            // A bucket this client's descriptor does not carry. It is the
            // server's to enforce and always was; inventing a bound for it here
            // would be this library deciding a policy it was not given.
            return ok();
        }

        const held = this.buckets.get(bucket) ?? bucketAt(spec, monotonicNowMs);
        this.buckets.set(bucket, held);

        if (monotonicNowMs < held.heldUntilMs) {
            return fail(spent(held.heldUntilMs - monotonicNowMs));
        }

        refill(held, spec, monotonicNowMs);
        if (held.tokens < 1) {
            // What is left until one token exists, which is a real duration
            // rather than the window: a caller told to wait a whole window for a
            // bucket that refills continuously waits for nothing.
            const missing = 1 - held.tokens;
            return fail(spent(Math.ceil((missing * spec.windowMs) / spec.maxEvents)));
        }

        held.tokens -= 1;
        return ok();
    }

    // What a `429` does. Applies to the whole bucket, for exactly the duration
    // the server named — `Retry-After` is honoured and never adjusted, because a
    // client that invents its own number retries straight back into the window
    // it was told to wait out.
    holdBack(bucket: string | null, retryAfterMs: number, monotonicNowMs: number): void {
        if (bucket === null || retryAfterMs <= 0) {
            return;
        }
        const spec = this.table[bucket];
        if (spec === undefined) {
            return;
        }

        const held = this.buckets.get(bucket) ?? bucketAt(spec, monotonicNowMs);
        held.heldUntilMs = Math.max(held.heldUntilMs, monotonicNowMs + retryAfterMs);

        // Empty, and refilling so that EXACTLY ONE event is available at the
        // instant the hold lifts. Both halves are corrections, in opposite
        // directions, and getting either wrong is a lockout:
        //
        //   The count is zeroed because the server has just said this client's
        //   local one was wrong. Resuming with a full bucket the moment the
        //   hold expires is the burst that earned the 429.
        //
        //   The refill is dated so that the hold's end is when the next event
        //   is due. Otherwise a `Retry-After: 1` against a fifteen-minute
        //   window would be honoured by the server and then refused locally for
        //   another quarter of an hour — the client overriding the only
        //   participant entitled to name that number.
        held.tokens = 0;
        held.refilledAtMs = held.heldUntilMs - spec.windowMs / spec.maxEvents;
        this.buckets.set(bucket, held);
    }
}
