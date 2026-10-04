//
// The image editor in a real document: what it draws, and that every way in —
// the toolbar, the keyboard, the pointer — ends in the recipe and nowhere else.

import { describe, expect, it } from "../support/test.js";

import type { ClassNames } from "../../src/core/tables.js";
import { renderImageEditor } from "../../src/edit/editor.js";
import type { EditorCopy, EditorOptions, EditorPart } from "../../src/edit/editor.js";
import { kFixedOne } from "../../src/edit/recipe.js";

const kClasses: ClassNames<EditorPart> = {
    root: "e",
    toolbar: "e-toolbar",
    button: "e-button",
    canvas: "e-canvas",
    shade: "e-shade",
    cropBox: "e-crop",
    handle: "e-handle",
    stroke: "e-stroke",
    selected: "e-selected",
    strokeList: "e-strokes",
    strokeItem: "e-stroke-item",
    palette: "e-palette",
    widths: "e-widths",
    resize: "e-resize",
    status: "e-status",
};

const kCopy: EditorCopy = {
    canvas: "Photo",
    toolbar: "Tools",
    crop: "Crop",
    draw: "Draw",
    rotateLeft: "Rotate left",
    rotateRight: "Rotate right",
    flip: "Flip",
    undo: "Undo",
    redo: "Redo",
    resetCrop: "Remove crop",
    cropBox: "Crop area",
    handles: { n: "Top", s: "Bottom", e: "Right", w: "Left", ne: "Top right", nw: "Top left", se: "Bottom right", sw: "Bottom left" },
    strokes: "Lines",
    strokeName: (index, total) => `Line ${index} of ${total}`,
    deleteStroke: "Delete line",
    resize: "Longest side",
    cropSize: (width, height) => `${width}×${height}`,
    refused: {
        "edit.format": "format",
        "edit.canonical": "canonical",
        "edit.empty": "empty",
        "edit.crop": "crop",
        "edit.too_small": "too small",
        "edit.upscale": "upscale",
        "edit.stroke": "stroke",
        "edit.bounds": "no more lines",
        "stroke-limit": "too long",
    },
};

function mount(over: Partial<EditorOptions> = {}) {
    const host = document.createElement("div");
    document.body.append(host);
    const view = renderImageEditor(host, {
        href: "https://media.example.com/media/media/6f051c76-293f-41e5-9774-295a8f4b6f97/hero",
        source: { widthPx: 400, heightPx: 300 },
        limits: { maxStrokes: 64, maxPoints: 4096, maxEdgePx: 2560, minEdgePx: 320 },
        initial: null,
        colours: [
            { rgba: 0xe53935ff, label: "Red", className: "red" },
            { rgba: 0x00000080, label: "Shadow", className: "shadow" },
        ],
        widths: [{ width: 655, label: "Medium" }],
        classes: kClasses,
        copy: kCopy,
        ...over,
    });
    const canvas = host.querySelector("svg") as SVGSVGElement;
    const button = (label: string): HTMLButtonElement => {
        const found = Array.from(host.querySelectorAll("button")).find((candidate) => candidate.textContent === label);
        if (found === undefined) throw new Error(`no button labelled ${label}`);
        return found;
    };
    // A point in the oriented frame, as the client coordinates a pointer would
    // report for it.
    const client = (x: number, y: number): { clientX: number; clientY: number } => {
        const point = canvas.createSVGPoint();
        point.x = x;
        point.y = y;
        const matrix = canvas.getScreenCTM();
        if (matrix === null) throw new Error("the canvas is not laid out");
        const at = point.matrixTransform(matrix);
        return { clientX: at.x, clientY: at.y };
    };
    const pointer = (type: string, x: number, y: number): void => {
        canvas.dispatchEvent(new PointerEvent(type, { ...client(x, y), pointerId: 1, button: 0, bubbles: true, cancelable: true }));
    };
    const key = (target: Element, type: "keydown" | "keyup", keyName: string, extra: KeyboardEventInit = {}): void => {
        target.dispatchEvent(new KeyboardEvent(type, { key: keyName, bubbles: true, cancelable: true, ...extra }));
    };
    return { host, view, canvas, button, pointer, key, close: () => { view.close(); host.remove(); } };
}

