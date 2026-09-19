// A form, as a state machine over a definition the server declared.
//
// --- bounds are in code points, and that is the whole of §8 ------------------
//
// `String.length` counts UTF-16 code units. An emoji is two of them, every
// astral character is two, and a limit written against it silently halves the
// allowance for the scripts that need it most. anvil's limits are in code
// points, so the client's must be the identical number or the two disagree at
// the boundary — which a person experiences as a form that accepted what the
// server then refused, with no error on the field.
//
// Grapheme clusters are a CARET concern and not a limit concern. `Intl.Segmenter`
// answers "how many characters did the person see"; code points answer "will the
// server take this", and this module is about the second (`core/text.ts`).
//
// --- what this module is not -------------------------------------------------
//
// It is not a control. The server validates the same input against the same
// bounds, because the server is the only participant an attacker does not own.
// What generating both sides from one descriptor buys is that the two AGREE, and
// agreement is not only about accept-versus-refuse: the checks run in anvil's
// order so the same input produces the same REASON, and a field that says one
// thing before the request and a different thing after it is a field a person
// cannot act on.
//
// It ships no sentence. A field's failure is a REASON — a member of the
// descriptor's append-only enum — and the words for it are the application's
// (`docs/01-seams.md` §13).
//
// --- the two things a form gets wrong that nothing else does -----------------
//
// A `VALIDATION_FAILED` naming a field this form does not have is placed on the
// FORM rather than dropped, because a form that refuses to submit with nothing
// marked on it is the worst failure this surface has. And a PII field's value is
// write-only: anvil answers with `null` for one rather than echoing it back
// (`docs/01-seams.md` §7), so a form that re-read its own PII field would empty
// it on the next render.

import { foldDigits } from "../core/digits.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import { codePointLength, toNfc } from "../core/text.js";

import type { Readable } from "./store.js";
import { Store } from "./store.js";

// The part of the descriptor's field-type table this reads. A TYPE hammer ships
// is machinery; the table of types an application declared is not
// (`ENGINEERING_RULES.md` §1).
export type FieldTypeSpec = {
    // `null` for a PII type, and not `"text"`. A PII field produces no answer a
    // client ever reads back, and a renderer handed `"text"` draws a control for
    // a value that has silently emptied itself.
    readonly answer: "text" | "number" | "choice" | "choices" | "media" | null;

    readonly defaultCodePoints: number;

    readonly flags: {
        readonly options: boolean;
        readonly attachment: boolean;
        readonly ranged: boolean;
        readonly codePointCapped: boolean;
        readonly multiLine: boolean;
        readonly multiSelect: boolean;
    };
};

// What a field answers with. One union rather than five fields, because three
// booleans are eight states of which five are meaningless (`ENGINEERING_RULES.md` §3.1).
export type FieldValue = string | number | readonly string[] | null;

export type FieldDefinition = {
    readonly key: string;
    readonly type: FieldTypeSpec;
    readonly required: boolean;

    // Overrides the type's default. Code points, and the name says so.
    readonly maxCodePoints?: number;

    // The closed set this field admits, for a type whose flags say it has
    // options. Empty for every other type.
    readonly choices?: readonly string[];
};

// The reasons this module can produce, named by the application from the
// descriptor's enum. A union spelled here would be a second copy of a table that
// is append-only on the server (`core/validate.ts`).
export type FormReasons<Reason extends string> = {
    readonly required: Reason;
    readonly tooLong: Reason;
    readonly badFormat: Reason;
    readonly notAllowed: Reason;
};

export type FieldState<Reason extends string> = {
    readonly value: FieldValue;

    // The reason this field is invalid, or null. Never a sentence.
    readonly reason: Reason | null;

    // Changed since the form was opened. What decides whether leaving the page
    // should warn, and what a partial save sends.
    readonly dirty: boolean;

    // Visited and left. It is what stops a form shouting every reason at
    // somebody who has typed one character into the first field.
    readonly touched: boolean;
};

