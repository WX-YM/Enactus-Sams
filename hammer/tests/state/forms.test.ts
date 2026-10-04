// A form over a definition the server declared.
//
// The field types are the reference application's generated table, so the bounds
// under test are anvil's numbers rather than this file's — which is the entire
// point of generating both sides from one descriptor.

import { describe, expect, it } from "../support/test.js";

import { Form, definitionsFrom } from "../../src/state/forms.js";
import type { FieldDefinition, FormReasons } from "../../src/state/forms.js";
import { codePointLength } from "../../src/core/text.js";
import { kFieldTypes } from "../testapp/api/hammer.generated.js";
import type { ValidationReason } from "../testapp/api/hammer.generated.js";

const kReasons: FormReasons<ValidationReason> = {
    required: "REQUIRED",
    tooLong: "TOO_LONG",
    badFormat: "BAD_FORMAT",
    notAllowed: "NOT_ALLOWED",
};

function field(over: Partial<FieldDefinition> & { readonly key: string }): FieldDefinition {
    return {
        type: kFieldTypes.TEXT_SHORT,
        required: false,
        ...over,
    };
}

function form(fields: readonly FieldDefinition[], initial?: Record<string, unknown>) {
    return new Form<ValidationReason>({
        fields,
        reasons: kReasons,
        ...(initial === undefined ? {} : { initial: initial as never }),
    });
}

