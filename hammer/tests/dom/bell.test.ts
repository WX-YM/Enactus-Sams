// @vitest-environment happy-dom
//
// The bell: a count that is the server's, a popover that hands focus back, and
// an arrival that is heard without being felt.
//
// It is driven from a plain store rather than from an `Inbox`, and that is the
// point of the store taking `Readable<InboxState<T>>`: the dedupe, the ring and
// the reconciliation are asserted in `tests/state/inbox.test.ts` with no
// document at all, and what is left for here is what a document is needed for.

import { describe, expect, it } from "vitest";

import type { ClassNames } from "../../src/core/tables.js";
import { renderBell } from "../../src/dom/bell.js";
import type { BellCopy, BellPart } from "../../src/dom/bell.js";
import type { InboxState, Notification } from "../../src/state/inbox.js";
import { Store } from "../../src/state/store.js";

type Body = { readonly text: string };

const kClasses: ClassNames<BellPart> = {
    root: "b",
    trigger: "b-trigger",
    badge: "b-badge",
    popover: "b-popover",
    list: "b-list",
    item: "b-item",
    unreadItem: "b-item-unread",
    empty: "b-empty",
    markRead: "b-mark",
    live: "b-live",
};

const kCopy: BellCopy = {
    label: "Notifications",
    unread: (count) => `${count} unread`,
    arrived: (count) => `${count} arrived`,
    empty: "Nothing here.",
    markRead: "Mark all read",
};

function note(id: string, text: string, read = false): Notification<Body> {
    return { id, type: "t", body: { text }, read };
}

function state(over: Partial<InboxState<Body>> = {}): InboxState<Body> {
    return { unread: 0, items: [], live: true, ...over };
}

function mount(initial: InboxState<Body> = state()) {
    const store = new Store<InboxState<Body>>(initial);
    const marked: readonly string[][] = [];
    const calls: string[][] = marked as string[][];

    const host = document.createElement("div");
    document.body.append(host);

    const view = renderBell(host, {
        inbox: store,
        markRead: (ids) => calls.push([...ids]),
        describe: (item) => item.body.text,
        classes: kClasses,
        copy: kCopy,
    });

    const find = <E extends Element>(selector: string): E => {
        const found = host.querySelector<E>(selector);
        if (found === null) {
            throw new Error(`the bell did not draw ${selector}`);
        }
        return found;
    };

    return {
        store,
        host,
        view,
        calls,
        find,
        trigger: find<HTMLButtonElement>(".b-trigger"),
        popover: find<HTMLElement>(".b-popover"),
        close: () => {
            view.close();
            host.remove();
        },
    };
}

describe("the count", () => {
    // The store holds a bounded ring, so a tally of what is on screen would be a
    // badge that stops climbing at whatever the ring holds. The number is the
    // server's and the badge renders it verbatim.
    it("is the server's number and not a tally of the rows", () => {
        const app = mount(state({ unread: 412, items: [note("a", "one")] }));
        expect(app.find(".b-badge").textContent).toBe("412");
        app.close();
    });

    it("says what it means on the control rather than only in the digit", () => {
        const app = mount(state({ unread: 3 }));
        expect(app.trigger.getAttribute("aria-label")).toBe("3 unread");
        app.close();
    });

    // A screen reader announcing the digit twice is worse than not announcing it
    // at all: the accessible name on the trigger already carries the meaning.
    it("hides the digit from the name it would duplicate", () => {
        const app = mount(state({ unread: 3 }));
        expect(app.find(".b-badge").getAttribute("aria-hidden")).toBe("true");
        app.close();
    });

    it("shows nothing at all when there is nothing unread", () => {
        const app = mount(state({ unread: 0 }));
        expect(app.find(".b-badge").textContent).toBe("");
        app.close();
    });

    // The count the store publishes is what shows, whatever route it took to get
    // there — a duplicate event the store dropped, a reconnect that replayed, or
    // the server's own answer replacing an adjustment.
    it("follows the store rather than counting arrivals itself", () => {
        const app = mount(state({ unread: 1, items: [note("a", "one")] }));
        app.store.set(state({ unread: 1, items: [note("a", "one")] }));
        expect(app.find(".b-badge").textContent).toBe("1");

        app.store.set(state({ unread: 9, items: [note("a", "one")] }));
        expect(app.find(".b-badge").textContent).toBe("9");
        app.close();
    });
});

