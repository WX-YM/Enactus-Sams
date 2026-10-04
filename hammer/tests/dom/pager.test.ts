//
// The paging control. What is asserted is mostly what it CANNOT do: there is no
// page number, no total and no offset, because the cursor model has no way to
// express one and `core/cursor.ts` has no field that is a position.

import { describe, expect, it } from "../support/test.js";

import type { ClassNames } from "../../src/core/tables.js";
import { renderPager } from "../../src/dom/pager.js";
import type { PagerCopy, PagerPart } from "../../src/dom/pager.js";
import type { PagerState } from "../../src/state/paginate.js";
import { Store } from "../../src/state/store.js";

type Row = { readonly id: string };
type Failure = "unreadable-page";

const kClasses: ClassNames<PagerPart> = {
    root: "p",
    more: "p-more",
    busy: "p-busy",
    error: "p-error",
};

const kCopy: PagerCopy = {
    more: "Show more",
    loading: "Loading",
    failed: "That did not load.",
    retry: "Try again",
};

const kRows: readonly Row[] = [{ id: "a" }, { id: "b" }];

function ready(hasMore: boolean): PagerState<Row, Failure> {
    return { status: "ready", items: kRows, hasMore, error: null };
}

function mount(initial: PagerState<Row, Failure>) {
    const store = new Store<PagerState<Row, Failure>>(initial);
    let asked = 0;
    const host = document.createElement("div");
    document.body.append(host);
    const view = renderPager(host, {
        pager: store,
        more: () => {
            asked += 1;
        },
        classes: kClasses,
        copy: kCopy,
    });
    const find = <E extends Element>(s: string): E => {
        const f = host.querySelector<E>(s);
        if (f === null) {
            throw new Error(`the pager did not draw ${s}`);
        }
        return f;
    };
    return {
        store,
        host,
        view,
        find,
        asked: () => asked,
        more: find<HTMLButtonElement>(".p-more"),
        close: () => {
            view.close();
            host.remove();
        },
    };
}

describe("what it will not render", () => {
    // `skip(n)` is O(n) server-side, so there is no offset to render a page
    // number from and no total to render a count from.
    it("shows no page number and no total", () => {
        const app = mount(ready(true));
        const words = app.host.textContent ?? "";
        expect(words).not.toMatch(/\d/);
        app.close();
    });
});

describe("the control", () => {
    // Never `items.length === limit`. That guess is wrong in both directions.
    it("offers more only while the server says there is more", () => {
        const app = mount(ready(true));
        expect(app.more.hidden).toBe(false);

        app.store.set(ready(false));
        expect(app.more.hidden).toBe(true);
        app.close();
    });

    it("asks for the next page when pressed", () => {
        const app = mount(ready(true));
        app.more.click();
        expect(app.asked()).toBe(1);
        app.close();
    });

    // Concurrent callers join the one in flight, so a double press is one page
    // rather than two — and the control says it is busy rather than looking
    // broken.
    it("cannot be pressed while a page is on its way", () => {
        const app = mount({ status: "loading", items: kRows, hasMore: true, error: null });
        expect(app.more.disabled).toBe(true);
        expect(app.more.getAttribute("aria-busy")).toBe("true");
        app.close();
    });

    // A control that silently does nothing for two seconds on a slow connection
    // is a control people press again.
    it("says it is loading, politely", () => {
        const app = mount(ready(true));
        expect(app.find(".p-busy").textContent).toBe("");

        app.store.set({ status: "loading", items: kRows, hasMore: true, error: null });
        expect(app.find(".p-busy").getAttribute("aria-live")).toBe("polite");
        expect(app.find(".p-busy").textContent).toBe("Loading");
        app.close();
    });
});

describe("a page that did not arrive", () => {
    const failed: PagerState<Row, Failure> = {
        status: "failed",
        items: kRows,
        hasMore: true,
        error: "unreadable-page",
    };

    it("says so, and announces it", () => {
        const app = mount(failed);
        expect(app.find(".p-error").getAttribute("role")).toBe("alert");
        expect(app.find(".p-error").textContent).toBe("That did not load.");
        app.close();
    });

    // The rows it already has are still there, and the next attempt is the whole
    // point of the button.
    it("keeps the control, as a retry", () => {
        const app = mount(failed);
        expect(app.more.hidden).toBe(false);
        expect(app.more.textContent).toBe("Try again");
        app.close();
    });

    it("offers the retry even where the server had said there was no more", () => {
        const app = mount({ ...failed, hasMore: false });
        expect(app.more.hidden).toBe(false);
        app.close();
    });

    it("clears the failure when a page does arrive", () => {
        const app = mount(failed);
        app.store.set(ready(true));
        expect(app.find(".p-error").textContent).toBe("");
        expect(app.more.textContent).toBe("Show more");
        app.close();
    });
});

describe("closing", () => {
    it("takes the control out and stops following the store", () => {
        const app = mount(ready(true));
        app.view.close();
        expect(app.host.querySelector(".p")).toBeNull();
        expect(() => app.store.set(ready(false))).not.toThrow();
        app.host.remove();
    });
});