describe("Form", () => {
    it("counts a bound in code points, not in UTF-16 code units", () => {
        // `String.length` counts an emoji as two and every astral character as
        // two, so a limit written against it silently halves the allowance for
        // the scripts that need it most.
        const held = form([field({ key: "title", maxCodePoints: 4 })]);
        const four = "👍👍👍👍";

        expect(four.length).toBe(8);
        expect(codePointLength(four)).toBe(4);

        held.set("title", four);
        held.touch("title");

        expect(held.field("title")?.reason).toBeNull();
    });

    it("refuses one code point over the bound", () => {
        const held = form([field({ key: "title", maxCodePoints: 4 })]);

        held.set("title", "👍👍👍👍👍");
        held.touch("title");

        expect(held.field("title")?.reason).toBe("TOO_LONG");
    });

    it("falls back to the type's own bound when the field names none", () => {
        const held = form([field({ key: "body", type: kFieldTypes.TEXT_SHORT })]);

        held.set("body", "x".repeat(kFieldTypes.TEXT_SHORT.defaultCodePoints + 1));
        held.touch("body");

        expect(held.field("body")?.reason).toBe("TOO_LONG");
    });

    it("normalises to NFC on the way in, so what is counted is what is sent", () => {
        // The composed and decomposed forms are different keys in every index on
        // both sides, and the one that was typed rather than pasted is the one
        // that finds nothing.
        const held = form([field({ key: "name", type: kFieldTypes.NAME })]);

        held.set("name", "é");

        expect(held.field("name")?.value).toBe("é");
        expect(held.body()["name"]).toBe("é");
    });

    it("folds Arabic-Indic digits in a numeric field", () => {
        // The server refuses the shaped form, and the browser gives no
        // indication.
        const held = form([field({ key: "count", type: kFieldTypes.NUMBER })]);

        held.set("count", "١٢٣");
        held.touch("count");

        expect(held.field("count")?.value).toBe("123");
        expect(held.field("count")?.reason).toBeNull();
    });

    it("refuses a number that is not one", () => {
        const held = form([field({ key: "count", type: kFieldTypes.NUMBER })]);

        // `parseFloat("12abc")` is 12 and the server will not agree.
        held.set("count", "12abc");
        held.touch("count");

        expect(held.field("count")?.reason).toBe("BAD_FORMAT");
    });

    it("refuses a line break in a single-line field", () => {
        const held = form([field({ key: "title", type: kFieldTypes.TEXT_SHORT })]);

        held.set("title", "one\ntwo");
        held.touch("title");

        expect(held.field("title")?.reason).toBe("BAD_FORMAT");
    });

    it("allows one in a field whose type says multi-line", () => {
        const held = form([field({ key: "body", type: kFieldTypes.TEXT_LONG })]);

        held.set("body", "one\ntwo");
        held.touch("body");

        expect(held.field("body")?.reason).toBeNull();
    });

    it("refuses a choice outside the closed set", () => {
        const held = form([
            field({ key: "size", type: kFieldTypes.SELECT_SINGLE, choices: ["s", "m"] }),
        ]);

        held.set("size", "xl");
        held.touch("size");
        expect(held.field("size")?.reason).toBe("NOT_ALLOWED");

        held.set("size", "m");
        expect(held.field("size")?.reason).toBeNull();
    });

    it("refuses any member outside the set on a multi-select", () => {
        const held = form([
            field({ key: "tags", type: kFieldTypes.CHECKBOX_MULTI, choices: ["a", "b"] }),
        ]);

        held.set("tags", ["a", "z"]);
        held.touch("tags");
        expect(held.field("tags")?.reason).toBe("NOT_ALLOWED");

        held.set("tags", ["a", "b"]);
        expect(held.field("tags")?.reason).toBeNull();
    });

    it("reports a required field that is empty, and only a required one", () => {
        const held = form([
            field({ key: "title", required: true }),
            field({ key: "note", required: false }),
        ]);

        expect(held.validate()).toBe(false);
        expect(held.field("title")?.reason).toBe("REQUIRED");
        expect(held.field("note")?.reason).toBeNull();
    });

    it("shows no reason before the field has been left", () => {
        // A form that shouts TOO_LONG at the second character is a form people
        // fight.
        const held = form([field({ key: "title", maxCodePoints: 2 })]);

        held.set("title", "abcdef");
        expect(held.field("title")?.reason).toBeNull();

        held.touch("title");
        expect(held.field("title")?.reason).toBe("TOO_LONG");
    });

    it("tracks dirty per field and for the form", () => {
        const held = form([field({ key: "a" }), field({ key: "b" })], { a: "one" });

        expect(held.store.get().dirty).toBe(false);
        held.set("b", "two");

        expect(held.field("a")?.dirty).toBe(false);
        expect(held.field("b")?.dirty).toBe(true);
        expect(held.store.get().dirty).toBe(true);
        expect(held.changed()).toEqual({ b: "two" });
    });

    it("never holds a PII field's value, whatever it was handed", () => {
        // anvil answers `null` for a PII field rather than echoing it back, so
        // an initial value for one came from somewhere that should not have had
        // it.
        const held = form([field({ key: "id", type: kFieldTypes.IDENTITY })], {
            id: "a-national-id",
        });

        expect(held.field("id")?.value).toBeNull();
    });

    it("empties a PII field once the write is accepted", () => {
        const held = form([field({ key: "id", type: kFieldTypes.IDENTITY })]);
        held.set("id", "typed once");
        expect(held.field("id")?.value).toBe("typed once");

        held.accepted();

        // Leaving it on screen is a control showing a value the next read cannot
        // reproduce.
        expect(held.field("id")?.value).toBeNull();
        expect(held.store.get().dirty).toBe(false);
    });

    it("applies no bound to a type the descriptor says is not code-point capped", () => {
        // `IDENTITY` carries `codePointCapped: false`, so the server applies no
        // length bound to it. A client that invented one would refuse input the
        // server would have accepted, which is the same disagreement as the
        // other direction and just as hard to report.
        expect(kFieldTypes.IDENTITY.flags.codePointCapped).toBe(false);
        const held = form([field({ key: "id", type: kFieldTypes.IDENTITY, maxCodePoints: 4 })]);

        held.set("id", "far longer than four");
        held.touch("id");

        expect(held.field("id")?.reason).toBeNull();
    });

    it("bounds a PII field whose type IS capped, because the server will", () => {
        const held = form([
            field({
                key: "secret",
                type: { ...kFieldTypes.IDENTITY, flags: { ...kFieldTypes.IDENTITY.flags, codePointCapped: true } },
                maxCodePoints: 4,
            }),
        ]);

        held.set("secret", "far too long");
        held.touch("secret");

        // Write-only is about reading it BACK. The bound still applies on the
        // way out, because it is the one the server applies on the way in.
        expect(held.field("secret")?.reason).toBe("TOO_LONG");
    });

    it("places the server's reasons on the fields it named", () => {
        const held = form([field({ key: "email" }), field({ key: "title" })]);

        held.applyServerReasons(new Map([["email", "BAD_FORMAT" as ValidationReason]]));

        expect(held.field("email")?.reason).toBe("BAD_FORMAT");
        expect(held.field("email")?.touched).toBe(true);
        expect(held.field("title")?.reason).toBeNull();
    });

    it("keeps a reason for a field this form does not have", () => {
        // A form that refuses to submit with nothing marked on it is the worst
        // failure this surface has.
        const held = form([field({ key: "email" })]);

        held.applyServerReasons(new Map([["retired_field", "NOT_ALLOWED" as ValidationReason]]));

        expect(held.store.get().unplaced.get("retired_field")).toBe("NOT_ALLOWED");
    });

    it("publishes a new map, so a subscriber hears about a field change", () => {
        const held = form([field({ key: "a" })]);
        const seen: number[] = [];
        held.store.subscribe((state) => seen.push(state.fields.size));

        held.set("a", "one");

        // A map edited in place is a change no subscriber hears about, because
        // the store compares by identity.
        expect(seen).toEqual([1]);
    });

    it("ignores a field it does not carry", () => {
        const held = form([field({ key: "a" })]);

        held.set("invented", "x");
        held.touch("invented");

        expect(held.field("invented")).toBeUndefined();
    });
});

describe("definitionsFrom", () => {
    it("resolves a section's fields against the field-type table", () => {
        const built = definitionsFrom(
            [
                {
                    key: "title",
                    type: "TEXT_SHORT",
                    maxCodePoints: 80,
                    required: true,
                    choices: [],
                },
            ],
            kFieldTypes,
        );

        expect(built.ok).toBe(true);
        expect(built.ok && built.value[0]?.maxCodePoints).toBe(80);
    });

    it("refuses a field type the table does not carry", () => {
        // A fallback renders a text box for a signature pad and posts a string
        // the server rejects.
        const built = definitionsFrom(
            [
                {
                    key: "signature",
                    type: "SIGNATURE" as "TEXT_SHORT",
                    maxCodePoints: 80,
                    required: true,
                    choices: [],
                },
            ],
            kFieldTypes,
        );

        expect(built).toEqual({ ok: false, error: "signature" });
    });
});
