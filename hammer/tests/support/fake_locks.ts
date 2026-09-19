// One browser's lock manager, shared by as many tabs as a test makes.
//
// A hand-written stand-in rather than a mock, for the reason the test plan
// gives: a mock configured to return what the test expects asserts that the
// test knows what it expects. This one models the two properties the election
// rests on — a lock is held by one context at a time, and `ifAvailable` answers
// null rather than waiting — and nothing else.

import type { ExclusiveLocks, LockGrant } from "../../src/wire/leader.js";

export class LockRoom {
    private readonly held = new Set<string>();
    private readonly waiting = new Map<string, (() => void)[]>();

    // How many times a lock was actually granted, so a test can assert that the
    // work happened once rather than that it looked like it did.
    grants = 0;

    tab(): ExclusiveLocks {
        return {
            request: async <T>(
                name: string,
                options: { readonly ifAvailable: boolean },
                callback: (lock: LockGrant) => Promise<T>,
            ): Promise<T> => {
                if (this.held.has(name)) {
                    if (options.ifAvailable) {
                        return await callback(null);
                    }
                    await new Promise<void>((resolve) => {
                        const queue = this.waiting.get(name) ?? [];
                        queue.push(resolve);
                        this.waiting.set(name, queue);
                    });
                }

                this.held.add(name);
                this.grants += 1;
                try {
                    return await callback({ name });
                } finally {
                    this.held.delete(name);
                    const next = this.waiting.get(name)?.shift();
                    if (next !== undefined) {
                        next();
                    }
                }
            },
        };
    }
}
