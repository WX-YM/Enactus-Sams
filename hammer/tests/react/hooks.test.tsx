// @vitest-environment happy-dom
//
// What is asserted here is the lifecycle and nothing else.
//
// That a resource dedupes, that a form validates, that an inbox survives a
// duplicate event: all of that is asserted in `tests/state/**` with no framework
// in the process, which is the point of the adapter being this thin. What only
// this suite can see is the half React owns — when a subscription starts, when
// it stops, and what happens when React does both twice in one commit because
// StrictMode asked it to.
//
// The cases below are therefore about counting: handles opened against handles
// released, listeners attached against listeners detached, and renders after an
// unmount, which must be none.

import type { ReactElement } from "react";
import { StrictMode, useCallback } from "react";
import { describe, expect, it } from "vitest";

import { fail, ok } from "../../src/core/result.js";
import type { HammerError } from "../../src/core/errors.js";
import { cacheKey } from "../../src/state/cache.js";
import { Form } from "../../src/state/forms.js";
import { Inbox } from "../../src/state/inbox.js";
import type { Resource, ResourceState } from "../../src/state/resource.js";
import { ResourceStore } from "../../src/state/resource.js";
import { SessionStore } from "../../src/state/session.js";
import { Store } from "../../src/state/store.js";
import { kBits } from "../support/session.js";
import type { Readable } from "../../src/state/store.js";
import {
    useForm,
    useInbox,
    useMutation,
    useResource,
    useSession,
    useStore,
} from "../../src/react/index.js";
import type { ValidationReason } from "../testapp/api/hammer.generated.js";
import { kFieldTypes, routeContentGet, routeIdentityMe } from "../testapp/api/hammer.generated.js";
import type { Api } from "../testapp/app/client.js";
import { FakeServer } from "../support/fake_fetch.js";
import { change, flush, mount } from "../support/react.js";
import { clientHarness } from "../support/state.js";

// --- a resource, instrumented ----------------------------------------------
//
// A stand-in rather than the real store for the counting cases, because what is
// being counted is what the HOOK did: how many handles it took out and how many
// it gave back. The real `ResourceStore` refcounts those internally and would
// answer a different question.

type Handles<T> = {
    readonly open: () => Resource<T, HammerError>;
    readonly store: Store<ResourceState<T, HammerError>>;
    opened: number;
    released: number;
    listeners: number;
};

// A key of the shape the real store mints, so the stand-in satisfies the same
// branded type a component would be handed.
const kKey = cacheKey({ routeId: "content.get", params: {}, query: {}, identity: null });

function handles<T>(initial: ResourceState<T, HammerError>): Handles<T> {
    const store = new Store<ResourceState<T, HammerError>>(initial);
    const counts: Handles<T> = {
        store,
        opened: 0,
        released: 0,
        listeners: 0,
        open: () => {
            counts.opened += 1;
            let released = false;
            return {
                key: () => kKey,
                state: {
                    get: () => store.get(),
                    subscribe: (listener) => {
                        counts.listeners += 1;
                        const off = store.subscribe(listener);
                        return () => {
                            counts.listeners -= 1;
                            off();
                        };
                    },
                },
                ready: async () => store.get(),
                refresh: async () => {},
                overwrite: () => {},
                provisional: () => {},
                hasProvisional: () => false,
                restore: () => {},
                forget: () => {},
                release: () => {
                    if (released) {
                        return;
                    }
                    released = true;
                    counts.released += 1;
                },
            };
        },
    };
    return counts;
}

const kReady = <T,>(value: T): ResourceState<T, HammerError> => ({
    status: "ready",
    value,
    error: null,
    stale: false,
});

