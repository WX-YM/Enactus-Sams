// What an upload is refused for, asserted in the layer that owns the bounds.
//
// It lives here rather than beside the transport because three layers need the
// identical answer and only this one is reachable from all of them: the
// component that refuses a dropped file may not import `hammer/wire`
// (`tools/check-layering.sh`). A second copy of these bounds is the shape of
// defect where a control accepts what the transport then refuses, and the person
// watches a file disappear with no reason attached to it.

import { describe, expect, it } from "vitest";

import { checkUpload } from "../../src/core/upload_bounds.js";

const kLimits = { maxBytes: 1024, accept: ["image/jpeg", "image/png"] };

describe("what is refused before the first byte", () => {
    it("accepts a file inside the cap and of a declared type", () => {
        expect(checkUpload({ size: 1024, type: "image/jpeg" }, kLimits)).toEqual({
            ok: true,
            value: undefined,
        });
    });

    // The check saves an upload that was going to be refused after the last
    // byte, on a connection where forty megabytes is minutes rather than
    // seconds. It is not the enforcement — anvil enforces during the read.
    it("refuses a file past the descriptor's cap", () => {
        expect(checkUpload({ size: 1025, type: "image/jpeg" }, kLimits)).toEqual({
            ok: false,
            error: { kind: "client", cause: "too-large", retryAfterMs: null },
        });
    });

    it("refuses a type the application does not accept", () => {
        expect(checkUpload({ size: 10, type: "application/pdf" }, kLimits)).toMatchObject({
            ok: false,
            error: { cause: "unsupported-media" },
        });
    });

    // A picker that returned a directory, or a file moved between the choice
    // and the read. Neither is an upload.
    it("refuses an empty file", () => {
        expect(checkUpload({ size: 0, type: "image/jpeg" }, kLimits)).toMatchObject({
            ok: false,
            error: { cause: "bad-parameter" },
        });
    });

    // An empty accept list means the server decides, which is where the
    // decision lives anyway: the descriptor carries no media types.
    it("accepts any type where the application declared none", () => {
        expect(
            checkUpload({ size: 10, type: "application/x-thing" }, { maxBytes: 1024, accept: [] }),
        ).toMatchObject({
            ok: true,
        });
    });
});