export type FormState<Reason extends string> = {
    readonly fields: ReadonlyMap<string, FieldState<Reason>>;

    // A reason the server named for a field this form does not have. Kept rather
    // than dropped: a form that refuses to submit with nothing marked on it is
    // a person staring at a screen with no way forward.
    readonly unplaced: ReadonlyMap<string, Reason>;

    readonly dirty: boolean;
    readonly submitting: boolean;
};

export class Form<Reason extends string> {
    private readonly definitions: ReadonlyMap<string, FieldDefinition>;
    private readonly reasons: FormReasons<Reason>;
    private readonly state: Store<FormState<Reason>>;

    constructor(config: {
        readonly fields: readonly FieldDefinition[];
        readonly reasons: FormReasons<Reason>;
        readonly initial?: Readonly<Record<string, FieldValue>>;
    }) {
        const definitions = new Map<string, FieldDefinition>();
        const fields = new Map<string, FieldState<Reason>>();
        for (const field of config.fields) {
            definitions.set(field.key, field);
            fields.set(field.key, {
                // A PII field starts empty whatever it was handed. anvil answers
                // `null` for one, so an initial value for it came from
                // somewhere that should not have had it.
                value: field.type.answer === null ? null : (config.initial?.[field.key] ?? null),
                reason: null,
                dirty: false,
                touched: false,
            });
        }

        this.definitions = definitions;
        this.reasons = config.reasons;
        this.state = new Store<FormState<Reason>>({
            fields,
            unplaced: new Map(),
            dirty: false,
            submitting: false,
        });
    }

    get store(): Readable<FormState<Reason>> {
        return this.state;
    }

    field(key: string): FieldState<Reason> | undefined {
        return this.state.get().fields.get(key);
    }

    // What this form is validating against, for whatever draws it.
    //
    // A renderer has to know the field's type to pick a control and its bound to
    // show a counter, and handing it a second copy of the definitions is handing
    // it a copy that can disagree with the one the validation uses — which is a
    // form whose counter says a different number from the reason it then
    // publishes.
    definition(key: string): FieldDefinition | undefined {
        return this.definitions.get(key);
    }

    // A keystroke. The value is normalised on the way IN rather than on the way
    // out, so what is counted, what is compared and what is sent are one string
    // (`ENGINEERING_RULES.md` §8) — a form that checked the composed form and sent the
    // decomposed one is two different keys to the server's index.
    set(key: string, value: FieldValue): void {
        const definition = this.definitions.get(key);
        if (definition === undefined) {
            return;
        }

        const cleaned = this.clean(definition, value);
        this.patch(key, (held) => ({
            value: cleaned,
            // Re-validated on every keystroke, but the reason is only PUBLISHED
            // once the field has been left: a form that shouts `TOO_LONG` at the
            // second character is a form people fight.
            reason: held.touched ? this.check(definition, cleaned) : null,
            dirty: true,
            touched: held.touched,
        }));
    }

    // The field was left. From here on its reason is shown.
    touch(key: string): void {
        const definition = this.definitions.get(key);
        if (definition === undefined) {
            return;
        }
        this.patch(key, (held) => ({
            value: held.value,
            reason: this.check(definition, held.value),
            dirty: held.dirty,
            touched: true,
        }));
    }

    // Every field, whether or not it has been touched. What a submit does first.
    validate(): boolean {
        const fields = new Map<string, FieldState<Reason>>();
        let valid = true;
        for (const [key, held] of this.state.get().fields) {
            const definition = this.definitions.get(key);
            const reason = definition === undefined ? null : this.check(definition, held.value);
            if (reason !== null) {
                valid = false;
            }
            fields.set(key, { value: held.value, reason, dirty: held.dirty, touched: true });
        }
        this.publish(fields, this.state.get().unplaced);
        return valid;
    }

