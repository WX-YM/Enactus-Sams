//
// The form renderer: which control a field type draws, where a reason goes, and
// what a screen reader is told about the relationship between the two.
//
// The state machine is asserted in `tests/state/forms.test.ts` with no document
// at all. What is asserted here is only the part that needs one, which is why
// there is no case below about when a reason becomes visible — that is the
// store's decision and it is tested where the store is.

import { describe, expect, it } from "../support/test.js";

import { renderForm } from "../../src/dom/form.js";
import type { FormCopy, FormPart } from "../../src/dom/form.js";
import type { ClassNames } from "../../src/core/tables.js";
import { Form } from "../../src/state/forms.js";
import type { FieldDefinition, FieldTypeSpec } from "../../src/state/forms.js";

type Reason = "REQUIRED" | "TOO_LONG" | "BAD_FORMAT" | "NOT_ALLOWED";

const kClasses: ClassNames<FormPart> = {
    root: "f",
    field: "f-field",
    label: "f-label",
    required: "f-required",
    control: "f-control",
    description: "f-description",
    counter: "f-counter",
    error: "f-error",
    summary: "f-summary",
    choices: "f-choices",
    choice: "f-choice",
    submit: "f-submit",
};

const kCopy: FormCopy = {
    required: "required",
    remaining: (codePoints) => `${codePoints} left`,
    summary: "problems",
    submit: "save",
};

const kReasons: Readonly<Record<Reason, string>> = {
    REQUIRED: "Fill this in.",
    TOO_LONG: "That is too long.",
    BAD_FORMAT: "Check the format.",
    NOT_ALLOWED: "That value is not allowed.",
};

function spec(over: Partial<FieldTypeSpec["flags"]> = {}, answer: FieldTypeSpec["answer"] = "text"): FieldTypeSpec {
    return {
        answer,
        defaultCodePoints: 20,
        flags: {
            options: false,
            attachment: false,
            ranged: false,
            codePointCapped: false,
            multiLine: false,
            multiSelect: false,
            ...over,
        },
    };
}

function mount(
    fields: readonly FieldDefinition[],
    views: readonly { key: string; label: string; description?: string }[],
) {
    const form = new Form<Reason>({
        fields,
        reasons: {
            required: "REQUIRED",
            tooLong: "TOO_LONG",
            badFormat: "BAD_FORMAT",
            notAllowed: "NOT_ALLOWED",
        },
    });
    const host = document.createElement("div");
    document.body.append(host);

    let submitted = 0;
    const view = renderForm(host, {
        form,
        fields: views,
        classes: kClasses,
        copy: kCopy,
        reasons: kReasons,
        onSubmit: () => {
            submitted += 1;
        },
    });

    return {
        form,
        host,
        view,
        submits: () => submitted,
        close: () => {
            view.close();
            host.remove();
        },
    };
}

const kText: FieldDefinition = { key: "name", type: spec(), required: true };

