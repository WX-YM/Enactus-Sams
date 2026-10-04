// The image editor: crop, rotate, flip, resize and freehand strokes, drawn as
// one SVG over the stored image, producing a recipe the server renders.
//
// --- what it never does -------------------------------------------------------
//
// It never produces an edited pixel. There is no canvas, no `fetch`, no
// `createObjectURL` and no decode into script: the `<image>` is an address on the
// media origin that the browser decodes exactly as it would an `<img>`, through
// the CDN cache, and the recipe is what leaves the tab (`CLAUDE.md` §2.2). A
// canvas editor would make two renderers, hold a full-size bitmap for as long
// as a person fiddled with a crop, and bake every edit into the next one
// (`docs/04-image-edits.md` §1).
//
// --- why one SVG --------------------------------------------------------------
//
// Every position is an ATTRIBUTE — `transform`, `d`, `x`, `width`, `stroke` —
// and never inline style, so it works under anvil's CSP, which has no
// `unsafe-inline` (`CLAUDE.md` §5). And an SVG path with round caps, round joins
// and `stroke-opacity` paints exactly the union of capsules anvil's rasteriser
// draws, blended in sRGB, so the preview is the render up to edge anti-aliasing.
//
// --- what the application styles -----------------------------------------------
//
// The canvas's class needs `touch-action: none`, or touch scrolls the page under
// a stroke. It is the application's stylesheet that says so, for the reason
// every look is: this component sets no style (`CLAUDE.md` §5).
//
// --- what it holds ------------------------------------------------------------
//
// Mechanism only (`CLAUDE.md` §9): every word, every class name and every
// colour a person can pick are the application's. The one colour this component
// ever writes is the stroke colour the PERSON chose, into the `stroke` attribute
// of the path they drew.

import type { ClassNames } from "../core/tables.js";
import type { Result } from "../core/result.js";

import { Closers, detach, documentOf, elementIn, uniqueId } from "../dom/mount.js";
import type { Mounted } from "../dom/mount.js";

import type { CropAspect, CropHandle } from "./geometry.js";
import {
    cropBetween,
    dragCorner,
    fromFixed,
    holdsAspect,
    kCropStep,
    kCropStepLarge,
    largestCrop,
    lockCrop,
    mirror,
    nudgeCrop,
    orientTransform,
    rotate,
    simplify,
    strokePath,
    strokeWidthPx,
    toFixed,
} from "./geometry.js";
import { EditHistory } from "./history.js";
import type { EditLimits, EncodedRecipe, FixedRect, Recipe, RecipeError, RecipeFault, SourceSize, Stroke } from "./recipe.js";
import { encodeRecipe, kEmptyRecipe, kFixedOne, orientedSize, planEdit } from "./recipe.js";

const kSvg = "http://www.w3.org/2000/svg";

export type EditorPart =
    | "root"
    | "toolbar"
    | "button"
    | "canvas"
    | "shade"
    | "cropBox"
    | "handle"
    | "stroke"
    | "selected"
    | "strokeList"
    | "strokeItem"
    | "palette"
    | "widths"
    | "resize"
    | "status";

export type EditorTool = "crop" | "draw";

type SizedHandle = Exclude<CropHandle, "move">;
const kHandles: readonly SizedHandle[] = ["nw", "n", "ne", "e", "se", "s", "sw", "w"];

// Why an action was refused, in the server's vocabulary where it is the
// server's rule, plus the one refusal that happens mid-gesture.
export type EditorRefusal = RecipeFault | "stroke-limit";

