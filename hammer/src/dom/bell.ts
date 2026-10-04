// The notification bell.
//
// A bell is not a widget. It is an unread count reconciled against an
// at-least-once stream and against the server's own count, a popover with focus
// management and a return path, a mark-read that is idempotent because the
// stream is, and a live region that announces an arrival without stealing focus
// from whatever the person was typing. Every application built on anvil needs
// exactly that, and every one of them gets some part of it wrong the first time
// (`CLAUDE.md` §9).
//
// What it does NOT own is the word for "notifications", which topics deserve a
// badge, or what "read" means in this product.
//
// --- the count is the server's ----------------------------------------------
//
// The badge renders `state.unread` and never a tally of the items on screen.
// Those are two different numbers and the difference is the point: the store
// holds a bounded ring of the most recent notifications, so counting them would
// show a badge that stops climbing at whatever the ring holds. The stream
// adjusts the count so a badge appears on arrival, and the next server answer
// replaces that adjustment outright (`docs/01-seams.md` §18).
//
// --- an arrival announces, and does not interrupt ---------------------------
//
// Arrivals are announced in a polite live region. Never `assertive`, never a
// focus move: an arrival that steals focus interrupts whatever the person was
// typing, and that is a worse defect than a missed notification. Opening the
// popover DOES move focus, because that is a thing the person just asked for.
//
// --- an id is not a key -----------------------------------------------------
//
// `Notification.id` is `""` for an event that carried none, and the store says
// it is the one field a caller must not treat as unique. Nothing here keys a
// node by it.

import type { ClassNames } from "../core/tables.js";

import type { InboxState, Notification } from "../state/inbox.js";
import type { Readable } from "../state/store.js";

import { Closers, bind, detach, documentOf, elementIn, setUserText, uniqueId } from "./mount.js";
import type { Mounted } from "./mount.js";

export type BellPart =
    | "root"
    | "trigger"
    | "badge"
    | "popover"
    | "list"
    | "item"
    | "unreadItem"
    | "empty"
    | "markRead"
    | "live";

export type BellCopy = {
    // Names the trigger. Without it the control is a button with a picture in
    // it, and a picture has no accessible name.
    readonly label: string;

    // What the badge means, for the people who are not looking at it. The count
    // is a parameter because a plural rule and a digit shape belong to the
    // locale rather than to the number (`CLAUDE.md` §8).
    readonly unread: (count: number) => string;

    // Announced when something arrives.
    readonly arrived: (count: number) => string;

    readonly empty: string;
    readonly markRead: string;
};

export type BellOptions<T> = {
    readonly inbox: Readable<InboxState<T>>;

    // One idempotent, versioned call, wired by the application: marking a read
    // notification read again is a no-op in the store, so a retry cannot
    // double-count (`state/versioned.ts`). The route it goes to is the
    // application's, and this layer may not import `hammer/wire` anyway.
    readonly markRead: (ids: readonly string[]) => void;

    // The words for one notification. A topic name and the sentence about it are
    // the application's (`CLAUDE.md` §9); what this ships is that they end up in
    // a list somebody can walk.
    readonly describe: (item: Notification<T>) => string;

    readonly classes: ClassNames<BellPart>;
    readonly copy: BellCopy;
};

