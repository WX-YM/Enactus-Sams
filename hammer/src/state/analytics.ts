// Reporting what happened, and refusing to record what may not be recorded.
//
// --- consent is at the door, and that is the whole design --------------------
//
// An event that requires consent is **not queued** before consent. Not queued
// and filtered later, not buffered pending a decision, not written down and
// excluded at the end: a buffer that flushes when consent arrives is a buffer of
// pre-consent data, which is the thing consent was about. anvil's `ingest.h`
// makes exactly this argument on the server side, and "recorded and then
// excluded" is one forgotten filter away from being no policy at all
// (`docs/01-seams.md` §10).
//
// So the gate is the first statement in `report`, before the batch exists, and
// the refusal is counted rather than silent — a rate that is not zero means a
// screen is reporting events it was never given permission to report, and
// noticing that from the server side means noticing an absence.
//
// --- the client does not sample ----------------------------------------------
//
// anvil samples whole sessions, deterministically, above a high-water mark. A
// second sampler here would produce a compound rate nobody can reason about, and
// the person who eventually needs the number cannot recover it.
//
// --- the last batch is the one that matters ----------------------------------
//
// A `fetch` from an unloading document is cancelled without an error, which is
// how a funnel loses its last step. The final flush goes through `sendBeacon`
// (`wire/beacon.ts`), and the beacon can refuse — so the flush reports whether it
// was taken rather than assuming.
//
// Nothing here schedules anything durable. A timer is a hint about the UI and
// never a guarantee about the work (`ENGINEERING_RULES.md` §6): a frozen or discarded tab
// loses whatever was still batched, which is why an event that MUST be recorded
// is recorded by the server on the request that caused it.

import type { Beacon } from "../wire/beacon.js";
import type { Sleep } from "../wire/schedule.js";

import type { CountSink } from "./counts.js";
import { kNoCounts } from "./counts.js";
import type { Readable } from "./store.js";
import { Store } from "./store.js";

// The part of a generated event `const` this reads. Structural, so no table from
// any application reaches this layer (`ENGINEERING_RULES.md` §1).
export type EventSpec = {
    readonly name: string;
    readonly code: number;
    readonly requiresConsent: boolean;

    // Each dimension's CLOSED set of values, as the descriptor emits it. The
    // server stores an index into that set, so a value outside it is a row the
    // ingest path drops — and a dropped row is the analytics defect nobody
    // notices for a quarter (`docs/01-seams.md` §10).
    readonly dimensions: Readonly<Record<string, readonly string[]>>;
};

// The dimensions of one event, as a type. Against an `as const` generated table
// each value narrows to the union the server will accept, so a typo is a compile
// error rather than a row that silently vanishes.
export type Dimensions<E extends EventSpec> = {
    readonly [K in keyof E["dimensions"]]: E["dimensions"][K] extends readonly (infer V)[]
        ? V
        : never;
};

// What leaves this module. The wire shape is the application's — anvil's ingest
// route is the application's route — so a batch of these is handed over and the
// application is what serialises it (`docs/01-seams.md` §4).
export type ReportedEvent = {
    readonly name: string;
    readonly code: number;
    readonly dimensions: Readonly<Record<string, string>>;
};

// What waits, which is a reported event plus the one fact a withdrawal needs.
// It is internal rather than part of `ReportedEvent` because the flag is this
// sink's bookkeeping and not something the ingest route was ever told about —
// putting it on the wire would be inventing a field the server has no column
// for (`ENGINEERING_RULES.md` §1).
type Queued = ReportedEvent & { readonly requiresConsent: boolean };

// Three states, not a boolean. "Not yet asked" and "asked and refused" are
// different facts: one of them may still become a grant, and the interface that
// asks needs to know which it is looking at. A boolean makes the pre-consent
// state indistinguishable from a refusal, which is how a consent prompt ends up
// asking somebody who already said no.
export type Consent = "unknown" | "granted" | "denied";