    // What the server said, placed on the fields it named.
    //
    // anvil's `fields` map arrives on a `VALIDATION_FAILED` and carries a reason
    // per field name and never a sentence (`docs/00-architecture.md` §6). A
    // reason for a name this form does not carry goes to `unplaced` rather than
    // being discarded.
    applyServerReasons(reported: ReadonlyMap<string, Reason>): void {
        const fields = new Map<string, FieldState<Reason>>();
        const unplaced = new Map<string, Reason>();

        for (const [key, held] of this.state.get().fields) {
            const reason = reported.get(key);
            fields.set(key, {
                value: held.value,
                reason: reason ?? null,
                dirty: held.dirty,
                // Shown, because the server has now said something about it.
                touched: reason === undefined ? held.touched : true,
            });
        }
        for (const [key, reason] of reported) {
            if (!fields.has(key)) {
                unplaced.set(key, reason);
            }
        }

        this.publish(fields, unplaced);
    }

    // What to send: every field that has a value the server reads back, with the
    // PII fields included because they are write-only rather than absent.
    body(): Readonly<Record<string, FieldValue>> {
        const out: Record<string, FieldValue> = {};
        for (const [key, held] of this.state.get().fields) {
            out[key] = held.value;
        }
        return out;
    }

    // Only what changed. For a route that takes a partial document, and for the
    // "are you sure you want to leave" question, which is about this and nothing
    // else.
    changed(): Readonly<Record<string, FieldValue>> {
        const out: Record<string, FieldValue> = {};
        for (const [key, held] of this.state.get().fields) {
            if (held.dirty) {
                out[key] = held.value;
            }
        }
        return out;
    }

    submitting(is: boolean): void {
        const held = this.state.get();
        this.state.set({ ...held, submitting: is });
    }

    // Accepted. Every field is clean again, and a PII field is emptied — anvil
    // will never hand its value back, so leaving it on screen is a control
    // showing a value the next read cannot reproduce.
    accepted(): void {
        const fields = new Map<string, FieldState<Reason>>();
        for (const [key, held] of this.state.get().fields) {
            const definition = this.definitions.get(key);
            fields.set(key, {
                value: definition?.type.answer === null ? null : held.value,
                reason: null,
                dirty: false,
                touched: false,
            });
        }
        this.state.set({ fields, unplaced: new Map(), dirty: false, submitting: false });
    }

    close(): void {
        this.state.close();
    }

    // --- the checks, in anvil's order ---------------------------------------

    private clean(definition: FieldDefinition, value: FieldValue): FieldValue {
        if (typeof value === "string") {
            const text = toNfc(value);
            // An Arabic-Indic digit in a numeric field is a validation failure
            // at the server and a silent one in the browser, so it is folded to
            // ASCII before anything counts or sends it (`ENGINEERING_RULES.md` §8).
            return definition.type.answer === "number" ? foldDigits(text) : text;
        }
        return value;
    }

    private check(definition: FieldDefinition, value: FieldValue): Reason | null {
        if (isEmpty(value)) {
            return definition.required ? this.reasons.required : null;
        }

        switch (definition.type.answer) {
            case "text":
                return this.checkText(definition, value);
            case "number":
                return this.checkNumber(value);
            case "choice":
                return this.checkChoice(definition, value);
            case "choices":
                return this.checkChoices(definition, value);
            case "media":
                return typeof value === "string" ? null : this.reasons.badFormat;
            case null:
                // Write-only. There is nothing to check against a value the
                // server will never hand back, beyond the shape of what was
                // typed — and the length bound still applies, because it is the
                // one the server will apply too.
                return typeof value === "string" ? this.checkText(definition, value) : this.reasons.badFormat;
            default:
                return this.reasons.badFormat;
        }
    }

    private checkText(definition: FieldDefinition, value: FieldValue): Reason | null {
        if (typeof value !== "string") {
            return this.reasons.badFormat;
        }
        if (!definition.type.flags.multiLine && hasLineBreak(value)) {
            return this.reasons.badFormat;
        }
        // Code points, never `String.length`. The number is the descriptor's,
        // which is the number the server checks against.
        const max = definition.maxCodePoints ?? definition.type.defaultCodePoints;
        if (definition.type.flags.codePointCapped && codePointLength(value) > max) {
            return this.reasons.tooLong;
        }
        return null;
    }

