//
// The upload control: what it refuses before a byte moves, and the path in that
// does not need a pointer.
//
// Nothing here reads a file, and there is no assertion that it does not —
// `tools/check-source-bans.sh` is what asserts that for the whole library, and a
// test can only speak for one module.

import { describe, expect, it } from "../support/test.js";

import type { ClassNames } from "../../src/core/tables.js";
import type { UploadLimits } from "../../src/core/upload_bounds.js";
import { renderUpload } from "../../src/dom/upload.js";
import type { UploadCopy, UploadPart } from "../../src/dom/upload.js";

const kClasses: ClassNames<UploadPart> = {
    root: "u",
    label: "u-label",
    input: "u-input",
    dropZone: "u-zone",
    dragging: "u-dragging",
    progress: "u-progress",
    cancel: "u-cancel",
    error: "u-error",
};

const kCopy: UploadCopy = {
    label: "Choose a file",
    drop: "Drop one here",
    cancel: "Cancel",
    progress: (sent, total) => `${sent}/${total}`,
    refused: {
        "too-large": "That file is too large.",
        "unsupported-media": "That file type is not accepted.",
        "bad-parameter": "That is not a file.",
    },
};

const kLimits: UploadLimits = { maxBytes: 1024, accept: ["image/jpeg", "image/png"] };

// A stand-in for the handle. A `File` is metadata until something reads it, and
// nothing here reads one — so size and type are all a test needs to supply.
function handle(size: number, type: string): File {
    return { size, type, name: "f" } as unknown as File;
}

function mount(over: { readonly limits?: UploadLimits } = {}) {
    const sent: File[] = [];
    let report: ((p: { sentBytes: number; totalBytes: number }) => void) | null = null;
    let settle: ((value: unknown) => void) | null = null;
    let signal: AbortSignal | null = null;

    const host = document.createElement("div");
    document.body.append(host);

    const view = renderUpload(host, {
        limits: over.limits ?? kLimits,
        upload: (file, onProgress, abort) => {
            sent.push(file);
            report = onProgress;
            signal = abort;
            return new Promise((resolve) => {
                settle = resolve;
            });
        },
        classes: kClasses,
        copy: kCopy,
    });

    const find = <E extends Element>(selector: string): E => {
        const found = host.querySelector<E>(selector);
        if (found === null) {
            throw new Error(`the control did not draw ${selector}`);
        }
        return found;
    };

    const input = find<HTMLInputElement>(".u-input");

    // The picker having chosen something. `files` is read-only on the element,
    // so the test supplies the list the browser would have.
    const choose = (...files: readonly File[]): void => {
        Object.defineProperty(input, "files", { configurable: true, value: files });
        input.dispatchEvent(new Event("change"));
    };

    return {
        host,
        view,
        sent,
        input,
        find,
        choose,
        progressed: (p: { sentBytes: number; totalBytes: number }) => report?.(p),
        finish: () => settle?.(undefined),
        signal: () => signal,
        close: () => {
            view.close();
            host.remove();
        },
    };
}

describe("the way in", () => {
    // A drop zone with no input is a control that cannot be reached by keyboard,
    // by switch, by voice, or by anybody who cannot hold a pointer steady.
    it("is a real file input, labelled", () => {
        const app = mount();
        expect(app.input.type).toBe("file");
        expect(app.find(".u-label").getAttribute("for")).toBe(app.input.id);
        expect(app.find(".u-label").textContent).toBe("Choose a file");
        app.close();
    });

    it("hints the picker with the same list it refuses against", () => {
        const app = mount();
        expect(app.input.accept).toBe("image/jpeg,image/png");
        app.close();
    });

    it("hints nothing where the application declared nothing", () => {
        const app = mount({ limits: { maxBytes: 10, accept: [] } });
        expect(app.input.getAttribute("accept")).toBeNull();
        app.close();
    });

    it("sends a file the picker chose", () => {
        const app = mount();
        app.choose(handle(10, "image/jpeg"));
        expect(app.sent.length).toBe(1);
        app.close();
    });

    it("sends a file that was dropped on it", () => {
        const app = mount();
        const event = new Event("drop", { bubbles: true, cancelable: true });
        Object.defineProperty(event, "dataTransfer", {
            value: { files: [handle(10, "image/jpeg")] },
        });
        app.find(".u-zone").dispatchEvent(event);
        expect(app.sent.length).toBe(1);
        app.close();
    });

    // Without this the browser navigates to the file, which is the default and
    // is never what a drop zone wants.
    it("stops the browser navigating to a dragged file", () => {
        const app = mount();
        const over = new Event("dragover", { bubbles: true, cancelable: true });
        app.find(".u-zone").dispatchEvent(over);
        expect(over.defaultPrevented).toBe(true);
        expect(app.find(".u-zone").className).toContain("u-dragging");
        app.close();
    });

    it("stops marking itself dragged when the pointer leaves", () => {
        const app = mount();
        const zone = app.find(".u-zone");
        zone.dispatchEvent(new Event("dragover", { bubbles: true, cancelable: true }));
        zone.dispatchEvent(new Event("dragleave", { bubbles: true }));
        expect(zone.className).not.toContain("u-dragging");
        app.close();
    });
});

