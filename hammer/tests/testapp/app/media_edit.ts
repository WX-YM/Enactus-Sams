// Editing a stored image: the reference consumer's proof that `hammer/edit` is
// satisfiable from outside hammer with nothing but generated constants.
//
// Everything application-shaped arrives from here: the two routes, by their
// generated constants; the bounds, as `kEditLimits`; the palette, the widths,
// the words and the class names. The editor holds none of them.
//
// Saving does not bind the result. Pointing a document at the new image is this
// application's own versioned write, and the one thing the editor hands it is
// the new id.

import type { Result } from "hammer";
import type { HammerError } from "hammer";
import type { ApiTypes, Client } from "hammer/wire";
import type { EditAnswerError, EditedMedia, EditorHandle, RecipeError } from "hammer/edit";
import { editCall, renderImageEditor, reopenEdit, submitEdit } from "hammer/edit";

import type { Locale } from "../api/hammer.generated.js";
import { kEditLimits, routeMediaEdit, routeMediaEditState } from "../api/hammer.generated.js";
import { editorClasses } from "./classes.js";
import { componentCopy } from "./component_copy.js";
import { editPreview } from "./state.js";

// The colours this product lets a person draw with, and what each swatch is
// called. A product decision, and therefore here.
const kColours = [
    { rgba: 0xe53935ff, label: "Red", className: "tk-swatch-red" },
    { rgba: 0xfdd835ff, label: "Yellow", className: "tk-swatch-yellow" },
    { rgba: 0x0000007f, label: "Shadow", className: "tk-swatch-shadow" },
] as const;

const kWidths = [
    { width: 164, label: "Fine" },
    { width: 655, label: "Medium" },
    { width: 2621, label: "Marker" },
] as const;

export type OpenedEditor = {
    readonly editor: EditorHandle;
    readonly save: (detach: boolean, signal: AbortSignal) => Promise<Result<EditedMedia, HammerError | EditAnswerError | RecipeError>>;
};

// Opens an editor on any stored image in the `media` namespace. An image that is
// itself an edit opens on its SOURCE with its recipe drawn over it, so saving is
// always the source plus the whole recipe.
export async function openEditor<A extends ApiTypes>(
    client: Client<A>,
    mount: Element,
    id: string,
    locale: Locale,
    signal: AbortSignal,
): Promise<Result<OpenedEditor, HammerError | EditAnswerError | RecipeError>> {
    const call = editCall(client);
    const opened = await reopenEdit(call, {
        route: routeMediaEditState,
        subject: { ns: "media", id },
        limits: kEditLimits,
        signal,
    });
    if (!opened.ok) {
        return opened;
    }
    const source = { ns: "media", id: opened.value.source };
    const href = editPreview(source);
    if (!href.ok) {
        // A subject the route builder refuses cannot reach here from a server
        // answer; it is programmer error if it does.
        throw new Error("the edit source has no preview address");
    }

    const editor = renderImageEditor(mount, {
        href: href.value,
        source: opened.value.size,
        limits: kEditLimits,
        initial: opened.value.recipe,
        colours: kColours,
        widths: kWidths,
        classes: editorClasses,
        copy: componentCopy[locale].editor,
    });

    const save = async (detach: boolean, saving: AbortSignal) => {
        const encoded = editor.encode();
        if (!encoded.ok) {
            return encoded;
        }
        return await submitEdit(call, { route: routeMediaEdit, source, recipe: encoded.value, detach, signal: saving });
    };
    return { ok: true, value: { editor, save } };
}