export type AnalyticsConfig = {
    // Where a batch goes on an ordinary flush. Returns whether it was taken; a
    // batch that was not taken is put back rather than dropped.
    readonly deliver: (
        batch: readonly ReportedEvent[],
        signal: AbortSignal,
    ) => Promise<boolean>;

    // Where the LAST batch goes, from a document that is unloading.
    readonly beacon: Beacon;

    // How many events may wait. A bound rather than a growing array, because a
    // tab open for days with a broken ingest route is otherwise a leak with a
    // slow fuse — and because the oldest events are the ones a person has
    // already stopped caring about, which is what makes dropping from the front
    // the right end to drop from.
    readonly maxBatch: number;

    // Flush when this many are waiting. Below `maxBatch`, so the bound is a
    // backstop rather than the normal path.
    readonly flushAt: number;

    // Flush every so often even when the batch is short, so a quiet session does
    // not hold its events until the tab closes. Zero is "only on demand".
    readonly flushEveryMs?: number;

    readonly sleep?: Sleep;
    readonly count?: CountSink;
};

export class AnalyticsSink {
    private readonly config: AnalyticsConfig;
    private readonly count: CountSink;
    private readonly batch: Queued[];
    private readonly lifetime: AbortController;

    // A store rather than a field, because the answer is rendered. A consent gate
    // has to redraw when consent changes — including when it changes in code the
    // gate did not call, which is every revocation made from a settings screen —
    // and a plain getter gives it no way to hear about that. The alternative was
    // every consumer keeping its own `Store<Consent>` beside the sink and writing
    // through it, which is two places that can disagree about whether somebody
    // consented.
    private readonly answer: Store<Consent>;

    private flushing: Promise<void> | null;
    private closed: boolean;

    // Counted rather than logged, and exposed so an application can report them
    // through its own seam (`docs/00-architecture.md` §9).
    refused = 0;
    dropped = 0;

    constructor(config: AnalyticsConfig) {
        if (config.flushAt < 1 || config.maxBatch < config.flushAt) {
            throw new Error("a sink flushes below the bound it drops at");
        }
        this.config = config;
        this.count = config.count ?? kNoCounts;
        this.batch = [];
        this.lifetime = new AbortController();
        this.answer = new Store<Consent>("unknown");
        this.flushing = null;
        this.closed = false;
    }

    get pending(): number {
        return this.batch.length;
    }

    // The read side, for whatever draws the gate. The write side stays on
    // `setConsent`: a consumer handed this cannot decide that somebody consented.
    get consent(): Readable<Consent> {
        return this.answer;
    }

    consentIs(): Consent {
        return this.answer.get();
    }

    // The answer, when there is one. Withdrawing consent DROPS what is waiting:
    // a batch collected under a grant that has since been withdrawn is exactly
    // the data the withdrawal was about, and sending it because it was already
    // in memory is the "recorded and then excluded" policy wearing a different
    // hat.
    setConsent(answer: Consent): void {
        if (this.answer.get() === answer) {
            return;
        }
        // Dropped BEFORE the store notifies. A subscriber that re-read `pending`
        // from inside its own callback would otherwise see the batch that the
        // withdrawal is in the middle of discarding.
        if (answer !== "granted") {
            this.dropConsented();
        }
        this.answer.set(answer);
    }

    // The gate, and it is the first thing here rather than a filter later.
    report<E extends EventSpec>(event: E, dimensions: Dimensions<E>): void {
        if (this.closed) {
            return;
        }
        if (event.requiresConsent && this.answer.get() !== "granted") {
            this.refused += 1;
            this.count("consent-refused");
            return;
        }

        if (this.batch.length >= this.config.maxBatch) {
            // From the front: the oldest event is the one whose moment has
            // already passed.
            this.batch.shift();
            this.dropped += 1;
        }

        this.batch.push({
            name: event.name,
            code: event.code,
            requiresConsent: event.requiresConsent,
            // Copied rather than held. The caller's object may be reused for the
            // next report, and a batch of aliases is a batch of whatever the last
            // one said.
            dimensions: { ...(dimensions as Readonly<Record<string, string>>) },
        });

        if (this.batch.length >= this.config.flushAt) {
            void this.flush();
        }
    }