describe("the control a field type draws", () => {
    it("draws a single line for a plain text type", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        expect(app.host.querySelector("input")?.type).toBe("text");
        app.close();
    });

    it("draws a text area where the type says many lines", () => {
        const field: FieldDefinition = { key: "bio", type: spec({ multiLine: true }), required: false };
        const app = mount([field], [{ key: "bio", label: "Bio" }]);
        expect(app.host.querySelector("textarea")).not.toBeNull();
        app.close();
    });

    it("draws a number where the type answers with one", () => {
        const field: FieldDefinition = { key: "age", type: spec({}, "number"), required: false };
        const app = mount([field], [{ key: "age", label: "Age" }]);
        expect(app.host.querySelector("input")?.type).toBe("number");
        app.close();
    });

    it("draws a file control for an attachment", () => {
        const field: FieldDefinition = { key: "cv", type: spec({ attachment: true }), required: false };
        const app = mount([field], [{ key: "cv", label: "CV" }]);
        expect(app.host.querySelector("input")?.type).toBe("file");
        app.close();
    });

    it("draws one option per choice for a closed set", () => {
        const field: FieldDefinition = {
            key: "size",
            type: spec({ options: true }),
            required: false,
            choices: ["s", "m", "l"],
        };
        const app = mount([field], [{ key: "size", label: "Size" }]);
        expect(app.host.querySelectorAll("option").length).toBe(3);
        app.close();
    });

    // An option VALUE is `[A-Za-z0-9_-]` on the server, so it cannot be a word
    // in Arabic or in anything else that is not written in ASCII. The value is
    // what is submitted and the label is what is read, and a renderer that
    // printed the value would put `new_site` on screen in both editions of a
    // bilingual site.
    it("prints the label for a choice and submits the value", () => {
        const field: FieldDefinition = {
            key: "kind",
            type: spec({ options: true }),
            required: false,
            choices: ["new_site", "rebuild"],
            choiceLabels: { new_site: "موقع جديد", rebuild: "إعادة بناء" },
        };
        const app = mount([field], [{ key: "kind", label: "النوع" }]);
        const options = app.host.querySelectorAll("option");
        expect(options[0]?.value).toBe("new_site");
        expect(options[0]?.textContent).toBe("موقع جديد");
        expect(options[1]?.value).toBe("rebuild");
        expect(options[1]?.textContent).toBe("إعادة بناء");
        app.close();
    });

    // The ordinary case for a SECTION field: the descriptor carries its
    // `choices` as bare values and has nowhere to put a word, so there are no
    // labels to offer. A blank option would be worse than an unlocalised one.
    it("falls back to the value for a choice with no label", () => {
        const field: FieldDefinition = {
            key: "size",
            type: spec({ options: true }),
            required: false,
            choices: ["s", "m"],
            choiceLabels: { s: "Small" },
        };
        const app = mount([field], [{ key: "size", label: "Size" }]);
        const options = app.host.querySelectorAll("option");
        expect(options[0]?.textContent).toBe("Small");
        expect(options[1]?.textContent).toBe("m");
        app.close();
    });

    // A multi-select listbox is operable with a mouse and hostile without one.
    it("draws a group of boxes where more than one may be chosen", () => {
        const field: FieldDefinition = {
            key: "tags",
            type: spec({ options: true, multiSelect: true }),
            required: false,
            choices: ["a", "b"],
        };
        const app = mount([field], [{ key: "tags", label: "Tags" }]);
        const ticks = app.host.querySelectorAll('input[type="checkbox"]');
        expect(ticks.length).toBe(2);
        expect(app.host.querySelector('[role="group"]')).not.toBeNull();
        app.close();
    });

    // The same rule on the other control. A checkbox's text is its label, and
    // a box whose only name is a stored value is a box nobody can read.
    it("labels a checkbox from the same table", () => {
        const field: FieldDefinition = {
            key: "tags",
            type: spec({ options: true, multiSelect: true }),
            required: false,
            choices: ["a", "b"],
            choiceLabels: { a: "الأول", b: "التاني" },
        };
        const app = mount([field], [{ key: "tags", label: "Tags" }]);
        const lines = app.host.querySelectorAll(".f-choice");
        expect(lines[0]?.textContent).toBe("الأول");
        expect(lines[1]?.textContent).toBe("التاني");
        const ticks = app.host.querySelectorAll<HTMLInputElement>('input[type="checkbox"]');
        expect(ticks[0]?.value).toBe("a");
        app.close();
    });

    // A PII type produces no answer a client reads back, and it still has to be
    // typeable: the value goes up and does not come down again.
    it("still draws a control for a type whose value never comes back", () => {
        const field: FieldDefinition = { key: "ssn", type: spec({}, null), required: false };
        const app = mount([field], [{ key: "ssn", label: "Number" }]);
        expect(app.host.querySelector("input")).not.toBeNull();
        app.close();
    });

    // Drawing it would be a control whose value nothing validates and nothing
    // submits.
    it("draws nothing for a field the form does not hold", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }, { key: "ghost", label: "Ghost" }]);
        expect(app.host.querySelectorAll("input").length).toBe(1);
        app.close();
    });
});

