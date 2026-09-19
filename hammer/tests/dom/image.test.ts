// @vitest-environment happy-dom
//
// The image element. Short, because the implementation is: an image is an
// `<img>` and this module sets its attributes.
//
// The property that matters most is not asserted here and cannot be —
// `tools/check-source-bans.sh` is what proves no module in this library reaches
// `createObjectURL`, `readAsDataURL` or a `fetch` for image bytes. A test can
// only speak for one module; the ban speaks for all of them.

import { describe, expect, it } from "vitest";

import { stripBidiControls } from "../../src/core/bidi.js";
import type { ClassNames } from "../../src/core/tables.js";
import { renderImage } from "../../src/dom/image.js";
import type { ImagePart } from "../../src/dom/image.js";
import type { ImageSources } from "../../src/state/media.js";

const kClasses: ClassNames<ImagePart> = { root: "img" };

const kSources: ImageSources = {
    src: "https://media.test/a/1/card",
    srcset: "https://media.test/a/1/thumb 320w, https://media.test/a/1/card 1024w",
    widths: [320, 1024],
};

function mount(over: Partial<Parameters<typeof renderImage>[1]> = {}) {
    const host = document.createElement("div");
    document.body.append(host);
    const view = renderImage(host, {
        sources: kSources,
        alt: "A harbour at dawn",
        width: 1024,
        height: 768,
        classes: kClasses,
        ...over,
    });
    const image = view.element as HTMLImageElement;
    return { host, view, image, close: () => { view.close(); host.remove(); } };
}

describe("the sources", () => {
    it("carries the address and the set the media table built", () => {
        const app = mount();
        expect(app.image.getAttribute("src")).toBe(kSources.src);
        expect(app.image.getAttribute("srcset")).toBe(kSources.srcset);
        app.close();
    });

    // Without a width descriptor the browser has no basis on which to choose
    // between the sources it is handed, which is why the width is published
    // beside the role at all.
    it("keeps the width descriptors, which are what make a set choosable", () => {
        const app = mount();
        expect(app.image.getAttribute("srcset")).toContain("320w");
        expect(app.image.getAttribute("srcset")).toContain("1024w");
        app.close();
    });

    it("sets no set at all where the namespace had one role", () => {
        const app = mount({ sources: { src: "https://media.test/a/1/full", srcset: "", widths: [] } });
        expect(app.image.getAttribute("srcset")).toBeNull();
        app.close();
    });

    // A layout fact, and the layout is the application's. Without it the browser
    // assumes the full viewport and picks the largest source every time.
    it("takes the layout width from the application, and invents none", () => {
        expect(mount().image.getAttribute("sizes")).toBeNull();
        const app = mount({ sizes: "(min-width: 40em) 33vw, 100vw" });
        expect(app.image.getAttribute("sizes")).toBe("(min-width: 40em) 33vw, 100vw");
        app.close();
    });
});

// Without both, the browser has no aspect ratio until the bytes arrive, and
// every image is a layout shift: the thing somebody was about to tap moves out
// from under their finger.
describe("the intrinsic box", () => {
    it("carries both dimensions", () => {
        const app = mount();
        expect(app.image.getAttribute("width")).toBe("1024");
        expect(app.image.getAttribute("height")).toBe("768");
        app.close();
    });
});

describe("the words", () => {
    it("labels the image", () => {
        expect(mount().image.getAttribute("alt")).toContain("A harbour at dawn");
    });

    // The case people forget: when the image does not load, the browser renders
    // this text inline in the page flow. That is user text interpolated into a
    // sentence, and it carries its own direction or it reorders the words
    // around it.
    it("isolates the text it will be rendered inline as", () => {
        const read = mount().image.getAttribute("alt") ?? "";
        expect(read).not.toBe("A harbour at dawn");
        expect(stripBidiControls(read)).toBe("A harbour at dawn");
    });

    // An image that carries no information a reader would otherwise miss. An
    // empty string is a real answer; leaving it out is not available.
    it("takes an empty answer for a decorative one", () => {
        const app = mount({ alt: "" });
        expect(app.image.getAttribute("alt")).toBe("");
        app.close();
    });

    it("isolates a name in another script", () => {
        const app = mount({ alt: "ميناء" });
        const read = app.image.getAttribute("alt") ?? "";
        expect(read).not.toBe("ميناء");
        expect(stripBidiControls(read)).toBe("ميناء");
        app.close();
    });
});

describe("how it loads", () => {
    it("defers and decodes off the main thread unless told otherwise", () => {
        const app = mount();
        expect(app.image.getAttribute("loading")).toBe("lazy");
        expect(app.image.getAttribute("decoding")).toBe("async");
        app.close();
    });

    // The image at the top of the page is never worth deferring, and which one
    // that is depends on where the application puts it.
    it("lets the application say when it is the one at the top", () => {
        const app = mount({ loading: "eager" });
        expect(app.image.getAttribute("loading")).toBe("eager");
        app.close();
    });
});

describe("closing", () => {
    it("takes the image out of the document", () => {
        const app = mount();
        app.view.close();
        expect(app.host.querySelector("img")).toBeNull();
        app.host.remove();
    });
});
