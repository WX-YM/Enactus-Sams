// One tab does the thing; the others hear about it.
//
// Two mechanisms in this library must happen once per SESSION rather than once
// per tab, and both are expensive to get wrong:
//
//   THE REFRESH. anvil rotates the refresh token as a compare-and-swap, so two
//   tabs refreshing concurrently is a rotation race whose loser is logged out —
//   with a valid session, in the tab somebody was using (`ENGINEERING_RULES.md` §4).
//
//   THE STREAM. A person with twelve tabs is twelve SSE connections, against a
//   ceiling anvil derives from `RLIMIT_NOFILE`. The twelfth is not free; it is
//   somebody else's connection.
//
// The platform already has the primitive: `navigator.locks` is an election
// whose loser is the browser's problem rather than this library's, and it
// releases the lock when a tab is discarded, which is the case a hand-rolled
// election gets wrong.
//
// --- injected, not read ------------------------------------------------------
//
// `navigator.locks` and `BroadcastChannel` are global singletons the platform
// imposes, so they arrive as parameters (`ENGINEERING_RULES.md` §3.3). Two tabs in one test
// process are then two of these with one fake between them, which is the only
// way the refresh race is deterministic (docs/16-test-plan.md).
//
// --- the degraded path is reported, not hidden ------------------------------
//
// `navigator.locks` needs a secure context and is absent in a few places that
// otherwise work. Without it there is no election, and the honest behaviour is
// one refresh per tab with the race left in — the alternative, a hand-rolled
// election over a broadcast channel, is a distributed consensus protocol with
// no fencing token, written to avoid admitting that the platform said no.
//
// So the fallback runs, and `degraded` says so. An application that can see the
// flag can report the rotation race it is now exposed to; one that cannot see it
// would report "signed out unexpectedly", which is the bug report this library
// exists to make rare.

export type LockGrant = { readonly name: string } | null;

// The slice of `navigator.locks` this library uses, as a type it can be handed.
// A test supplies its own; the adapter below is what the platform's goes
// through, so the shape is asserted in exactly one place.
export type ExclusiveLocks = {
    readonly request: <T>(
        name: string,
        options: { readonly ifAvailable: boolean },
        callback: (lock: LockGrant) => Promise<T>,
    ) => Promise<T>;
};

export function locksFrom(manager: LockManager): ExclusiveLocks {
    return {
        // The platform's callback type says it returns `T` where the platform
        // itself awaits a promise, so the type argument is the promise and the
        // await is what flattens it. Spelled here, once, rather than at every
        // call site that would otherwise have to know.
        request: async <T>(
            name: string,
            options: { readonly ifAvailable: boolean },
            callback: (lock: LockGrant) => Promise<T>,
        ): Promise<T> =>
            await manager.request<Promise<T>>(name, { ifAvailable: options.ifAvailable }, (lock) =>
                callback(lock === null ? null : { name: lock.name }),
            ),
    };
}

export type Leadership = {
    // Runs the work if this context can take the lock WITHOUT WAITING, and
    // answers null when another context already holds it.
    //
    // Not-waiting is the whole design. A follower that queued on the lock would
    // acquire it the moment the leader released — before the leader's broadcast
    // had been delivered, because delivery is asynchronous — and would then do
    // the work a second time. Not queueing means a follower has nothing to do
    // but wait for the message, which is what makes "exactly one" true rather
    // than likely.
    readonly tryExclusive: <T>(name: string, work: () => Promise<T>) => Promise<T | null>;

    // Runs the work while holding the lock, waiting for it if necessary. For
    // the recovery path: a leader that died mid-work releases the lock and
    // somebody has to pick the work up.
    readonly exclusive: <T>(name: string, work: () => Promise<T>) => Promise<T>;

    // True when the platform gave no lock manager, so the election is not
    // happening and every tab acts for itself.
    readonly degraded: boolean;
};

export function leadership(locks: ExclusiveLocks | null): Leadership {
    if (locks === null) {
        return {
            tryExclusive: async <T>(_name: string, work: () => Promise<T>): Promise<T | null> =>
                work(),
            exclusive: async <T>(_name: string, work: () => Promise<T>): Promise<T> => work(),
            degraded: true,
        };
    }

    return {
        tryExclusive: <T>(name: string, work: () => Promise<T>): Promise<T | null> =>
            locks.request<T | null>(name, { ifAvailable: true }, async (lock) =>
                lock === null ? null : work(),
            ),
        exclusive: <T>(name: string, work: () => Promise<T>): Promise<T> =>
            locks.request<T>(name, { ifAvailable: false }, async () => work()),
        degraded: false,
    };
}

// --- the fan-out -------------------------------------------------------------

// A message crossing tabs, and the unsubscribe that comes with listening
// (`ENGINEERING_RULES.md` §3.3): a listener with no way off is a leak and a double-handled
// message.
export type FanOut = {
    readonly post: (message: unknown) => void;

    // The returned function detaches this listener and nothing else.
    readonly listen: (onMessage: (message: unknown) => void) => () => void;

    readonly close: () => void;
};

// `BroadcastChannel` is same-origin by construction, which is why there is no
// origin check here and why `postMessage`'s rule about naming a target origin
// does not apply: there is no other origin it could reach. What still applies is
// that the DATA is not trusted — every message is decoded by whoever listens,
// the way a response body is, because a message may come from a tab running a
// different build of this application.
export function channelFanOut(channel: BroadcastChannel): FanOut {
    return {
        post: (message) => {
            channel.postMessage(message);
        },
        listen: (onMessage) => {
            const handler = (event: MessageEvent): void => {
                onMessage(event.data);
            };
            channel.addEventListener("message", handler);
            return () => {
                channel.removeEventListener("message", handler);
            };
        },
        close: () => {
            channel.close();
        },
    };
}

// The fan-out for a tab that has no channel: a platform without
// `BroadcastChannel`, or a test that does not care about a second tab. Posting
// goes nowhere and listening hears nothing, which is exactly one tab's worth of
// behaviour rather than an error path.
export const noFanOut: FanOut = {
    post: () => {},
    listen: () => () => {},
    close: () => {},
};
