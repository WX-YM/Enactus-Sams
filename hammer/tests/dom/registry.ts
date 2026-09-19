// What the three enumerating passes iterate.
//
// The list is the reference consumer's `screenComponents`, so it is the same
// mounts an application makes rather than a set written for the suite — a
// component that only satisfies its seams under test conditions satisfies
// nothing. `coverage` below is what makes the enumeration real: it compares the
// registry against what the entry points actually EXPORT, so a component nobody
// mounted fails rather than being quietly uncovered.
//
// That is the difference `docs/15-tasks.md` §Phase 5 names — "the accessibility
// suite enumerates the components rather than being written per component" — and
// it is why a new component with no label cannot be merged.

import * as chart from "hammer/chart";
import * as dom from "hammer/dom";

import type { Consent, InboxState, PagerState } from "hammer/state";
import { SessionStore, Store } from "hammer/state";

import type { Locale } from "../testapp/api/hammer.generated.js";
import type { Alert, ScreenComponent, ScreenParts } from "../testapp/app/screen.js";
import { screenComponents } from "../testapp/app/screen.js";
import { kServerHash, kBits,} from "../support/session.js";

export function parts(): ScreenParts {
    return {
        inbox: new Store<InboxState<Alert>>({
            unread: 2,
            items: [
                { id: "a", type: "content.published", body: { headline: "One" }, read: false },
                { id: "b", type: "content.published", body: { headline: "اثنان" }, read: true },
            ],
            live: true,
        }),
        pager: new Store<PagerState<{ readonly id: string }, never>>({
            status: "ready",
            items: [{ id: "a" }],
            hasMore: true,
            error: null,
        }),
        consent: new Store<Consent>("unknown"),
        session: new SessionStore({ clientHash: kServerHash, permissionBits: kBits }),
        markRead: () => undefined,
        more: () => undefined,
        setConsent: () => undefined,
        upload: async () => undefined,
        signIn: async () => ({ ok: true, value: undefined }),
    };
}

export function components(locale: Locale = "en"): readonly ScreenComponent[] {
    return screenComponents(locale, parts());
}

// Every `render*` the entry points publish. A component is only real to a
// consumer if it is exported, so this is the list the registry has to cover.
export function published(): readonly string[] {
    const out: string[] = [];
    for (const source of [dom, chart] as readonly Record<string, unknown>[]) {
        for (const name of Object.keys(source)) {
            if (name.startsWith("render") && typeof source[name] === "function") {
                out.push(name);
            }
        }
    }
    return out.sort();
}

// A mounted component, and the host it is in. The host is returned because the
// passes below assert over the whole subtree rather than over the root alone.
export type Mounted = {
    readonly name: string;
    readonly host: HTMLElement;
    readonly close: () => void;
};

export function mountEach(doc: Document, locale: Locale = "en"): readonly Mounted[] {
    const out: Mounted[] = [];
    for (const component of components(locale)) {
        const host = doc.createElement("div");
        doc.body.append(host);
        const view = component.mount(host);
        out.push({
            name: component.name,
            host,
            close: () => {
                view.close();
                host.remove();
            },
        });
    }
    return out;
}

// Everything in a subtree that can take focus. Written out rather than taken
// from a library, because the answer has to include what this library actually
// produces and exclude what it does not.
export function focusable(root: Element): readonly HTMLElement[] {
    const out: HTMLElement[] = [];
    const candidates = root.querySelectorAll<HTMLElement>(
        "a[href], button, input, select, textarea, [tabindex]",
    );
    for (const node of candidates) {
        if (node.hasAttribute("disabled")) {
            continue;
        }
        if (node.getAttribute("tabindex") === "-1") {
            continue;
        }
        out.push(node);
    }
    return out;
}

// Whether anything between this node and the root hides it from assistive
// technology — `aria-hidden`, or `hidden`, which does the same thing by a
// different route.
export function hiddenFromReaders(node: Element, root: Element): boolean {
    let at: Element | null = node;
    while (at !== null) {
        if (at.getAttribute("aria-hidden") === "true") {
            return true;
        }
        if (at instanceof HTMLElement && at.hidden) {
            return true;
        }
        if (at === root) {
            return false;
        }
        at = at.parentElement;
    }
    return false;
}

// The accessible name of a control, by the routes this library actually uses.
export function accessibleName(node: Element, root: Element): string {
    const label = node.getAttribute("aria-label");
    if (label !== null && label.trim().length > 0) {
        return label;
    }

    const by = node.getAttribute("aria-labelledby");
    if (by !== null) {
        const named = root.querySelector(`#${by}`) ?? root.ownerDocument.getElementById(by);
        const words = named?.textContent ?? "";
        if (words.trim().length > 0) {
            return words;
        }
    }

    if (node.id.length > 0) {
        const explicit = root.ownerDocument.querySelector(`label[for="${node.id}"]`);
        const words = explicit?.textContent ?? "";
        if (words.trim().length > 0) {
            return words;
        }
    }

    const wrapping = node.closest("label")?.textContent ?? "";
    if (wrapping.trim().length > 0) {
        return wrapping;
    }

    // Named by what is inside it. A screen reader landing on a focusable row,
    // list item or cell announces its contents, so text inside one of these IS
    // the name — unlike an `<input>`, whose value is not a label and whose
    // neighbours are not either. That asymmetry is the whole reason the routes
    // above exist.
    const kNamedByContents = new Set(["BUTTON", "A", "LI", "TR", "TD", "TH", "SUMMARY", "OPTION"]);
    if (kNamedByContents.has(node.tagName)) {
        return node.textContent ?? "";
    }
    return "";
}