export function renderBell<T>(mount: Element, options: BellOptions<T>): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes, copy } = options;

    const root = elementIn(doc, "div", classes.root);

    const popoverId = uniqueId(doc, "hammer-bell");
    const trigger = elementIn(doc, "button", classes.trigger);
    trigger.type = "button";
    trigger.setAttribute("aria-haspopup", "true");
    trigger.setAttribute("aria-expanded", "false");
    trigger.setAttribute("aria-controls", popoverId);

    const badge = elementIn(doc, "span", classes.badge);
    // The number is decoration for anybody reading it: the accessible name on
    // the trigger already says what it means, and a screen reader announcing the
    // digit twice is worse than not announcing it at all.
    badge.setAttribute("aria-hidden", "true");
    trigger.append(badge);

    const popover = elementIn(doc, "div", classes.popover);
    popover.id = popoverId;
    popover.hidden = true;
    popover.setAttribute("aria-label", copy.label);
    popover.tabIndex = -1;

    const list = elementIn(doc, "ul", classes.list);
    popover.append(list);

    const empty = elementIn(doc, "p", classes.empty);
    empty.textContent = copy.empty;
    popover.append(empty);

    const markRead = elementIn(doc, "button", classes.markRead);
    markRead.type = "button";
    markRead.textContent = copy.markRead;
    popover.append(markRead);

    // Polite and atomic. It holds no markup and is never focused; it exists so
    // an arrival is heard by somebody who is not looking at the bell.
    const live = elementIn(doc, "div", classes.live);
    live.setAttribute("aria-live", "polite");
    live.setAttribute("aria-atomic", "true");

    root.append(trigger, popover, live);

    let open = false;
    // What had focus before the popover opened, so it can be given back. Without
    // it, closing leaves focus on the document body and a keyboard user is
    // returned to the top of the page.
    let returnTo: Element | null = null;

    const setOpen = (next: boolean): void => {
        if (open === next) {
            return;
        }
        open = next;

        // Read before `hidden` is set, not after: a real browser blurs an
        // element the instant it is hidden, moving focus to the document
        // body SYNCHRONOUSLY and before this function's next line ever runs.
        // Checking `doc.activeElement` after `popover.hidden = true` was
        // reading the browser's own post-blur state, which is never inside
        // the popover — so the restore below never ran, silently, and focus
        // was left on the body every time. happy-dom does not blur a hidden
        // element, so this was invisible to the whole suite until it ran
        // against Chromium (`docs/15-tasks.md` Phase 8 B2).
        const wasFocusedInPopover = !next && popover.contains(doc.activeElement);

        popover.hidden = !next;
        trigger.setAttribute("aria-expanded", next ? "true" : "false");

        if (next) {
            returnTo = doc.activeElement;
            popover.focus();
            return;
        }

        // Only where focus was still inside the popover. If the person had
        // already clicked into something else, taking focus back would be the
        // theft this whole path exists to avoid.
        if (wasFocusedInPopover) {
            const back = returnTo;
            if (back !== null && "focus" in back && typeof back.focus === "function") {
                (back as HTMLElement).focus();
            } else {
                trigger.focus();
            }
        }
        returnTo = null;
    };

    const onTrigger = (): void => setOpen(!open);
    trigger.addEventListener("click", onTrigger);
    closers.add(() => trigger.removeEventListener("click", onTrigger));

    const onKeyDown = (event: KeyboardEvent): void => {
        if (event.key === "Escape" && open) {
            // Not a trap: Escape always closes and always hands focus back
            // (`CLAUDE.md` §9).
            event.stopPropagation();
            setOpen(false);
        }
    };
    root.addEventListener("keydown", onKeyDown);
    closers.add(() => root.removeEventListener("keydown", onKeyDown));

    // A click anywhere else closes it. The listener is on the document this
    // component was mounted in, and it comes off on close — a library adding a
    // listener to a document it did not create is a listener nobody can remove
    // (`docs/01-seams.md` §18), so this one is removed by the handle that
    // created it.
    const onElsewhere = (event: Event): void => {
        const target = event.target;
        if (open && target instanceof Node && !root.contains(target)) {
            setOpen(false);
        }
    };
    doc.addEventListener("click", onElsewhere, true);
    closers.add(() => doc.removeEventListener("click", onElsewhere, true));

    const onMarkRead = (): void => {
        const ids: string[] = [];
        for (const item of options.inbox.get().items) {
            // An empty id is an event that carried none, and the store says it
            // is not unique. Sending it would ask the server to mark whatever
            // else shares it.
            if (!item.read && item.id.length > 0) {
                ids.push(item.id);
            }
        }
        if (ids.length > 0) {
            options.markRead(ids);
        }
    };
    markRead.addEventListener("click", onMarkRead);
    closers.add(() => markRead.removeEventListener("click", onMarkRead));

    // Not initialised from the first state. Announcing on mount would read out a
    // backlog to somebody who has just arrived on the page.
    let announced: number | null = null;

    const draw = (state: InboxState<T>): void => {
        // The server's number, not a tally of what is on screen: the store holds
        // a bounded ring, so counting it would show a badge that stops climbing.
        badge.textContent = state.unread > 0 ? String(state.unread) : "";
        trigger.setAttribute("aria-label", copy.unread(state.unread));

        while (list.firstChild !== null) {
            list.removeChild(list.firstChild);
        }
        for (const item of state.items) {
            const row = elementIn(doc, "li", item.read ? classes.item : classes.unreadItem);
            // Text, never markup. A server value rendered as markup is the
            // defect the one insertion site exists to make unspellable.
            setUserText(row, options.describe(item));
            list.append(row);
        }
        empty.hidden = state.items.length > 0;

        const arrivals = announced === null ? 0 : state.unread - announced;
        announced = state.unread;
        if (arrivals > 0) {
            // Text into a polite region. No focus move: an arrival that stole
            // focus would interrupt whatever the person was typing.
            live.textContent = copy.arrived(arrivals);
        }
    };

    closers.add(bind(options.inbox, draw));

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
