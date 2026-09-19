// Mounting a component the way React itself says to, and nothing more.
//
// No testing library. The adapter's whole surface is three of React's own hooks,
// so what a test needs is a root, an `act` that flushes it, and an unmount —
// and a dependency that wraps those would put a second opinion about batching
// between the assertion and the thing asserted (`docs/16-test-plan.md`: no
// mocking framework, for the same reason).
//
// `act` is React's own, from `react` rather than from a test package, which is
// where it has lived since 18.3. It is what makes "the effect has run" a fact
// rather than a sleep.

import type { ReactNode } from "react";
import { act } from "react";
import type { Root } from "react-dom/client";
import { createRoot } from "react-dom/client";

// React refuses to run `act` without this, and refusing is right: `act` flushes
// effects synchronously, which is a lie about how the browser schedules them and
// is only safe in a test.
(globalThis as { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true;

export type Mounted = {
    readonly container: HTMLElement;
    readonly root: Root;

    // Re-render with a different tree, for the cases where a prop change is the
    // subject: a resource's address changing under a mounted component.
    readonly render: (node: ReactNode) => Promise<void>;

    readonly unmount: () => Promise<void>;
};

export async function mount(document: Document, node: ReactNode): Promise<Mounted> {
    const container = document.createElement("div");
    document.body.append(container);
    const root = createRoot(container);

    const render = async (next: ReactNode): Promise<void> => {
        await act(async () => {
            root.render(next);
        });
    };

    await render(node);

    return {
        container,
        root,
        render,
        unmount: async () => {
            await act(async () => {
                root.unmount();
            });
            container.remove();
        },
    };
}

// A turn of the microtask queue inside `act`, for the cases where the subject is
// a promise settling rather than a render: a mutation's answer, a resource's
// first response.
export async function flush(turns = 24): Promise<void> {
    await act(async () => {
        for (let i = 0; i < turns; i += 1) {
            await Promise.resolve();
        }
    });
}

// Something outside React moves — a store publishes, a handler starts a write —
// and the renders it causes are flushed before the assertion reads them.
//
// It is `act` rather than a bare call for the reason React's warning gives: an
// update from outside a render is scheduled, not applied, so a test that read
// the DOM straight afterwards would assert the frame before the one the user
// sees.
export async function change(mutate: () => void, turns = 24): Promise<void> {
    await act(async () => {
        mutate();
        for (let i = 0; i < turns; i += 1) {
            await Promise.resolve();
        }
    });
}
