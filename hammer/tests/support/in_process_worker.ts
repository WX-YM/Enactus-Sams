// A worker in this process, running a REAL `serve*` function.
//
// `fake_worker.ts` answers FOR the worker, which is the right tool for
// asserting a pool's bounds and the wrong one for anything the worker's own
// body decides. This runs the body. Each message crosses through
// `structuredClone` with its transfer list, so a buffer the sender transfers is
// detached on the sending side at the moment of sending — which is the
// property a pool that moves a password to a worker depends on, and the one a
// stand-in passing object references would hide.
//
// Delivery is on a timer, as a platform worker's is, so nothing is answered
// on the stack that sent it.

import type { WorkerFactory, WorkerLike, WorkerScope } from "../../src/state/workers/pool.js";

type Handler = (event: Event) => void;

export type InProcessWorkers = {
    readonly create: WorkerFactory;
    readonly spawned: () => number;
    readonly terminated: () => number;
};

export function inProcessWorkers(serve: (scope: WorkerScope) => void): InProcessWorkers {
    let spawned = 0;
    let terminated = 0;

    const create = (): WorkerLike => {
        spawned += 1;
        const toMain = new Set<Handler>();
        const toWorker = new Set<Handler>();
        let alive = true;

        const deliver = (handlers: Set<Handler>, message: unknown, transfer: readonly Transferable[]): void => {
            const data: unknown = structuredClone(message, { transfer: [...transfer] });
            setTimeout(() => {
                if (!alive) {
                    return;
                }
                for (const handler of Array.from(handlers)) {
                    handler({ data } as unknown as Event);
                }
            }, 0);
        };

        serve({
            addEventListener: (_type, handler) => toWorker.add(handler),
            postMessage: (message, transfer) => deliver(toMain, message, transfer ?? []),
        });

        return {
            postMessage: (message, transfer) => deliver(toWorker, message, transfer ?? []),
            addEventListener: (type, handler) => {
                if (type === "message") {
                    toMain.add(handler);
                }
            },
            removeEventListener: (type, handler) => {
                if (type === "message") {
                    toMain.delete(handler);
                }
            },
            terminate: () => {
                alive = false;
                terminated += 1;
            },
        };
    };

    return { create, spawned: () => spawned, terminated: () => terminated };
}