describe("the popover", () => {
    it("starts closed and says so", () => {
        const app = mount();
        expect(app.popover.hidden).toBe(true);
        expect(app.trigger.getAttribute("aria-expanded")).toBe("false");
        app.close();
    });

    it("opens on the trigger and takes focus, because that was asked for", () => {
        const app = mount();
        app.trigger.click();
        expect(app.popover.hidden).toBe(false);
        expect(app.trigger.getAttribute("aria-expanded")).toBe("true");
        expect(document.activeElement).toBe(app.popover);
        app.close();
    });

    // Not a trap: Escape always closes and always hands focus back.
    it("closes on Escape and gives focus back to what had it", () => {
        const app = mount();
        const before = document.createElement("input");
        document.body.append(before);
        before.focus();

        app.trigger.click();
        expect(document.activeElement).toBe(app.popover);

        app.popover.dispatchEvent(
            new KeyboardEvent("keydown", { key: "Escape", bubbles: true }),
        );
        expect(app.popover.hidden).toBe(true);
        expect(document.activeElement).toBe(before);

        before.remove();
        app.close();
    });

    it("closes on a click outside it", () => {
        const app = mount();
        app.trigger.click();
        document.body.dispatchEvent(new Event("click", { bubbles: true }));
        expect(app.popover.hidden).toBe(true);
        app.close();
    });

    it("stays open for a click inside it", () => {
        const app = mount(state({ items: [note("a", "one")] }));
        app.trigger.click();
        app.find(".b-list").dispatchEvent(new Event("click", { bubbles: true }));
        expect(app.popover.hidden).toBe(false);
        app.close();
    });

    // Taking focus back from something the person has since clicked into would
    // be the theft this whole path exists to avoid.
    it("does not take focus back when it no longer holds it", () => {
        const app = mount();
        const elsewhere = document.createElement("input");
        document.body.append(elsewhere);

        app.trigger.click();
        elsewhere.focus();
        app.trigger.click();

        expect(document.activeElement).toBe(elsewhere);
        elsewhere.remove();
        app.close();
    });

    it("names itself, so the region is not an unlabelled box", () => {
        const app = mount();
        expect(app.popover.getAttribute("aria-label")).toBe("Notifications");
        app.close();
    });
});

describe("an arrival", () => {
    // Announcing on mount would read out a backlog to somebody who has just
    // arrived on the page.
    it("says nothing about what was already there", () => {
        const app = mount(state({ unread: 5, items: [note("a", "one")] }));
        expect(app.find(".b-live").textContent).toBe("");
        app.close();
    });

    it("is announced politely when the count climbs", () => {
        const app = mount(state({ unread: 1 }));
        app.store.set(state({ unread: 3, items: [note("a", "one")] }));

        const live = app.find(".b-live");
        expect(live.getAttribute("aria-live")).toBe("polite");
        expect(live.getAttribute("aria-atomic")).toBe("true");
        expect(live.textContent).toBe("2 arrived");
        app.close();
    });

    // An arrival that stole focus would interrupt whatever the person was
    // typing, which is a worse defect than a missed notification.
    it("does not move focus", () => {
        const app = mount(state({ unread: 0 }));
        const typing = document.createElement("input");
        document.body.append(typing);
        typing.focus();

        app.store.set(state({ unread: 2, items: [note("a", "one")] }));

        expect(app.find(".b-live").textContent).toBe("2 arrived");
        expect(document.activeElement).toBe(typing);
        typing.remove();
        app.close();
    });

    it("says nothing when the count falls", () => {
        const app = mount(state({ unread: 4 }));
        app.store.set(state({ unread: 4 }));
        app.store.set(state({ unread: 1 }));
        expect(app.find(".b-live").textContent).toBe("");
        app.close();
    });
});

describe("the list", () => {
    it("draws the words the application gave for each item", () => {
        const app = mount(state({ items: [note("a", "one"), note("b", "two", true)] }));
        const rows = app.host.querySelectorAll("li");
        expect(rows.length).toBe(2);
        expect(rows[0]?.textContent).toBe("one");
        expect(rows[0]?.className).toBe("b-item-unread");
        expect(rows[1]?.className).toBe("b-item");
        app.close();
    });

    it("places a notification as text and never as markup", () => {
        const app = mount(state({ items: [note("a", "<img src=x onerror=y>")] }));
        const row = app.host.querySelector("li");
        expect(row?.children.length).toBe(0);
        expect(row?.textContent).toBe("<img src=x onerror=y>");
        app.close();
    });

    it("shows the empty line only while there is nothing", () => {
        const app = mount();
        expect(app.find<HTMLElement>(".b-empty").hidden).toBe(false);
        app.store.set(state({ items: [note("a", "one")] }));
        expect(app.find<HTMLElement>(".b-empty").hidden).toBe(true);
        app.close();
    });
});

describe("marking read", () => {
    it("sends the unread ids in one call", () => {
        const app = mount(
            state({ unread: 2, items: [note("a", "one"), note("b", "two", true), note("c", "three")] }),
        );
        app.find<HTMLButtonElement>(".b-mark").click();
        expect(app.calls).toEqual([["a", "c"]]);
        app.close();
    });

    // An empty id is an event that carried none, and the store says it is the
    // one field a caller must not treat as unique. Sending it would ask the
    // server to mark whatever else shares it.
    it("does not send an id the stream never gave", () => {
        const app = mount(state({ unread: 2, items: [note("", "one"), note("c", "three")] }));
        app.find<HTMLButtonElement>(".b-mark").click();
        expect(app.calls).toEqual([["c"]]);
        app.close();
    });

    it("makes no call when there is nothing unread", () => {
        const app = mount(state({ items: [note("a", "one", true)] }));
        app.find<HTMLButtonElement>(".b-mark").click();
        expect(app.calls).toEqual([]);
        app.close();
    });
});

describe("closing", () => {
    it("stops listening to the document it borrowed", () => {
        const app = mount();
        app.trigger.click();
        app.view.close();

        // The handler is gone with the component; a click that would have closed
        // it now reaches nothing.
        expect(() => document.body.dispatchEvent(new Event("click", { bubbles: true }))).not.toThrow();
        expect(app.host.querySelector(".b")).toBeNull();
        app.host.remove();
    });

    it("stops following the store", () => {
        const app = mount(state({ unread: 1 }));
        app.view.close();
        expect(() => app.store.set(state({ unread: 99 }))).not.toThrow();
        app.host.remove();
    });
});