describe("what is refused before a byte moves", () => {
    it("refuses one past the descriptor's cap, with the application's words", () => {
        const app = mount();
        app.choose(handle(1025, "image/jpeg"));
        expect(app.sent).toEqual([]);
        expect(app.find(".u-error").textContent).toBe("That file is too large.");
        app.close();
    });

    it("refuses a type the application does not accept", () => {
        const app = mount();
        app.choose(handle(10, "application/pdf"));
        expect(app.sent).toEqual([]);
        expect(app.find(".u-error").textContent).toBe("That file type is not accepted.");
        app.close();
    });

    it("refuses an empty file", () => {
        const app = mount();
        app.choose(handle(0, "image/jpeg"));
        expect(app.sent).toEqual([]);
        expect(app.find(".u-error").textContent).toBe("That is not a file.");
        app.close();
    });

    it("announces the refusal, because it answers a choice already made", () => {
        const app = mount();
        expect(app.find(".u-error").getAttribute("role")).toBe("alert");
        expect(app.input.getAttribute("aria-describedby")).toBe(app.find(".u-error").id);
        app.close();
    });

    it("clears the last refusal when a good file follows it", () => {
        const app = mount();
        app.choose(handle(2000, "image/jpeg"));
        app.choose(handle(10, "image/jpeg"));
        expect(app.find(".u-error").textContent).toBe("");
        app.close();
    });
});

describe("while it runs", () => {
    it("reports progress politely", () => {
        const app = mount();
        app.choose(handle(100, "image/jpeg"));
        expect(app.find(".u-progress").getAttribute("aria-live")).toBe("polite");
        expect(app.find(".u-progress").textContent).toBe("0/100");

        app.progressed({ sentBytes: 40, totalBytes: 100 });
        expect(app.find(".u-progress").textContent).toBe("40/100");
        app.close();
    });

    it("offers a cancel only while there is something to cancel", () => {
        const app = mount();
        expect(app.find<HTMLElement>(".u-cancel").hidden).toBe(true);

        app.choose(handle(100, "image/jpeg"));
        expect(app.find<HTMLElement>(".u-cancel").hidden).toBe(false);

        app.finish();
        return Promise.resolve().then(() => {
            expect(app.find<HTMLElement>(".u-cancel").hidden).toBe(true);
            app.close();
        });
    });

    it("aborts the call when cancelled", () => {
        const app = mount();
        app.choose(handle(100, "image/jpeg"));
        app.find<HTMLButtonElement>(".u-cancel").click();
        expect(app.signal()?.aborted).toBe(true);
        app.close();
    });

    // An upload nothing can stop is one that outlives the screen that wanted it,
    // over a connection somebody is waiting on.
    it("aborts the call when the screen goes away", () => {
        const app = mount();
        app.choose(handle(100, "image/jpeg"));
        app.view.close();
        expect(app.signal()?.aborted).toBe(true);
        app.host.remove();
    });

    it("stops reporting progress once it has been aborted", () => {
        const app = mount();
        app.choose(handle(100, "image/jpeg"));
        app.find<HTMLButtonElement>(".u-cancel").click();
        app.progressed({ sentBytes: 90, totalBytes: 100 });
        expect(app.find(".u-progress").textContent).toBe("");
        app.close();
    });

    it("abandons the first when a second file is chosen", () => {
        const app = mount();
        app.choose(handle(100, "image/jpeg"));
        const first = app.signal();
        app.choose(handle(200, "image/png"));
        expect(first?.aborted).toBe(true);
        expect(app.sent.length).toBe(2);
        app.close();
    });
});
