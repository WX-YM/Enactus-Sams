// Two tabs, one expiry, exactly one refresh.
//
// This is the assertion the whole module exists for, and the one the plan calls
// the highest-signal counter in the library: a second concurrent refresh is a
// rotation race against anvil's compare-and-swap, whose loser is signed out
// with a valid session, in the tab they were using.

import { describe, expect, it } from "vitest";

import type { CredentialsConfig } from "../../src/wire/credentials.js";
import {
    Credentials,
    credentials,
    kDefaultLeaderWaitMs,
    kRefreshLock,
} from "../../src/wire/credentials.js";
import { leadership } from "../../src/wire/leader.js";
import { ChannelBus } from "../support/fake_channel.js";
import { LockRoom } from "../support/fake_locks.js";

// A refresh whose completion the test controls, counting how many times it was
// actually called across every tab.
class Server {
    calls = 0;
    private readonly gate: Promise<void> | null;
    private survives: boolean;

    constructor(survives = true, gate: Promise<void> | null = null) {
        this.survives = survives;
        this.gate = gate;
    }

    refresh = async (_signal: AbortSignal): Promise<boolean> => {
        this.calls += 1;
        if (this.gate !== null) {
            await this.gate;
        }
        return this.survives;
    };

    reject(): void {
        this.survives = false;
    }
}

type Tab = {
    readonly credentials: Credentials;
    readonly logouts: () => number;
};

function tab(
    room: LockRoom | null,
    bus: ChannelBus,
    server: Server,
    over: Partial<CredentialsConfig> = {},
): Tab {
    let logouts = 0;
    const held = credentials({
        leadership: leadership(room === null ? null : room.tab()),
        fanOut: bus.tab(),
        refresh: server.refresh,
        onLogout: () => {
            logouts += 1;
        },
        ...over,
    });
    return { credentials: held, logouts: () => logouts };
}

function signal(): AbortSignal {
    return new AbortController().signal;
}

// Enough turns of the microtask queue for a broadcast to be delivered and acted
// on. The fake bus delivers asynchronously on purpose: a synchronous stand-in
// would assert a race this library does not have.
async function settle(): Promise<void> {
    for (let i = 0; i < 8; i += 1) {
        await Promise.resolve();
    }
}

describe("two tabs and one expiry", () => {
    it("refreshes exactly once and both tabs replay", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        let open = (): void => {};
        const gate = new Promise<void>((resolve) => {
            open = resolve;
        });
        const server = new Server(true, gate);

        const first = tab(room, bus, server);
        const second = tab(room, bus, server);

        const both = Promise.all([
            first.credentials.refreshOnce(signal()),
            second.credentials.refreshOnce(signal()),
        ]);
        await settle();

        open();
        await expect(both).resolves.toEqual(["refreshed", "refreshed"]);
        expect(server.calls).toBe(1);
    });

    // The follower must not queue on the lock: it would acquire it the moment
    // the leader released, before the broadcast had been delivered, and refresh
    // a second time. That is the rotation race.
    it("keeps the follower off the lock while the leader holds it", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        let open = (): void => {};
        const gate = new Promise<void>((resolve) => {
            open = resolve;
        });
        const server = new Server(true, gate);

        const first = tab(room, bus, server);
        const second = tab(room, bus, server);

        const both = Promise.all([
            first.credentials.refreshOnce(signal()),
            second.credentials.refreshOnce(signal()),
        ]);
        await settle();

        expect(room.grants).toBe(1);
        open();
        await both;
        expect(room.grants).toBe(1);
    });

    it("counts one refresh in the tab that led and none in the tab that heard", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new Server();

        const first = tab(room, bus, server);
        const second = tab(room, bus, server);

        await Promise.all([
            first.credentials.refreshOnce(signal()),
            second.credentials.refreshOnce(signal()),
        ]);

        const led = first.credentials.counts().refreshes + second.credentials.counts().refreshes;
        expect(led).toBe(1);
    });
});

