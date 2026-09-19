// @vitest-environment happy-dom
//
// Every component, enumerated.
//
// This file names no component. It iterates what the reference consumer mounts
// and asserts the same things of all of them, which is the difference
// `docs/15-tasks.md` §Phase 5 asks for: "the accessibility suite enumerates the
// components rather than being written per component". A suite written per
// component tests whatever somebody remembered; this one makes a new component
// fail until it is labelled, reachable and operable.

import { afterEach, describe, expect, it } from "vitest";

import { accessibleName, components, focusable, hiddenFromReaders, mountEach, published } from "./registry.js";

let mounted: readonly { readonly close: () => void }[] = [];

afterEach(() => {
    for (const one of mounted) {
        one.close();
    }
    mounted = [];
});

function each() {
    const all = mountEach(document);
    mounted = all;
    return all;
}

describe("the enumeration itself", () => {
    // Without this the suite below is a list somebody has to remember to add to,
    // which is exactly what it exists not to be.
    it("covers every render function the entry points publish", () => {
        const names = new Set(components().map((one) => one.name.toLowerCase()));
        const missing: string[] = [];
        for (const exported of published()) {
            const bare = exported.slice("render".length).toLowerCase();
            if (!names.has(bare)) {
                missing.push(exported);
            }
        }
        expect(missing).toEqual([]);
    });

    it("has something to say about", () => {
        expect(published().length).toBeGreaterThan(0);
        expect(components().length).toBeGreaterThanOrEqual(published().length);
    });
});

describe("every component", () => {
    // An unlabelled control is a defect rather than a polish item, and one
    // shipped from a library is shipped to every consumer at once.
    it("labels every control a reader can reach", () => {
        const unlabelled: string[] = [];
        for (const one of each()) {
            for (const control of focusable(one.host)) {
                if (hiddenFromReaders(control, one.host)) {
                    continue;
                }
                if (accessibleName(control, one.host).trim().length === 0) {
                    unlabelled.push(`${one.name}: <${control.tagName.toLowerCase()}>`);
                }
            }
        }
        expect(unlabelled).toEqual([]);
    });

    // A positive tabindex reorders the whole page around the component, and the
    // page does not belong to the component.
    it("never uses a positive tabindex", () => {
        const positive: string[] = [];
        for (const one of each()) {
            for (const node of one.host.querySelectorAll("[tabindex]")) {
                const value = Number(node.getAttribute("tabindex"));
                if (value > 0) {
                    positive.push(`${one.name}: ${value}`);
                }
            }
        }
        expect(positive).toEqual([]);
    });

    // A focusable node under `aria-hidden` is reachable by Tab and invisible to
    // the thing announcing what has focus — the reader is told nothing about
    // where they are.
    it("puts nothing focusable under an aria-hidden node", () => {
        const buried: string[] = [];
        for (const one of each()) {
            for (const control of focusable(one.host)) {
                let at: Element | null = control.parentElement;
                while (at !== null && at !== one.host) {
                    if (at.getAttribute("aria-hidden") === "true") {
                        buried.push(`${one.name}: <${control.tagName.toLowerCase()}>`);
                        break;
                    }
                    at = at.parentElement;
                }
            }
        }
        expect(buried).toEqual([]);
    });

    // A list of peers, a popover and a figure each need a name; an unnamed
    // region is a box of unrelated content to anybody not looking at it.
    it("names every region, group and list it declares", () => {
        const unnamed: string[] = [];
        for (const one of each()) {
            for (const node of one.host.querySelectorAll('[role="region"], [role="group"], ul, table')) {
                if (hiddenFromReaders(node, one.host)) {
                    continue;
                }
                const named =
                    accessibleName(node, one.host).trim().length > 0 ||
                    node.querySelector("caption") !== null;
                if (!named) {
                    unnamed.push(`${one.name}: <${node.tagName.toLowerCase()}>`);
                }
            }
        }
        expect(unnamed).toEqual([]);
    });

    it("can be operated from the keyboard wherever it can be operated at all", () => {
        const pointerOnly: string[] = [];
        for (const one of each()) {
            // Anything carrying a click handler has to be reachable. A div with
            // a listener on it is the classic control nobody can tab to.
            for (const node of one.host.querySelectorAll("div[onclick], span[onclick]")) {
                pointerOnly.push(`${one.name}: <${node.tagName.toLowerCase()}>`);
            }
        }
        expect(pointerOnly).toEqual([]);
    });

    it("mounts and closes without leaving anything behind", () => {
        const host = document.createElement("div");
        document.body.append(host);
        const before = document.body.childElementCount;

        const all = mountEach(document);
        for (const one of all) {
            one.close();
        }

        expect(document.body.childElementCount).toBe(before);
        host.remove();
    });

    // Closing twice is ordinary: a gate tears a branch down while the owner is
    // also letting go of it.
    it("survives being closed twice", () => {
        const all = mountEach(document);
        for (const one of all) {
            one.close();
            expect(() => one.close()).not.toThrow();
        }
    });
});
