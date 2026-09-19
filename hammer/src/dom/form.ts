// The form renderer: a control per field type, the ARIA that ties it together,
// and a reason placed where the person who has to fix it is looking.
//
// The state machine is `state/forms.ts` and is testable with no document at all.
// What is here is the part that needs one — which control a field type draws,
// where an error goes, and what a screen reader is told about the relationship
// between them.
//
// --- the words are not here, and neither are the field types ----------------
//
// A field's label is passed in per field, so a control cannot be drawn without
// one: an unlabelled control is a defect rather than a polish item, and one
// shipped from a library is shipped to every consumer at once (`ENGINEERING_RULES.md` §9).
// A reason is a CODE — `TOO_LONG`, not a sentence — and the sentence comes from
// the application's copy table keyed by that code (`docs/01-seams.md` §13).
//
// The control is chosen from the descriptor's flags and never from a table of
// type names held here. A table hammer populates is a bug (`ENGINEERING_RULES.md` §1), and
// an unknown field type is a generation failure rather than a runtime fallback:
// a fallback draws a text box for a signature pad and posts a string the server
// rejects.
//
// --- the bound is in code points --------------------------------------------
//
// The counter and the validation both measure code points, which is the number
// the server checks against. `maxlength` is deliberately not set: it counts
// UTF-16 code units, so an emoji is two and every astral character is two, and
// the attribute would silently halve the allowance for the scripts that need it
// most (`ENGINEERING_RULES.md` §8). A person typing past the bound is told; they are not
// prevented mid-word by a counter that disagrees with the server.
//
// --- submitting does not reload ---------------------------------------------
//
// The root is a real `<form>`, so the platform's own behaviour is inherited —
// Enter submits, a required field is announced, the control is in the tab order
// — and the default navigation is the one thing replaced. A component that
// replaced the element instead would owe all of that back (`ENGINEERING_RULES.md` §9).

import type { ClassNames } from "../core/tables.js";
import { codePointLength } from "../core/text.js";

import type { FieldDefinition, FieldValue, Form, FormState } from "../state/forms.js";

import { Closers, bind, detach, documentOf, elementIn, setUserText, uniqueId } from "./mount.js";
import type { Mounted } from "./mount.js";

export type FormPart =
    | "root"
    | "field"
    | "label"
    | "required"
    | "control"
    | "description"
    | "counter"
    | "error"
    | "summary"
    | "choices"
    | "choice"
    | "submit";

// Every word this renderer puts on a screen. Each is the application's, and the
// two that carry a number are functions because a plural rule and a digit shape
// belong to the locale rather than to the count (`ENGINEERING_RULES.md` §8).
export type FormCopy = {
    // Marks a field that must be answered. It is read out, so it is a word and
    // not a punctuation mark the application happens to style as one.
    readonly required: string;

    // How much of the bound is left, in code points.
    readonly remaining: (codePoints: number) => string;

    // Names the region that collects reasons no field on this form could take.
    readonly summary: string;

    readonly submit: string;
};

// One field to draw, in the order it is drawn.
//
// The label is on this object rather than in a lookup because that is what makes
// it impossible to omit: a field this renderer draws is a field somebody wrote
// words for.
export type FormField = {
    readonly key: string;
    readonly label: string;
    readonly description?: string;
};

export type FormOptions<Reason extends string> = {
    readonly form: Form<Reason>;
    readonly fields: readonly FormField[];
    readonly classes: ClassNames<FormPart>;
    readonly copy: FormCopy;

    // The words for a reason, total over the descriptor's enum
    // (`core/tables.ts`). This is `FailureCopy["reasons"]` for the locale being
    // read.
    readonly reasons: Readonly<Record<Reason, string>>;

    // Called when the form validates. A submit that does not validate never
    // reaches here, and neither does one already in flight.
    readonly onSubmit: () => void;
};

type FieldParts = {
    readonly control: HTMLInputElement | HTMLTextAreaElement | HTMLSelectElement | null;
    readonly boxes: readonly HTMLInputElement[];
    readonly error: HTMLElement;
    readonly counter: HTMLElement | null;
    readonly definition: FieldDefinition;
};

