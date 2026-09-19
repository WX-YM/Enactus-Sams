// The credential lifecycle, which holds no credential.
//
// anvil's tokens are `__Host-` cookies with `HttpOnly`, and every property that
// makes them worth using — host-only scope, unreadable by script, cleared by the
// server — is one hammer gets by having no implementation
// (`docs/00-architecture.md` §5). What is here is the ORCHESTRATION: who
// refreshes, when, exactly once, what replays afterwards, and what every other
// tab does about it.
//
// So nothing below reads, stores or forwards a token, and the fan-out carries
// no credential either. It carries NEWS. The refresh response sets cookies for
// the whole origin, so a tab that hears "it was refreshed" already has the new
// credential and never had to be handed one — which is the property that makes
// a broadcast channel an acceptable place to coordinate this at all.
//
// --- exactly one refresh, and why the obvious shape is not it ---------------
//
// The lock is not an optimisation. anvil rotates the refresh token as a
// compare-and-swap, so two tabs refreshing concurrently is a rotation race whose
// loser is logged out — with a valid session, in the tab somebody was using.
//
// The obvious implementation is "every tab that gets a 401 queues on the lock",
// and it produces exactly the second refresh it was meant to prevent: the
// follower acquires the lock the moment the leader releases it, which is BEFORE
// the leader's broadcast has been delivered, because delivery is asynchronous.
// So a follower does not queue. It fails to take the lock, and then waits for
// the message — and the only thing it needs a timer for is a leader that died
// mid-refresh, which the browser reports by releasing the lock and never
// sending anything.
//
// --- the refresh belongs to the session, not to the call that noticed -------
//
// A refresh is started because some request got a 401, and that request has a
// signal belonging to a screen that may be unmounting. Forwarding it would let a
// component's teardown cancel the session's refresh, which loses the session for
// every other screen in the tab. So the refresh runs under a signal this object
// owns; the caller's signal decides only whether the caller is still waiting.

import type { FanOut, Leadership } from "./leader.js";
import type { Sleep } from "./schedule.js";
import { sleep } from "./schedule.js";

// The lock and the channel are named by hammer, not by an application. They
// name a mechanism, so two applications on one origin would be coordinating the
// same thing anyway.
export const kRefreshLock = "hammer.refresh";

export type RefreshOutcome =
    // New credentials are in place. Replay the request that found the 401.
    | "refreshed"
    // The session is over. Every tab has dropped its caches and closed its
    // streams; there is nothing to replay.
    | "rejected"
    // The caller stopped waiting. Says nothing about the refresh, which is still
    // running under the session's own signal.
    | "aborted";

// What crosses tabs. Deliberately tiny and deliberately without a token: a
// message is data from a tab that may be running a different build of this
// application, and it is decoded rather than trusted.
type CredentialMessage = { readonly kind: "refreshed" } | { readonly kind: "logout" };

function decodeMessage(message: unknown): CredentialMessage | null {
    if (typeof message !== "object" || message === null) {
        return null;
    }
    const kind = (message as { readonly kind?: unknown }).kind;
    if (kind === "refreshed" || kind === "logout") {
        return { kind };
    }
    return null;
}

export type CredentialsConfig = {
    readonly leadership: Leadership;
    readonly fanOut: FanOut;

    // The refresh request itself, injected because a request is the client's to
    // make and this module is the one that decides WHEN. It answers whether the
    // session survived; it hands back nothing, because there is nothing to hand
    // back that is not a cookie.
    readonly refresh: (signal: AbortSignal) => Promise<boolean>;

    // What this tab drops when the session ends: caches, streams, anything
    // keyed by an identity that no longer exists. A tab left rendering a
    // signed-in shell after another tab signed out is one shared device away
    // from being a disclosure.
    readonly onLogout: () => void;

    // How long a follower waits for the leader's news before concluding the
    // leader is gone. Long enough to cover a slow refresh on a slow network,
    // short enough that a dead leader does not strand every other tab.
    readonly leaderWaitMs: number;

    readonly sleep: Sleep;
};

export const kDefaultLeaderWaitMs = 10_000;

