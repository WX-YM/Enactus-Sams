// The core suite runs with no environment at all, and this is the test that
// notices when that stops being true.
//
// It is not pedantry about globals: `hammer` is a separate entry point because a
// layer that cannot reach a document is a layer whose validators cannot
// accidentally render, and because a suite that needs no environment is fast
// enough to run on every save. Both properties are lost by a stray import of
// something that reaches for `document` landing in this directory, in a commit
// about something else, and nothing else in the repository would report it
// (docs/16-test-plan.md, Structure).
//
// Before Phase 8 B2 the thing to land by accident was
// `import "../support/happy_dom_env.js";`, a per-file environment import every
// DOM test file carried explicitly. That file is gone: the DOM and React suite
// now runs in a real Chromium page, driven by `tests/dom/in_browser.test.ts`
// from a hardcoded file list rather than a self-declared import, so this
// module has nothing of that shape left to guard against by name — but the
// property this test asserts is the same one either way, and this repository's
// own history (`docs/15-tasks.md` §Phase 8 B1/B2) is the reason it still gets
// its own file rather than being folded into a comment.

import { describe, expect, it } from "../support/test.js";

describe("the core suite needs nothing", () => {
    it("runs with no document", () => {
        expect(typeof globalThis.document).toBe("undefined");
        expect(typeof globalThis.window).toBe("undefined");
    });
});
