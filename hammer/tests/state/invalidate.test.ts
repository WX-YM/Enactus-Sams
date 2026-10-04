// A mutation's declared invalidations, here and in the other tabs.
//
// Two store instances in one process over one fake channel is the only way the
// multi-tab behaviour is deterministic (`docs/16-test-plan.md`), and the table
// under test is the reference application's own — a table hammer populated would
// be a table hammer had no business having.

import { describe, expect, it } from "../support/test.js";

import { Invalidator } from "../../src/state/invalidate.js";
import type { Invalidatable } from "../../src/state/invalidate.js";
import type { StateCount } from "../../src/state/counts.js";
import { ChannelBus } from "../support/fake_channel.js";
import { invalidations } from "../testapp/app/invalidate.js";
import type { RouteId } from "../testapp/api/hammer.generated.js";

class Recorder implements Invalidatable {
    readonly applied: string[][] = [];

    invalidate = (routeIds: Iterable<string>): void => {
        this.applied.push(Array.from(routeIds));
    };
}

function tab(bus: ChannelBus, counts: StateCount[] = []) {
    const resources = new Recorder();
    const invalidator = new Invalidator<RouteId>({
        table: invalidations,
        resources,
        fanOut: bus.tab(),
        count: (name) => counts.push(name),
    });
    return { resources, invalidator, counts };
}

// Delivery over the channel is asynchronous, the way it is in a browser.
async function delivered(): Promise<void> {
    for (let i = 0; i < 4; i += 1) {
        await Promise.resolve();
    }
}

describe("Invalidator", () => {
    it("applies a mutation's declared invalidations locally", () => {
        const { resources, invalidator } = tab(new ChannelBus());

        invalidator.after("content.delete");

        expect(resources.applied).toEqual([["content.get"]]);
    });

    it("does nothing for a route the table does not name", () => {
        const { resources, invalidator } = tab(new ChannelBus());

        invalidator.after("content.get");

        // A read invalidates nothing, which is why the table is `Partial`: an
        // entry for every route would be a table of empty arrays whose only
        // effect is to hide the routes that matter.
        expect(resources.applied).toEqual([]);
    });

    it("reaches a second store instance over the channel", async () => {
        const bus = new ChannelBus();
        const a = tab(bus);
        const b = tab(bus);

        a.invalidator.after("media.delete");
        await delivered();

        // Tab B rendering the row tab A just changed is the defect multi-tab
        // coherence exists for.
        expect(b.resources.applied).toEqual([["media.list"]]);
        expect(b.counts).toContain("invalidated");
    });

    it("does not apply its own broadcast twice", async () => {
        const bus = new ChannelBus();
        const a = tab(bus);

        a.invalidator.after("content.delete");
        await delivered();

        expect(a.resources.applied).toEqual([["content.get"]]);
    });

    it("stops listening when it is closed", async () => {
        const bus = new ChannelBus();
        const a = tab(bus);
        const b = tab(bus);

        b.invalidator.close();
        a.invalidator.after("content.delete");
        await delivered();

        expect(b.resources.applied).toEqual([]);
    });

    it("ignores a message that is not one of its own", async () => {
        const bus = new ChannelBus();
        const sender = bus.tab();
        const b = tab(bus);

        // Every one of these is something another conversation on this channel
        // sends, or something a tab running a different build might.
        sender.post({ kind: "refreshed" });
        sender.post(null);
        sender.post("hammer.invalidate");
        sender.post({ kind: "hammer.invalidate" });
        sender.post({ kind: "hammer.invalidate", routes: [] });
        sender.post({ kind: "hammer.invalidate", routes: "content.get" });
        await delivered();

        expect(b.resources.applied).toEqual([]);
    });

    it("drops a message whose route list holds something that is not a string", async () => {
        const bus = new ChannelBus();
        const sender = bus.tab();
        const b = tab(bus);

        // Malformed in one place is not a message to keep the rest of: a
        // non-string reaching a cache would be a key nobody wrote.
        sender.post({ kind: "hammer.invalidate", routes: ["content.get", 7] });
        await delivered();

        expect(b.resources.applied).toEqual([]);
    });

    it("refuses a message naming an implausible number of routes", async () => {
        const bus = new ChannelBus();
        const sender = bus.tab();
        const b = tab(bus);

        const many = Array.from({ length: 257 }, (_unused, i) => `route.${i}`);
        sender.post({ kind: "hammer.invalidate", routes: many });
        await delivered();

        expect(b.resources.applied).toEqual([]);
    });

    it("applies a route id this build does not know rather than filtering it", async () => {
        const bus = new ChannelBus();
        const sender = bus.tab();
        const b = tab(bus);

        // Mid-deploy, the other tab may be a newer build. Over-applying costs a
        // sweep that matches nothing; under-applying shows somebody data that
        // has changed.
        sender.post({ kind: "hammer.invalidate", routes: ["a.route.from.the.future"] });
        await delivered();

        expect(b.resources.applied).toEqual([["a.route.from.the.future"]]);
    });

    it("here() applies locally and tells nobody", async () => {
        const bus = new ChannelBus();
        const a = tab(bus);
        const b = tab(bus);

        a.invalidator.here(["content.get"]);
        await delivered();

        expect(a.resources.applied).toEqual([["content.get"]]);
        // A reconciliation is a write that did NOT happen; there is nothing for
        // another tab to hear about.
        expect(b.resources.applied).toEqual([]);
    });
});
