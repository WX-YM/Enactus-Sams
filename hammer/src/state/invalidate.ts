// What a write makes stale, in this tab and in the others.
//
// The mapping is the application's and the mechanism is hammer's
// (`docs/01-seams.md` §13). The server knows what a write touched; it does not
// know what a screen is showing, and the table that bridges the two is written
// once in TypeScript against the generated route ids — so a mutation naming a
// resource the server retired stops compiling rather than silently ceasing to
// invalidate anything.
//
// --- why it crosses tabs ----------------------------------------------------
//
// Tab B rendering the row tab A just changed is the defect multi-tab coherence
// exists for, and it is the one a user reports as "it did not save". The write
// happened; the other tab simply never heard.
//
// --- the message is data, not an instruction ---------------------------------
//
// `BroadcastChannel` is same-origin by construction, so there is no origin to
// check (`wire/leader.ts`). What still applies is that the SENDER may be a tab
// running a different build of this application — an older one, mid-deploy, with
// route ids this build does not have. So the message is decoded the way a
// response body is, and an entry that is not a string is dropped rather than
// handed to a cache as a key.
//
// An unknown route id is kept rather than filtered, and that is deliberate:
// invalidating a route this build does not know costs a sweep that matches
// nothing, and filtering it would mean this tab deciding the other tab's
// vocabulary is wrong. Over-applying an invalidation costs a request.
// Under-applying it shows somebody data that has changed.

import type { Invalidations } from "../core/tables.js";
import type { FanOut } from "../wire/leader.js";

import type { CountSink } from "./counts.js";
import { kNoCounts } from "./counts.js";
import type { Unsubscribe } from "./store.js";

// The channel topic. Namespaced because the fan-out is shared with the
// credential lifecycle and with every stream (`wire/leader.ts`): one channel per
// tab, several conversations on it.
const kInvalidateKind = "hammer.invalidate";

// A cap on what one message may name. The table is compiled in, so an honest
// message is a handful of route ids; a message with ten thousand is either a
// build nobody shipped or a tab spending this one's main thread for it.
const kMaxRoutesPerMessage = 256;

type InvalidateMessage = {
    readonly kind: typeof kInvalidateKind;
    readonly routes: readonly string[];
};

function decodeMessage(message: unknown): readonly string[] | null {
    if (typeof message !== "object" || message === null) {
        return null;
    }
    const { kind, routes } = message as { readonly kind?: unknown; readonly routes?: unknown };
    if (kind !== kInvalidateKind || !Array.isArray(routes)) {
        return null;
    }
    if (routes.length === 0 || routes.length > kMaxRoutesPerMessage) {
        return null;
    }
    for (const entry of routes) {
        if (typeof entry !== "string") {
            // Malformed in one place is not a message to keep the rest of, the
            // way `wire/envelope.ts` drops a whole field map rather than an
            // entry of one.
            return null;
        }
    }
    return routes as readonly string[];
}

// What an invalidation is applied to. A type rather than the class, so that
// `state/invalidate.ts` does not have to know what a resource store is beyond
// this — and so a test can count applications without building one.
export type Invalidatable = {
    readonly invalidate: (routeIds: Iterable<string>) => void;
};

export type InvalidatorConfig<RouteId extends string> = {
    readonly table: Invalidations<RouteId>;
    readonly resources: Invalidatable;
    readonly fanOut: FanOut;
    readonly count?: CountSink;
};

export class Invalidator<RouteId extends string> {
    private readonly config: InvalidatorConfig<RouteId>;
    private readonly detach: Unsubscribe;
    private readonly count: CountSink;

    constructor(config: InvalidatorConfig<RouteId>) {
        this.config = config;
        this.count = config.count ?? kNoCounts;
        this.detach = config.fanOut.listen((message) => {
            this.receive(message);
        });
    }

    // A mutation on this route succeeded. Applied here and announced to the
    // other tabs, in that order: this tab's own screens are the ones the person
    // is looking at, and a broadcast that went first would put the refetch of
    // every other tab ahead of the refetch of this one.
    after(routeId: RouteId): void {
        const routes = this.routesFor(routeId);
        if (routes === null) {
            return;
        }
        this.config.resources.invalidate(routes);
        const message: InvalidateMessage = { kind: kInvalidateKind, routes };
        this.config.fanOut.post(message);
    }

    // Local only. For a caller that has already decided what is stale — a
    // reconciliation after a `VersionMismatch`, where the write did not happen
    // and there is nothing to tell another tab about.
    here(routeIds: readonly RouteId[]): void {
        if (routeIds.length === 0) {
            return;
        }
        this.config.resources.invalidate(routeIds);
    }

    close(): void {
        this.detach();
    }

    private routesFor(routeId: RouteId): readonly string[] | null {
        // The table is `Partial`, because a read invalidates nothing and an
        // entry for every route would be a table of empty arrays whose only
        // effect is to hide the routes that matter (`core/tables.ts`).
        const held: readonly RouteId[] | undefined = this.config.table[routeId];
        if (held === undefined || held.length === 0) {
            return null;
        }
        return held;
    }

    private receive(message: unknown): void {
        const routes = decodeMessage(message);
        if (routes === null) {
            return;
        }
        this.config.resources.invalidate(routes);
        this.count("invalidated");
    }
}
