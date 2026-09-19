// The consent gate, and everything downstream of it.
//
// The events are the reference application's generated `const`s, so
// `requiresConsent` is what anvil declared rather than a flag this file set.

import { describe, expect, it } from "vitest";

import { AnalyticsSink } from "../../src/state/analytics.js";
import type { ReportedEvent } from "../../src/state/analytics.js";
import type { StateCount } from "../../src/state/counts.js";
import { beaconFrom, noBeacon } from "../../src/wire/beacon.js";
import {
    eventPageViewed,
    eventSignupCompleted,
    eventSignupStarted,
} from "../testapp/api/hammer.generated.js";

type Sent = readonly ReportedEvent[];

function sink(
    over: {
        readonly deliver?: (batch: Sent) => Promise<boolean>;
        readonly beaconTakes?: boolean;
        readonly flushAt?: number;
        readonly maxBatch?: number;
    } = {},
) {
    const delivered: Sent[] = [];
    const beaconed: string[] = [];
    const counts: StateCount[] = [];

    const held = new AnalyticsSink({
        deliver: async (batch) => {
            delivered.push(batch);
            return over.deliver === undefined ? true : await over.deliver(batch);
        },
        beacon: (body) => {
            beaconed.push(body);
            return over.beaconTakes ?? true;
        },
        flushAt: over.flushAt ?? 3,
        maxBatch: over.maxBatch ?? 64,
        count: (name) => counts.push(name),
    });

    return { sink: held, delivered, beaconed, counts };
}

describe("AnalyticsSink", () => {
    it("does not queue an event that requires consent before consent", () => {
        const { sink: held, counts } = sink();

        held.report(eventPageViewed, { surface: "web", referrer: "direct" });

        // Not queued and filtered later, not buffered pending a decision: a
        // buffer that flushes when consent arrives is a buffer of pre-consent
        // data, which is the thing consent was about.
        expect(held.pending).toBe(0);
        expect(held.refused).toBe(1);
        expect(counts).toContain("consent-refused");
    });

    it("stays shut after a refusal, not merely before an answer", () => {
        const { sink: held } = sink();
        held.setConsent("denied");

        held.report(eventPageViewed, { surface: "web", referrer: "direct" });

        expect(held.pending).toBe(0);
        expect(held.refused).toBe(1);
    });

    it("queues an event that does not require consent, whatever the answer", () => {
        const { sink: held } = sink();

        // `SignupCompleted` is declared `requires_consent: false` server-side.
        held.report(eventSignupCompleted, { surface: "web" });

        expect(held.pending).toBe(1);
        expect(held.refused).toBe(0);
    });

    it("queues a consented event once consent is granted", () => {
        const { sink: held } = sink();
        held.setConsent("granted");

        held.report(eventPageViewed, { surface: "web", referrer: "search" });

        expect(held.pending).toBe(1);
    });

    it("drops what consent covered when consent is withdrawn, and keeps what it did not", () => {
        const { sink: held } = sink({ flushAt: 10 });
        held.setConsent("granted");
        held.report(eventPageViewed, { surface: "web", referrer: "direct" });
        held.report(eventSignupCompleted, { surface: "ios" });
        held.report(eventSignupStarted, { surface: "web" });
        expect(held.pending).toBe(3);

        held.setConsent("denied");

        // Sending a batch collected under a grant that has since been withdrawn
        // is "recorded and then excluded" wearing a different hat. The
        // operational event that never needed consent stays.
        expect(held.pending).toBe(1);
    });

    it("copies the dimensions rather than holding the caller's object", () => {
        const { sink: held, beaconed } = sink({ flushAt: 10 });
        const dimensions = { surface: "web" } as { surface: "web" | "ios" | "android" };

        held.report(eventSignupCompleted, dimensions);
        dimensions.surface = "android";
        held.flushFinal();

        // A batch of aliases is a batch of whatever the last report said.
        expect(beaconed[0]).toContain('"surface":"web"');
    });

    it("flushes when the batch reaches its mark", async () => {
        const { sink: held, delivered } = sink({ flushAt: 2 });

        held.report(eventSignupCompleted, { surface: "web" });
        held.report(eventSignupCompleted, { surface: "ios" });
        await Promise.resolve();
        await Promise.resolve();

        expect(delivered).toHaveLength(1);
        expect(delivered[0]).toHaveLength(2);
        expect(held.pending).toBe(0);
    });

    it("does not put the bookkeeping flag on the wire", async () => {
        const { sink: held, delivered } = sink({ flushAt: 1 });

        held.report(eventSignupCompleted, { surface: "web" });
        await Promise.resolve();
        await Promise.resolve();

        expect(delivered[0]?.[0]).toEqual({
            name: "SignupCompleted",
            code: 2,
            dimensions: { surface: "web" },
        });
    });

    it("puts a batch back when delivery was refused", async () => {
        const { sink: held } = sink({ flushAt: 2, deliver: async () => false });

        held.report(eventSignupCompleted, { surface: "web" });
        held.report(eventSignupCompleted, { surface: "ios" });
        await Promise.resolve();
        await Promise.resolve();
        await Promise.resolve();

        expect(held.pending).toBe(2);
    });

    it("joins a second flush onto the one in flight", async () => {
        const { sink: held, delivered } = sink({ flushAt: 10 });
        held.report(eventSignupCompleted, { surface: "web" });

        await Promise.all([held.flush(), held.flush()]);

        // Two flushes over one array is one batch sent twice and one sent never.
        expect(delivered).toHaveLength(1);
    });

    it("drops the oldest rather than growing past its bound", async () => {
        // A tab open for days with a route that has been failing for an hour is
        // otherwise a leak with a slow fuse, and the oldest event is the one
        // whose moment has already passed.
        const { sink: held } = sink({ flushAt: 2, maxBatch: 2, deliver: async () => false });

        for (let i = 0; i < 6; i += 1) {
            held.report(eventSignupCompleted, { surface: "web" });
            await Promise.resolve();
            await Promise.resolve();
        }

        expect(held.pending).toBeLessThanOrEqual(2);
        expect(held.dropped).toBeGreaterThan(0);
    });

    it("sends the last batch by beacon", () => {
        const { sink: held, beaconed } = sink({ flushAt: 10 });
        held.report(eventSignupCompleted, { surface: "web" });

        expect(held.flushFinal()).toBe(true);
        expect(beaconed).toHaveLength(1);
        expect(held.pending).toBe(0);
    });

    it("keeps the batch when the browser refuses the beacon", () => {
        // `pagehide` fires for a hidden page as well as an unloading one, so a
        // refused batch may still get a chance on an ordinary flush.
        const { sink: held } = sink({ flushAt: 10, beaconTakes: false });
        held.report(eventSignupCompleted, { surface: "web" });

        expect(held.flushFinal()).toBe(false);
        expect(held.pending).toBe(1);
    });

    it("reports nothing once it is closed", () => {
        const { sink: held } = sink({ flushAt: 10 });
        held.setConsent("granted");
        held.close();

        held.report(eventPageViewed, { surface: "web", referrer: "direct" });

        expect(held.pending).toBe(0);
    });

    it("refuses a configuration that would drop below its flush mark", () => {
        expect(
            () =>
                new AnalyticsSink({
                    deliver: async () => true,
                    beacon: noBeacon,
                    flushAt: 10,
                    maxBatch: 4,
                }),
        ).toThrow();
    });
});

