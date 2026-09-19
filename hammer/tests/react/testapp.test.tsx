// @vitest-environment happy-dom
//
// The reference consumer's React screens, driven through the package specifiers
// a real application would use.
//
// `tests/react/hooks.test.tsx` asserts the lifetime against stand-ins, which is
// where a counting assertion belongs. This asserts the thing type-checking alone
// would let stay a fiction: that `hammer/react` RESOLVES at run time, that its
// hooks compose with the same application state the other suites build, and that
// a screen whose reads, writes, session, stream, form and consent all come from
// this library renders once and comes apart cleanly.
//
// It is the adapter's half of the proof `tests/dom/testapp.test.ts` makes for
// the components.

import { describe, expect, it } from "vitest";

import { ChannelBus } from "../support/fake_channel.js";
import { FakeServer } from "../support/fake_fetch.js";
import { LockRoom } from "../support/fake_locks.js";
import { WorkerRoom } from "../support/fake_worker.js";
import { change, flush, mount } from "../support/react.js";
import { sessionPayload } from "../support/session.js";
import { kStateRoutes } from "../support/state.js";
import { appState } from "../testapp/app/state.js";
import { ReactScreen } from "../testapp/app/react_screen.js";

const kDocument = {
    body: { title: "the document", starred: false, version: 1 },
    headers: { "Cache-Control": "max-age=600" },
};

const kSession = {
    body: { ...sessionPayload({ routes: kStateRoutes, bits: [0, 8] }), user: { id: "u1" } },
};

function app(server: FakeServer) {
    return appState({
        fetch: server.fetch,
        locks: new LockRoom().tab(),
        fanOut: new ChannelBus().tab(),
        pageOrigin: "https://app.example.com",
        apiOrigin: "https://app.example.com",
        imageWorker: new WorkerRoom().create,
        beaconTo: { sendBeacon: () => true },
        count: () => undefined,
    });
}

describe("the reference consumer's React screen", () => {
    it("renders every hook this library ships over one application state", async () => {
        // The session first, because every route below it is a HOLDER route
        // whose address arrives with the session — which is the order a real
        // application loads in.
        const server = new FakeServer().reply(kSession, kDocument).always(kDocument);
        const state = app(server);
        await state.session.load(new AbortController().signal);

        const mounted = await mount(document, <ReactScreen state={state} locale="en" id="7" />);
        await flush();

        // The read reached the wire and came back through the resource store.
        expect(mounted.container.textContent).toContain("the document");

        // The session is active and affords the guarded control, so it is
        // rendered — from the session's own route table rather than from a bit
        // count.
        expect(mounted.container.querySelectorAll("button").length).toBeGreaterThan(0);

        // The stream is at-least-once and the badge counts what it decoded.
        await change(() =>
            state.inbox.accept({
                id: "e1",
                type: "message",
                data: JSON.stringify({ topic: "content.published", subject: "one" }),
            }),
        );
        expect(mounted.container.textContent).toContain("1");

        // Consent is a store like any other, read through `useStore`.
        await change(() => state.analytics.setConsent("granted"));
        expect(mounted.container.textContent).toContain("Measurement is on.");

        await mounted.unmount();
        state.close();
    });

    // The property the components' suite makes for `close()`: a screen that goes
    // away takes its reads with it. Here it is one level up — React's unmount is
    // what calls the release, and nothing else does.
    it("leaves no read open when the screen unmounts", async () => {
        const server = new FakeServer().reply(kSession, kDocument).always(kDocument);
        const state = app(server);
        await state.session.load(new AbortController().signal);

        const mounted = await mount(document, <ReactScreen state={state} locale="ar" id="7" />);
        await flush();
        const sent = server.calls;

        await mounted.unmount();
        await flush();

        // Nothing is fetched after the screen is gone: no refresh, no retry, and
        // no second read from a store that still believed somebody was watching.
        expect(server.calls).toBe(sent);
        state.close();
    });
});
