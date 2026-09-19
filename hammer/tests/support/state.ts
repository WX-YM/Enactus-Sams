// A client and a clock the state suite can drive.
//
// The same shape as the wire harness and for the same reason: everything is
// built through the REFERENCE CONSUMER's generated module, so a seam that cannot
// be satisfied from outside hammer fails here rather than in an application
// (`ENGINEERING_RULES.md` §1). The clock is a number the test moves, because freshness is
// the whole subject of this suite and a suite that slept through a `max-age` is
// a suite nobody runs.

import type { Client } from "../../src/wire/client.js";
import type { SessionSource } from "../../src/wire/resolve.js";
import type { SessionView } from "../../src/wire/session_view.js";
import { createClient } from "../../src/wire/index.js";

import type { Api } from "../testapp/app/client.js";
import { kApiTables, routeAuthRefresh } from "../testapp/api/hammer.generated.js";
import { ChannelBus } from "./fake_channel.js";
import { FakeServer } from "./fake_fetch.js";
import { LockRoom } from "./fake_locks.js";
import { sessionView } from "./session.js";

// The holder-scoped table a server would send, through the real decode.
export const kStateRoutes = {
    "content.get": "GET /content/{id}",
    "content.delete": "DELETE /content/{id}",
    "media.list": "GET /media/{ns}/{id}",
    "media.delete": "DELETE /media/{ns}/{id}",
    "identity.me": "GET /me",
};

export class Source implements SessionSource {
    refetches = 0;

    private view: SessionView | null;

    constructor(view: SessionView | null = sessionView({ routes: kStateRoutes })) {
        this.view = view;
    }

    current = (): SessionView | null => this.view;

    refetch = async (_signal: AbortSignal): Promise<SessionView | null> => {
        this.refetches += 1;
        return this.view;
    };

    replace(view: SessionView | null): void {
        this.view = view;
    }
}

export type ClientHarness = {
    readonly client: Client<Api>;
    readonly server: FakeServer;
    readonly source: Source;
    readonly bus: ChannelBus;
    readonly now: () => number;
    tick: (ms: number) => void;
};

export function clientHarness(
    over: { readonly server?: FakeServer; readonly bus?: ChannelBus; readonly source?: Source } = {},
): ClientHarness {
    const server = over.server ?? new FakeServer();
    const source = over.source ?? new Source();
    const bus = over.bus ?? new ChannelBus();
    let clock = 0;

    const client = createClient<Api>({
        origin: {
            pageOrigin: "https://app.example.com",
            apiOrigin: "https://app.example.com",
            site: null,
        },
        fetch: server.fetch,
        session: source,
        api: kApiTables,
        refreshRoute: routeAuthRefresh,
        locks: new LockRoom().tab(),
        fanOut: bus.tab(),
        now: () => clock,
        // Backoff is recorded by advancing nothing: the suite asserts what was
        // sent, not how long it waited.
        sleep: async () => {},
    });

    return {
        client,
        server,
        source,
        bus,
        now: () => clock,
        tick: (ms: number) => {
            clock += ms;
        },
    };
}

// Enough turns of the microtask queue for a call to have reached its `fetch` and
// come back: resolution, the build and queue admission are each asynchronous, so
// "the load has finished" is a state a test waits for rather than assumes.
export async function settle(turns = 24): Promise<void> {
    for (let i = 0; i < turns; i += 1) {
        await Promise.resolve();
    }
}
