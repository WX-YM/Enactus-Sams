// Cursor paging controls. There are no page numbers here, and there is no way
// to add one.
//
// `skip(n)` is O(n) server-side, so anvil does not offer an offset and hammer
// has no way to express one — `core/cursor.ts` carries no field that is a
// position and no function that takes a number as one (`CLAUDE.md` §7). What
// follows from that is the whole shape of this control: there is a "more", there
// is no "page 7", and there is no total, because a total is a `COUNT(*)` over
// the same table the cursor exists to avoid walking.
//
// --- `hasMore` is the server's answer ---------------------------------------
//
// Never `items.length === limit`. That guess is wrong in both directions: a page
// that happens to land exactly on the boundary shows a control that fetches
// nothing, and a server that returned a short page because a filter removed rows
// hides a control that would have found more. The store reads it off the
// cursor the server sent (`state/paginate.ts`).

import type { ClassNames } from "../core/tables.js";

import type { PagerState } from "../state/paginate.js";
import type { Readable } from "../state/store.js";

import { Closers, bind, detach, documentOf, elementIn } from "./mount.js";
import type { Mounted } from "./mount.js";

export type PagerPart = "root" | "more" | "busy" | "error";

export type PagerCopy = {
    readonly more: string;

    // Said while a page is on its way, in a polite region. A control that
    // silently does nothing for two seconds on a slow connection is a control
    // people press again.
    readonly loading: string;

    // What to say when a page did not arrive. Keyed by nothing: the pager's
    // failures are the transport's, and the application decides how much of that
    // to explain here versus in its own error surface (`dom/error.ts`).
    readonly failed: string;

    readonly retry: string;
};

export type PagerOptions<Row, E> = {
    readonly pager: Readable<PagerState<Row, E>>;

    // Concurrent callers join the one in flight (`state/paginate.ts`), so a
    // double press is one page rather than two.
    readonly more: () => void;

    readonly classes: ClassNames<PagerPart>;
    readonly copy: PagerCopy;
};

export function renderPager<Row, E>(mount: Element, options: PagerOptions<Row, E>): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes, copy } = options;

    const root = elementIn(doc, "div", classes.root);

    const more = elementIn(doc, "button", classes.more);
    more.type = "button";
    more.textContent = copy.more;

    const busy = elementIn(doc, "p", classes.busy);
    busy.setAttribute("aria-live", "polite");

    const failure = elementIn(doc, "p", classes.error);
    failure.role = "alert";

    root.append(more, busy, failure);

    const onMore = (): void => options.more();
    more.addEventListener("click", onMore);
    closers.add(() => more.removeEventListener("click", onMore));

    const draw = (state: PagerState<Row, E>): void => {
        const loading = state.status === "loading";

        // Hidden when there is nothing further, because the server said so.
        // A failed page keeps its control: the rows it already has are still
        // there and the next attempt is the point of the button.
        const offer = state.hasMore || state.status === "failed";
        more.hidden = !offer;
        more.disabled = loading;
        more.textContent = state.status === "failed" ? copy.retry : copy.more;

        // Said, not only spun. A control that silently does nothing for two
        // seconds is a control people press again.
        busy.textContent = loading ? copy.loading : "";
        more.setAttribute("aria-busy", loading ? "true" : "false");

        failure.textContent = state.status === "failed" ? copy.failed : "";
    };

    closers.add(bind(options.pager, draw));

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