export type EditorCopy = {
    readonly canvas: string;
    readonly toolbar: string;
    readonly crop: string;
    readonly draw: string;
    readonly rotateLeft: string;
    readonly rotateRight: string;
    readonly flip: string;
    readonly undo: string;
    readonly redo: string;
    readonly resetCrop: string;
    readonly cropBox: string;
    readonly handles: Readonly<Record<SizedHandle, string>>;
    readonly strokes: string;
    readonly strokeName: (index: number, total: number) => string;
    readonly deleteStroke: string;
    // Labels the long-edge input. Empty means "as large as the server keeps".
    readonly resize: string;
    // Announced as the crop moves: the size the result will be, in the locale's
    // digits — which is why it is a function and not a template.
    readonly cropSize: (widthPx: number, heightPx: number) => string;
    readonly refused: Readonly<Record<EditorRefusal, string>>;
};

// A colour a person may draw with. `className` is how its button LOOKS — a
// swatch is styled by the application, never by an inline style from here.
export type EditorColour = {
    readonly rgba: number;
    readonly label: string;
    readonly className: string;
};

export type EditorWidth = {
    // A fraction of the oriented short edge, 1..8192 (`kMaxStrokeWidth`).
    readonly width: number;
    readonly label: string;
};

export type EditorOptions = {
    // The address the preview draws, from the route builder: a role of the
    // SOURCE, wide enough for the space the editor has. Never the master.
    readonly href: string;
    // The source's pixel size, from the edit state route.
    readonly source: SourceSize;
    readonly limits: EditLimits;
    // What to open with: the recipe `reopenEdit` returned, or null for none.
    readonly initial: Recipe | null;
    readonly colours: readonly EditorColour[];
    readonly widths: readonly EditorWidth[];
    readonly classes: ClassNames<EditorPart>;
    readonly copy: EditorCopy;
    // The shape the crop must keep, width to height, when the result is bound
    // for a place that has one — a slot that refuses any other. The editor then
    // opens on the largest centred crop of that shape, and every drag, key and
    // rotation keeps it. Null or absent: any shape.
    readonly aspect?: CropAspect | null;
    // Every change, with the plan's refusal when the recipe as it stands cannot
    // be sent — so a save button can say why it is disabled.
    readonly onChange?: (recipe: Recipe, refused: RecipeError | null) => void;
};

export type EditorHandle = Mounted & {
    readonly recipe: () => Recipe;
    // The recipe as the edit route takes it, or the server's reason it would not.
    readonly encode: () => Result<EncodedRecipe, RecipeError>;
    readonly setTool: (tool: EditorTool) => void;
};

type Gesture =
    | { readonly kind: "crop"; readonly handle: CropHandle | "new"; readonly startX: number; readonly startY: number; readonly from: Recipe }
    | { readonly kind: "draw"; readonly raw: number[]; readonly path: SVGPathElement }
    | null;

function svg<K extends keyof SVGElementTagNameMap>(doc: Document, tag: K, className?: string): SVGElementTagNameMap[K] {
    const made = doc.createElementNS(kSvg, tag);
    if (className !== undefined && className.length > 0) {
        made.setAttribute("class", className);
    }
    return made;
}

function hex(rgba: number): string {
    return `#${((rgba >>> 8) & 0xffffff).toString(16).padStart(6, "0")}`;
}

function opacity(rgba: number): string {
    return String(Math.round(((rgba & 0xff) / 255) * 1000) / 1000);
}

function sameRect(a: FixedRect, b: FixedRect): boolean {
    return a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h;
}

function button(doc: Document, className: string, label: string): HTMLButtonElement {
    const made = elementIn(doc, "button", className);
    made.type = "button";
    made.textContent = label;
    return made;
}