describe("one tab with a dozen requests in flight", () => {
    it("starts one refresh for all of them", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        let open = (): void => {};
        const gate = new Promise<void>((resolve) => {
            open = resolve;
        });
        const server = new Server(true, gate);
        const only = tab(room, bus, server);

        const waiting = [];
        for (let i = 0; i < 12; i += 1) {
            waiting.push(only.credentials.refreshOnce(signal()));
        }
        await settle();
        open();

        await expect(Promise.all(waiting)).resolves.toEqual(Array(12).fill("refreshed"));
        expect(server.calls).toBe(1);
    });

    it("refreshes again for the next expiry", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new Server();
        const only = tab(room, bus, server);

        await only.credentials.refreshOnce(signal());
        await only.credentials.refreshOnce(signal());
        expect(server.calls).toBe(2);
    });
});

describe("a refresh that fails", () => {
    // Not a race: a rejection. Looping on it is how a client hammers a server
    // that has already said no.
    it("ends the session in every tab", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new Server(false);

        const first = tab(room, bus, server);
        const second = tab(room, bus, server);

        await expect(first.credentials.refreshOnce(signal())).resolves.toBe("rejected");
        await settle();

        expect(first.logouts()).toBe(1);
        expect(second.logouts()).toBe(1);
    });

    it("tells a waiting follower the session is over rather than stranding it", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        let open = (): void => {};
        const gate = new Promise<void>((resolve) => {
            open = resolve;
        });
        const server = new Server(false, gate);

        const first = tab(room, bus, server);
        const second = tab(room, bus, server);

        const both = Promise.all([
            first.credentials.refreshOnce(signal()),
            second.credentials.refreshOnce(signal()),
        ]);
        await settle();
        open();

        await expect(both).resolves.toEqual(["rejected", "rejected"]);
        expect(server.calls).toBe(1);
    });

    // A rejected promise here would be an unhandled rejection on the one path
    // that is certain to be unattended.
    it("reports a refresh that threw as a rejection", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const only = tab(room, bus, new Server(), {
            refresh: async () => {
                throw new Error("the network went away");
            },
        });

        await expect(only.credentials.refreshOnce(signal())).resolves.toBe("rejected");
    });
});

describe("a logout", () => {
    it("fans out to every tab", async () => {
        const bus = new ChannelBus();
        const room = new LockRoom();
        const server = new Server();

        const first = tab(room, bus, server);
        const second = tab(room, bus, server);
        const third = tab(room, bus, server);

        first.credentials.logout();
        await settle();

        expect([first.logouts(), second.logouts(), third.logouts()]).toEqual([1, 1, 1]);
    });

    // A tab that echoed the message it received would be a loop.
    it("is not re-broadcast by the tabs that hear it", async () => {
        const bus = new ChannelBus();
        const room = new LockRoom();
        const server = new Server();

        const first = tab(room, bus, server);
        tab(room, bus, server);

        first.credentials.logout();
        await settle();
        expect(bus.posts).toBe(1);
    });

    it("ignores a message that is not one of hammer's", async () => {
        const bus = new ChannelBus();
        const room = new LockRoom();
        const other = bus.tab();
        const only = tab(room, bus, new Server());

        other.post({ kind: "something-else" });
        other.post("logout");
        other.post(null);
        await settle();

        expect(only.logouts()).toBe(0);
    });
});

describe("a leader that died mid-refresh", () => {
    // The browser releases a dead tab's lock and sends nothing, so the only
    // evidence a follower has is silence. The window is what bounds how long
    // every other tab is stranded by it.
    it("is taken over once the wait elapses", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new Server();

        // A tab that takes the lock and never comes back, which is what a
        // discarded tab looks like from here minus the browser's release.
        const zombie = room.tab();
        let release = (): void => {};
        const holding = new Promise<void>((resolve) => {
            release = resolve;
        });
        void zombie.request(kRefreshLock, { ifAvailable: true }, async () => {
            await holding;
        });
        await Promise.resolve();

        const survivor = tab(room, bus, server, {
            leaderWaitMs: 1,
            sleep: async () => {
                release();
                await Promise.resolve();
            },
        });

        await expect(survivor.credentials.refreshOnce(signal())).resolves.toBe("refreshed");
        expect(survivor.credentials.counts().takeovers).toBe(1);
        expect(server.calls).toBe(1);
    });
});