describe("the surface", () => {
    it("is one SVG in the SVG namespace, over the address it was given", () => {
        const app = mount();
        expect(app.canvas.namespaceURI).toBe("http://www.w3.org/2000/svg");
        expect(app.canvas.getAttribute("viewBox")).toBe("0 0 400 300");
        const image = app.canvas.querySelector("image");
        expect(image?.getAttribute("href")).toContain("/hero");
        expect(image?.parentElement?.getAttribute("transform")).toBe("matrix(1 0 0 1 0 0)");
        app.close();
    });

    it("sets no inline style anywhere, because the CSP it ships under has no unsafe-inline", () => {
        const app = mount();
        app.view.setTool("draw");
        app.pointer("pointerdown", 50, 50);
        app.pointer("pointermove", 120, 90);
        app.pointer("pointerup", 120, 90);
        expect(app.view.element.querySelectorAll("[style]").length).toBe(0);
        app.close();
    });

    it("labels every control, and names the toolbar and the canvas", () => {
        const app = mount();
        expect(app.host.querySelector("[role=toolbar]")?.getAttribute("aria-label")).toBe("Tools");
        expect(app.canvas.getAttribute("aria-label")).toBe("Photo");
        for (const focusable of Array.from(app.canvas.querySelectorAll("[tabindex='0']"))) {
            expect((focusable.getAttribute("aria-label") ?? "").length).toBeGreaterThan(0);
        }
        app.close();
    });

    it("refuses to preview anything but a web address", () => {
        let threw = false;
        try {
            mount({ href: "data:image/png;base64,AAAA" });
        } catch {
            threw = true;
        }
        expect(threw).toBe(true);
    });

    it("lets go of everything it made when closed", () => {
        const app = mount();
        app.close();
        expect(app.host.children.length).toBe(0);
    });
});

describe("the toolbar", () => {
    it("turns the picture, and undo turns it back", () => {
        const app = mount();
        app.button("Rotate right").click();
        expect(app.view.recipe().turns).toBe(1);
        expect(app.canvas.getAttribute("viewBox")).toBe("0 0 300 400");
        expect(app.canvas.querySelector("image")?.parentElement?.getAttribute("transform")).toBe("matrix(0 1 -1 0 300 0)");
        app.button("Undo").click();
        expect(app.view.recipe().turns).toBe(0);
        expect(app.button("Undo").disabled).toBe(true);
        app.close();
    });

    it("encodes what the toolbar did as the server's own bytes", () => {
        const app = mount();
        app.button("Flip").click();
        const encoded = app.view.encode();
        expect(encoded.ok).toBe(true);
        if (encoded.ok) expect(encoded.value.text).toBe("AQQA");
        app.close();
    });
});

describe("the crop, by keyboard", () => {
    it("moves an edge a percent per press, and a held key is one undo", () => {
        const app = mount();
        const right = app.canvas.querySelector("[aria-label='Right']") as SVGElement;
        app.key(right, "keydown", "ArrowLeft");
        app.key(right, "keydown", "ArrowLeft");
        app.key(right, "keydown", "ArrowLeft");
        app.key(right, "keyup", "ArrowLeft");
        expect(app.view.recipe().crop).toEqual({ x: 0, y: 0, w: kFixedOne - 3 * 655, h: kFixedOne });
        app.button("Undo").click();
        expect(app.view.recipe().crop).toBeNull();
        app.close();
    });

    it("takes a whole step of ten with Shift, and Home to the frame's edge", () => {
        const app = mount();
        const left = app.canvas.querySelector("[aria-label='Left']") as SVGElement;
        app.key(left, "keydown", "ArrowRight", { shiftKey: true });
        app.key(left, "keyup", "ArrowRight");
        expect(app.view.recipe().crop?.x).toBe(6554);
        app.key(left, "keydown", "Home");
        app.key(left, "keyup", "Home");
        expect(app.view.recipe().crop?.x).toBe(0);
        app.close();
    });
});

