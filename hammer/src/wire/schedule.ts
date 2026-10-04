// Waiting, in the one place that does it.
//
// Three mechanisms here need to not-act for a while — the backoff between
// attempts, the fallback that covers a leader which died mid-refresh, and the
// pause before a stream reconnects — and each of them would otherwise grow its
// own timer, its own abort handling and its own leak.
//
// --- nothing scheduled in a tab is durable ----------------------------------
//
// `setTimeout` dies when the tab is frozen, discarded or reloaded, and so does
// every promise waiting on one (`CLAUDE.md` §6). So a wait here is a hint about
// the interface and never a guarantee about work: anything that must happen
// happens server-side. Where this library waits, the worst case of the wait
// never resolving is a request that is not made — never a write that is lost.
//
// --- resolving rather than rejecting on abort -------------------------------
//
// The obvious shape is a promise that rejects when the signal fires, and it is
// the wrong one here: an abort is an expected condition on every screen that
// unmounts, and an exception thrown for an expected condition is a `catch`
// somebody forgets to write (`CLAUDE.md` §3.1). This resolves, and the caller
// checks the signal — which it has to do anyway, because the signal can fire
// while it is doing something else.

export type Sleep = (ms: number, signal: AbortSignal) => Promise<void>;

export const sleep: Sleep = (ms, signal) =>
    new Promise<void>((resolve) => {
        if (signal.aborted || ms <= 0) {
            resolve();
            return;
        }

        // The listener and the timer are each other's cleanup. A timer left
        // running holds its closure — and whatever that closure captured — for
        // the whole delay, and a listener left attached holds it for the life
        // of the signal, which on a long-lived screen is the life of the tab.
        const timer = setTimeout(() => {
            signal.removeEventListener("abort", onAbort);
            resolve();
        }, ms);

        function onAbort(): void {
            clearTimeout(timer);
            resolve();
        }

        signal.addEventListener("abort", onAbort, { once: true });
    });