// The gate that asks is drawn from this, and the answer changes in code the gate
// did not call — a revocation made from a settings screen is the ordinary case.
describe("the answer a consent gate reads", () => {
    it("starts unknown, which is not a refusal", () => {
        const { sink: held } = sink();
        expect(held.consent.get()).toBe("unknown");
        expect(held.consentIs()).toBe("unknown");
    });

    it("tells a subscriber about an answer given elsewhere", () => {
        const { sink: held } = sink();
        const seen: string[] = [];
        const off = held.consent.subscribe((answer) => seen.push(answer));

        held.setConsent("granted");
        held.setConsent("denied");
        off();
        held.setConsent("granted");

        expect(seen).toEqual(["granted", "denied"]);
    });

    it("says nothing when the answer did not change", () => {
        const { sink: held } = sink();
        held.setConsent("granted");
        const seen: string[] = [];
        const off = held.consent.subscribe((answer) => seen.push(answer));
        held.setConsent("granted");
        off();
        expect(seen).toEqual([]);
    });

    // The withdrawal drops the batch collected under the grant, and a subscriber
    // reading `pending` from inside its own callback has to see that already
    // done — otherwise the gate renders a count of events the withdrawal is in
    // the middle of discarding.
    it("has already dropped the consented batch by the time it notifies", () => {
        const { sink: held } = sink({ flushAt: 10 });
        held.setConsent("granted");
        held.report({ name: "a", code: 1, requiresConsent: true, dimensions: {} }, {});
        held.report({ name: "b", code: 2, requiresConsent: true, dimensions: {} }, {});
        expect(held.pending).toBe(2);

        let pendingWhenTold = -1;
        const off = held.consent.subscribe(() => {
            pendingWhenTold = held.pending;
        });
        held.setConsent("denied");
        off();

        expect(pendingWhenTold).toBe(0);
    });
});

describe("beaconFrom", () => {
    it("sends to the URL it was built with and reports what the browser said", () => {
        const asked: { url: string; type: string }[] = [];
        const beacon = beaconFrom(
            {
                sendBeacon: (url, data) => {
                    asked.push({ url, type: (data as Blob).type });
                    return true;
                },
            },
            "https://app.example.com/ingest",
        );

        expect(beacon("[]")).toBe(true);
        expect(asked[0]?.url).toBe("https://app.example.com/ingest");
        // A bare string is sent as `text/plain`, which a server reading JSON
        // either rejects or has to be configured to ignore.
        expect(asked[0]?.type).toBe("application/json");
    });

    it("reports false rather than throwing when the policy refuses", () => {
        // A refusal to send analytics must never be a refusal to unload the page.
        const beacon = beaconFrom(
            {
                sendBeacon: () => {
                    throw new Error("refused by policy");
                },
            },
            "https://app.example.com/ingest",
        );

        expect(beacon("[]")).toBe(false);
    });

    it("has an honest no-op for a platform without one", () => {
        expect(noBeacon("[]")).toBe(false);
    });
});
