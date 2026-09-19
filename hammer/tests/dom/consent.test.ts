// @vitest-environment happy-dom
//
// The consent gate. Driven against a real `AnalyticsSink` rather than a stand-in
// store, because the property worth asserting from here is the one that spans
// both: pressing the control changes what the sink will accept, and nothing
// requiring consent is queued before it has been pressed.

import { describe, expect, it } from "vitest";

import type { ClassNames } from "../../src/core/tables.js";
import { renderConsent } from "../../src/dom/consent.js";
import type { ConsentCopy, ConsentPart } from "../../src/dom/consent.js";
import { AnalyticsSink } from "../../src/state/analytics.js";
import { noBeacon } from "../../src/wire/beacon.js";

const kClasses: ClassNames<ConsentPart> = {
    root: "c",
    question: "c-question",
    grant: "c-grant",
    deny: "c-deny",
    granted: "c-settled",
    revoke: "c-revoke",
};

const kCopy: ConsentCopy = {
    question: "May we count what you do here?",
    grant: "Yes",
    deny: "No",
    granted: "You said yes.",
    denied: "You said no.",
    revoke: "Change to no",
    reconsider: "Change to yes",
};

const kMeasured = { name: "signup", code: 1, requiresConsent: true, dimensions: {} };
const kOperational = { name: "error", code: 2, requiresConsent: false, dimensions: {} };

function mount() {
    const sink = new AnalyticsSink({
        deliver: async () => true,
        beacon: noBeacon,
        flushAt: 100,
        maxBatch: 200,
    });

    const host = document.createElement("div");
    document.body.append(host);
    const view = renderConsent(host, {
        consent: sink.consent,
        setConsent: (answer) => sink.setConsent(answer),
        classes: kClasses,
        copy: kCopy,
    });

    const find = <E extends Element>(s: string): E => {
        const f = host.querySelector<E>(s);
        if (f === null) {
            throw new Error(`the gate did not draw ${s}`);
        }
        return f;
    };

    return {
        sink,
        host,
        view,
        find,
        grant: find<HTMLButtonElement>(".c-grant"),
        deny: find<HTMLButtonElement>(".c-deny"),
        revoke: find<HTMLButtonElement>(".c-revoke"),
        close: () => {
            view.close();
            sink.close();
            host.remove();
        },
    };
}

describe("before it has been answered", () => {
    it("asks, and offers both answers", () => {
        const app = mount();
        expect(app.find<HTMLElement>(".c-question").hidden).toBe(false);
        expect(app.grant.hidden).toBe(false);
        expect(app.deny.hidden).toBe(false);
        expect(app.revoke.hidden).toBe(true);
        app.close();
    });

    it("names the region by the question it is asking", () => {
        const app = mount();
        const root = app.find(".c");
        expect(root.getAttribute("role")).toBe("region");
        expect(root.getAttribute("aria-labelledby")).toBe(app.find(".c-question").id);
        app.close();
    });

    // A buffer that flushes when consent arrives is a buffer of pre-consent
    // data, which is the thing consent was about.
    it("queues nothing that requires consent", () => {
        const app = mount();
        app.sink.report(kMeasured, {});
        expect(app.sink.pending).toBe(0);
        expect(app.sink.refused).toBe(1);
        app.close();
    });

    it("does not stand in the way of what never needed consent", () => {
        const app = mount();
        app.sink.report(kOperational, {});
        expect(app.sink.pending).toBe(1);
        app.close();
    });
});

describe("answering it", () => {
    it("opens the gate when granted", () => {
        const app = mount();
        app.grant.click();

        expect(app.sink.consentIs()).toBe("granted");
        app.sink.report(kMeasured, {});
        expect(app.sink.pending).toBe(1);
        app.close();
    });

    it("keeps the gate shut when denied", () => {
        const app = mount();
        app.deny.click();

        expect(app.sink.consentIs()).toBe("denied");
        app.sink.report(kMeasured, {});
        expect(app.sink.pending).toBe(0);
        app.close();
    });

    // A boolean makes "not asked yet" indistinguishable from "said no", which is
    // how a prompt ends up asking somebody who has already declined.
    it("stops asking once there is an answer either way", () => {
        for (const press of ["c-grant", "c-deny"]) {
            const app = mount();
            app.find<HTMLButtonElement>(`.${press}`).click();
            expect(app.find<HTMLElement>(".c-question").hidden).toBe(true);
            expect(app.grant.hidden).toBe(true);
            expect(app.deny.hidden).toBe(true);
            app.close();
        }
    });

    it("says which answer was given", () => {
        const app = mount();
        app.grant.click();
        expect(app.find(".c-settled").textContent).toBe("You said yes.");
        app.deny.click();
        expect(app.find(".c-settled").textContent).toBe("You said no.");
        app.close();
    });
});

// A grant that can only be withdrawn by finding a settings page is a grant in
// one direction.
describe("changing the answer", () => {
    it("offers the other way, whichever way it currently points", () => {
        const app = mount();
        app.grant.click();
        expect(app.revoke.textContent).toBe("Change to no");

        app.revoke.click();
        expect(app.sink.consentIs()).toBe("denied");
        expect(app.revoke.textContent).toBe("Change to yes");

        app.revoke.click();
        expect(app.sink.consentIs()).toBe("granted");
        app.close();
    });

    it("drops what was already collected when consent is withdrawn", () => {
        const app = mount();
        app.grant.click();
        app.sink.report(kMeasured, {});
        expect(app.sink.pending).toBe(1);

        app.revoke.click();
        expect(app.sink.pending).toBe(0);
        app.close();
    });
});

// A revocation made from a settings screen is the ordinary case, not an edge
// one, and it is the whole reason the sink publishes a store rather than a
// getter.
describe("an answer given somewhere else", () => {
    it("redraws the gate", () => {
        const app = mount();
        app.sink.setConsent("granted");

        expect(app.find<HTMLElement>(".c-question").hidden).toBe(true);
        expect(app.find(".c-settled").textContent).toBe("You said yes.");
        app.close();
    });
});

describe("closing", () => {
    it("takes the gate out and stops following the sink", () => {
        const app = mount();
        app.view.close();
        expect(app.host.querySelector(".c")).toBeNull();
        expect(() => app.sink.setConsent("granted")).not.toThrow();
        app.sink.close();
        app.host.remove();
    });
});
