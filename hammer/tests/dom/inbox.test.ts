//
// The inbox list, and the part of it that is not a list of divs: one tab stop
// for the whole thing, arrows inside it, and read state that is said rather than
// only shown.

import { describe, expect, it } from "../support/test.js";

import type { ClassNames } from "../../src/core/tables.js";
import { renderInbox } from "../../src/dom/inbox.js";
import type { InboxCopy, InboxPart } from "../../src/dom/inbox.js";
import type { InboxState, Notification } from "../../src/state/inbox.js";
import { Store } from "../../src/state/store.js";

type Body = { readonly text: string };

const kClasses: ClassNames<InboxPart> = {
    root: "i",
    list: "i-list",
    item: "i-item",
    unreadItem: "i-item-unread",
    empty: "i-empty",
    live: "i-live",
};

const kCopy: InboxCopy = {
    label: "Notifications",
    arrived: (count) => `${count} arrived`,
    empty: "Nothing here.",
};

function note(id: string, text: string, read = false): Notification<Body> {
    return { id, type: "t", body: { text }, read };
}

function state(over: Partial<InboxState<Body>> = {}): InboxState<Body> {
    return { unread: 0, items: [], live: true, ...over };
}

function mount(initial: InboxState<Body> = state()) {
    const store = new Store<InboxState<Body>>(initial);
    const marked: string[][] = [];
    const opened: string[] = [];

    const host = document.createElement("div");
    document.body.append(host);

    const view = renderInbox(host, {
        inbox: store,
        markRead: (ids) => marked.push([...ids]),
        describe: (item) => item.body.text,
        onOpen: (item) => opened.push(item.id),
        classes: kClasses,
        copy: kCopy,
    });

    const rows = (): HTMLLIElement[] => Array.from(host.querySelectorAll("li"));
    const list = (): HTMLElement => {
        const found = host.querySelector<HTMLElement>(".i-list");
        if (found === null) {
            throw new Error("the inbox drew no list");
        }
        return found;
    };

    const press = (key: string): void => {
        list().dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));
    };

    return {
        store,
        host,
        view,
        marked,
        opened,
        rows,
        list,
        press,
        close: () => {
            view.close();
            host.remove();
        },
    };
}

const kThree = state({ unread: 2, items: [note("a", "one"), note("b", "two"), note("c", "3", true)] });

describe("the list", () => {
    it("names itself", () => {
        const app = mount(kThree);
        expect(app.list().getAttribute("aria-label")).toBe("Notifications");
        app.close();
    });

    it("draws the words the application gave, as text", () => {
        const app = mount(state({ items: [note("a", "<b>x</b>")] }));
        const row = app.rows()[0];
        expect(row?.children.length).toBe(0);
        expect(row?.textContent).toBe("<b>x</b>");
        app.close();
    });

    // A colour is not available to everybody, and it is the application's
    // anyway. The state is on the element.
    it("says which rows are unread rather than only showing it", () => {
        const app = mount(kThree);
        const rows = app.rows();
        expect(rows[0]?.getAttribute("aria-current")).toBe("true");
        expect(rows[2]?.getAttribute("aria-current")).toBe("false");
        app.close();
    });

    it("shows the empty line only while there is nothing", () => {
        const app = mount();
        expect(app.host.querySelector<HTMLElement>(".i-empty")?.hidden).toBe(false);
        expect(app.list().hidden).toBe(true);

        app.store.set(kThree);
        expect(app.host.querySelector<HTMLElement>(".i-empty")?.hidden).toBe(true);
        expect(app.list().hidden).toBe(false);
        app.close();
    });
});

