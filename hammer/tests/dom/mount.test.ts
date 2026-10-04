//
// The shape every component has, driven directly.
//
// This file runs in a real Chromium page, driven by
// `tests/dom/in_browser.test.ts` rather than by a shared configuration flag or
// a per-file environment import — `tests/core/environment.test.ts` asserts
// that the default `node --test` run has no `document` in it at all, and this
// file is one of the nineteen named explicitly in the driver's own file list
// for exactly that reason: the DOM stays where the DOM is needed and nowhere
// else, and which files need it is a list a reviewer reads in one place rather
// than an import scattered across every file in the suite.
//
// What is asserted here is the part every other component in this layer inherits
// and therefore never re-asserts: a document that came from the mount, a close
// that runs once and in the right order, and user text that cannot reorder the
// sentence around it.

import { describe, expect, it } from "../support/test.js";

import {
    Closers,
    bind,
    detach,
    documentOf,
    elementIn,
    isolatedAttribute,
    setUserText,
    uniqueId,
} from "../../src/dom/mount.js";
import { Store } from "../../src/state/store.js";

const kArabicName = "محمد";

describe("the document a component builds in", () => {
    it("comes from the element it was asked to mount in", () => {
        const mount = document.createElement("div");
        expect(documentOf(mount)).toBe(document);
    });

    // The property this buys, and the reason the global is a build failure in
    // this layer: the same component renders into a document that is not the
    // tab's — a preview, a print view, a frame — with no branch for it.
    it("is the foreign one when the mount belongs to a foreign document", () => {
        // `document.implementation.createHTMLDocument` rather than
        // `DOMParser.parseFromString`: the parser is a Trusted Types sink
        // (`src/dom/sanitized.ts`'s own header, found in Phase 7) and this
        // page enforces `require-trusted-types-for 'script'`
        // (`tests/dom/in_browser.test.ts`) with no policy of ITS OWN to hand
        // it — only hammer's "hammer" policy is installed, and only for
        // hammer's own parse. Building the foreign document from empty
        // markup needs no parse at all.
        const other = document.implementation.createHTMLDocument("");
        other.body.append(other.createElement("main"));
        const mount = other.querySelector("main");
        expect(mount).not.toBeNull();
        if (mount === null) {
            return;
        }
        expect(documentOf(mount)).toBe(other);
        expect(documentOf(mount)).not.toBe(document);
        expect(elementIn(documentOf(mount), "span").ownerDocument).toBe(other);
    });
});

describe("an element", () => {
    it("carries the application's class name", () => {
        const made = elementIn(document, "button", "app-btn");
        expect(made.tagName).toBe("BUTTON");
        expect(made.className).toBe("app-btn");
    });

    // hammer ships no class name and no default for one. An empty string is the
    // application saying this part is unstyled, not a prompt to invent one.
    it("carries none where the application named none", () => {
        expect(elementIn(document, "span").getAttribute("class")).toBeNull();
        expect(elementIn(document, "span", "").getAttribute("class")).toBeNull();
    });
});

describe("text somebody typed", () => {
    // Without the isolation an RTL name reorders the Latin sentence around it,
    // and that is not cosmetic: it changes which words the sentence appears to
    // contain (`CLAUDE.md` §8).
    it("is placed with a direction resolved from the value", () => {
        const holder = elementIn(document, "span");
        setUserText(holder, kArabicName);
        expect(holder.textContent).toBe(kArabicName);
        expect(holder.dir).toBe("auto");
    });

    it("reaches the node as text and never as markup", () => {
        const holder = elementIn(document, "span");
        setUserText(holder, "<img src=x onerror=alert(1)>");
        expect(holder.children.length).toBe(0);
        expect(holder.textContent).toBe("<img src=x onerror=alert(1)>");
    });

    // An `aria-label` has no element to carry `dir`, so the isolate characters
    // travel in the value itself.
    it("carries its isolate where an attribute cannot reach", () => {
        const wrapped = isolatedAttribute(kArabicName);
        expect(wrapped).not.toBe(kArabicName);
        expect(wrapped).toContain(kArabicName);
    });

    // A string with no strong character has no direction to assert, and wrapping
    // it would spend two code points saying nothing.
    it("leaves a value with no direction alone", () => {
        expect(isolatedAttribute("123")).toBe("123");
    });
});

describe("binding a store", () => {
    it("renders what the store already holds, before anything changes", () => {
        const store = new Store(7);
        const seen: number[] = [];
        const off = bind(store, (value) => seen.push(value));
        off();
        expect(seen).toEqual([7]);
    });

    it("stops at the unsubscribe", () => {
        const store = new Store(1);
        const seen: number[] = [];
        const off = bind(store, (value) => seen.push(value));
        store.set(2);
        off();
        store.set(3);
        expect(seen).toEqual([1, 2]);
    });
});