// The bound this field is held to, in code points. The field's own override
// where it has one, and the type's default otherwise.
function boundOf(definition: FieldDefinition): number {
    return definition.maxCodePoints ?? definition.type.defaultCodePoints;
}

function textOf(value: FieldValue): string {
    if (value === null) {
        return "";
    }
    return typeof value === "string" ? value : String(value);
}

// Which control a field type draws.
//
// Read off the descriptor's named flags, in the order that makes each decision
// once. `answer` being null is a PII type: it still draws a control, because
// somebody has to be able to type into it, and the value simply never comes back
// (`docs/01-seams.md` §7).
function controlFor(
    doc: Document,
    definition: FieldDefinition,
    className: string,
): HTMLInputElement | HTMLTextAreaElement | HTMLSelectElement | null {
    const flags = definition.type.flags;

    if (flags.options && flags.multiSelect) {
        // Drawn as a group of boxes below rather than as one control: a
        // multi-select listbox is operable with a mouse and hostile without one.
        return null;
    }
    if (flags.multiLine) {
        return elementIn(doc, "textarea", className);
    }
    if (flags.options) {
        const control = elementIn(doc, "select", className);
        // Filled here rather than by a caller testing what came back:
        // `instanceof` against a constructor from another document answers false
        // for an element that is perfectly good, and this layer renders into
        // documents that are not the tab's on purpose (`dom/mount.ts`).
        for (const choice of definition.choices ?? []) {
            const option = elementIn(doc, "option");
            option.value = choice;
            option.textContent = choice;
            control.append(option);
        }
        return control;
    }
    if (flags.attachment) {
        const control = elementIn(doc, "input", className);
        control.type = "file";
        return control;
    }

    const control = elementIn(doc, "input", className);
    if (definition.type.answer === "number" || flags.ranged) {
        control.type = "number";
    } else {
        control.type = "text";
    }
    return control;
}