export type CredentialCounts = {
    // Refreshes this tab performed.
    readonly refreshes: number;

    // Refreshes it performed with no election available — the degraded path.
    // This is `hammer_refresh_races_total` and it should be zero: a non-zero
    // value means the rotation race is live and people are being signed out by
    // their own second tab (`docs/00-architecture.md` §9).
    readonly unelected: number;

    // Times this tab concluded the leader had died and picked the work up.
    readonly takeovers: number;
};

export class Credentials {
    private readonly config: CredentialsConfig;
    private readonly session: AbortController;
    private readonly detach: () => void;

    // The refresh this tab is already doing. Every 401 in this tab joins it
    // rather than starting a second one: the lock coordinates tabs, and this
    // coordinates the dozen requests one screen had in flight when the
    // credential expired.
    private inFlight: Promise<RefreshOutcome> | null;

    // Resolvers for followers waiting on the leader's message.
    private readonly listeners: Set<(outcome: RefreshOutcome) => void>;

    private refreshes: number;
    private unelected: number;
    private takeovers: number;

    // How many times the credential this tab is using has changed, counting
    // refreshes performed anywhere in the session. See `generation()`.
    private credentialGeneration: number;

    constructor(config: CredentialsConfig) {
        this.config = config;
        this.session = new AbortController();
        this.inFlight = null;
        this.listeners = new Set();
        this.refreshes = 0;
        this.unelected = 0;
        this.takeovers = 0;
        this.credentialGeneration = 0;
        this.detach = config.fanOut.listen((message) => {
            this.onMessage(message);
        });
    }

    // Which credential a request was sent under.
    //
    // A request that left before a refresh and comes back with a 401 after one
    // is not evidence that the NEW credential is expired — it is evidence that
    // the old one was, which is already known. A caller that captures this
    // before sending and compares it afterwards can replay such a request
    // without asking for a second refresh, and the second refresh is the one
    // anvil's sixty-second rotation grace exists to survive rather than to
    // invite.
    generation(): number {
        return this.credentialGeneration;
    }

    counts(): CredentialCounts {
        return {
            refreshes: this.refreshes,
            unelected: this.unelected,
            takeovers: this.takeovers,
        };
    }

    // One refresh per expiry, per session, across every tab.
    //
    // The signal is the caller's and is used only to stop the caller waiting.
    // See the header: cancelling the refresh itself would let one screen's
    // teardown lose the session for every other screen in the tab.
    async refreshOnce(signal: AbortSignal): Promise<RefreshOutcome> {
        const running = this.inFlight ?? this.start();
        this.inFlight = running;

        const outcome = await this.untilAbort(running, signal);
        return outcome;
    }

    // The session is over: tell every tab, and drop what this one holds.
    //
    // Idempotent on purpose. It is reached from a second `401`, from an
    // application's sign-out and from another tab's message, and those can all
    // happen to one tab in any order.
    logout(): void {
        this.config.fanOut.post({ kind: "logout" });
        this.config.onLogout();
    }

    // Releases the channel listener and the session's signal. Nothing outlives
    // what created it (`ENGINEERING_RULES.md` §3.3).
    close(): void {
        this.detach();
        this.session.abort();
    }

    private async start(): Promise<RefreshOutcome> {
        try {
            return await this.elect();
        } finally {
            this.inFlight = null;
        }
    }

    private async elect(): Promise<RefreshOutcome> {
        if (this.config.leadership.degraded) {
            // No election to be had. One refresh per tab, and the race is
            // reported rather than hidden.
            this.unelected += 1;
            return await this.asLeader();
        }

        // The listener goes on BEFORE the lock is attempted, and the ordering is
        // load-bearing: taking the lock is asynchronous, so a leader whose
        // refresh is quick can finish and broadcast in the window between this
        // tab failing to take the lock and this tab starting to listen. A
        // follower that registered afterwards would then wait out the whole
        // leader window for news that had already been delivered. Found by a
        // test whose refresh resolved immediately, which is the only kind of
        // network a test has.
        const watch = this.watch();

        try {
            const led = await this.config.leadership.tryExclusive(kRefreshLock, () =>
                this.asLeader(),
            );
            if (led !== null) {
                return led;
            }

            // Another tab is refreshing. Wait for its news rather than queueing
            // on the lock — see the header.
            const heard = await this.awaitLeader(watch.news);
            if (heard !== null) {
                return heard;
            }

            // No news inside the window: the leader is gone. Take the lock the
            // waiting way and do the work it did not finish.
            this.takeovers += 1;
            return await this.config.leadership.exclusive(kRefreshLock, () => this.asLeader());
        } finally {
            watch.cancel();
        }
    }

