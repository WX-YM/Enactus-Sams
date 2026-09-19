// The inbox list: read and unread, a keyboard that can walk it, and a live
// region for what arrives while somebody is reading.
//
// The bell is a badge and a popover; this is the surface somebody actually goes
// to. What it owns that a list of divs does not: a roving tab stop, so the list
// is one stop in the page's tab order rather than one per row, and arrow keys
// inside it.
//
// --- one tab stop, not one per row ------------------------------------------
//
// A hundred notifications must not be a hundred presses of Tab to get past. The
// pattern is the platform's own for a list of peers: exactly one row is
// focusable at a time, arrows move both the focus and which row that is, and
// Home and End reach the ends. `tabindex` is 0 on one row and -1 on the rest,
// and never a positive number — a positive `tabindex` reorders the whole page
// around this component (`ENGINEERING_RULES.md` §9).
//
// --- opening a notification marks it read, and that is idempotent -----------
//
// The stream is at-least-once and the mark-read is retryable, so marking a read
// notification read again is a no-op in the store (`state/inbox.ts`). That is
// what makes it safe for this to fire on every open rather than tracking whether
// it already did.

import type { ClassNames } from "../core/tables.js";

import type { InboxState, Notification } from "../state/inbox.js";
import type { Readable } from "../state/store.js";

import { Closers, bind, detach, documentOf, elementIn, setUserText } from "./mount.js";
import type { Mounted } from "./mount.js";

export type InboxPart = "root" | "list" | "item" | "unreadItem" | "empty" | "live";

export type InboxCopy = {
    // Names the list. A list of peers with no accessible name is a box of
    // unrelated text to anybody not looking at it.
    readonly label: string;

    readonly arrived: (count: number) => string;
    readonly empty: string;
};

export type InboxOptions<T> = {
    readonly inbox: Readable<InboxState<T>>;
    readonly markRead: (ids: readonly string[]) => void;
    readonly describe: (item: Notification<T>) => string;

    // What this product does when a notification is opened. Navigating
    // somewhere is the application's decision, and so is whether there is
    // anywhere to go.
    readonly onOpen?: (item: Notification<T>) => void;

    readonly classes: ClassNames<InboxPart>;
    readonly copy: InboxCopy;
};

export function renderInbox<T>(mount: Element, options: InboxOptions<T>): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes, copy } = options;

    const root = elementIn(doc, "div", classes.root);

    const list = elementIn(doc, "ul", classes.list);
    list.setAttribute("aria-label", copy.label);

    const empty = elementIn(doc, "p", classes.empty);
    empty.textContent = copy.empty;

    const live = elementIn(doc, "div", classes.live);
    live.setAttribute("aria-live", "polite");
    live.setAttribute("aria-atomic", "true");

    root.append(list, empty, live);

    let rows: HTMLLIElement[] = [];
    // Which row holds the list's single tab stop. Kept across a redraw so an
    // arrival does not send somebody back to the top of the list.
    let at = 0;

    const focusRow = (next: number): void => {
        if (rows.length === 0) {
            return;
        }
        at = Math.max(0, Math.min(next, rows.length - 1));
        for (let i = 0; i < rows.length; i += 1) {
            const row = rows[i];
            if (row !== undefined) {
                row.tabIndex = i === at ? 0 : -1;
            }
        }
        rows[at]?.focus();
    };

    const open = (item: Notification<T>): void => {
        // Safe to fire whether or not it is already read: the store's mark-read
        // is a no-op on a read notification, which is what makes the whole path
        // idempotent.
        if (item.id.length > 0) {
            options.markRead([item.id]);
        }
        options.onOpen?.(item);
    };

    const onKeyDown = (event: KeyboardEvent): void => {
        const moves: Readonly<Record<string, number>> = {
            ArrowDown: at + 1,
            ArrowUp: at - 1,
            Home: 0,
            End: rows.length - 1,
        };
        if (Object.prototype.hasOwnProperty.call(moves, event.key)) {
            // The page would otherwise scroll under the list, which is the
            // defect that makes an arrow-navigable list unusable.
            event.preventDefault();
            focusRow(moves[event.key] ?? at);
            return;
        }
        if (event.key === "Enter" || event.key === " ") {
            const item = options.inbox.get().items[at];
            if (item !== undefined) {
                event.preventDefault();
                open(item);
            }
        }
    };
    list.addEventListener("keydown", onKeyDown);
    closers.add(() => list.removeEventListener("keydown", onKeyDown));

    let announced: number | null = null;

    const draw = (state: InboxState<T>): void => {
        while (list.firstChild !== null) {
            list.removeChild(list.firstChild);
        }
        rows = [];

        for (let i = 0; i < state.items.length; i += 1) {
            const item = state.items[i];
            if (item === undefined) {
                continue;
            }
            const row = elementIn(doc, "li", item.read ? classes.item : classes.unreadItem);
            // Text, never markup: a notification body is a server value.
            setUserText(row, options.describe(item));
            // One stop for the whole list, and never a positive number.
            row.tabIndex = i === at ? 0 : -1;
            // Read state is said, not only shown: a colour is not available to
            // everybody and is the application's anyway.
            row.setAttribute("aria-current", item.read ? "false" : "true");

            const onClick = (): void => open(item);
            row.addEventListener("click", onClick);
            closers.add(() => row.removeEventListener("click", onClick));

            rows.push(row);
            list.append(row);
        }

        if (at >= rows.length) {
            at = Math.max(0, rows.length - 1);
            const row = rows[at];
            if (row !== undefined) {
                row.tabIndex = 0;
            }
        }

        empty.hidden = state.items.length > 0;
        list.hidden = state.items.length === 0;

        const arrivals = announced === null ? 0 : state.unread - announced;
        announced = state.unread;
        if (arrivals > 0) {
            live.textContent = copy.arrived(arrivals);
        }
    };

    closers.add(bind(options.inbox, draw));

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
