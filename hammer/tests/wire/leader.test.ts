// The election, and the fan-out that makes it worth having.
//
// Two tabs in one process, with one fake lock manager and one fake bus between
// them, which is the only arrangement in which "exactly one of them did it" is a
// deterministic assertion rather than a timing one.

import { describe, expect, it } from "../support/test.js";

import { channelFanOut, leadership, noFanOut } from "../../src/wire/leader.js";
import { ChannelBus } from "../support/fake_channel.js";
import { LockRoom } from "../support/fake_locks.js";

describe("taking the lock without waiting", () => {
    it("runs the work for the tab that gets it", async () => {
        const room = new LockRoom();
        const leader = leadership(room.tab());
        await expect(leader.tryExclusive("hammer.refresh", async () => "done")).resolves.toBe(
            "done",
        );
    });

    // The property everything else rests on. A follower that queued on the lock
    // would acquire it the moment the leader released — before the leader's
    // broadcast had been delivered — and would do the work a second time.
    it("answers null for the tab that does not, rather than queueing", async () => {
        const room = new LockRoom();
        const first = leadership(room.tab());
        const second = leadership(room.tab());

        let release = (): void => {};
        const holding = new Promise<void>((resolve) => {
            release = resolve;
        });

        let ran = 0;
        const leading = first.tryExclusive("hammer.refresh", async () => {
            ran += 1;
            await holding;
            return "leader";
        });
        await Promise.resolve();

        await expect(
            second.tryExclusive("hammer.refresh", async () => {
                ran += 1;
                return "follower";
            }),
        ).resolves.toBe(null);

        release();
        await leading;
        expect(ran).toBe(1);
    });

    it("frees the lock for the next taker once the work is done", async () => {
        const room = new LockRoom();
        const first = leadership(room.tab());
        const second = leadership(room.tab());

        await first.tryExclusive("hammer.refresh", async () => "first");
        await expect(second.tryExclusive("hammer.refresh", async () => "second")).resolves.toBe(
            "second",
        );
        expect(room.grants).toBe(2);
    });

    // A leader that throws must not hold the lock for the life of the tab.
    it("frees the lock when the work throws", async () => {
        const room = new LockRoom();
        const first = leadership(room.tab());
        const second = leadership(room.tab());

        await expect(
            first.tryExclusive("hammer.refresh", async () => {
                throw new Error("the refresh threw");
            }),
        ).rejects.toThrow();

        await expect(second.tryExclusive("hammer.refresh", async () => "second")).resolves.toBe(
            "second",
        );
    });
});

describe("waiting for the lock", () => {
    it("picks the work up after the holder releases", async () => {
        const room = new LockRoom();
        const first = leadership(room.tab());
        const second = leadership(room.tab());

        const order: string[] = [];
        let release = (): void => {};
        const holding = new Promise<void>((resolve) => {
            release = resolve;
        });

        const leading = first.tryExclusive("hammer.refresh", async () => {
            await holding;
            order.push("leader");
        });
        await Promise.resolve();

        const waiting = second.exclusive("hammer.refresh", async () => {
            order.push("waiter");
        });

        release();
        await leading;
        await waiting;
        expect(order).toEqual(["leader", "waiter"]);
    });
});

describe("a platform with no lock manager", () => {
    // The honest fallback is one refresh per tab with the race left in. The
    // alternative — a hand-rolled election over a broadcast channel — is a
    // consensus protocol with no fencing token, written to avoid admitting that
    // the platform said no.
    it("runs the work anyway and says the election is not happening", async () => {
        const alone = leadership(null);
        expect(alone.degraded).toBe(true);
        await expect(alone.tryExclusive("hammer.refresh", async () => "ran")).resolves.toBe("ran");
        await expect(alone.exclusive("hammer.refresh", async () => "ran")).resolves.toBe("ran");
    });

    it("says so when the election is happening", () => {
        expect(leadership(new LockRoom().tab()).degraded).toBe(false);
    });
});

describe("the fan-out", () => {
    it("delivers to the other tabs and not to the sender", async () => {
        const bus = new ChannelBus();
        const sender = bus.tab();
        const listener = bus.tab();

        const heard: unknown[] = [];
        listener.listen((message) => heard.push(message));
        const echoed: unknown[] = [];
        sender.listen((message) => echoed.push(message));

        sender.post({ kind: "identity" });
        await Promise.resolve();
        await Promise.resolve();

        expect(heard).toEqual([{ kind: "identity" }]);
        expect(echoed).toEqual([]);
    });

    // A listener with no way off is a leak and a double-handled message.
    it("detaches the listener the unsubscribe belongs to, and no other", async () => {
        const bus = new ChannelBus();
        const sender = bus.tab();
        const listener = bus.tab();

        const first: unknown[] = [];
        const second: unknown[] = [];
        const off = listener.listen((message) => first.push(message));
        listener.listen((message) => second.push(message));

        off();
        sender.post("after");
        await Promise.resolve();
        await Promise.resolve();

        expect(first).toEqual([]);
        expect(second).toEqual(["after"]);
    });

    it("is a real channel's shape, driven through a real channel", async () => {
        const channel = new BroadcastChannel("hammer.test");
        const fanOut = channelFanOut(channel);
        const off = fanOut.listen(() => {});
        off();
        fanOut.close();
        expect(typeof fanOut.post).toBe("function");
    });

    // One tab's worth of behaviour, rather than an error path, for a platform
    // with no channel at all.
    it("posts nowhere and hears nothing when there is no channel", () => {
        const off = noFanOut.listen(() => {
            throw new Error("nothing can arrive here");
        });
        noFanOut.post("ignored");
        off();
        noFanOut.close();
    });
});
