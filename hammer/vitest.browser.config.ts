import { defineConfig } from "vitest/config";

import base from "./vitest.config.js";

// The browser suite, and only it.
//
// A third configuration for the same reason there is a second: `npm run check`
// is what every commit passes, and this needs a Chromium on the machine. A suite
// that made the default run depend on a browser binary is a suite that turns
// `npm run check` into something people learn to skip.
//
// The tests here drive a browser from Node — they are not `environment:
// "browser"` runs. The page is a real page served over a real origin by
// `tests/browser/harness.ts`, because the policy under test is delivered as an
// HTTP header and a test runner's own harness page does not carry one.
//
// `mergeConfig` is deliberately not used, for the reason
// `vitest.live.config.ts` records: it concatenates arrays, so the merged
// `include` would be every suite in the repository, run through a browser.
export default defineConfig({
    resolve: base.resolve,
    test: {
        include: ["tests/browser/**/*.test.ts"],
        environment: "node",
        // Launching a browser, bundling a page and loading it is seconds, not
        // microtask turns.
        testTimeout: 60_000,
        hookTimeout: 60_000,
        // One browser at a time. Two of these runs share one session against one
        // server, and parallel files would race the credential rotation this
        // suite exists to observe rather than to cause.
        fileParallelism: false,
    },
});