    // Sends what is waiting. Concurrent callers join the one in flight: two
    // flushes over one array is one batch sent twice and one sent never.
    flush(): Promise<void> {
        const running = this.flushing;
        if (running !== null) {
            return running;
        }
        if (this.batch.length === 0) {
            return Promise.resolve();
        }

        const started = this.deliver();
        this.flushing = started;
        const settle = (): void => {
            if (this.flushing === started) {
                this.flushing = null;
            }
        };
        void started.then(settle, settle);
        return started;
    }

    // The last one, from a document that is going away. The application calls
    // this from `pagehide` — hammer names no document (`tools/check-layering.sh`)
    // and would be adding a listener to something it did not create.
    //
    // Returns whether the browser took it. False is a real answer: the payload
    // may be over the user agent's queue limit, or a policy may refuse the
    // destination.
    flushFinal(): boolean {
        if (this.batch.length === 0) {
            return true;
        }
        const held = this.batch.splice(0, this.batch.length);
        const taken = this.config.beacon(JSON.stringify(sendable(held)));
        if (!taken) {
            // Put back. The page may be hidden rather than unloading — `pagehide`
            // fires for both — so a batch the browser refused may still get a
            // chance on an ordinary flush.
            for (let i = held.length - 1; i >= 0; i -= 1) {
                const event = held[i];
                if (event !== undefined) {
                    this.batch.unshift(event);
                }
            }
        }
        return taken;
    }

    // Starts the periodic flush. Separate from the constructor because it is the
    // one thing here that runs on its own, and a store that began a loop the
    // moment it was built would be a module with a side effect
    // (`ENGINEERING_RULES.md` §2.1).
    start(): void {
        const everyMs = this.config.flushEveryMs ?? 0;
        const sleep = this.config.sleep;
        if (everyMs <= 0 || sleep === undefined) {
            return;
        }
        void this.tick(everyMs, sleep);
    }

    close(): void {
        this.closed = true;
        this.lifetime.abort();
        this.batch.length = 0;
        // The answer itself is not reset. A closed sink has no opinion to
        // publish, and a gate still on screen reading `"unknown"` from a sink
        // that was torn down would ask somebody who has already answered.
        this.answer.close();
    }

    private async tick(everyMs: number, sleep: Sleep): Promise<void> {
        // Every task body catches (`docs/00-architecture.md` §3). An unhandled
        // rejection here would be a loop that stopped without anybody noticing
        // it had, which reads as analytics that quietly went quiet.
        while (!this.closed) {
            try {
                await sleep(everyMs, this.lifetime.signal);
            } catch {
                return;
            }
            if (this.closed) {
                return;
            }
            try {
                await this.flush();
            } catch {
                // A `deliver` that threw rather than returning false. The batch
                // is still there and the next tick will try again.
            }
        }
    }

    private async deliver(): Promise<void> {
        const held = this.batch.splice(0, this.batch.length);
        let taken = false;
        try {
            taken = await this.config.deliver(sendable(held), this.lifetime.signal);
        } catch {
            taken = false;
        }
        if (taken || this.closed) {
            return;
        }

        // Put back at the front, oldest first, and re-bounded: a route that has
        // been failing for an hour must not be a growing array.
        for (let i = held.length - 1; i >= 0; i -= 1) {
            const event = held[i];
            if (event !== undefined) {
                this.batch.unshift(event);
            }
        }
        while (this.batch.length > this.config.maxBatch) {
            this.batch.shift();
            this.dropped += 1;
        }
    }

    // Everything that needed consent goes; everything that did not, stays. A
    // sink that dropped the lot would lose the operational events an application
    // is entitled to have — a failed request, a refused upload — over a decision
    // that was never about them.
    //
    // In place and backwards, so the events that survive keep their order
    // without a second array being built out of a decision that is usually about
    // nothing.
    private dropConsented(): void {
        for (let i = this.batch.length - 1; i >= 0; i -= 1) {
            if (this.batch[i]?.requiresConsent === true) {
                this.batch.splice(i, 1);
            }
        }
    }
}

// The bookkeeping flag off, because it was never part of what an ingest route
// was told to expect.
function sendable(batch: readonly Queued[]): readonly ReportedEvent[] {
    const out: ReportedEvent[] = [];
    for (const event of batch) {
        out.push({ name: event.name, code: event.code, dimensions: event.dimensions });
    }
    return out;
}