describe("a platform with no election", () => {
    // One refresh per tab with the race left in, and counted so an application
    // can report it. This is hammer_refresh_races_total, and it should be zero.
    it("refreshes anyway and counts the refresh as unelected", async () => {
        const bus = new ChannelBus();
        const server = new Server();
        const first = tab(null, bus, server);
        const second = tab(null, bus, server);

        await Promise.all([
            first.credentials.refreshOnce(signal()),
            second.credentials.refreshOnce(signal()),
        ]);

        expect(server.calls).toBe(2);
        expect(first.credentials.counts().unelected).toBe(1);
        expect(second.credentials.counts().unelected).toBe(1);
    });
});

describe("the caller's signal", () => {
    // A component's teardown must not cancel the session's refresh: the other
    // eleven screens in the tab did not ask to be signed out.
    it("stops the caller waiting without stopping the refresh", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        let open = (): void => {};
        const gate = new Promise<void>((resolve) => {
            open = resolve;
        });
        const server = new Server(true, gate);
        const only = tab(room, bus, server);

        const controller = new AbortController();
        const waiting = only.credentials.refreshOnce(controller.signal);
        await settle();
        controller.abort();

        await expect(waiting).resolves.toBe("aborted");
        open();
        await settle();
        expect(server.calls).toBe(1);
    });

    it("does not start a refresh for a caller that has already gone", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new Server();
        const only = tab(room, bus, server);

        const controller = new AbortController();
        controller.abort();
        await expect(only.credentials.refreshOnce(controller.signal)).resolves.toBe("aborted");
    });
});

describe("which credential a request was sent under", () => {
    it("advances in the tab that refreshed", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const only = tab(room, bus, new Server());

        expect(only.credentials.generation()).toBe(0);
        await only.credentials.refreshOnce(signal());
        expect(only.credentials.generation()).toBe(1);
    });

    // The cookies are the origin's rather than the tab's, so a refresh
    // anywhere is a new credential everywhere. A tab that did not notice would
    // ask for a second refresh on a 401 that is already answered.
    it("advances in the tab that only heard about it", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const server = new Server();
        const first = tab(room, bus, server);
        const second = tab(room, bus, server);

        await first.credentials.refreshOnce(signal());
        await settle();

        expect(second.credentials.generation()).toBe(1);
        expect(server.calls).toBe(1);
    });

    it("does not advance when the session ended", async () => {
        const room = new LockRoom();
        const bus = new ChannelBus();
        const only = tab(room, bus, new Server(false));

        await only.credentials.refreshOnce(signal());
        expect(only.credentials.generation()).toBe(0);
    });
});

describe("teardown", () => {
    it("stops hearing the channel once it is closed", async () => {
        const bus = new ChannelBus();
        const room = new LockRoom();
        const other = tab(room, bus, new Server());
        const closing = tab(room, bus, new Server());

        closing.credentials.close();
        other.credentials.logout();
        await settle();

        expect(closing.logouts()).toBe(0);
    });
});

describe("the two names and numbers this module publishes", () => {
    // The lock is named by hammer and never by an application, and it is
    // published so a deployment can see what its tabs contend on. It has to be
    // the SAME string in every tab of every bundle on one origin: two spellings
    // is two elections, which is the rotation race the lock exists to prevent —
    // and anvil resolves a refresh rotation as a compare-and-swap whose loser is
    // signed out. The cost of a typo here is somebody's session.
    //
    // The tie to behaviour is in the takeover case above, which takes the lock
    // by this constant and would stop being a takeover if the module elected on
    // a different name.
    it("elects on a name hammer owns", () => {
        expect(kRefreshLock).toBe("hammer.refresh");
    });

    it("waits ten seconds on the leader before concluding it is gone", () => {
        // Not a tuning knob. A follower that gives up early takes the lock
        // behind a leader that is merely slow, which manufactures the race the
        // election exists to remove.
        expect(kDefaultLeaderWaitMs).toBe(10_000);
    });
});
