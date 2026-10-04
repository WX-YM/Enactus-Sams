// The switch that stops a client attacking a server that is already down.
//
// Twenty tabs retrying independently is a self-inflicted denial of service
// (`CLAUDE.md` §6), and it arrives at the worst possible moment: a server that
// is failing is a server whose capacity to answer a retry storm is exactly the
// capacity it has already run out of. The breaker converts "every request in
// this tab fails after a timeout" into "every request in this tab fails
// immediately, and one of them per window actually goes", which is also a
// better interface — a spinner that resolves in a second is more useful than one
// that resolves in thirty.
//
// --- what counts as a failure -----------------------------------------------
//
// Transport failures, and the one server answer that means the same thing: a
// 503. The distinction is whether the FAILURE IS ABOUT THIS REQUEST. A 404, a
// 409 and a validation failure are answers — the server is up, it read the
// request and it disagreed with it — and counting them would open the circuit
// on a screen whose form is being filled in wrongly. A timeout, a dropped
// connection and a shed 503 are the server or the path to it being unable to
// answer at all, and the next request will find the same thing.
//
// A 500 is deliberately NOT counted. It is a bug in one handler far more often
// than it is a process in trouble, and opening the circuit on it would take out
// every other route in the application because one of them threw.
//
// --- per origin --------------------------------------------------------------
//
// The scope is the origin because that is what is failing: a route cannot be
// down on its own, and a per-route breaker would need N consecutive failures on
// each of N routes before any of them stopped trying. One client drives one API
// origin, so one client holds one of these.

import type { TransportError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

export type BreakerConfig = {
    // Consecutive failures before the circuit opens. Consecutive, not a rate:
    // one success is evidence the origin is answering, and a rate over a window
    // is a second clock to reason about for a mechanism whose whole value is
    // that it is obvious.
    readonly failureThreshold: number;

    // How long it stays open before one probe is allowed through.
    readonly openMs: number;
};

// Five failures in a row is a server that is down rather than a request that was
// unlucky; five seconds is short enough that a recovery is noticed inside the
// attention span of whoever is watching a spinner, and long enough that a tab is
// not itself the load.
export const kDefaultBreaker: BreakerConfig = { failureThreshold: 5, openMs: 5000 };

export type BreakerState =
    // Requests go.
    | "closed"
    // Requests fail immediately.
    | "open"
    // One request is through and every other is still failing immediately. The
    // state exists because "let one through" and "let everything through" are
    // very different amounts of load on a server that has just come back.
    | "probing";

const kOpen: TransportError = { kind: "transport", cause: "circuit-open" };

export class CircuitBreaker {
    private readonly config: BreakerConfig;
    private consecutiveFailures: number;
    private openedAtMs: number;
    private probing: boolean;

    constructor(config: BreakerConfig = kDefaultBreaker) {
        this.config = config;
        this.consecutiveFailures = 0;
        this.openedAtMs = 0;
        this.probing = false;
    }

    state(monotonicNowMs: number): BreakerState {
        if (this.consecutiveFailures < this.config.failureThreshold) {
            return "closed";
        }
        if (this.probing) {
            return "probing";
        }
        return monotonicNowMs - this.openedAtMs < this.config.openMs ? "open" : "probing";
    }

    // Whether this request may be attempted. Admitting the probe is a state
    // change, which is why this is not a predicate: two callers asking "is it
    // open" and both proceeding is exactly the burst the probe exists to avoid.
    admit(monotonicNowMs: number): Result<void, TransportError> {
        if (this.consecutiveFailures < this.config.failureThreshold) {
            return ok();
        }
        if (this.probing) {
            return fail(kOpen);
        }
        if (monotonicNowMs - this.openedAtMs < this.config.openMs) {
            return fail(kOpen);
        }
        this.probing = true;
        return ok();
    }

    // The origin answered. One success closes it, and that is deliberate: the
    // probe is the evidence, and requiring several would keep the circuit open
    // through a recovery while refusing the requests that would have proven it.
    succeeded(): void {
        this.consecutiveFailures = 0;
        this.probing = false;
        this.openedAtMs = 0;
    }

    failed(monotonicNowMs: number): void {
        if (this.probing) {
            // The probe found it still down. Straight back to open, and the
            // window restarts from now rather than from when it first opened.
            this.probing = false;
            this.openedAtMs = monotonicNowMs;
            this.consecutiveFailures += 1;
            return;
        }

        this.consecutiveFailures += 1;
        if (this.consecutiveFailures === this.config.failureThreshold) {
            this.openedAtMs = monotonicNowMs;
        }
    }
}
