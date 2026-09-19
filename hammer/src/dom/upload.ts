// The upload control: a refusal before the first byte, progress where the
// platform offers it, and a keyboard path that does not depend on dragging.
//
// --- the file input is the control, and the drop zone is an addition --------
//
// A real `<input type="file">` is always present and is what the label points
// at. Dragging is a convenience layered over it, never the way in: a drop zone
// with no input is a control that cannot be reached by keyboard, by switch, by
// voice, or by anybody who cannot hold a pointer steady across a screen. The
// platform's own picker is also the only thing that works with the operating
// system's file dialogue, which is where most people actually find a file.
//
// --- refused before a byte moves --------------------------------------------
//
// The size and the type are checked against the descriptor's cap and the
// application's accept list before anything is sent, through the same
// `checkUpload` the transport uses (`core/upload_bounds.ts`). One
// implementation, so a control cannot accept what the transport then refuses.
//
// It is a round-trip saver and NOT a control: anvil enforces the cap during the
// read, because a client-side check is one an attacker skips
// (`docs/01-seams.md` §12). What it buys is the forty-megabyte upload that was
// going to be refused after the last byte, on a connection where that is minutes.
//
// --- the bytes never enter the heap -----------------------------------------
//
// Nothing here reads the file. There is no preview, no `createObjectURL` and no
// `readAsDataURL` — `tools/check-source-bans.sh` refuses all three across the
// library. `file.size` and `file.type` are metadata on the handle; reading one
// to show a thumbnail would put the whole file in the heap with the least room
// (`ENGINEERING_RULES.md` §2.2).

import type { UploadFile, UploadLimits, UploadRefusal } from "../core/upload_bounds.js";
import { checkUpload } from "../core/upload_bounds.js";
import type { ClassNames } from "../core/tables.js";

import { Closers, detach, documentOf, elementIn, uniqueId } from "./mount.js";
import type { Mounted } from "./mount.js";

export type UploadPart =
    | "root"
    | "label"
    | "input"
    | "dropZone"
    | "dragging"
    | "progress"
    | "cancel"
    | "error";

export type UploadCopy = {
    // Names the file input. Without it the control announces as an unlabelled
    // button, which is the most common single accessibility defect there is.
    readonly label: string;

    // Shown in the drop zone. It is an invitation, not the only way in.
    readonly drop: string;

    readonly cancel: string;

    // How far along. Bytes are a unit the application formats — a thousand-
    // separator and a digit shape belong to the locale (`ENGINEERING_RULES.md` §8).
    readonly progress: (sentBytes: number, totalBytes: number) => string;

    // Why a file was refused. Total over exactly the causes the check can
    // produce, so a new one is a compile error rather than a silent blank
    // (`docs/01-seams.md` §13).
    readonly refused: Readonly<Record<UploadRefusal, string>>;
};

export type UploadProgressReport = {
    readonly sentBytes: number;
    readonly totalBytes: number;
};

export type UploadOptions = {
    // The descriptor's cap and the application's accept list. The accept list is
    // the application's because anvil has no table of media types to emit — a
    // cross-repo row in `docs/15-tasks.md`.
    readonly limits: UploadLimits;

    // The call. It is the application's because `hammer/dom` may not import
    // `hammer/wire`, and because the route is the application's.
    readonly upload: (
        file: File,
        report: (progress: UploadProgressReport) => void,
        signal: AbortSignal,
    ) => Promise<unknown>;

    readonly classes: ClassNames<UploadPart>;
    readonly copy: UploadCopy;
};