describe("taking a node out of the tree", () => {
    it("removes it from its parent", () => {
        const host = document.createElement("div");
        const child = document.createElement("span");
        host.append(child);
        detach(child);
        expect(host.children.length).toBe(0);
    });

    it("does nothing to a node that has no parent", () => {
        expect(() => detach(document.createElement("span"))).not.toThrow();
    });

    // The reason this exists rather than `node.remove()`, asserted so the
    // workaround cannot be tidied away by somebody who does not hit it:
    // happy-dom 15 resolves a `<form>`'s parent through its form-owner and
    // throws `removeChild` at itself. A form renderer using the short spelling
    // would leave its root behind here and nowhere else.
    // A workaround this layer used to need: happy-dom 15 resolved a
    // `<form>`'s parent through its form-owner rather than through the tree,
    // and `node.remove()` threw `removeChild` at itself as a result — a fake
    // DOM's bug, not a real one, and gone now that `detach` is checked
    // against Chromium rather than against a stand-in (`docs/15-tasks.md`
    // Phase 8 B2). What is left worth asserting is the ordinary case: a
    // `<form>` is a node like any other node, and taking it out of the tree
    // does not need a special case.
    it("removes a form like any other node", () => {
        const host = document.createElement("div");
        const form = document.createElement("form");
        host.append(form);
        detach(form);
        expect(host.children.length).toBe(0);
    });
});

describe("closing", () => {
    it("releases the last thing built first", () => {
        const order: string[] = [];
        const closers = new Closers();
        closers.add(() => order.push("resource"));
        closers.add(() => order.push("subscription"));
        closers.run();
        expect(order).toEqual(["subscription", "resource"]);
    });

    // A caller that closes twice must not run a cleanup a second time, and a
    // component closed from inside its own subscriber — a session gate tearing
    // down on the logout it just heard about — is ordinary rather than exotic.
    it("runs once however many times it is asked", () => {
        let ran = 0;
        const closers = new Closers();
        closers.add(() => {
            ran += 1;
        });
        closers.run();
        closers.run();
        expect(ran).toBe(1);
    });

    it("runs a cleanup added during teardown rather than holding it", () => {
        let late = 0;
        const closers = new Closers();
        closers.run();
        closers.add(() => {
            late += 1;
        });
        expect(late).toBe(1);
    });

    // One listener that throws on the way out must not strand the resource
    // handle behind it, which would be a request outliving the screen.
    it("keeps going past one that throws", () => {
        const order: string[] = [];
        const closers = new Closers();
        closers.add(() => order.push("resource"));
        closers.add(() => {
            throw new Error("a subscriber refused to let go");
        });
        closers.run();
        expect(order).toEqual(["resource"]);
    });
});

// The id minter every labelled control in this layer depends on.
//
// An unlabelled control is a defect rather than a polish item (`CLAUDE.md` §9),
// and a label is tied to its control by an id — so a collision is not a cosmetic
// problem: two elements with one id means `aria-describedby` points at the wrong
// text, and a screen reader announces one field's error on another field.
describe("the id a label is tied to a control by", () => {
    it("never repeats, and carries the stem it was given", () => {
        const first = uniqueId(document, "hammer-field");
        const second = uniqueId(document, "hammer-field");

        expect(first).not.toBe(second);
        expect(first.startsWith("hammer-field-")).toBe(true);
        expect(second.startsWith("hammer-field-")).toBe(true);
    });

    // The half a counter cannot do on its own. The counter knows what THIS
    // library has minted; the document knows what the application put there
    // before the component mounted, and a server-rendered page is full of ids
    // nothing here chose.
    it("steps over an id the application already used", () => {
        const taken = document.createElement("div");
        const before = uniqueId(document, "hammer-taken");
        // Reserve the id the counter would hand out next.
        const next = `hammer-taken-${Number(before.slice("hammer-taken-".length)) + 1}`;
        taken.id = next;
        document.body.append(taken);

        try {
            expect(uniqueId(document, "hammer-taken")).not.toBe(next);
            expect(document.getElementById(next)).toBe(taken);
        } finally {
            detach(taken);
        }
    });

    // A counter and not `crypto.getRandomValues`, and the distinction is the one
    // §5's rule is actually about: that rule governs a value whose
    // UNPREDICTABILITY is the control. This one sits in the markup, readable by
    // anyone who can read the markup at all, so a random one would buy nothing
    // and cost a CSPRNG call per field.
    it("is not built from randomness, which would buy nothing here", () => {
        const ids = [uniqueId(document, "s"), uniqueId(document, "s"), uniqueId(document, "s")];
        const numbers = ids.map((id) => Number(id.slice("s-".length)));
        expect(numbers[1]).toBe((numbers[0] ?? 0) + 1);
        expect(numbers[2]).toBe((numbers[1] ?? 0) + 1);
    });
});