// What a field is to the platform. A password drawn as plain text is on the
// screen of everybody behind the person typing it; a sign-in form a password
// manager cannot recognise is one filled by pasting from somewhere worse.
describe("a field's purpose", () => {
    const rows: readonly (readonly [NonNullable<FieldDefinition["purpose"]>, string, string])[] = [
        ["current-password", "password", "current-password"],
        ["new-password", "password", "new-password"],
        ["email", "email", "email"],
        ["username", "text", "username"],
        ["tel", "tel", "tel"],
        ["one-time-code", "text", "one-time-code"],
        ["given-name", "text", "given-name"],
        ["family-name", "text", "family-name"],
    ];
    it.each(rows)("%s draws type %s with the platform's own token", (purpose, type, token) => {
        const field: FieldDefinition = { key: "f", type: spec({}, null), required: true, purpose };
        const app = mount([field], [{ key: "f", label: "Field" }]);
        const control = app.host.querySelector("input");
        expect(control?.type).toBe(type);
        expect(control?.autocomplete).toBe(token);
        app.close();
    });

    it("masks a secret and never corrects it", () => {
        const field: FieldDefinition = { key: "pw", type: spec({}, null), required: true, purpose: "current-password" };
        const app = mount([field], [{ key: "pw", label: "Password" }]);
        const control = app.host.querySelector("input");
        expect(control?.type).toBe("password");
        expect(control?.spellcheck).toBe(false);
        expect(control?.getAttribute("autocapitalize")).toBe("none");
        app.close();
    });

    it("offers a phone's digit keyboard for a code", () => {
        const field: FieldDefinition = { key: "code", type: spec(), required: true, purpose: "one-time-code" };
        const app = mount([field], [{ key: "code", label: "Code" }]);
        expect(app.host.querySelector("input")?.inputMode).toBe("numeric");
        app.close();
    });

    it("leaves a field with no purpose exactly as it was", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const control = app.host.querySelector("input");
        expect(control?.type).toBe("text");
        expect(control?.getAttribute("autocomplete")).toBeNull();
        app.close();
    });
});

describe("what a screen reader is told", () => {
    it("labels every control", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const control = app.host.querySelector("input");
        const label = app.host.querySelector("label");
        expect(control?.id).not.toBe("");
        expect(label?.getAttribute("for")).toBe(control?.id);
        expect(label?.textContent).toContain("Name");
        app.close();
    });

    // The marker is read out, so it is a word the application chose rather than
    // a punctuation mark it happens to style as one.
    it("marks a required field with the application's word", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        expect(app.host.querySelector(".f-required")?.textContent).toBe("required");
        expect(app.host.querySelector("input")?.required).toBe(true);
        app.close();
    });

    it("points the control at its description, its counter and its error", () => {
        const field: FieldDefinition = {
            key: "bio",
            type: spec({ codePointCapped: true }),
            required: false,
        };
        const app = mount([field], [{ key: "bio", label: "Bio", description: "A short line." }]);
        const control = app.host.querySelector("input");
        const described = control?.getAttribute("aria-describedby")?.split(" ") ?? [];
        expect(described.length).toBe(3);
        for (const id of described) {
            expect(app.host.querySelector(`#${id}`)).not.toBeNull();
        }
        app.close();
    });

    it("says a field is invalid only while it has a reason", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const control = app.host.querySelector("input");
        expect(control?.getAttribute("aria-invalid")).toBe("false");

        app.form.touch("name");
        expect(control?.getAttribute("aria-invalid")).toBe("true");
        expect(app.host.querySelector(".f-error")?.textContent).toBe("Fill this in.");
        app.close();
    });

    // A count that interrupted on every keystroke would make the field unusable
    // with a screen reader.
    it("announces a counter politely rather than assertively", () => {
        const field: FieldDefinition = {
            key: "bio",
            type: spec({ codePointCapped: true }),
            required: false,
        };
        const app = mount([field], [{ key: "bio", label: "Bio" }]);
        expect(app.host.querySelector(".f-counter")?.getAttribute("aria-live")).toBe("polite");
        app.close();
    });
});

describe("the bound", () => {
    // `maxlength` counts UTF-16 code units, so an emoji is two and every astral
    // character is two. Setting it would silently halve the allowance for the
    // scripts that need it most, and disagree with what the server checks.
    it("is not enforced with a maxlength attribute", () => {
        const field: FieldDefinition = {
            key: "bio",
            type: spec({ codePointCapped: true }),
            required: false,
        };
        const app = mount([field], [{ key: "bio", label: "Bio" }]);
        expect(app.host.querySelector("input")?.getAttribute("maxlength")).toBeNull();
        app.close();
    });

    it("counts what is left in code points, not code units", () => {
        const field: FieldDefinition = {
            key: "bio",
            type: spec({ codePointCapped: true }),
            required: false,
        };
        const app = mount([field], [{ key: "bio", label: "Bio" }]);
        const counter = app.host.querySelector(".f-counter");
        expect(counter?.textContent).toBe("20 left");

        // Two emoji are four UTF-16 code units and two code points. A counter
        // measuring `String.length` would say 16.
        app.form.set("bio", "👍👍");
        expect(counter?.textContent).toBe("18 left");
        app.close();
    });

    it("uses the field's own bound over the type's default", () => {
        const field: FieldDefinition = {
            key: "bio",
            type: spec({ codePointCapped: true }),
            required: false,
            maxCodePoints: 5,
        };
        const app = mount([field], [{ key: "bio", label: "Bio" }]);
        expect(app.host.querySelector(".f-counter")?.textContent).toBe("5 left");
        app.close();
    });
});

