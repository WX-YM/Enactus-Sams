// The adapter, and it is deliberately the smallest thing in this repository.
//
// Every behaviour a screen needs already exists below this layer and is tested
// with no framework at all: the resource is refcounted and aborts when the last
// watcher lets go (`state/resource.ts`), the session is one store with one
// reader (`state/session.ts`), the form validates on its own schedule
// (`state/forms.ts`), the inbox dedupes an at-least-once stream
// (`state/inbox.ts`). What is missing is the part only React can supply —
// WHEN a subscription starts and when it stops — and that is all that is here.
//
// So the rule for this file is one line: it binds stores, and it holds no
// behaviour of its own. `tests/react/boundary.test.ts` is what makes that
// checkable rather than aspirational — a gzipped ceiling, and an assertion that
// the built adapter imports nothing but `react` and the one state constant it
// cannot invent. A hook that started deciding something would show up as bytes.
//
// --- why `useSyncExternalStore` and nothing else ----------------------------
//
// A store read in `useEffect` and copied into `useState` is the tearing bug
// React added this hook to remove: the copy is read during a render that a
// concurrent update may interrupt, so two components can render two different
// values of one store in one frame. `Store.notify` is synchronous for the same
// reason from the other side (`state/store.ts`), so the two halves of the
// contract meet.
//
// --- StrictMode is not a special case ---------------------------------------
//
// React invokes effects twice in development to surface the mount/unmount
// asymmetry it will eventually really do (an offscreen tree, a restored
// back/forward cache). Everything below therefore has to be correct when
// subscribe/unsubscribe/subscribe happens in one commit, and nothing here is
// written to survive it specially: `Store.subscribe`'s unsubscribe is
// idempotent, `Resource.release` is refcounted and idempotent, and the double
// invoke is a +1/-1/+1 on a counter that was always going to have to be right.

import type { Result } from "../core/result.js";
import type { Form, FormState } from "../state/forms.js";
import type { Inbox, InboxState } from "../state/inbox.js";
import type { Resource, ResourceState } from "../state/resource.js";
import { kResourceLoading } from "../state/resource.js";
import type { SessionState, SessionStore } from "../state/session.js";
import type { Readable } from "../state/store.js";
import type { ApiTypes } from "../wire/client.js";

import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from "react";

// One store, bound to one component's lifetime.
//
// Every other hook here is this one with a type on it, and it is exported
// because the stores this library hands an application are not an enumerable
// set: a `Pager`'s state, a `Sections` stage, an `AnalyticsSink`'s consent and
// an application's own `Store` are all `Readable`, and a consumer without this
// writes it again, usually with the two traps below in it.
export function useStore<T>(store: Readable<T>): T {
    // Wrapped rather than passed as `store.subscribe`.
    //
    // `Readable` is satisfied structurally by a class whose `subscribe` is a
    // prototype method, so the extracted reference arrives with no receiver and
    // throws on the first call. It is the kind of defect that appears only for
    // the consumers who happen to hold a class rather than an object literal,
    // which is all of them here.
    const subscribe = useCallback(
        (onChange: () => void) => store.subscribe(onChange),
        [store],
    );
    const snapshot = useCallback(() => store.get(), [store]);

    // The third argument is the server snapshot, and it is the same function on
    // purpose. These stores are constructed by the application, so one rendered
    // on a server was constructed there too and its initial value is the same
    // deterministic one hydration will read. Omitting it would throw during any
    // server render instead.
    return useSyncExternalStore(subscribe, snapshot, snapshot);
}

// --- a read --------------------------------------------------------------

export type ResourceView<T, E> = {
    readonly state: ResourceState<T, E>;

    // The handle, for the two things that take one: `optimistic()` and
    // `writeVersioned()` (`state/optimistic.ts`, `state/versioned.ts`). Null
    // until the subscription exists, which is one render on mount and never
    // again — an event handler fires after a commit, so a click cannot observe
    // the null.
    //
    // `release` is on it and is the hook's to call. Calling it from a component
    // drops a watcher React still believes it has; the release that matters
    // happens on unmount, below.
    readonly resource: Resource<T, E> | null;
};

// A resource, opened for as long as the component is mounted.
//
// `open` is the identity that decides the lifetime, exactly the way
// `useSyncExternalStore`'s own `subscribe` argument does, and it carries the
// same obligation: it must be stable — a `useCallback` over the address's
// parameters — because a new identity means release this entry and open the
// next. An inline arrow is a release and a re-open on every render, which is an
// aborted request per keystroke of whatever else is on the screen.
//
// The alternative considered was a dependency array of hammer's own, and it was
// rejected for being the same footgun with a second spelling: a caller who
// forgets a dependency gets an entry that never re-opens, which is worse than
// one that re-opens too often because it is silent.
export function useResource<T, E>(open: () => Resource<T, E>): ResourceView<T, E> {
    // The handle lives in two places, and the duplication is load-bearing.
    //
    // The REF is what the snapshot reads, because the snapshot is read during
    // the render React forces immediately after subscribing and must already be
    // this entry's value — a snapshot still reading the previous entry renders
    // one commit of the old address's body under the new address's parameters.
    //
    // The STATE is what the render returns, because a ref read during render is
    // not something React will re-render for: a component that showed
    // `resource === null` forever would have no handle to hand `optimistic()`.
    const held = useRef<Resource<T, E> | null>(null);
    const [resource, setResource] = useState<Resource<T, E> | null>(null);

    const subscribe = useCallback(
        (onChange: () => void) => {
            const opened = open();
            held.current = opened;
            setResource(opened);
            const off = opened.state.subscribe(onChange);
            return () => {
                off();
                held.current = null;
                // Not `setResource(null)`. This cleanup runs on unmount as well
                // as on a re-open, and a state update on an unmounting fiber is
                // work nothing will ever render. The re-open path overwrites it
                // in the same commit, before anything can observe the released
                // handle.
                opened.release();
            };
        },
        [open],
    );

    const snapshot = useCallback(
        (): ResourceState<T, E> => held.current?.state.get() ?? kResourceLoading,
        [],
    );

    const state = useSyncExternalStore(subscribe, snapshot, snapshot);
    return { state, resource };
}

