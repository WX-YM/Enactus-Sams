// @vitest-environment happy-dom
//
// Every component under `dir="rtl"`, enumerated.
//
// Direction is not a stylesheet concern. It is a property of the document AND of
// every element holding text, and a component that assumes left is wrong for a
// whole audience rather than slightly off for everybody (`ENGINEERING_RULES.md` §8).
//
// The words are the reference consumer's real Arabic table, not a stand-in:
// `tests/testapp/app/component_copy.ts` carries both locales because the
// application has to, and rendering the real ones is what makes this a test of
// the components rather than of a fixture.

import { afterEach, describe, expect, it } from "vitest";

import { components, mountEach } from "./registry.js";

let mounted: readonly { readonly close: () => void }[] = [];

afterEach(() => {
    for (const one of mounted) {
        one.close();
    }
    mounted = [];
    document.documentElement.dir = "";
});

function inRtl() {
    document.documentElement.dir = "rtl";
    const all = mountEach(document, "ar");
    mounted = all;
    return all;
}

describe("under a right-to-left document", () => {
    it("renders every component", () => {
        const all = inRtl();
        expect(all.length).toBeGreaterThan(0);
        for (const one of all) {
            expect(one.host.childElementCount).toBeGreaterThan(0);
        }
    });

    // Logical properties and `dir` where the text is. A physical one is the
    // layout that is correct in one direction and wrong in the other, and it is
    // exactly what a component ships to every consumer at once.
    it("sets no physical-direction style of its own", () => {
        const physical: string[] = [];
        for (const one of inRtl()) {
            for (const node of one.host.querySelectorAll<HTMLElement>("*")) {
                const style = node.getAttribute("style");
                if (style === null) {
                    continue;
                }
                if (/\b(left|right|margin-left|margin-right|padding-left|padding-right|float|text-align)\b/.test(style)) {
                    physical.push(`${one.name}: ${style}`);
                }
            }
        }
        expect(physical).toEqual([]);
    });

    // Without it an RTL name reorders the Latin sentence around it, and the
    // result is not cosmetic: it changes which words the sentence appears to
    // contain.
    it("lets text somebody typed resolve its own direction", () => {
        const undirected: string[] = [];
        for (const one of inRtl()) {
            // Every element this library fills with a value from a person or a
            // server carries `dir="auto"`. The ones it fills with the
            // application's own copy do not need to: the application chose those
            // words for this locale.
            for (const node of one.host.querySelectorAll<HTMLElement>("[dir]")) {
                if (node.dir !== "auto" && node.dir !== "rtl" && node.dir !== "ltr") {
                    undirected.push(`${one.name}: ${node.dir}`);
                }
            }
        }
        expect(undirected).toEqual([]);
    });

    it("carries dir=auto on the notification text it was handed", () => {
        for (const one of inRtl()) {
            if (one.name !== "inbox") {
                continue;
            }
            const rows = one.host.querySelectorAll<HTMLElement>("li");
            expect(rows.length).toBeGreaterThan(0);
            for (const row of rows) {
                expect(row.dir).toBe("auto");
            }
        }
    });

    it("carries dir=auto on a section's editor-authored value", () => {
        for (const one of inRtl()) {
            if (one.name !== "section") {
                continue;
            }
            for (const value of one.host.querySelectorAll<HTMLElement>(".ts-value")) {
                expect(value.dir).toBe("auto");
            }
        }
    });

    // The label list is indexed by the locale table's order, which is persisted
    // and append-only. Reading index 0 for every locale is the defect nobody
    // sees in the language they develop in.
    it("reads a section's labels at the locale's own index", () => {
        for (const one of inRtl()) {
            if (one.name !== "section") {
                continue;
            }
            const headings = Array.from(one.host.querySelectorAll(".ts-label")).map(
                (node) => node.textContent ?? "",
            );
            expect(headings.length).toBeGreaterThan(0);
            for (const heading of headings) {
                // Arabic, from the descriptor's second label. An English heading
                // here would mean index 0 was read for every locale.
                expect(heading).toMatch(/[؀-ۿ]/);
            }
        }
    });

    it("puts the application's Arabic words on screen, not a default", () => {
        const all = inRtl();
        const words = all.map((one) => one.host.textContent ?? "").join(" ");
        expect(words).toMatch(/[؀-ۿ]/);
    });

    it("mounts the same components in both directions", () => {
        expect(components("ar").map((one) => one.name)).toEqual(
            components("en").map((one) => one.name),
        );
    });
});