    private async asLeader(): Promise<RefreshOutcome> {
        this.refreshes += 1;
        const survived = await this.config.refresh(this.session.signal);
        if (!survived) {
            // A refresh that fails is the session ending, and it ends for every
            // tab at once.
            this.logout();
            return "rejected";
        }
        this.credentialGeneration += 1;
        this.config.fanOut.post({ kind: "refreshed" });
        return "refreshed";
    }

    private watch(): { readonly news: Promise<RefreshOutcome>; readonly cancel: () => void } {
        let deliver: (outcome: RefreshOutcome) => void = () => {};
        const news = new Promise<RefreshOutcome>((resolve) => {
            deliver = resolve;
        });
        this.listeners.add(deliver);
        return {
            news,
            cancel: () => {
                this.listeners.delete(deliver);
            },
        };
    }

    private async awaitLeader(news: Promise<RefreshOutcome>): Promise<RefreshOutcome | null> {
        // The wait has a signal of its own so that news arriving first CANCELS
        // the timer rather than leaving it to fire ten seconds later: a timer
        // that is still pending holds its closure, and this one closes over the
        // follower's whole wait.
        const waiting = new AbortController();
        const onSessionClosed = (): void => {
            waiting.abort();
        };
        this.session.signal.addEventListener("abort", onSessionClosed, { once: true });

        try {
            return await Promise.race([
                news,
                this.config.sleep(this.config.leaderWaitMs, waiting.signal).then(() => null),
            ]);
        } finally {
            this.session.signal.removeEventListener("abort", onSessionClosed);
            waiting.abort();
        }
    }

    private onMessage(raw: unknown): void {
        const message = decodeMessage(raw);
        if (message === null) {
            return;
        }

        if (message.kind === "logout") {
            // Another tab's session ended, which means this one's did too: the
            // cookies are the origin's, not the tab's. Dropped locally, and not
            // re-broadcast — a fan-out that echoed would be a loop.
            this.config.onLogout();
            this.wake("rejected");
            return;
        }

        // Another tab refreshed, so this tab's cookies are new too: the
        // credential is the origin's rather than the tab's, which is why the
        // message carries no token and never could.
        this.credentialGeneration += 1;
        this.wake("refreshed");
    }

    private wake(outcome: RefreshOutcome): void {
        for (const deliver of this.listeners) {
            deliver(outcome);
        }
    }

    private untilAbort(
        running: Promise<RefreshOutcome>,
        signal: AbortSignal,
    ): Promise<RefreshOutcome> {
        if (signal.aborted) {
            return Promise.resolve("aborted");
        }
        return new Promise<RefreshOutcome>((resolve) => {
            const onAbort = (): void => {
                resolve("aborted");
            };
            signal.addEventListener("abort", onAbort, { once: true });
            void running.then(
                (outcome) => {
                    signal.removeEventListener("abort", onAbort);
                    resolve(outcome);
                },
                () => {
                    signal.removeEventListener("abort", onAbort);
                    // A refresh that threw is a refresh that did not happen.
                    // Reported as a rejection rather than propagated: every
                    // caller of this is handling a failed request already, and
                    // an exception here would be a rejected promise on the one
                    // path that is certain to be unattended.
                    resolve("rejected");
                },
            );
        });
    }
}

export function credentials(
    config: Omit<CredentialsConfig, "leaderWaitMs" | "sleep"> &
        Partial<Pick<CredentialsConfig, "leaderWaitMs" | "sleep">>,
): Credentials {
    return new Credentials({
        ...config,
        leaderWaitMs: config.leaderWaitMs ?? kDefaultLeaderWaitMs,
        sleep: config.sleep ?? sleep,
    });
}