describe("typing into it", () => {
    it("puts a keystroke through the form's own normalisation", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const control = app.host.querySelector("input");
        if (control === null) {
            throw new Error("the control was not drawn");
        }
        control.value = "Ada";
        control.dispatchEvent(new Event("input"));
        expect(app.form.field("name")?.value).toBe("Ada");
        app.close();
    });

    it("marks the field touched when it is left", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const control = app.host.querySelector("input");
        control?.dispatchEvent(new Event("blur"));
        expect(app.form.field("name")?.touched).toBe(true);
        app.close();
    });

    // Writing the value back on every keystroke moves the caret to the end,
    // which is what makes a controlled input impossible to edit the middle of.
    it("does not write a value back that already matches", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const control = app.host.querySelector("input");
        if (control === null) {
            throw new Error("the control was not drawn");
        }
        let writes = 0;
        Object.defineProperty(control, "value", {
            configurable: true,
            get: () => "Ada",
            set: () => {
                writes += 1;
            },
        });
        app.form.set("name", "Ada");
        expect(writes).toBe(0);
        app.close();
    });

    it("collects every box that is ticked", () => {
        const field: FieldDefinition = {
            key: "tags",
            type: spec({ options: true, multiSelect: true }),
            required: false,
            choices: ["a", "b"],
        };
        const app = mount([field], [{ key: "tags", label: "Tags" }]);
        const ticks = Array.from(app.host.querySelectorAll('input[type="checkbox"]'));
        const first = ticks[0];
        if (!(first instanceof HTMLInputElement)) {
            throw new Error("the boxes were not drawn");
        }
        first.checked = true;
        first.dispatchEvent(new Event("change"));
        expect(app.form.field("tags")?.value).toEqual(["a"]);
        app.close();
    });
});

describe("submitting", () => {
    it("does not navigate", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const event = new Event("submit", { cancelable: true, bubbles: true });
        app.view.element.dispatchEvent(event);
        expect(event.defaultPrevented).toBe(true);
        app.close();
    });

    it("does not call back while a field is invalid", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        app.view.element.dispatchEvent(new Event("submit", { cancelable: true }));
        expect(app.submits()).toBe(0);
        // `validate` marks every field touched, so the reason is now placed.
        expect(app.host.querySelector(".f-error")?.textContent).toBe("Fill this in.");
        app.close();
    });

    it("calls back once the form validates", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        app.form.set("name", "Ada");
        app.view.element.dispatchEvent(new Event("submit", { cancelable: true }));
        expect(app.submits()).toBe(1);
        app.close();
    });

    it("does not call back a second time while one is in flight", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        app.form.set("name", "Ada");
        app.form.submitting(true);
        app.view.element.dispatchEvent(new Event("submit", { cancelable: true }));
        expect(app.submits()).toBe(0);
        app.close();
    });
});

// A form that refuses to submit with nothing marked on it is a person staring
// at a screen with no way forward.
describe("a reason for a field this form does not have", () => {
    it("is rendered rather than dropped", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        app.form.applyServerReasons(new Map([["captcha", "NOT_ALLOWED"]]));
        expect(app.host.querySelector(".f-summary")?.textContent).toBe("That value is not allowed.");
        app.close();
    });

    it("is announced, because it answers a submit that already happened", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        expect(app.host.querySelector(".f-summary")?.getAttribute("role")).toBe("alert");
        app.close();
    });
});

describe("closing", () => {
    it("takes the form out of the document and stops listening", () => {
        const app = mount([kText], [{ key: "name", label: "Name" }]);
        const control = app.host.querySelector("input");
        if (control === null) {
            throw new Error("the control was not drawn");
        }
        app.view.close();

        expect(app.host.querySelector("form")).toBeNull();
        control.value = "Ada";
        control.dispatchEvent(new Event("input"));
        expect(app.form.field("name")?.value).toBeNull();
        app.host.remove();
    });
});
