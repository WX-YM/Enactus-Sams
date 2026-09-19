// One origin's BroadcastChannel, as a bus a test can hold both ends of.
//
// Two properties are modelled because the credential fan-out depends on both:
// a sender does not receive its own message, and delivery is ASYNCHRONOUS. The
// second is what makes the refresh race real — a follower that queued on the
// lock would acquire it before the leader's broadcast arrived — so a stand-in
// that delivered synchronously would assert a race this library does not have.

import type { FanOut } from "../../src/wire/leader.js";

type Listener = (message: unknown) => void;

export class ChannelBus {
    private readonly tabs: Set<Set<Listener>> = new Set();

    posts = 0;

    tab(): FanOut {
        const listeners = new Set<Listener>();
        this.tabs.add(listeners);

        return {
            post: (message) => {
                this.posts += 1;
                for (const other of this.tabs) {
                    if (other === listeners) {
                        continue;
                    }
                    for (const listener of other) {
                        queueMicrotask(() => listener(message));
                    }
                }
            },
            listen: (onMessage) => {
                listeners.add(onMessage);
                return () => {
                    listeners.delete(onMessage);
                };
            },
            close: () => {
                this.tabs.delete(listeners);
            },
        };
    }
}
