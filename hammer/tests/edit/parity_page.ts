// The page `tests/edit/parity.test.ts` screenshots: the real editor, mounted
// over the fixture's source with one case's recipe, showing nothing but the
// picture.
//
// Nothing about the preview is re-implemented here. The crop box and its
// handles are hidden by the editor itself when the draw tool is in hand, and
// the shade is hidden by attribute; everything left on the canvas is what a
// person editing the picture sees.

import type { ClassNames } from "../../src/core/tables.js";
import type { EditorCopy, EditorPart } from "../../src/edit/editor.js";
import { renderImageEditor } from "../../src/edit/editor.js";
import type { EditLimits, SourceSize } from "../../src/edit/recipe.js";
import { decodeRecipe, orientedSize } from "../../src/edit/recipe.js";

const kParts: readonly EditorPart[] = [
    "root", "toolbar", "button", "canvas", "shade", "cropBox", "handle", "stroke",
    "selected", "strokeList", "strokeItem", "palette", "widths", "resize", "status",
];

const kClasses = Object.fromEntries(kParts.map((part) => [part, `parity-${part}`])) as ClassNames<EditorPart>;

const kCopy: EditorCopy = {
    canvas: "canvas", toolbar: "toolbar", crop: "crop", draw: "draw",
    rotateLeft: "left", rotateRight: "right", flip: "flip", undo: "undo", redo: "redo",
    resetCrop: "reset", cropBox: "crop box",
    handles: { nw: "nw", n: "n", ne: "ne", e: "e", se: "se", s: "s", sw: "sw", w: "w" },
    strokes: "strokes", strokeName: (i, n) => `${i}/${n}`, deleteStroke: "delete", resize: "resize",
    cropSize: (w, h) => `${w}x${h}`,
    refused: {
        "edit.format": "format", "edit.canonical": "canonical", "edit.empty": "empty", "edit.crop": "crop",
        "edit.too_small": "small", "edit.upscale": "upscale", "edit.stroke": "stroke", "edit.bounds": "bounds",
        "stroke-limit": "limit",
    },
};

export type Mounted = {
    // Where the canvas's top-left corner is, in CSS pixels. The canvas is drawn
    // at one CSS pixel per source pixel, so the crop box is at this offset
    // plus its own pixel position in the oriented source.
    readonly leftPx: number;
    readonly topPx: number;
};

async function mount(text: string, source: SourceSize, limits: EditLimits): Promise<Mounted> {
    const root = document.getElementById("root");
    if (root === null) {
        throw new Error("the page has no #root");
    }
    const decoded = decodeRecipe(text, limits);
    if (!decoded.ok) {
        throw new Error(`the fixture's recipe does not decode: ${decoded.error.fault}`);
    }
    const handle = renderImageEditor(root, {
        href: "/source.png",
        source,
        limits,
        initial: decoded.value,
        colours: [{ rgba: 0xff0000ff, label: "red", className: "parity-swatch" }],
        widths: [{ width: 655, label: "medium" }],
        classes: kClasses,
        copy: kCopy,
    });
    handle.setTool("draw");

    const canvas = root.querySelector("svg.parity-canvas");
    const shade = root.querySelector(".parity-shade");
    const image = root.querySelector("image");
    if (canvas === null || shade === null || image === null) {
        throw new Error("the editor drew no canvas");
    }
    shade.setAttribute("visibility", "hidden");
    // Width and height as attributes, as the application's stylesheet would
    // otherwise set them: one CSS pixel per oriented source pixel.
    const frame = orientedSize(source, decoded.value.turns);
    canvas.setAttribute("width", String(frame.widthPx));
    canvas.setAttribute("height", String(frame.heightPx));

    await new Promise<void>((resolve, reject) => {
        image.addEventListener("load", () => resolve(), { once: true });
        image.addEventListener("error", () => reject(new Error("the source did not load")), { once: true });
    });
    // Two frames: one to lay out the decoded image, one to paint it.
    await new Promise<void>((resolve) => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));

    const box = canvas.getBoundingClientRect();
    return { leftPx: box.left, topPx: box.top };
}

declare global {
    interface Window {
        hammerParity: { readonly mount: typeof mount };
    }
}

window.hammerParity = { mount };