// --- a write -------------------------------------------------------------

// One shape, every property declared (`ENGINEERING_RULES.md` §2.3), discriminated so that a
// caller narrows rather than null-checks.
export type MutationState<T, E> =
    | { readonly status: "idle"; readonly value: null; readonly error: null }
    | { readonly status: "running"; readonly value: null; readonly error: null }
    | { readonly status: "done"; readonly value: T; readonly error: null }
    | { readonly status: "failed"; readonly value: null; readonly error: E };

export type Mutation<T, E> = {
    readonly state: MutationState<T, E>;

    // The call is handed in at the moment it is made rather than held from the
    // render that declared it. That is not an ergonomic preference: a callback
    // captured at render time is a closure over that render's props, and the
    // stale-closure bug it produces is a write that sends the value the field
    // held two keystrokes ago. Passing it here means there is nothing to go
    // stale.
    readonly run: (
        perform: (signal: AbortSignal) => Promise<Result<T, E>>,
    ) => Promise<Result<T, E>>;

    // Abandon what is in flight. The answer is not rendered, because a call
    // nobody is waiting for did not fail — it was dropped.
    readonly abort: () => void;

    // Back to `idle`, for dismissing a failure that has been read.
    readonly reset: () => void;
};

// What one mutation owns across renders. One object with one shape, allocated
// once: `useRef`'s initial value is evaluated on every render and kept from the
// first, which is a per-render allocation of three fields and no per-render
// mutation.
type MutationCell = {
    // Bumped by every `run` and by `abort`. Only the newest generation may
    // publish: an older answer landing after a newer one is the interface
    // showing a result that has been superseded, and the user cannot tell.
    generation: number;
    mounted: boolean;
    controller: AbortController | null;
};

const kIdle = { status: "idle", value: null, error: null } as const;
const kRunning = { status: "running", value: null, error: null } as const;

export function useMutation<T, E>(): Mutation<T, E> {
    const [state, setState] = useState<MutationState<T, E>>(kIdle);
    const cell = useRef<MutationCell>({ generation: 0, mounted: true, controller: null });

    useEffect(() => {
        const live = cell.current;
        live.mounted = true;
        return () => {
            live.mounted = false;
            // Nothing outlives what created it (`ENGINEERING_RULES.md` §3.3). A request
            // whose screen has gone is a request nobody will read, holding a
            // slot in the bounded queue that something visible is waiting for.
            live.generation += 1;
            live.controller?.abort();
            live.controller = null;
        };
    }, []);

    const run = useCallback(
        async (perform: (signal: AbortSignal) => Promise<Result<T, E>>): Promise<Result<T, E>> => {
            const live = cell.current;

            // A second run supersedes the first, and the first is aborted rather
            // than left to land. What this does NOT decide is whether a second
            // write may be sent at all: an unconfirmed value refuses the write
            // built on it where that matters (`state/optimistic.ts`), and a
            // double submit is the application's control to disable, from
            // `state.status`.
            live.controller?.abort();
            const controller = new AbortController();
            live.controller = controller;
            live.generation += 1;
            const generation = live.generation;

            if (live.mounted) {
                setState(kRunning);
            }

            try {
                const answered = await perform(controller.signal);
                if (live.mounted && live.generation === generation) {
                    setState(
                        answered.ok
                            ? { status: "done", value: answered.value, error: null }
                            : { status: "failed", value: null, error: answered.error },
                    );
                }
                return answered;
            } catch (thrown) {
                // A `perform` that throws is programmer error — failure belongs
                // in the return type (`ENGINEERING_RULES.md` §3.1) — so there is no error
                // value of the caller's own type to publish and none is
                // invented. The state goes back to where it can be retried from
                // and the throw continues to the caller who wrote it.
                if (live.mounted && live.generation === generation) {
                    setState(kIdle);
                }
                throw thrown;
            } finally {
                if (live.controller === controller) {
                    live.controller = null;
                }
            }
        },
        [],
    );

    const abort = useCallback(() => {
        const live = cell.current;
        live.generation += 1;
        live.controller?.abort();
        live.controller = null;
        if (live.mounted) {
            setState(kIdle);
        }
    }, []);

    const reset = useCallback(() => {
        setState(kIdle);
    }, []);

    return { state, run, abort, reset };
}

// --- the three stores an application already holds -------------------------
//
// Each is `useStore` with the store's own type on it. They exist rather than
// being left to the caller because the store is reached through a getter in all
// three cases, and `useStore(session.store)` is one place for a consumer to
// hand over the wrong one.

export function useSession(session: SessionStore): SessionState {
    return useStore(session.store);
}

// The stream is NOT opened or closed here, and that is the whole of the
// interesting part. It is leader-owned across every tab on one session
// (`wire/sse.ts`), `open` is idempotent, and `close` is not scoped to one
// component — a bell unmounting while an inbox screen is open would take the
// connection out from under it. The lifetime belongs to whoever constructed the
// inbox, which is the application (`ENGINEERING_RULES.md` §3.3).
export function useInbox<A extends ApiTypes, T>(inbox: Inbox<A, T>): InboxState<T> {
    return useStore(inbox.store);
}

export function useForm<Reason extends string>(form: Form<Reason>): FormState<Reason> {
    return useStore(form.store);
}