describe("the keyboard", () => {
    // A hundred notifications must not be a hundred presses of Tab to get past.
    it("is one tab stop for the whole list", () => {
        const app = mount(kThree);
        const stops = app.rows().filter((row) => row.tabIndex === 0);
        expect(stops.length).toBe(1);
        app.close();
    });

    // A positive tabindex reorders the whole page around this component.
    it("never uses a positive tabindex", () => {
        const app = mount(kThree);
        for (const row of app.rows()) {
            expect(row.tabIndex).toBeLessThanOrEqual(0);
        }
        app.close();
    });

    it("moves the stop and the focus together", () => {
        const app = mount(kThree);
        app.press("ArrowDown");

        const rows = app.rows();
        expect(document.activeElement).toBe(rows[1]);
        expect(rows[1]?.tabIndex).toBe(0);
        expect(rows[0]?.tabIndex).toBe(-1);
        app.close();
    });

    it("reaches both ends", () => {
        const app = mount(kThree);
        app.press("End");
        expect(document.activeElement).toBe(app.rows()[2]);
        app.press("Home");
        expect(document.activeElement).toBe(app.rows()[0]);
        app.close();
    });

    it("stops at the ends rather than wrapping", () => {
        const app = mount(kThree);
        app.press("ArrowUp");
        expect(document.activeElement).toBe(app.rows()[0]);
        app.press("End");
        app.press("ArrowDown");
        expect(document.activeElement).toBe(app.rows()[2]);
        app.close();
    });

    // The page would otherwise scroll under the list, which is what makes an
    // arrow-navigable list unusable.
    it("does not let the page scroll under it", () => {
        const app = mount(kThree);
        const event = new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true, cancelable: true });
        app.list().dispatchEvent(event);
        expect(event.defaultPrevented).toBe(true);
        app.close();
    });

    it("opens the focused row on Enter and on Space", () => {
        const app = mount(kThree);
        app.press("Enter");
        app.press("ArrowDown");
        app.press(" ");
        expect(app.opened).toEqual(["a", "b"]);
        app.close();
    });

    // An arrival must not send somebody back to the top of the list they were
    // reading.
    it("keeps its place across a redraw", () => {
        const app = mount(kThree);
        app.press("ArrowDown");
        app.store.set(state({ unread: 3, items: [...kThree.items] }));
        expect(app.rows()[1]?.tabIndex).toBe(0);
        app.close();
    });

    it("pulls the stop back when the list gets shorter", () => {
        const app = mount(kThree);
        app.press("End");
        app.store.set(state({ items: [note("a", "one")] }));
        expect(app.rows()[0]?.tabIndex).toBe(0);
        app.close();
    });
});

describe("opening one", () => {
    // The store's mark-read is a no-op on a notification that is already read,
    // which is what makes it safe to fire on every open rather than tracking
    // whether it already did.
    it("marks it read", () => {
        const app = mount(kThree);
        app.rows()[0]?.click();
        expect(app.marked).toEqual([["a"]]);
        app.close();
    });

    it("is safe to do twice, because the store's mark-read is", () => {
        const app = mount(kThree);
        app.rows()[0]?.click();
        app.rows()[0]?.click();
        expect(app.marked).toEqual([["a"], ["a"]]);
        app.close();
    });

    it("does not send an id the stream never gave", () => {
        const app = mount(state({ items: [note("", "one")] }));
        app.rows()[0]?.click();
        expect(app.marked).toEqual([]);
        expect(app.opened).toEqual([""]);
        app.close();
    });
});

describe("an arrival", () => {
    it("says nothing about what was already there", () => {
        const app = mount(kThree);
        expect(app.host.querySelector(".i-live")?.textContent).toBe("");
        app.close();
    });

    it("is announced politely, without moving focus", () => {
        const app = mount(state({ unread: 1 }));
        const typing = document.createElement("input");
        document.body.append(typing);
        typing.focus();

        app.store.set(state({ unread: 4, items: [note("a", "one")] }));

        expect(app.host.querySelector(".i-live")?.textContent).toBe("3 arrived");
        expect(document.activeElement).toBe(typing);
        typing.remove();
        app.close();
    });
});

describe("closing", () => {
    it("takes the list out and stops following the store", () => {
        const app = mount(kThree);
        app.view.close();
        expect(app.host.querySelector(".i")).toBeNull();
        expect(() => app.store.set(state({ unread: 9 }))).not.toThrow();
        app.host.remove();
    });
});