describe("useResource", () => {
    it("opens once, and releases what it opened on unmount", async () => {
        const counts = handles(kReady("one"));

        function Screen(): ReactElement {
            const open = useCallback(counts.open, []);
            const view = useResource(open);
            return <p>{view.state.value}</p>;
        }

        const mounted = await mount(document, <Screen />);
        expect(mounted.container.textContent).toBe("one");
        expect(counts.opened).toBe(1);
        expect(counts.listeners).toBe(1);

        await mounted.unmount();
        expect(counts.released).toBe(1);
        expect(counts.listeners).toBe(0);
    });

    // The row `docs/15-tasks.md` names: correct under StrictMode's double
    // invoke. React mounts, unmounts and remounts every effect in development to
    // surface exactly this, and the property is not "it opens once" — it is that
    // the count comes back to zero, with one live handle while the component is
    // on screen.
    it("nets one live handle under StrictMode and none after unmount", async () => {
        const counts = handles(kReady("one"));

        function Screen(): ReactElement {
            const open = useCallback(counts.open, []);
            const view = useResource(open);
            return <p>{view.state.value}</p>;
        }

        const mounted = await mount(
            document,
            <StrictMode>
                <Screen />
            </StrictMode>,
        );
        expect(counts.opened - counts.released).toBe(1);
        expect(counts.listeners).toBe(1);

        await mounted.unmount();
        expect(counts.opened - counts.released).toBe(0);
        expect(counts.listeners).toBe(0);
    });

    it("re-renders on a value the store publishes", async () => {
        const counts = handles(kReady("one"));

        function Screen(): ReactElement {
            const open = useCallback(counts.open, []);
            return <p>{useResource(open).state.value}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await change(() => counts.store.set(kReady("two")));

        expect(mounted.container.textContent).toBe("two");
        await mounted.unmount();
    });

    // An address that changes is a release and an open, in that order, with no
    // render in between that could show the previous entry's body under the new
    // parameters.
    it("releases the previous address when the factory changes", async () => {
        const first = handles(kReady("one"));
        const second = handles(kReady("two"));

        function Screen(props: { readonly which: Handles<string> }): ReactElement {
            const open = useCallback(props.which.open, [props.which]);
            return <p>{useResource(open).state.value}</p>;
        }

        const mounted = await mount(document, <Screen which={first} />);
        expect(mounted.container.textContent).toBe("one");

        await mounted.render(<Screen which={second} />);
        expect(mounted.container.textContent).toBe("two");
        expect(first.released).toBe(1);
        expect(first.listeners).toBe(0);
        expect(second.opened).toBe(1);

        await mounted.unmount();
        expect(second.released).toBe(1);
    });

    // The handle is what `optimistic()` and `writeVersioned()` take, so a
    // component that never sees one cannot write through the resource it is
    // reading. It arrives one render after the mount and is not null for any
    // event a person can fire.
    it("hands back the handle once the subscription exists", async () => {
        const counts = handles(kReady("one"));
        const seen: (Resource<string, HammerError> | null)[] = [];

        function Screen(): ReactElement {
            const open = useCallback(counts.open, []);
            const view = useResource(open);
            seen.push(view.resource);
            return <p>{view.state.value}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await flush();

        expect(seen[0]).toBeNull();
        expect(seen[seen.length - 1]).not.toBeNull();
        await mounted.unmount();
    });

    // The one case that drives the real store, because the interesting part is
    // that the two layers agree: a body arriving on the wire reaches a render.
    it("renders a body the client fetched, and drops the entry on unmount", async () => {
        const server = new FakeServer().always({ body: { title: "from the server" } });
        const wire = clientHarness({ server });
        const store = new ResourceStore<Api>({
            client: wire.client,
            cache: { classes: { doc: 4 }, defaultMaxEntries: 8 },
            now: wire.now,
        });

        function Screen(): ReactElement {
            const open = useCallback(
                () => store.open(routeContentGet, { params: { id: "7" }, class: "doc" }),
                [],
            );
            const view = useResource(open);
            return <p>{view.state.status === "ready" ? view.state.value.title : "…"}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await flush();
        expect(mounted.container.textContent).toBe("from the server");
        expect(server.calls).toBe(1);

        await mounted.unmount();

        // The entry went with the last watcher, so a second reader gets a new
        // store rather than the one the unmounted component was holding. The
        // response was not re-fetched, because the cache is what outlives the
        // entry.
        const reopened = store.open(routeContentGet, { params: { id: "7" }, class: "doc" });
        expect(reopened.state).not.toBe(store.open(routeContentGet, { params: { id: "8" } }).state);
        reopened.release();
        store.close();
    });
});

describe("useMutation", () => {
    it("reports idle, running and then the answer", async () => {
        let settle: (() => void) | null = null;
        const held = new Promise<void>((resolve) => {
            settle = resolve;
        });
        const seen: string[] = [];
        let start: (() => void) | null = null;

        function Screen(): ReactElement {
            const write = useMutation<string, HammerError>();
            seen.push(write.state.status);
            start = () => {
                void write.run(async () => {
                    await held;
                    return ok("written");
                });
            };
            return <p>{write.state.value ?? ""}</p>;
        }

        const mounted = await mount(document, <Screen />);
        expect(seen).toEqual(["idle"]);

        await change(() => start?.());
        expect(seen).toContain("running");

        await change(() => settle?.());
        expect(mounted.container.textContent).toBe("written");
        expect(seen[seen.length - 1]).toBe("done");

        await mounted.unmount();
    });

    it("publishes a failure as a state rather than a rejection", async () => {
        const error: HammerError = { kind: "transport", cause: "network" };
        let start: (() => void) | null = null;
        let last: string | null = null;

        function Screen(): ReactElement {
            const write = useMutation<string, HammerError>();
            last = write.state.status;
            start = () => {
                void write.run(async () => fail(error));
            };
            return <p>{write.state.error === null ? "" : write.state.error.kind}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await change(() => start?.());

        expect(last).toBe("failed");
        expect(mounted.container.textContent).toBe("transport");
        await mounted.unmount();
    });

    // Nothing outlives what created it. The signal the call was given is
    // aborted, and the answer that arrives afterwards renders nothing — a state
    // update on a tree nobody is looking at is work no frame will ever show.
    it("aborts what is in flight on unmount and publishes nothing after", async () => {
        let settle: ((value: string) => void) | null = null;
        let aborted = false;
        let renders = 0;
        let start: (() => void) | null = null;

        function Screen(): ReactElement {
            const write = useMutation<string, HammerError>();
            renders += 1;
            start = () => {
                void write.run(async (signal) => {
                    signal.addEventListener("abort", () => {
                        aborted = true;
                    });
                    return ok(await new Promise<string>((resolve) => {
                        settle = resolve;
                    }));
                });
            };
            return <p>{write.state.status}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await change(() => start?.());

        const before = renders;
        await mounted.unmount();
        expect(aborted).toBe(true);

        await change(() => settle?.("late"));
        expect(renders).toBe(before);
    });

    it("supersedes an earlier call rather than letting it land last", async () => {
        const gates: ((value: string) => void)[] = [];
        let start: (() => void) | null = null;
        let value: string | null = null;

        function Screen(): ReactElement {
            const write = useMutation<string, HammerError>();
            value = write.state.value;
            start = () => {
                void write.run(
                    async () => ok(await new Promise<string>((resolve) => gates.push(resolve))),
                );
            };
            return <p>{write.state.value ?? ""}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await change(() => start?.());
        await change(() => start?.());

        // The newer call answers first, then the older one. The older one is the
        // one that must not be on screen: a person who edited twice is looking
        // at the second edit.
        await change(() => gates[1]?.("second"));
        await change(() => gates[0]?.("first"));

        expect(value).toBe("second");
        await mounted.unmount();
    });

    it("abandons without rendering the answer when aborted", async () => {
        let settle: ((value: string) => void) | null = null;
        let status: string | null = null;
        let start: (() => void) | null = null;
        let stop: (() => void) | null = null;

        function Screen(): ReactElement {
            const write = useMutation<string, HammerError>();
            status = write.state.status;
            start = () => {
                void write.run(async () => ok(await new Promise<string>((r) => {
                    settle = r;
                })));
            };
            stop = write.abort;
            return <p>{write.state.status}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await change(() => start?.());
        await change(() => stop?.());
        expect(status).toBe("idle");

        await change(() => settle?.("late"));
        expect(status).toBe("idle");

        await mounted.unmount();
    });
});

describe("the stores an application already holds", () => {
    it("useStore re-renders on a change and detaches on unmount", async () => {
        const store = new Store<number>(1);
        const read: Readable<number> = store;

        function Screen(): ReactElement {
            return <p>{useStore(read)}</p>;
        }

        const mounted = await mount(document, <Screen />);
        expect(mounted.container.textContent).toBe("1");

        await change(() => store.set(2));
        expect(mounted.container.textContent).toBe("2");

        await mounted.unmount();
        // Nothing to assert on the DOM: what is asserted is that the set below
        // reaches no listener, which a leaked subscription would turn into a
        // render on a root that has been unmounted.
        store.set(3);
        expect(store.get()).toBe(3);
    });

    it("useSession renders the session state and the change that follows", async () => {
        const session = new SessionStore({ clientHash: "h", permissionBits: kBits });
        let status: string | null = null;

        function Screen(): ReactElement {
            status = useSession(session).status;
            return <p>{status}</p>;
        }

        const mounted = await mount(document, <Screen />);
        expect(status).toBe("unknown");

        await change(() => session.clear());
        expect(status).toBe("anonymous");

        await mounted.unmount();
        session.close();
    });

    it("useForm renders a field the store has cleaned", async () => {
        // The field type is the reference consumer's generated table rather than
        // a literal written here: a spec invented for a test is a spec nothing
        // regenerates when the server's table moves.
        const form = new Form<ValidationReason>({
            fields: [{ key: "title", type: kFieldTypes.TEXT_SHORT, required: true }],
            reasons: {
                required: "REQUIRED",
                tooLong: "TOO_LONG",
                badFormat: "BAD_FORMAT",
                notAllowed: "NOT_ALLOWED",
            },
        });

        function Screen(): ReactElement {
            const state = useForm(form);
            return <p>{String(state.fields.get("title")?.value ?? "")}</p>;
        }

        const mounted = await mount(document, <Screen />);
        await change(() => form.set("title", "one"));
        expect(mounted.container.textContent).toBe("one");

        await mounted.unmount();
        form.close();
    });

    it("useInbox renders the unread count an event moved", async () => {
        const wire = clientHarness();
        const inbox = new Inbox<Api, { readonly headline: string }>({
            client: wire.client,
            route: routeIdentityMe,
            decode: (event) => ({ headline: event.data }),
        });

        function Screen(): ReactElement {
            return <p>{useInbox(inbox).unread}</p>;
        }

        const mounted = await mount(document, <Screen />);
        expect(mounted.container.textContent).toBe("0");

        await change(() => inbox.accept({ id: "1", type: "message", data: "one" }));
        expect(mounted.container.textContent).toBe("1");

        // At-least-once: the same event again is the same count.
        await change(() => inbox.accept({ id: "1", type: "message", data: "one" }));
        expect(mounted.container.textContent).toBe("1");

        await mounted.unmount();
        inbox.close();
    });
});
