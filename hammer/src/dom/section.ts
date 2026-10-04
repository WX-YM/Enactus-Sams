// The section renderer, and the only caller of the insertion site.
//
// A section is editor-authored content: a key, a path it renders at, and fields
// carrying labels in every declared locale (`docs/01-seams.md` §8). What this
// draws is one labelled value per field, and the single thing that makes it
// interesting is that one of those values is markup.
//
// --- rich text goes through the one site and nothing else -------------------
//
// `setSanitized` takes a `SanitizedHtml` and the only producer of one is
// `sanitize`. There is no path here that reaches a parser any other way, and
// `tools/check-source-bans.sh` is what asserts that for the library rather than
// for this file.
//
// The pass runs on the way IN, on every render, and not once at the edge. Markup
// reaches a client from a bounded cache entry, a broadcast from another tab and
// a replayed stream event, and none of those was touched by a server render — so
// the value arriving here has no provenance this module can assume.
//
// --- a label is never substituted from another locale -----------------------
//
// `labelAt` answers null rather than falling back, and this honours that: a
// field whose label list is short renders its VALUE with no heading, rather than
// a heading in a language nobody reading this page chose. Silently substituting
// is the failure that reads as a translation somebody wrote and nobody can find.
// A short list is refused at generate time, so reaching this is a descriptor
// that was built before that check existed.

import type { ClassNames } from "../core/tables.js";

import { labelAt } from "../state/sections.js";

import { Closers, detach, documentOf, elementIn, setUserText } from "./mount.js";
import type { Mounted } from "./mount.js";
import { sanitize, setSanitized } from "./sanitized.js";

export type SectionPart = "root" | "field" | "label" | "value";

// One field of a section, as the generated module carries it.
//
// Structural, so no application's table reaches this layer (`CLAUDE.md` §1).
// `labels` is indexed by the LOCALE TABLE's order, which is persisted and
// append-only — index 0 means nothing in particular (`docs/01-seams.md` §5).
export type SectionFieldView = {
    readonly key: string;
    readonly labels: readonly string[];

    // Whether this field's stored value is markup.
    //
    // The application's answer, because it is the application's: which stored
    // field type a section control writes into is a product decision, and
    // `SectionFieldType` and `FieldTypeName` are two closed sets with nothing
    // bridging them (`docs/01-seams.md` §18).
    readonly rich: boolean;
};

export type SectionOptions = {
    readonly fields: readonly SectionFieldView[];

    // The stored value per field key, read out of the document by the
    // application: a response shape is the application's to declare (§4).
    readonly values: Readonly<Record<string, string>>;

    // Which entry of each label list to read.
    readonly localeIndex: number;

    readonly classes: ClassNames<SectionPart>;
};

export function renderSection(mount: Element, options: SectionOptions): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes } = options;

    const root = elementIn(doc, "div", classes.root);

    for (const field of options.fields) {
        const box = elementIn(doc, "div", classes.field);

        const label = labelAt(field.labels, options.localeIndex);
        if (label !== null) {
            const heading = elementIn(doc, "h3", classes.label);
            // Editor-authored, in whatever language this locale is, so it
            // resolves its own direction rather than inheriting the page's.
            setUserText(heading, label);
            box.append(heading);
        }

        const value = elementIn(doc, "div", classes.value);
        // Unconditionally, and before anything is put in it. Whatever this
        // holds is somebody's words in whatever language they wrote them, so it
        // resolves its own direction rather than inheriting the page's — and a
        // field that is empty today is a field an editor fills tomorrow.
        value.dir = "auto";
        const stored = Object.prototype.hasOwnProperty.call(options.values, field.key)
            ? options.values[field.key]
            : undefined;

        if (stored !== undefined) {
            if (field.rich) {
                setSanitized(value, sanitize(stored));
            } else {
                setUserText(value, stored);
            }
        }

        box.append(value);
        root.append(box);
    }

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
