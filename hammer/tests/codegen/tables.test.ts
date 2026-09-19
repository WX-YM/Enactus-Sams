// The three tables the descriptor cannot carry, checked against the types it
// generates. Half of this file is type-level, and that half is the mechanism:
// the value of `Record` over `Partial<Record>` is that an addition server-side
// stops the build until somebody answers it, and a test that only ran would
// never see that.

import { describe, expect, it } from "vitest";

import type { ClassNames, Copy, Invalidations } from "hammer";

import type {
    ErrorCode,
    Locale,
    RouteId,
    ValidationReason,
} from "../testapp/api/hammer.generated.js";
import { kErrorCodeOk, kValidationReasonValues } from "../testapp/api/hammer.generated.js";
// The part union comes from the COMPONENT now, not from the consumer. That is
// the seam closing: until phase 5 the reference consumer declared the union it
// intended to satisfy, because there was no component to declare one.
import type { FormPart } from "hammer/dom";
import { formClasses } from "../testapp/app/classes.js";
import { copy } from "../testapp/app/copy.js";
import { invalidations } from "../testapp/app/invalidate.js";

describe("the copy table", () => {
    it("answers every code and every reason in every locale", () => {
        for (const locale of ["en", "ar"] as const) {
            for (const reason of Object.keys(kValidationReasonValues)) {
                if (reason === kErrorCodeOk) {
                    continue;
                }
                expect(copy[locale].reasons[reason as ValidationReason].length).toBeGreaterThan(0);
            }
            expect(copy[locale].errors.Unknown.length).toBeGreaterThan(0);
            expect(copy[locale].reasons.Unknown.length).toBeGreaterThan(0);
        }
    });

    it("says a 404 was not found, and never that it was refused", () => {
        // anvil answers a denied request on a stealth route with a
        // byte-identical 404. A client that renders a denial there hands back
        // the oracle the server spent a whole design removing — and this is the
        // one place in the repository where the words exist to be looked at.
        expect(copy.en.errors.NOT_FOUND.toLowerCase()).not.toContain("permission");
        expect(copy.en.errors.NOT_FOUND.toLowerCase()).not.toContain("access");
    });

    it("does not compile with a reason the server names and the table does not", () => {
        const { BREACHED: _dropped, ...withoutBreached } = copy.en.reasons;
        const incomplete: Copy<"en", ErrorCode, ValidationReason> = {
            en: {
                errors: copy.en.errors,
                // @ts-expect-error the map is total over ValidationReason. This
                // is the mechanism that stops a wire enumerator reaching a
                // reader in an interface that is otherwise entirely in Arabic.
                reasons: withoutBreached,
            },
        };
        expect(incomplete).toBeTruthy();
    });

    it("does not compile with a locale the server declares and the table does not", () => {
        // @ts-expect-error a locale added server-side is a compile error until
        // somebody writes the words for it.
        const incomplete: Copy<Locale, ErrorCode, ValidationReason> = { en: copy.en };
        expect(incomplete).toBeTruthy();
    });

    it("owes no sentence for the member that is not a failure", () => {
        // @ts-expect-error the success member is not in the failure union, so
        // an application never writes a sentence for a case no error surface
        // can reach.
        const success: ErrorCode = "OK";
        expect(success).toBe(kErrorCodeOk);
    });
});

describe("the class-name table", () => {
    it("is total over the parts the component declares", () => {
        expect(formClasses.label.length).toBeGreaterThan(0);
    });

    it("does not compile with a part the component declares and the table does not", () => {
        const { error: _dropped, ...withoutError } = formClasses;
        // @ts-expect-error an unlabelled or unstyled part is a defect that
        // ships from here to every consumer at once.
        const incomplete: ClassNames<FormPart> = withoutError;
        expect(incomplete).toBeTruthy();
    });

    it("does not compile with a part the table has and the component does not", () => {
        // The other direction, and it is worth having: a class name for a part
        // that no longer exists is dead styling nobody deletes, and the first
        // sign of it is usually a renamed part still being styled by its old
        // name somewhere else.
        // @ts-expect-error `legend` is not a part `FormPart` declares.
        const extra: ClassNames<FormPart> = { ...formClasses, legend: "tf-legend" };
        expect(extra).toBeTruthy();
    });
});

describe("the invalidation table", () => {
    it("is partial, because a read invalidates nothing", () => {
        // An entry for every route would be a table of empty arrays whose only
        // effect is to hide the routes that matter.
        const sparse: Invalidations<RouteId> = {};
        expect(sparse).toBeTruthy();
        expect(invalidations["content.delete"]).toEqual(["content.get"]);
    });

    it("does not compile with a route id the server does not carry", () => {
        // @ts-expect-error a mutation naming a resource the server retired is
        // an invalidation that silently stops happening, and nothing renders
        // wrongly until somebody notices a stale row.
        const retired: Invalidations<RouteId> = { "content.publish": ["content.get"] };
        expect(retired).toBeTruthy();
    });

    it("does not compile with a target the server does not carry", () => {
        // @ts-expect-error the same check, on the other side of the arrow.
        const retired: Invalidations<RouteId> = { "content.delete": ["content.list"] };
        expect(retired).toBeTruthy();
    });
});