export function renderImageEditor(mount: Element, options: EditorOptions): EditorHandle {
    const { classes, copy, limits, source } = options;
    // Anything but a web address is a programmer error, an inline image or a
    // script scheme included: the preview is a role of a stored image on the
    // media origin, and nothing else (`CLAUDE.md` §2.2).
    if (!/^(https?:\/\/|\/)/.test(options.href)) {
        throw new Error("the editor previews a media address, not an inline image");
    }
    if (options.colours.length === 0 || options.widths.length === 0) {
        throw new Error("the editor needs at least one colour and one width to draw with");
    }

    const doc = documentOf(mount);
    const view = doc.defaultView;
    const closers = new Closers();
    const aspect = options.aspect ?? null;
    if (aspect !== null && !(aspect.num > 0 && aspect.den > 0)) {
        throw new Error("a crop aspect is two positive numbers");
    }
    // The recipe with its crop brought to the held shape, if it has lost it: on
    // opening, and after a quarter turn, which turns a wide box tall.
    const fit = (recipe: Recipe): Recipe => {
        if (aspect === null) {
            return recipe;
        }
        const frame = orientedSize(source, recipe.turns);
        return holdsAspect(recipe.crop, frame, aspect) ? recipe : { ...recipe, crop: largestCrop(frame, aspect) };
    };
    const lock = (crop: FixedRect, handle: CropHandle): FixedRect =>
        aspect === null ? crop : lockCrop(crop, handle, orientedSize(source, history.current.turns), aspect);
    const history = new EditHistory(fit(options.initial ?? kEmptyRecipe));

    let tool: EditorTool = "crop";
    let colour = options.colours[0] as EditorColour;
    let width = options.widths[0] as EditorWidth;
    let selected = -1;
    let gesture: Gesture = null;

    // --- structure -------------------------------------------------------------

    const root = elementIn(doc, "div", classes.root);

    const toolbar = elementIn(doc, "div", classes.toolbar);
    toolbar.setAttribute("role", "toolbar");
    toolbar.setAttribute("aria-label", copy.toolbar);
    const cropTool = button(doc, classes.button, copy.crop);
    const drawTool = button(doc, classes.button, copy.draw);
    const rotateLeft = button(doc, classes.button, copy.rotateLeft);
    const rotateRight = button(doc, classes.button, copy.rotateRight);
    const flip = button(doc, classes.button, copy.flip);
    const undo = button(doc, classes.button, copy.undo);
    const redo = button(doc, classes.button, copy.redo);
    const resetCrop = button(doc, classes.button, copy.resetCrop);
    toolbar.append(cropTool, drawTool, rotateLeft, rotateRight, flip, resetCrop, undo, redo);

    const palette = elementIn(doc, "div", classes.palette);
    palette.setAttribute("role", "group");
    palette.setAttribute("aria-label", copy.draw);
    const swatches = options.colours.map((entry) => {
        const swatch = button(doc, entry.className, entry.label);
        palette.append(swatch);
        return swatch;
    });
    const widthPicker = elementIn(doc, "div", classes.widths);
    widthPicker.setAttribute("role", "group");
    const widthButtons = options.widths.map((entry) => {
        const choice = button(doc, classes.button, entry.label);
        widthPicker.append(choice);
        return choice;
    });

    const resizeId = uniqueId(doc, "hammer-edit-resize");
    const resizeLabel = elementIn(doc, "label");
    resizeLabel.htmlFor = resizeId;
    resizeLabel.textContent = copy.resize;
    const resize = elementIn(doc, "input", classes.resize);
    resize.type = "number";
    resize.id = resizeId;
    resize.min = String(limits.minEdgePx);
    resize.step = "1";
    resize.inputMode = "numeric";

    const canvas = svg(doc, "svg", classes.canvas);
    canvas.setAttribute("role", "group");
    canvas.setAttribute("aria-label", copy.canvas);
    const picture = svg(doc, "g");
    const image = svg(doc, "image");
    image.setAttribute("href", options.href);
    image.setAttribute("width", String(source.widthPx));
    image.setAttribute("height", String(source.heightPx));
    image.setAttribute("preserveAspectRatio", "none");
    picture.append(image);
    const inked = svg(doc, "g");
    const shade = svg(doc, "path", classes.shade);
    shade.setAttribute("fill-rule", "evenodd");
    shade.setAttribute("pointer-events", "none");
    const cropBox = svg(doc, "rect", classes.cropBox);
    cropBox.setAttribute("tabindex", "0");
    cropBox.setAttribute("role", "button");
    cropBox.setAttribute("aria-label", copy.cropBox);
    cropBox.setAttribute("fill", "transparent");
    const handles = kHandles.map((name) => {
        const handle = svg(doc, "rect", classes.handle);
        handle.setAttribute("tabindex", "0");
        handle.setAttribute("role", "button");
        handle.setAttribute("aria-label", copy.handles[name]);
        handle.dataset["handle"] = name;
        return handle;
    });
    canvas.append(picture, inked, shade, cropBox, ...handles);

    const listId = uniqueId(doc, "hammer-edit-strokes");
    const strokeList = elementIn(doc, "ul", classes.strokeList);
    strokeList.id = listId;
    strokeList.setAttribute("role", "listbox");
    strokeList.setAttribute("tabindex", "0");
    strokeList.setAttribute("aria-label", copy.strokes);
    const deleteStroke = button(doc, classes.button, copy.deleteStroke);

    const status = elementIn(doc, "p", classes.status);
    status.setAttribute("aria-live", "polite");

    root.append(toolbar, palette, widthPicker, resizeLabel, resize, canvas, strokeList, deleteStroke, status);

    // --- painting ---------------------------------------------------------------

    const oriented = (): { readonly widthPx: number; readonly heightPx: number } =>
        orientedSize(source, history.current.turns);

    const handleSize = (): number => {
        const frame = oriented();
        return Math.max(8, Math.round(Math.min(frame.widthPx, frame.heightPx) * 0.03));
    };

    const paintStrokes = (recipe: Recipe): void => {
        const frame = oriented();
        while (inked.firstChild !== null) {
            inked.firstChild.remove();
        }
        recipe.strokes.forEach((stroke, index) => {
            const path = svg(doc, "path", index === selected ? `${classes.stroke} ${classes.selected}` : classes.stroke);
            path.setAttribute("d", strokePath(stroke, frame));
            path.setAttribute("fill", "none");
            path.setAttribute("stroke", hex(stroke.rgba));
            path.setAttribute("stroke-opacity", opacity(stroke.rgba));
            path.setAttribute("stroke-width", String(strokeWidthPx(stroke, source)));
            path.setAttribute("stroke-linecap", "round");
            path.setAttribute("stroke-linejoin", "round");
            inked.append(path);
        });
    };

    const paintCrop = (recipe: Recipe): void => {
        const frame = oriented();
        const crop = recipe.crop ?? { x: 0, y: 0, w: kFixedOne, h: kFixedOne };
        const x = fromFixed(crop.x, frame.widthPx);
        const y = fromFixed(crop.y, frame.heightPx);
        const w = fromFixed(crop.w, frame.widthPx);
        const h = fromFixed(crop.h, frame.heightPx);
        shade.setAttribute(
            "d",
            `M0 0H${frame.widthPx}V${frame.heightPx}H0Z M${x} ${y}H${x + w}V${y + h}H${x}Z`,
        );
        cropBox.setAttribute("x", String(x));
        cropBox.setAttribute("y", String(y));
        cropBox.setAttribute("width", String(w));
        cropBox.setAttribute("height", String(h));
        const size = handleSize();
        const at: Readonly<Record<SizedHandle, readonly [number, number]>> = {
            nw: [x, y], n: [x + w / 2, y], ne: [x + w, y], e: [x + w, y + h / 2],
            se: [x + w, y + h], s: [x + w / 2, y + h], sw: [x, y + h], w: [x, y + h / 2],
        };
        handles.forEach((handle, i) => {
            const [cx, cy] = at[kHandles[i] as SizedHandle];
            handle.setAttribute("x", String(cx - size / 2));
            handle.setAttribute("y", String(cy - size / 2));
            handle.setAttribute("width", String(size));
            handle.setAttribute("height", String(size));
        });
        // The crop is only interactive while it is the tool in hand; hidden
        // rather than removed, so focus order does not reshuffle on a tool change.
        const cropping = tool === "crop";
        for (const node of [cropBox, ...handles]) {
            node.setAttribute("visibility", cropping ? "visible" : "hidden");
            node.setAttribute("tabindex", cropping ? "0" : "-1");
        }
    };

    const paintList = (recipe: Recipe): void => {
        while (strokeList.firstChild !== null) {
            strokeList.firstChild.remove();
        }
        recipe.strokes.forEach((_, index) => {
            const item = elementIn(doc, "li", classes.strokeItem);
            item.id = `${listId}-${index}`;
            item.setAttribute("role", "option");
            item.setAttribute("aria-selected", index === selected ? "true" : "false");
            item.textContent = copy.strokeName(index + 1, recipe.strokes.length);
            item.addEventListener("click", () => select(index));
            strokeList.append(item);
        });
        if (selected >= 0) {
            strokeList.setAttribute("aria-activedescendant", `${listId}-${selected}`);
        } else {
            strokeList.removeAttribute("aria-activedescendant");
        }
        deleteStroke.disabled = selected < 0;
    };

    let announce: number | null = null;
    const announceSize = (recipe: Recipe): void => {
        if (view === null) {
            return;
        }
        if (announce !== null) {
            view.clearTimeout(announce);
        }
        // Debounced: a held arrow key announces the size it stopped at, not
        // every step on the way.
        announce = view.setTimeout(() => {
            announce = null;
            const plan = planEdit(recipe, source, limits);
            status.textContent = plan.ok
                ? copy.cropSize(plan.value.outWidthPx, plan.value.outHeightPx)
                : copy.refused[plan.error.fault];
        }, 400);
    };

    const paint = (): void => {
        const recipe = history.current;
        const frame = oriented();
        canvas.setAttribute("viewBox", `0 0 ${frame.widthPx} ${frame.heightPx}`);
        picture.setAttribute("transform", orientTransform(source, recipe.turns, recipe.flip));
        if (selected >= recipe.strokes.length) {
            selected = recipe.strokes.length - 1;
        }
        paintStrokes(recipe);
        paintCrop(recipe);
        paintList(recipe);

        cropTool.setAttribute("aria-pressed", tool === "crop" ? "true" : "false");
        drawTool.setAttribute("aria-pressed", tool === "draw" ? "true" : "false");
        swatches.forEach((swatch, i) => swatch.setAttribute("aria-pressed", options.colours[i] === colour ? "true" : "false"));
        widthButtons.forEach((choice, i) => choice.setAttribute("aria-pressed", options.widths[i] === width ? "true" : "false"));
        palette.hidden = tool !== "draw";
        widthPicker.hidden = tool !== "draw";
        undo.disabled = !history.canUndo;
        redo.disabled = !history.canRedo;
        const reset = fit({ ...recipe, crop: null }).crop;
        resetCrop.disabled = recipe.crop === reset || (recipe.crop !== null && reset !== null && sameRect(recipe.crop, reset));
        resize.value = recipe.longEdgePx === null ? "" : String(recipe.longEdgePx);

        if (options.onChange !== undefined) {
            const encoded = encodeRecipe(recipe, limits, source);
            options.onChange(recipe, encoded.ok ? null : encoded.error);
        }
    };

    const commit = (next: Recipe): void => {
        history.push(next);
        paint();
    };

    const refuse = (why: EditorRefusal): void => {
        status.textContent = copy.refused[why];
    };

    const select = (index: number): void => {
        selected = index;
        paint();
    };

    // --- the toolbar ------------------------------------------------------------

    const listen = <K extends keyof HTMLElementEventMap>(
        target: HTMLElement,
        type: K,
        handler: (event: HTMLElementEventMap[K]) => void,
    ): void => {
        target.addEventListener(type, handler);
        closers.add(() => target.removeEventListener(type, handler));
    };

    const setTool = (next: EditorTool): void => {
        tool = next;
        paint();
    };

    listen(cropTool, "click", () => setTool("crop"));
    listen(drawTool, "click", () => setTool("draw"));
    listen(rotateLeft, "click", () => commit(fit(rotate(history.current, false))));
    listen(rotateRight, "click", () => commit(fit(rotate(history.current, true))));
    listen(flip, "click", () => commit(mirror(history.current)));
    listen(resetCrop, "click", () => commit(fit({ ...history.current, crop: null })));
    listen(undo, "click", () => {
        history.undo();
        paint();
    });
    listen(redo, "click", () => {
        history.redo();
        paint();
    });
    swatches.forEach((swatch, i) =>
        listen(swatch, "click", () => {
            colour = options.colours[i] ?? colour;
            paint();
        }),
    );
    widthButtons.forEach((choice, i) =>
        listen(choice, "click", () => {
            width = options.widths[i] ?? width;
            paint();
        }),
    );
    listen(resize, "change", () => {
        const text = resize.value.trim();
        const value = Number(text);
        if (text === "") {
            commit({ ...history.current, longEdgePx: null });
        } else if (Number.isInteger(value) && value > 0 && value <= kFixedOne) {
            commit({ ...history.current, longEdgePx: value });
        } else {
            refuse("edit.upscale");
            resize.value = history.current.longEdgePx === null ? "" : String(history.current.longEdgePx);
        }
    });

    const removeSelected = (): void => {
        if (selected < 0) {
            return;
        }
        const strokes = history.current.strokes.filter((_, i) => i !== selected);
        commit({ ...history.current, strokes });
    };
    listen(deleteStroke, "click", removeSelected);
    listen(strokeList, "keydown", (event) => {
        const count = history.current.strokes.length;
        if (event.key === "ArrowDown" && count > 0) {
            event.preventDefault();
            select(Math.min(count - 1, selected + 1));
        } else if (event.key === "ArrowUp" && count > 0) {
            event.preventDefault();
            select(Math.max(0, selected - 1));
        } else if (event.key === "Delete" || event.key === "Backspace") {
            event.preventDefault();
            removeSelected();
        }
    });

    // Undo and redo on the editor, never on the document: a shortcut bound to the
    // document would take Ctrl+Z from a text field elsewhere on the page.
    listen(root, "keydown", (event) => {
        if (!(event.ctrlKey || event.metaKey) || event.altKey) {
            return;
        }
        const key = event.key.toLowerCase();
        if (key === "z" && !event.shiftKey) {
            event.preventDefault();
            history.undo();
            paint();
        } else if ((key === "z" && event.shiftKey) || key === "y") {
            event.preventDefault();
            history.redo();
            paint();
        }
    });

    // --- the crop by keyboard -------------------------------------------------------

    const onCropKey = (handle: CropHandle) => (event: KeyboardEvent): void => {
        const step = event.shiftKey ? kCropStepLarge : kCropStep;
        const crop = history.current.crop;
        let dx = 0;
        let dy = 0;
        switch (event.key) {
            case "ArrowLeft": dx = -step; break;
            case "ArrowRight": dx = step; break;
            case "ArrowUp": dy = -step; break;
            case "ArrowDown": dy = step; break;
            case "Home":
            case "End": {
                // To the frame's edge, for the edge this handle owns.
                if (handle === "move") return;
                dx = handle.includes("w") ? -kFixedOne : handle.includes("e") ? kFixedOne : 0;
                dy = handle.includes("n") ? -kFixedOne : handle.includes("s") ? kFixedOne : 0;
                break;
            }
            default:
                return;
        }
        event.preventDefault();
        // Replaced while the key is held and committed when it is released, so a
        // held key is ONE undo rather than one per repeat.
        history.replace({ ...history.current, crop: lock(nudgeCrop(crop, handle, dx, dy), handle) });
        paint();
        announceSize(history.current);
    };
    const onCropKeyUp = (): void => commit(history.current);

    const listenSvg = <K extends keyof SVGElementEventMap>(
        target: SVGElement,
        type: K,
        handler: (event: SVGElementEventMap[K]) => void,
    ): void => {
        target.addEventListener(type, handler as EventListener);
        closers.add(() => target.removeEventListener(type, handler as EventListener));
    };
    listenSvg(cropBox, "keydown", onCropKey("move"));
    listenSvg(cropBox, "keyup", onCropKeyUp);
    handles.forEach((handle, i) => {
        const name = kHandles[i] as SizedHandle;
        listenSvg(handle, "keydown", onCropKey(name));
        listenSvg(handle, "keyup", onCropKeyUp);
    });

    // --- the pointer ----------------------------------------------------------------

    // Client coordinates to the oriented frame, through the SVG's own screen
    // matrix — so a fitted, scaled or scrolled canvas maps correctly with no
    // arithmetic of ours.
    const toFrame = (clientX: number, clientY: number): { readonly x: number; readonly y: number } | null => {
        const matrix = canvas.getScreenCTM();
        if (matrix === null) {
            return null;
        }
        const point = canvas.createSVGPoint();
        point.x = clientX;
        point.y = clientY;
        const local = point.matrixTransform(matrix.inverse());
        return { x: local.x, y: local.y };
    };

    let frameRequested: number | null = null;
    const drawLive = (): void => {
        frameRequested = null;
        if (gesture === null || gesture.kind !== "draw") {
            return;
        }
        const raw = gesture.raw;
        let d = "";
        for (let i = 0; i + 1 < raw.length; i += 2) {
            d += `${i === 0 ? "M" : "L"}${raw[i]} ${raw[i + 1]}`;
        }
        if (raw.length === 2) {
            d += `L${raw[0]} ${raw[1]}`;
        }
        gesture.path.setAttribute("d", d);
    };

    const onDown = (event: PointerEvent): void => {
        if (event.button !== 0) {
            return;
        }
        const at = toFrame(event.clientX, event.clientY);
        if (at === null) {
            return;
        }
        const frame = oriented();
        if (tool === "draw") {
            if (history.current.strokes.length >= limits.maxStrokes) {
                refuse("edit.bounds");
                return;
            }
            const path = svg(doc, "path", classes.stroke);
            path.setAttribute("fill", "none");
            path.setAttribute("stroke", hex(colour.rgba));
            path.setAttribute("stroke-opacity", opacity(colour.rgba));
            path.setAttribute("stroke-width", String((width.width * Math.min(frame.widthPx, frame.heightPx)) / kFixedOne));
            path.setAttribute("stroke-linecap", "round");
            path.setAttribute("stroke-linejoin", "round");
            inked.append(path);
            gesture = { kind: "draw", raw: [at.x, at.y], path };
            drawLive();
        } else {
            const target = event.target as Element | null;
            const named = target?.getAttribute("data-handle");
            const handle: CropHandle | "new" =
                named !== null && named !== undefined ? (named as CropHandle) : target === cropBox ? "move" : "new";
            gesture = {
                kind: "crop",
                handle,
                startX: toFixed(at.x, frame.widthPx),
                startY: toFixed(at.y, frame.heightPx),
                from: history.current,
            };
        }
        // Capture keeps a stroke drawing when the pointer leaves the canvas. It
        // is an aid, not a precondition: a pointer that has already ended, or an
        // event a script dispatched, has nothing to capture, and the browser
        // throws for it.
        try {
            canvas.setPointerCapture(event.pointerId);
        } catch {
            // Drawn without capture.
        }
        event.preventDefault();
    };

    const onMove = (event: PointerEvent): void => {
        if (gesture === null) {
            return;
        }
        const frame = oriented();
        if (gesture.kind === "draw") {
            // Every sample the device took since the last event, so a fast
            // stroke is a curve and not a polygon.
            const samples = typeof event.getCoalescedEvents === "function" ? event.getCoalescedEvents() : [event];
            for (const sample of samples.length > 0 ? samples : [event]) {
                const at = toFrame(sample.clientX, sample.clientY);
                if (at !== null) {
                    gesture.raw.push(at.x, at.y);
                }
            }
            // One `d` write per frame, never one per event.
            if (frameRequested === null && view !== null) {
                frameRequested = view.requestAnimationFrame(drawLive);
            }
            return;
        }
        const at = toFrame(event.clientX, event.clientY);
        if (at === null) {
            return;
        }
        const x = toFixed(at.x, frame.widthPx);
        const y = toFixed(at.y, frame.heightPx);
        const crop =
            gesture.handle === "new"
                ? lock(cropBetween(gesture.startX, gesture.startY, x, y), dragCorner(gesture.startX, gesture.startY, x, y))
                : lock(nudgeCrop(gesture.from.crop, gesture.handle, x - gesture.startX, y - gesture.startY), gesture.handle);
        history.replace({ ...history.current, crop });
        paint();
        announceSize(history.current);
    };

    const finishStroke = (raw: readonly number[]): void => {
        const recipe = history.current;
        const frame = oriented();
        // Half an OUTPUT pixel, in oriented pixels: detail the render cannot
        // show is dropped, and detail it can is kept, at any zoom.
        const plan = planEdit(recipe, source, limits);
        const tolerance = plan.ok ? (0.5 * plan.value.crop.width) / plan.value.outWidthPx : 0.5;
        const kept = simplify(raw, tolerance);
        let used = 0;
        for (const stroke of recipe.strokes) {
            used += stroke.points.length / 2;
        }
        if (used + kept.length / 2 > limits.maxPoints) {
            // Refused whole rather than truncated: a signature missing its last
            // letters is worse than one the person is told to redraw.
            refuse("stroke-limit");
            paint();
            return;
        }
        const points = new Uint16Array(kept.length);
        for (let i = 0; i + 1 < kept.length; i += 2) {
            points[i] = toFixed(kept[i] ?? 0, frame.widthPx);
            points[i + 1] = toFixed(kept[i + 1] ?? 0, frame.heightPx);
        }
        const stroke: Stroke = { rgba: colour.rgba, width: width.width, points };
        selected = recipe.strokes.length;
        commit({ ...recipe, strokes: [...recipe.strokes, stroke] });
    };

    const onUp = (event: PointerEvent): void => {
        if (gesture === null) {
            return;
        }
        const ended = gesture;
        gesture = null;
        if (canvas.hasPointerCapture(event.pointerId)) {
            canvas.releasePointerCapture(event.pointerId);
        }
        if (ended.kind === "draw") {
            finishStroke(ended.raw);
        } else {
            commit(history.current);
        }
    };

    const onCancel = (): void => {
        if (gesture === null) {
            return;
        }
        // A cancelled gesture — a palm, a system dialogue — leaves nothing
        // behind. A stroke was never committed, so repainting drops its live
        // path; a crop drag that moved is abandoned by the history's own undo,
        // and one that never moved has nothing to abandon.
        const ended = gesture;
        gesture = null;
        if (ended.kind === "crop" && history.current !== ended.from) {
            history.undo();
        }
        paint();
    };

    listenSvg(canvas, "pointerdown", onDown);
    listenSvg(canvas, "pointermove", onMove);
    listenSvg(canvas, "pointerup", onUp);
    listenSvg(canvas, "pointercancel", onCancel);

    closers.add(() => {
        if (view !== null && frameRequested !== null) view.cancelAnimationFrame(frameRequested);
        if (view !== null && announce !== null) view.clearTimeout(announce);
    });

    mount.append(root);
    closers.add(() => detach(root));
    paint();

    return {
        element: root,
        close: () => closers.run(),
        recipe: () => history.current,
        encode: () => encodeRecipe(history.current, limits, source),
        setTool,
    };
}
