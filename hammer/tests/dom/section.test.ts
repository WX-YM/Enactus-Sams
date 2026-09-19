// @vitest-environment happy-dom
//
// The section renderer, and the one thing that makes it more than a list of
// values: one of those values is markup, and it reaches the DOM through the
// single insertion site or it does not reach it at all.

import { describe, expect, it } from "vitest";

import type { ClassNames } from "../../src/core/tables.js";
import { renderSection } from "../../src/dom/section.js";
import type { SectionFieldView, SectionPart } from "../../src/dom/section.js";

const kClasses: ClassNames<SectionPart> = {
    root: "s",
    field: "s-field",
    label: "s-label",
    value: "s-value",
};

// Two locales, in the server's order. Index 0 means nothing in particular.
const kTitle: SectionFieldView = { key: "title", labels: ["Title", "العنوان"], rich: false };
const kBody: SectionFieldView = { key: "body", labels: ["Body", "المحتوى"], rich: true };

function mount(
    fields: readonly SectionFieldView[],
    values: Readonly<Record<string, string>>,
    localeIndex = 0,
) {
    const host = document.createElement("div");
    document.body.append(host);
    const view = renderSection(host, { fields, values, localeIndex, classes: kClasses });
    return {
        host,
        view,
        close: () => {
            view.close();
            host.remove();
        },
    };
}

describe("what it draws", () => {
    it("labels each value in the locale being read", () => {
        const app = mount([kTitle], { title: "Home" });
        expect(app.host.querySelector(".s-label")?.textContent).toBe("Title");
        expect(app.host.querySelector(".s-value")?.textContent).toBe("Home");
        app.close();
    });

    it("reads the label list by the server's own order", () => {
        const app = mount([kTitle], { title: "Home" }, 1);
        expect(app.host.querySelector(".s-label")?.textContent).toBe("العنوان");
        app.close();
    });

    // Editor-authored, in whatever language this locale is, so it resolves its
    // own direction rather than inheriting the page's.
    it("lets a label and a value resolve their own direction", () => {
        const app = mount([kTitle], { title: "مرحبا" }, 1);
        expect(app.host.querySelector<HTMLElement>(".s-label")?.dir).toBe("auto");
        expect(app.host.querySelector<HTMLElement>(".s-value")?.dir).toBe("auto");
        app.close();
    });

    it("draws the field with no value as empty rather than omitting it", () => {
        const app = mount([kTitle, kBody], { title: "Home" });
        expect(app.host.querySelectorAll(".s-field").length).toBe(2);
        app.close();
    });
});

// `labelAt` answers null rather than substituting, and this honours it: a
// heading in a language nobody chose is the failure that reads as a translation
// somebody wrote and nobody can find.
describe("a label list that is short", () => {
    it("renders the value with no heading rather than another locale's words", () => {
        const short: SectionFieldView = { key: "title", labels: ["Title"], rich: false };
        const app = mount([short], { title: "Home" }, 1);
        expect(app.host.querySelector(".s-label")).toBeNull();
        expect(app.host.querySelector(".s-value")?.textContent).toBe("Home");
        app.close();
    });
});

describe("rich text", () => {
    it("keeps the structure an editor wrote", () => {
        const app = mount([kBody], { body: "<p>a <strong>bold</strong> word</p>" });
        expect(app.host.querySelector(".s-value strong")?.textContent).toBe("bold");
        app.close();
    });

    // The pass runs on every render and not once at the edge: a value arriving
    // here came from a cache, a broadcast or a replayed event, none of which a
    // server render touched.
    it("goes through the pass on the way in", () => {
        const app = mount([kBody], { body: '<p onclick="x()">a</p><style>p{color:red}</style>' });
        const value = app.host.querySelector(".s-value");
        expect(value?.querySelector("style")).toBeNull();
        expect(value?.querySelector("p")?.getAttribute("onclick")).toBeNull();
        expect(value?.textContent).toBe("a");
        app.close();
    });

    // The difference between the two kinds of field, and the whole reason one
    // of them is marked.
    it("is text and not markup for a field that is not rich", () => {
        const app = mount([kTitle], { title: "<strong>x</strong>" });
        const value = app.host.querySelector(".s-value");
        expect(value?.querySelector("strong")).toBeNull();
        expect(value?.textContent).toBe("<strong>x</strong>");
        app.close();
    });
});

describe("closing", () => {
    it("takes what it drew out of the document", () => {
        const app = mount([kTitle], { title: "Home" });
        app.view.close();
        expect(app.host.querySelector(".s")).toBeNull();
        app.host.remove();
    });
});
