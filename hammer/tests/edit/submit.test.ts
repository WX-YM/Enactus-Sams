// The two edit calls: what they send, and how narrowly they read the answer.

import { describe, expect, it } from "../support/test.js";

import type { CallableRoute } from "../../src/wire/client.js";
import type { HammerError } from "../../src/core/errors.js";
import type { Result } from "../../src/core/result.js";
import { fail, ok } from "../../src/core/result.js";
import { encodeRecipe, kEmptyRecipe } from "../../src/edit/recipe.js";
import type { EditLimits } from "../../src/edit/recipe.js";
import type { EditCall, MediaSubject } from "../../src/edit/submit.js";
import { reopenEdit, submitEdit } from "../../src/edit/submit.js";

const kLimits: EditLimits = { maxStrokes: 64, maxPoints: 4096, maxEdgePx: 2560, minEdgePx: 320 };

const kRoute: CallableRoute = {
    id: "media.edit",
    visibility: "holder",
    method: null,
    path: null,
    capability: null,
    rateLimit: "media",
    idempotent: false,
};

type Sent = { readonly params: MediaSubject; readonly body: unknown };

function stub(answer: Result<unknown, HammerError>): { readonly call: EditCall; readonly sent: Sent[] } {
    const sent: Sent[] = [];
    const call: EditCall = async (_route, params, body) => {
        sent.push({ params, body });
        return answer;
    };
    return { call, sent };
}

function centreCrop() {
    const encoded = encodeRecipe(
        { ...kEmptyRecipe, crop: { x: 16384, y: 16384, w: 32768, h: 32768 } },
        kLimits,
        { widthPx: 2000, heightPx: 1000 },
    );
    if (!encoded.ok) throw new Error("the fixture recipe does not encode");
    return encoded.value;
}

const signal = new AbortController().signal;

describe("submitEdit", () => {
    it("sends the recipe text and the detach decision, and reads the new object", async () => {
        const { call, sent } = stub(ok({ id: "8f5bbb78-1d10-4ed6-a05c-104584383ddd", width: 800, height: 600 }));
        const recipe = centreCrop();
        const result = await submitEdit(call, {
            route: kRoute,
            source: { ns: "media", id: "6f051c76-293f-41e5-9774-295a8f4b6f97" },
            recipe,
            detach: false,
            signal,
        });
        expect(result).toEqual(ok({ id: "8f5bbb78-1d10-4ed6-a05c-104584383ddd", widthPx: 800, heightPx: 600 }));
        expect(sent[0]?.body).toEqual({ recipe: "AQhAAEAAgACAAAA", detach: false });
        expect(sent[0]?.params).toEqual({ ns: "media", id: "6f051c76-293f-41e5-9774-295a8f4b6f97" });
    });

    it("refuses an answer that is not the declared shape rather than guessing", async () => {
        const { call } = stub(ok({ id: "x", width: "800", height: 600 }));
        const result = await submitEdit(call, { route: kRoute, source: { ns: "media", id: "x" }, recipe: centreCrop(), detach: true, signal });
        expect(result).toEqual(fail({ kind: "edit-answer" }));
    });

    it("hands a server's refusal back as it came", async () => {
        const refusal: HammerError = { kind: "server", code: "VALIDATION_FAILED", status: 400, requestId: null, fields: new Map([["id", "NOT_ALLOWED"]]) };
        const { call } = stub(fail(refusal));
        const result = await submitEdit(call, { route: kRoute, source: { ns: "media", id: "x" }, recipe: centreCrop(), detach: false, signal });
        expect(result).toEqual(fail(refusal));
    });
});

describe("reopenEdit", () => {
    it("opens an edit on its source, with its recipe", async () => {
        const { call, sent } = stub(ok({ source: "src", width: 2000, height: 1000, recipe: "AQhAAEAAgACAAAA" }));
        const result = await reopenEdit(call, { route: kRoute, subject: { ns: "media", id: "edit" }, limits: kLimits, signal });
        expect(sent[0]?.body).toBeNull();
        expect(result.ok).toBe(true);
        if (!result.ok) return;
        expect(result.value.source).toBe("src");
        expect(result.value.size).toEqual({ widthPx: 2000, heightPx: 1000 });
        expect(result.value.recipe?.crop).toEqual({ x: 16384, y: 16384, w: 32768, h: 32768 });
    });

    it("opens a plain image as its own source with nothing drawn", async () => {
        const { call } = stub(ok({ source: "img", width: 800, height: 600, recipe: null }));
        const result = await reopenEdit(call, { route: kRoute, subject: { ns: "media", id: "img" }, limits: kLimits, signal });
        expect(result).toEqual(ok({ source: "img", size: { widthPx: 800, heightPx: 600 }, recipe: null }));
    });

    it("refuses a stored recipe it cannot read rather than opening without it", async () => {
        // Opening the source with no recipe would look like an edit that lost
        // its work, and saving from there would silently discard it.
        const { call } = stub(ok({ source: "src", width: 2000, height: 1000, recipe: "AgQA" }));
        const result = await reopenEdit(call, { route: kRoute, subject: { ns: "media", id: "edit" }, limits: kLimits, signal });
        expect(result).toEqual(fail({ kind: "recipe", fault: "edit.format" }));
    });
});
