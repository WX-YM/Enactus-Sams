// The core suite runs with no environment at all, and this is the test that
// notices when that stops being true.
//
// It is not pedantry about globals: `hammer` is a separate entry point because a
// layer that cannot reach a document is a layer whose validators cannot
// accidentally render, and because a suite that needs no environment is fast
// enough to run on every save. Both properties are lost by a one-line change to
// vitest.config.ts, in a commit about something else, and nothing else in the
// repository would report it (docs/16-test-plan.md, Structure).

import { describe, expect, it } from "vitest";

describe("the core suite needs nothing", () => {
    it("runs with no document", () => {
        expect(typeof globalThis.document).toBe("undefined");
        expect(typeof globalThis.window).toBe("undefined");
    });
});