export function renderUpload(mount: Element, options: UploadOptions): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes, copy, limits } = options;

    const root = elementIn(doc, "div", classes.root);

    const inputId = uniqueId(doc, "hammer-upload");
    const errorId = `${inputId}-error`;

    const label = elementIn(doc, "label", classes.label);
    label.htmlFor = inputId;
    label.textContent = copy.label;

    const input = elementIn(doc, "input", classes.input);
    input.type = "file";
    input.id = inputId;
    if (limits.accept.length > 0) {
        // A hint to the picker, from the same list the refusal is made against.
        // It narrows what the dialogue offers and decides nothing: a file chosen
        // past it is still checked below.
        input.accept = limits.accept.join(",");
    }
    input.setAttribute("aria-describedby", errorId);

    const zone = elementIn(doc, "div", classes.dropZone);
    const invitation = elementIn(doc, "p");
    invitation.textContent = copy.drop;
    zone.append(label, input, invitation);

    const progress = elementIn(doc, "p", classes.progress);
    progress.setAttribute("aria-live", "polite");

    const cancel = elementIn(doc, "button", classes.cancel);
    cancel.type = "button";
    cancel.textContent = copy.cancel;
    cancel.hidden = true;

    const failure = elementIn(doc, "p", classes.error);
    failure.id = errorId;
    // An alert: it answers a choice the person has already made.
    failure.role = "alert";

    root.append(zone, progress, cancel, failure);

    // Aborted on close, and by the cancel button. An upload nothing can stop is
    // one that outlives the screen that wanted it, over a connection somebody is
    // waiting on.
    let running: AbortController | null = null;
    closers.add(() => running?.abort());

    const facts = (file: File): UploadFile => ({ size: file.size, type: file.type });

    const send = (file: File): void => {
        failure.textContent = "";

        // Before a byte moves. The same check the transport makes, from the same
        // implementation.
        const allowed = checkUpload(facts(file), limits);
        if (!allowed.ok) {
            failure.textContent = copy.refused[allowed.error.cause];
            return;
        }

        running?.abort();
        const lifetime = new AbortController();
        running = lifetime;
        cancel.hidden = false;
        progress.textContent = copy.progress(0, file.size);

        const report = (seen: UploadProgressReport): void => {
            if (!lifetime.signal.aborted) {
                progress.textContent = copy.progress(seen.sentBytes, seen.totalBytes);
            }
        };

        const settle = (): void => {
            if (running !== lifetime) {
                // A later upload has already taken over. Publishing here would
                // put the finished one's state over the running one's.
                return;
            }
            running = null;
            cancel.hidden = true;
        };

        // Every task body catches (`docs/00-architecture.md` §3). An unhandled
        // rejection here would leave the cancel button on screen with nothing
        // behind it and a progress line that never moves again.
        void options
            .upload(file, report, lifetime.signal)
            .then(settle, settle);
    };

    const onChosen = (): void => {
        const chosen = input.files;
        const file = chosen === null ? undefined : chosen[0];
        if (file !== undefined) {
            send(file);
        }
    };
    input.addEventListener("change", onChosen);
    closers.add(() => input.removeEventListener("change", onChosen));

    const onCancel = (): void => {
        running?.abort();
        running = null;
        cancel.hidden = true;
        progress.textContent = "";
    };
    cancel.addEventListener("click", onCancel);
    closers.add(() => cancel.removeEventListener("click", onCancel));

    // Dragging. The class is toggled so the application can style the state; the
    // component decides when, and never what it looks like.
    const onOver = (event: DragEvent): void => {
        // Without this the browser navigates to the file, which is the default
        // and is never what a drop zone wants.
        event.preventDefault();
        zone.classList.add(classes.dragging);
    };
    const onLeave = (): void => zone.classList.remove(classes.dragging);
    const onDrop = (event: DragEvent): void => {
        event.preventDefault();
        zone.classList.remove(classes.dragging);
        const file = event.dataTransfer?.files[0];
        if (file !== undefined) {
            send(file);
        }
    };

    zone.addEventListener("dragover", onOver);
    zone.addEventListener("dragleave", onLeave);
    zone.addEventListener("drop", onDrop);
    closers.add(() => {
        zone.removeEventListener("dragover", onOver);
        zone.removeEventListener("dragleave", onLeave);
        zone.removeEventListener("drop", onDrop);
    });

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