describe("the crop, under a held shape", () => {
    const square = { num: 1, den: 1 };
    const side = (app: ReturnType<typeof mount>) => {
        const crop = app.view.recipe().crop;
        if (crop === null) throw new Error("a square shape on a 4:3 picture needs a crop");
        return { w: (crop.w * 400) / kFixedOne, h: (crop.h * 300) / kFixedOne };
    };

    it("opens on the largest centred box of the shape", () => {
        const app = mount({ aspect: square });
        const box = side(app);
        expect(Math.round(box.w)).toBe(300);
        expect(Math.round(box.h)).toBe(300);
        // The fitted crop is where the person starts, not a change to undo.
        expect(app.button("Undo").disabled).toBe(true);
        app.close();
    });

    it("keeps the shape when a handle is dragged and when a key moves an edge", () => {
        const app = mount({ aspect: square });
        app.pointer("pointerdown", 350, 300);
        app.pointer("pointermove", 300, 200);
        app.pointer("pointerup", 300, 200);
        const dragged = side(app);
        expect(Math.abs(dragged.w - dragged.h)).toBeLessThanOrEqual(1);
        const right = app.canvas.querySelector("[aria-label='Right']") as SVGElement;
        app.key(right, "keydown", "ArrowLeft", { shiftKey: true });
        app.key(right, "keyup", "ArrowLeft");
        const keyed = side(app);
        expect(Math.abs(keyed.w - keyed.h)).toBeLessThanOrEqual(1);
        expect(keyed.w).toBeLessThan(dragged.w);
        app.close();
    });

    it("refits the box after a quarter turn stands the picture up", () => {
        const app = mount({ aspect: { num: 16, den: 9 } });
        app.button("Rotate right").click();
        const crop = app.view.recipe().crop;
        if (crop === null) throw new Error("a 3:4 frame is not 16:9");
        const w = (crop.w * 300) / kFixedOne;
        const h = (crop.h * 400) / kFixedOne;
        expect(Math.abs(w - (h * 16) / 9)).toBeLessThanOrEqual(1);
        expect(Math.round(w)).toBe(300);
        app.close();
    });

    it("resets the crop to the fitted box, and has nothing to reset there", () => {
        const app = mount({ aspect: square });
        expect(app.button("Remove crop").disabled).toBe(true);
        const right = app.canvas.querySelector("[aria-label='Right']") as SVGElement;
        app.key(right, "keydown", "ArrowLeft");
        app.key(right, "keyup", "ArrowLeft");
        expect(app.button("Remove crop").disabled).toBe(false);
        app.button("Remove crop").click();
        expect(Math.round(side(app).w)).toBe(300);
        app.close();
    });

    it("refuses a shape that is not two positive numbers", () => {
        expect(() => mount({ aspect: { num: 0, den: 9 } })).toThrow();
    });
});

describe("drawing", () => {
    it("records a stroke through the pointer and draws it as the server will", () => {
        const app = mount();
        app.view.setTool("draw");
        app.pointer("pointerdown", 40, 40);
        app.pointer("pointermove", 100, 60);
        app.pointer("pointermove", 200, 150);
        app.pointer("pointerup", 200, 150);
        const recipe = app.view.recipe();
        expect(recipe.strokes.length).toBe(1);
        expect(recipe.strokes[0]?.rgba).toBe(0xe53935ff);
        const path = app.canvas.querySelector("path.e-stroke");
        expect(path?.getAttribute("stroke")).toBe("#e53935");
        expect(path?.getAttribute("stroke-linecap")).toBe("round");
        expect(path?.getAttribute("stroke-linejoin")).toBe("round");
        expect(app.host.querySelectorAll("[role=option]").length).toBe(1);
        app.close();
    });

    it("removes a stroke from the list by keyboard, which is how a keyboard user erases", () => {
        const app = mount();
        app.view.setTool("draw");
        app.pointer("pointerdown", 40, 40);
        app.pointer("pointerup", 40, 40);
        const list = app.host.querySelector("[role=listbox]") as HTMLElement;
        app.key(list, "keydown", "ArrowDown");
        app.key(list, "keydown", "Delete");
        expect(app.view.recipe().strokes.length).toBe(0);
        app.close();
    });

    it("refuses a stroke past the count before it starts, and says why", () => {
        const app = mount({ limits: { maxStrokes: 1, maxPoints: 4096, maxEdgePx: 2560, minEdgePx: 320 } });
        app.view.setTool("draw");
        app.pointer("pointerdown", 40, 40);
        app.pointer("pointerup", 40, 40);
        app.pointer("pointerdown", 80, 80);
        app.pointer("pointerup", 80, 80);
        expect(app.view.recipe().strokes.length).toBe(1);
        expect(app.host.querySelector("[aria-live]")?.textContent).toBe("no more lines");
        app.close();
    });

    it("undoes with Ctrl+Z on the editor", () => {
        const app = mount();
        app.view.setTool("draw");
        app.pointer("pointerdown", 40, 40);
        app.pointer("pointerup", 40, 40);
        app.key(app.view.element, "keydown", "z", { ctrlKey: true });
        expect(app.view.recipe().strokes.length).toBe(0);
        app.close();
    });
});
