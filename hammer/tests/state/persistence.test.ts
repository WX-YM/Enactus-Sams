// Nothing in this layer is persisted.
//
// A persisted API response outlives the cookie that authorised it, which is
// private data readable after the session ended, on a shared device
// (`docs/00-architecture.md` §7). `tools/check-source-bans.sh` refuses the names
// in `src/`, and that is the check that matters — but a source ban cannot see a
// property reached dynamically, and it says nothing about what happens when the
// whole layer is actually driven.
//
// So this file installs tripwires and drives the layer through the things that
// would be tempted: a read, a cache, a session, an invalidation across tabs, a
// form, an inbox, and an analytics batch that fails to send. Asserting an
// absence needs the APIs to EXIST and throw rather than to be missing from the
// environment, which is why the suite runs in `node` and this file puts them
// there.

import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { AnalyticsSink } from "../../src/state/analytics.js";
import { Form } from "../../src/state/forms.js";
import { Inbox } from "../../src/state/inbox.js";
import { Invalidator } from "../../src/state/invalidate.js";
import { ResourceStore } from "../../src/state/resource.js";
import { SessionStore } from "../../src/state/session.js";
import { noBeacon } from "../../src/wire/beacon.js";
import { ok } from "../../src/core/result.js";
import {
    kFieldTypes,
    routeContentGet,
    routeIdentityMe,
} from "../testapp/api/hammer.generated.js";
import type { RouteId, ValidationReason } from "../testapp/api/hammer.generated.js";
import type { Api } from "../testapp/app/client.js";
import { invalidations } from "../testapp/app/invalidate.js";
import { ChannelBus } from "../support/fake_channel.js";
import { FakeServer } from "../support/fake_fetch.js";
import { kServerHash, sessionPayload, kBits,} from "../support/session.js";
import { clientHarness, kStateRoutes, settle } from "../support/state.js";

const kStores = ["localStorage", "sessionStorage", "indexedDB", "caches"] as const;

// Every touch is recorded rather than thrown on, so a failure names what was
// reached instead of surfacing as whatever the caller's `catch` decided to do
// with it — and so one violation does not hide the next.
const touched: string[] = [];

function tripwire(name: string): unknown {
    return new Proxy(
        {},
        {
            get: (_target, property) => {
                touched.push(name + "." + String(property));
                return () => undefined;
            },
            set: (_target, property) => {
                touched.push(name + "." + String(property));
                return true;
            },
        },
    );
}

const removed: (() => void)[] = [];

beforeEach(() => {
    touched.length = 0;
    for (const name of kStores) {
        const scope = globalThis as unknown as Record<string, unknown>;
        const had = Object.prototype.hasOwnProperty.call(scope, name);
        const previous = scope[name];
        scope[name] = tripwire(name);
        removed.push(() => {
            if (had) {
                scope[name] = previous;
            } else {
                delete scope[name];
            }
        });
    }
});

afterEach(() => {
    for (const undo of removed.splice(0, removed.length)) {
        undo();
    }
});

function signal(): AbortSignal {
    return new AbortController().signal;
}

describe("persistence", () => {
    it("reads, caches, invalidates and signs out without touching a store", async () => {
        const server = new FakeServer().always({
            body: { title: "one", version: 1 },
            headers: { "Cache-Control": "max-age=600" },
        });
        const bus = new ChannelBus();
        const wire = clientHarness({ server, bus });

        const session = new SessionStore({
            clientHash: kServerHash,
            permissionBits: kBits,
            identityOf: () => "u1",
        });
        session.readsFrom(async () =>
            ok({ ...sessionPayload({ routes: kStateRoutes, bits: [0] }), user: { id: "u1" } }),
        );
        await session.load(signal());

        const resources = new ResourceStore<Api>({
            client: wire.client,
            cache: { classes: { doc: 2 }, defaultMaxEntries: 4 },
            now: wire.now,
        });
        resources.adopt(session.identity());

        // A read, cached and served again from the cache.
        const first = resources.open(routeContentGet, { params: { id: "7" }, class: "doc" });
        await settle();
        first.release();
        const again = resources.open(routeContentGet, { params: { id: "7" }, class: "doc" });
        await settle();
        again.release();

        // Enough entries to force an eviction, which is the other moment a cache
        // is tempted to spill somewhere.
        for (let i = 0; i < 5; i += 1) {
            const held = resources.open(routeContentGet, { params: { id: String(i) }, class: "doc" });
            await settle();
            held.release();
        }

        // An invalidation, here and over the channel.
        const invalidator = new Invalidator<RouteId>({
            table: invalidations,
            resources,
            fanOut: bus.tab(),
        });
        invalidator.after("content.delete");
        await settle();

        // And the session ending, which is where a client that persisted
        // anything would have to remember to clean it up.
        session.clear();
        resources.adopt(null);
        resources.close();
        invalidator.close();

        expect(touched).toEqual([]);
    });

    it("keeps a form, an inbox and an analytics batch in memory", async () => {
        const wire = clientHarness({ server: new FakeServer() });

        const form = new Form<ValidationReason>({
            fields: [{ key: "title", type: kFieldTypes.TEXT_SHORT, required: true }],
            reasons: {
                required: "REQUIRED",
                tooLong: "TOO_LONG",
                badFormat: "BAD_FORMAT",
                notAllowed: "NOT_ALLOWED",
            },
        });
        form.set("title", "a draft somebody typed");
        form.validate();
        form.close();

        const inbox = new Inbox<Api, { readonly topic: string }>({
            client: wire.client,
            route: routeIdentityMe,
            decode: () => ({ topic: "x" }),
        });
        inbox.accept({ id: "1", type: "message", data: "{}" });
        inbox.clear();
        inbox.close();

        // The one that is most tempting to persist: a batch the network refused.
        const sink = new AnalyticsSink({
            deliver: async () => false,
            beacon: noBeacon,
            flushAt: 1,
            maxBatch: 4,
        });
        sink.setConsent("granted");
        sink.report({ name: "E", code: 0, requiresConsent: true, dimensions: {} }, {});
        await settle();
        sink.flushFinal();
        sink.close();

        expect(touched).toEqual([]);
    });

    it("catches a write, so the absence above is an absence and not a blind spot", () => {
        // A check that cannot fail reports clean for the wrong reason.
        const scope = globalThis as unknown as {
            localStorage: { setItem: (key: string, value: string) => void };
        };
        scope.localStorage.setItem("hammer.proof", "x");

        expect(touched).toEqual(["localStorage.setItem"]);
    });
});
