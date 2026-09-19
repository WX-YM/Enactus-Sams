import { defineConfig } from "vitest/config";

import base from "./vitest.config.js";

// The live suite, and only it.
//
// A separate configuration rather than a flag, because the two runs have
// different meanings: `npm run check` is what every commit passes, and this is
// what a person runs against a server they started. Mixing them would make the
// first one depend on a machine's environment, which is how a suite becomes
// something people learn to ignore.
//
// The resolver is the default run's, reused rather than restated: the live suite
// imports `hammer/state` the way an application does, and a second copy of that
// map is a second thing to forget to update.
//
// `mergeConfig` is deliberately NOT used. It concatenates arrays, so the
// merged `include` would be the default run's plus this one's — which is every
// suite in the repository, run against a server. It was, once, which is why this
// comment is here.
export default defineConfig({
    resolve: base.resolve,
    test: {
        include: ["tests/live/**/*.test.ts"],
        environment: "node",
        // A real network is the subject. A backoff this library honours — anvil
        // names a `Retry-After` and hammer waits exactly that long — is measured
        // in seconds rather than in the microtask turns a unit suite counts.
        testTimeout: 30_000,
        hookTimeout: 30_000,
        // One file at a time, against one server, with one session in one jar.
        // Parallel files would race each other's credential rotation, which is a
        // property this suite exists to observe rather than to cause.
        fileParallelism: false,
    },
});
