import { fileURLToPath } from "node:url";

import { defineConfig } from "vitest/config";

// Suites split by what they need, not by what they are about
// (docs/16-test-plan.md). `core` needs nothing at all, and that is the property
// worth protecting: the day it needs a document, the layering is wrong and this
// configuration is where it shows.
//
// The aliases mirror `tsconfig.json`'s `paths`, and they exist because the
// reference consumer imports hammer the way a REAL consumer does — `hammer/wire`
// and `hammer/state`, not a relative hop into `src/`. That is the whole point of
// `tests/testapp/`: a seam that can only be satisfied by reaching inside the
// library is not a seam. Type-checking alone would have let the specifier be a
// fiction, because `paths` is the compiler's map and not the runtime's.
//
// Longest first. `hammer/state` has to match before `hammer`, or a prefix match
// resolves every entry point to the core one.
const entry = (path: string): string => fileURLToPath(new URL(path, import.meta.url));

export default defineConfig({
    resolve: {
        alias: [
            { find: /^hammer\/react$/, replacement: entry("./src/react/index.ts") },
            { find: /^hammer\/state$/, replacement: entry("./src/state/index.ts") },
            { find: /^hammer\/chart$/, replacement: entry("./src/chart/index.ts") },
            { find: /^hammer\/dom$/, replacement: entry("./src/dom/index.ts") },
            { find: /^hammer\/wire$/, replacement: entry("./src/wire/index.ts") },
            { find: /^hammer$/, replacement: entry("./src/core/index.ts") },
        ],
    },
    // The reference consumer's React screens are `.tsx`, because that is what a
    // consumer writes. `automatic` matches `tsconfig.json`'s `react-jsx`, so the
    // suite and the type-check compile the same source the same way.
    esbuild: { jsx: "automatic" },
    test: {
        include: ["tests/**/*.test.ts", "tests/**/*.test.tsx"],
        // The live suite needs a running anvil and the browser suite needs a
        // Chromium, and both are excluded from the default run rather than
        // skipped inside it (`docs/16-test-plan.md`): a suite that quietly
        // passes with no server is a suite whose green means nothing.
        // `npm run test:live` and `npm run test:browser` are the only things
        // that run them.
        exclude: ["node_modules/**", "dist/**", "tests/live/**", "tests/browser/**"],
        environment: "node",
        restoreMocks: true,
    },
});
