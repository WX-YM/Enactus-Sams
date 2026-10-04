// Image edits against a live anvil: the one run where the client's recipe codec
// and the server's meet over the wire rather than over a shared fixture.
//
// The golden vectors already prove the two codecs agree byte for byte. What
// only a live server can show is the rest: that a recipe this client encodes is
// one anvil renders at the size this client planned, that the same edit twice is
// one object, that an edit reopens on its source, and that a refusal the client
// did not pre-empt comes back with exactly the field and reason `kFaultWire`
// says it will.
//
// As the superadmin, because the reference editor account holds ContentRead and
// nothing else, and an edit needs the upload authority.

import { afterAll, beforeAll, describe, expect, it } from "../support/test.js";

import { editCall, reopenEdit, submitEdit } from "../../src/edit/submit.js";
import { encodeRecipe, kEmptyRecipe, kFaultWire } from "../../src/edit/recipe.js";
import { kEditLimits, routeMediaEdit, routeMediaEditState } from "../testapp/api/hammer.generated.js";
import { signIn } from "../testapp/app/state.js";
import type { LiveRun } from "./harness.js";
import { liveRun, liveSuperadmin, seededMedia, signal } from "./harness.js";

const who = liveSuperadmin();
let run: LiveRun;
let seeded: { readonly ns: string; readonly id: string } | null = null;

beforeAll(async () => {
    run = liveRun();
    seeded = await seededMedia();
    if (who !== null && seeded !== null) {
        const signedIn = await signIn(run.state, who, signal());
        if (!signedIn.ok) throw new Error("the superadmin could not sign in");
    }
});

afterAll(() => {
    run?.close();
});

const kCentreCrop = { ...kEmptyRecipe, crop: { x: 16384, y: 16384, w: 32768, h: 32768 } };

describe("editing an image, live", () => {
    it.runIf(who !== null)("opens a stored image as its own source, at its own size", async () => {
        expect(seeded).not.toBeNull();
        if (seeded === null) return;
        const opened = await reopenEdit(editCall(run.state.api), {
            route: routeMediaEditState,
            subject: seeded,
            limits: kEditLimits,
            signal: signal(),
        });
        expect(opened.ok).toBe(true);
        if (!opened.ok) return;
        expect(opened.value.source).toBe(seeded.id);
        expect(opened.value.size).toEqual({ widthPx: 1600, heightPx: 1200 });
        expect(opened.value.recipe).toBeNull();

        // Both routes reached this holder in the table the server scoped for it.
        // Scoping them AWAY from a holder without the upload authority is the
        // per-holder projection `anvil.test.ts` already asserts; a second
        // account signing in here would spend the run's login budget for it.
        const view = await run.state.session.load(signal());
        expect(view?.routes.has("media.edit")).toBe(true);
        expect(view?.routes.has("media.edit_state")).toBe(true);
    });

    it.runIf(who !== null)("renders at the size this client planned, and the same edit twice is one object", async () => {
        if (seeded === null) return;
        const encoded = encodeRecipe(kCentreCrop, kEditLimits, { widthPx: 1600, heightPx: 1200 });
        expect(encoded.ok).toBe(true);
        if (!encoded.ok) return;
        const call = editCall(run.state.api);
        const first = await submitEdit(call, { route: routeMediaEdit, source: seeded, recipe: encoded.value, detach: false, signal: signal(30_000) });
        expect(first.ok).toBe(true);
        if (!first.ok) return;
        expect([first.value.widthPx, first.value.heightPx]).toEqual([encoded.value.plan.outWidthPx, encoded.value.plan.outHeightPx]);

        const again = await submitEdit(call, { route: routeMediaEdit, source: seeded, recipe: encoded.value, detach: false, signal: signal(30_000) });
        expect(again.ok).toBe(true);
        if (again.ok) expect(again.value.id).toBe(first.value.id);

        // And it reopens on its SOURCE, with the recipe this client sent.
        const reopened = await reopenEdit(call, {
            route: routeMediaEditState,
            subject: { ns: seeded.ns, id: first.value.id },
            limits: kEditLimits,
            signal: signal(),
        });
        expect(reopened.ok).toBe(true);
        if (!reopened.ok) return;
        expect(reopened.value.source).toBe(seeded.id);
        expect(reopened.value.recipe?.crop).toEqual(kCentreCrop.crop);

        // An edit of that edit is refused: a re-edit is the source plus the
        // whole recipe.
        const flip = encodeRecipe({ ...kEmptyRecipe, flip: true }, kEditLimits, reopened.value.size);
        if (!flip.ok) return;
        const chained = await submitEdit(call, {
            route: routeMediaEdit,
            source: { ns: seeded.ns, id: first.value.id },
            recipe: flip.value,
            detach: false,
            signal: signal(30_000),
        });
        expect(chained.ok).toBe(false);
        if (!chained.ok && chained.error.kind === "server") {
            expect(chained.error.code).toBe("VALIDATION_FAILED");
            expect(chained.error.fields?.get("id")).toBe("NOT_ALLOWED");
        }
    });

    it.runIf(who !== null)("refuses what the client would have refused, with the field and reason it predicted", async () => {
        if (seeded === null) return;
        // A crop a tenth of the picture wide: 160 px, under the narrowest rung.
        // Sent past the client's own check, to see the server's answer to it.
        const tooSmall = "AQgAAAAAGZn__wA";
        const answered = await editCall(run.state.api)(routeMediaEdit, seeded, { recipe: tooSmall, detach: false }, signal());
        expect(answered.ok).toBe(false);
        if (answered.ok || answered.error.kind !== "server") return;
        expect(answered.error.code).toBe("VALIDATION_FAILED");
        const wire = kFaultWire["edit.too_small"];
        expect(answered.error.fields?.get(wire.field)).toBe(wire.reason);

        // And the client, given the same recipe, refuses it with that fault.
        const local = encodeRecipe({ ...kEmptyRecipe, crop: { x: 0, y: 0, w: 6553, h: 65535 } }, kEditLimits, { widthPx: 1600, heightPx: 1200 });
        expect(local.ok).toBe(false);
        if (!local.ok) expect(local.error.fault).toBe("edit.too_small");
    });

    it.runIf(who !== null)("makes a detached edit a new object every time, linked to nothing", async () => {
        if (seeded === null) return;
        const encoded = encodeRecipe({ ...kEmptyRecipe, flip: true }, kEditLimits, { widthPx: 1600, heightPx: 1200 });
        if (!encoded.ok) return;
        const call = editCall(run.state.api);
        const one = await submitEdit(call, { route: routeMediaEdit, source: seeded, recipe: encoded.value, detach: true, signal: signal(30_000) });
        const two = await submitEdit(call, { route: routeMediaEdit, source: seeded, recipe: encoded.value, detach: true, signal: signal(30_000) });
        expect(one.ok && two.ok).toBe(true);
        if (!one.ok || !two.ok) return;
        expect(one.value.id).not.toBe(two.value.id);
        const reopened = await reopenEdit(call, { route: routeMediaEditState, subject: { ns: seeded.ns, id: one.value.id }, limits: kEditLimits, signal: signal() });
        expect(reopened.ok && reopened.value.source).toBe(one.value.id);
    });
});