export function renderForm<Reason extends string>(
    mount: Element,
    options: FormOptions<Reason>,
): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { form, classes, copy } = options;

    const root = elementIn(doc, "form", classes.root);
    // The platform's own validation bubbles say things in a language nobody
    // chose, so the reasons this form publishes are the only ones shown. The
    // fields keep their `required` semantics for assistive technology.
    root.noValidate = true;

    const parts = new Map<string, FieldParts>();

    for (const field of options.fields) {
        const definition = form.definition(field.key);
        if (definition === undefined) {
            // A field the form does not hold. Drawing it would be a control
            // whose value nothing validates and nothing submits.
            continue;
        }

        const box = elementIn(doc, "div", classes.field);
        const controlId = uniqueId(doc, "hammer-field");
        const errorId = `${controlId}-error`;
        const describedBy: string[] = [];

        const label = elementIn(doc, "label", classes.label);
        label.htmlFor = controlId;
        label.append(doc.createTextNode(field.label));
        if (definition.required) {
            const mark = elementIn(doc, "span", classes.required);
            mark.textContent = copy.required;
            label.append(mark);
        }
        box.append(label);

        const control = controlFor(doc, definition, classes.control);
        const boxes: HTMLInputElement[] = [];

        if (control === null) {
            // A checkbox per choice, inside a group that carries the label. The
            // label element above cannot point at a group, so the group is named
            // by it instead.
            const group = elementIn(doc, "div", classes.choices);
            group.role = "group";
            group.setAttribute("aria-labelledby", `${controlId}-label`);
            label.id = `${controlId}-label`;
            for (const choice of definition.choices ?? []) {
                const line = elementIn(doc, "label", classes.choice);
                const tick = elementIn(doc, "input");
                tick.type = "checkbox";
                tick.value = choice;
                line.append(tick, doc.createTextNode(choice));
                group.append(line);
                boxes.push(tick);
            }
            box.append(group);
        } else {
            control.id = controlId;
            control.name = field.key;
            if (definition.required) {
                control.required = true;
            }
            box.append(control);
        }

        if (field.description !== undefined) {
            const description = elementIn(doc, "p", classes.description);
            description.id = `${controlId}-description`;
            setUserText(description, field.description);
            describedBy.push(description.id);
            box.append(description);
        }

        let counter: HTMLElement | null = null;
        if (definition.type.flags.codePointCapped) {
            counter = elementIn(doc, "span", classes.counter);
            counter.id = `${controlId}-counter`;
            // Polite, not assertive: a count that interrupted on every keystroke
            // would make the field unusable with a screen reader.
            counter.setAttribute("aria-live", "polite");
            describedBy.push(counter.id);
            box.append(counter);
        }

        const error = elementIn(doc, "p", classes.error);
        error.id = errorId;
        describedBy.push(errorId);
        box.append(error);

        const target = control ?? boxes[0];
        if (target !== undefined && describedBy.length > 0) {
            target.setAttribute("aria-describedby", describedBy.join(" "));
        }

        parts.set(field.key, { control, boxes, error, counter, definition });
        root.append(box);
    }

    // Reasons the server named for fields this form does not have.
    //
    // Rendered rather than dropped: a form that refuses to submit with nothing
    // marked on it is a person staring at a screen with no way forward. It is an
    // alert because it appears in answer to a submit that has already happened.
    const summary = elementIn(doc, "div", classes.summary);
    summary.role = "alert";
    summary.setAttribute("aria-label", copy.summary);
    root.append(summary);

    const submit = elementIn(doc, "button", classes.submit);
    submit.type = "submit";
    submit.textContent = copy.submit;
    root.append(submit);

    const draw = (state: FormState<Reason>): void => {
        for (const [key, held] of parts) {
            const field = state.fields.get(key);
            if (field === undefined) {
                continue;
            }

            const reason = field.reason;
            const words = reason === null ? "" : options.reasons[reason];
            // The reason is a code and the words are the application's. Placed as
            // text, never as markup: a server value rendered as markup is the
            // defect the one insertion site exists to make unspellable.
            held.error.textContent = words;

            const target = held.control ?? held.boxes[0];
            if (target !== undefined) {
                target.setAttribute("aria-invalid", reason === null ? "false" : "true");
            }

            if (held.counter !== null) {
                const left = boundOf(held.definition) - codePointLength(textOf(field.value));
                held.counter.textContent = copy.remaining(left);
            }

            if (held.control !== null && held.control.value !== textOf(field.value)) {
                // Only when it differs. Writing the value back on every keystroke
                // moves the caret to the end, which is what makes a controlled
                // input impossible to edit in the middle of.
                held.control.value = textOf(field.value);
            }

            if (held.boxes.length > 0) {
                const chosen = new Set(Array.isArray(field.value) ? field.value : []);
                for (const tick of held.boxes) {
                    tick.checked = chosen.has(tick.value);
                }
            }
        }

        const unplaced: string[] = [];
        for (const [, reason] of state.unplaced) {
            unplaced.push(options.reasons[reason]);
        }
        summary.textContent = unplaced.join(" ");
        submit.disabled = state.submitting;
    };

    for (const [key, held] of parts) {
        const read = (): FieldValue => {
            if (held.boxes.length > 0) {
                const chosen: string[] = [];
                for (const tick of held.boxes) {
                    if (tick.checked) {
                        chosen.push(tick.value);
                    }
                }
                return chosen;
            }
            return held.control === null ? null : held.control.value;
        };

        const onInput = (): void => form.set(key, read());
        const onBlur = (): void => form.touch(key);

        for (const node of held.control === null ? held.boxes : [held.control]) {
            node.addEventListener("input", onInput);
            node.addEventListener("change", onInput);
            node.addEventListener("blur", onBlur);
            closers.add(() => {
                node.removeEventListener("input", onInput);
                node.removeEventListener("change", onInput);
                node.removeEventListener("blur", onBlur);
            });
        }
    }

    const onSubmit = (event: Event): void => {
        // The one platform behaviour replaced, and the reason this is a `<form>`
        // at all: everything else about it is inherited rather than rebuilt.
        event.preventDefault();
        if (form.store.get().submitting) {
            return;
        }
        if (!form.validate()) {
            // `validate` marks every field touched, so the reasons are published
            // and the redraw below places them.
            return;
        }
        options.onSubmit();
    };
    root.addEventListener("submit", onSubmit);
    closers.add(() => root.removeEventListener("submit", onSubmit));

    closers.add(bind(form.store, draw));

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