    private checkNumber(value: FieldValue): Reason | null {
        if (typeof value === "number") {
            return Number.isFinite(value) ? null : this.reasons.badFormat;
        }
        if (typeof value !== "string") {
            return this.reasons.badFormat;
        }
        // Already folded to ASCII by `clean`. `Number()` rather than
        // `parseFloat`, because `parseFloat("12abc")` is 12 and the server will
        // not agree.
        const parsed = Number(value);
        return Number.isFinite(parsed) ? null : this.reasons.badFormat;
    }

    private checkChoice(definition: FieldDefinition, value: FieldValue): Reason | null {
        if (typeof value !== "string") {
            return this.reasons.badFormat;
        }
        const choices = definition.choices ?? [];
        return choices.includes(value) ? null : this.reasons.notAllowed;
    }

    private checkChoices(definition: FieldDefinition, value: FieldValue): Reason | null {
        if (!Array.isArray(value)) {
            return this.reasons.badFormat;
        }
        const choices = definition.choices ?? [];
        for (const chosen of value as readonly string[]) {
            if (!choices.includes(chosen)) {
                return this.reasons.notAllowed;
            }
        }
        return null;
    }

    private patch(key: string, next: (held: FieldState<Reason>) => FieldState<Reason>): void {
        const held = this.state.get();
        const current = held.fields.get(key);
        if (current === undefined) {
            return;
        }
        // A new `Map` rather than a mutation, because the store compares by
        // identity: a map edited in place is a change no subscriber hears about.
        const fields = new Map(held.fields);
        fields.set(key, next(current));
        this.publish(fields, held.unplaced);
    }

    private publish(
        fields: ReadonlyMap<string, FieldState<Reason>>,
        unplaced: ReadonlyMap<string, Reason>,
    ): void {
        let dirty = false;
        for (const held of fields.values()) {
            if (held.dirty) {
                dirty = true;
                break;
            }
        }
        this.state.set({
            fields,
            unplaced,
            dirty,
            submitting: this.state.get().submitting,
        });
    }
}

// Builds the definitions from a section's own field rows, which is the shape the
// descriptor emits (`docs/01-seams.md` §8). It takes the field-type table as a
// parameter for the reason everything else here does: the table is the
// application's and the lookup is machinery.
export function definitionsFrom<TypeName extends string>(
    fields: readonly {
        readonly key: string;
        readonly type: TypeName;
        readonly maxCodePoints: number;
        readonly required: boolean;
        readonly choices: readonly string[];
    }[],
    types: Readonly<Record<TypeName, FieldTypeSpec>>,
): Result<readonly FieldDefinition[], string> {
    const out: FieldDefinition[] = [];
    for (const field of fields) {
        const spec = types[field.type];
        if (spec === undefined) {
            // An unknown field type is a GENERATION failure, not a runtime
            // fallback (`docs/01-seams.md` §7): a fallback renders a text box
            // for a signature pad and posts a string the server rejects. This is
            // the run-time half of the same refusal, for a definition that
            // arrived over the wire rather than from the descriptor.
            return fail(field.key);
        }
        out.push({
            key: field.key,
            type: spec,
            required: field.required,
            maxCodePoints: field.maxCodePoints,
            choices: field.choices,
        });
    }
    return ok(out);
}

function isEmpty(value: FieldValue): boolean {
    if (value === null) {
        return true;
    }
    if (typeof value === "string") {
        return value.length === 0;
    }
    if (Array.isArray(value)) {
        return value.length === 0;
    }
    return false;
}

function hasLineBreak(text: string): boolean {
    for (let i = 0; i < text.length; i += 1) {
        const unit = text.charCodeAt(i);
        if (unit === 0x0a || unit === 0x0d) {
            return true;
        }
    }
    return false;
}
