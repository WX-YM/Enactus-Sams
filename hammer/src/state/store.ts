// The one observable, and it is deliberately smaller than the ones a framework
// ships.
//
// Every store in this layer is this class with a different value in it: the
// session, a resource entry, the unread count, a form. What they need from an
// observable is three things — read the current value, hear about the next one,
// and stop hearing — and everything past that is a scheduler somebody else's
// render loop already has.
//
// --- notification is synchronous, and that is not a performance choice -------
//
// A subscriber told about a change asynchronously reads `get()` at a moment when
// the value may already have moved on, so what it renders is a state that never
// existed. React's `useSyncExternalStore` makes the same demand from the other
// side: `subscribe` must call back before the value can change again, or the
// snapshot it reads is torn. Batching belongs to whoever is rendering.
//
// --- nothing here is global --------------------------------------------------
//
// No registry, no module-level instance, no `close()` that reaches something it
// did not create (`ENGINEERING_RULES.md` §3.3). One owner per store: whoever constructed it
// disposes it, and a subscription hands back its own unsubscribe rather than a
// token to look up in a table.

export type Unsubscribe = () => void;

// The read side. A consumer given this cannot write, which is what lets a store
// be handed to a renderer without handing over the right to change it — and it
// is structural, so the class below satisfies it with no wrapper object and no
// second allocation.
export type Readable<T> = {
    readonly get: () => T;

    // The return value is the unsubscribe and it is not optional to keep
    // (`ENGINEERING_RULES.md` §3.3): a listener with no way off is a leak in a tab that
    // stays open for days, and a double-subscribed listener is a double render.
    readonly subscribe: (listener: (value: T) => void) => Unsubscribe;
};

export class Store<T> implements Readable<T> {
    private value: T;
    private readonly listeners: Set<(value: T) => void>;

    constructor(initial: T) {
        this.value = initial;
        this.listeners = new Set();
    }

    get(): T {
        return this.value;
    }

    subscribe(listener: (value: T) => void): Unsubscribe {
        this.listeners.add(listener);
        // Idempotent, because a caller that unsubscribes twice must not remove a
        // listener some later subscribe added: `Set.delete` is keyed by identity
        // and the same function subscribed twice is one entry, so the second
        // call has to be a no-op rather than a delete of a live subscription.
        let attached = true;
        return () => {
            if (!attached) {
                return;
            }
            attached = false;
            this.listeners.delete(listener);
        };
    }

    // No notification when nothing changed. A resource that re-decodes an
    // identical response, a form that re-validates an untouched field and a
    // session refetch that returns the same view are all common, and each would
    // otherwise be a render of the same pixels.
    //
    // `Object.is` and not `===`, for `NaN`: a numeric store set to `NaN` twice
    // would notify forever under `===`.
    set(next: T): void {
        if (Object.is(this.value, next)) {
            return;
        }
        this.value = next;
        this.notify();
    }

    // For a value whose next state is derived from its current one. It exists so
    // that a caller does not read, compute and write in three statements with an
    // await in the middle — which is the shape of a lost update in a single
    // thread.
    update(next: (current: T) => T): void {
        this.set(next(this.value));
    }

    close(): void {
        this.listeners.clear();
    }

    private notify(): void {
        // A copy, taken once. A listener is allowed to subscribe or unsubscribe
        // from inside its own callback — a form field that removes itself, a
        // resource that drops its last subscriber on the value it just received
        // — and iterating the live set makes what happens next depend on
        // insertion order. The cost is one array of the listeners this store has,
        // which is the number of mounted views of it.
        const current = Array.from(this.listeners);
        for (const listener of current) {
            // Still attached? The listener before this one may have removed it,
            // and calling it after it asked to stop is the double-render this
            // whole mechanism exists to avoid.
            if (!this.listeners.has(listener)) {
                continue;
            }
            try {
                listener(this.value);
            } catch (thrown) {
                // Every task body catches (`docs/00-architecture.md` §3), and a
                // subscriber that throws must not take out the other four. It is
                // rethrown from a microtask instead of swallowed: that reaches
                // the platform's unhandled-error path, where an application's
                // reporter can see it, without unwinding this loop.
                queueMicrotask(() => {
                    throw thrown;
                });
            }
        }
    }
}
