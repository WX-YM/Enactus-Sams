// The adapter boundary, asserted as bytes and as edges.
//
// `docs/15-tasks.md` §Phase 6 states the rule as a sentence — "the React entry
// point contains no logic of its own; it binds stores" — and a sentence is not
// a check. Behaviour arrives in an adapter one helpful commit at a time: a
// retry here, a default there, a bit of caching because the hook already had the
// value. Each one is invisible in review and each one is a second
// implementation of something the store below already does, reachable only by
// consumers who chose this framework.
//
// Two things make it mechanical.
//
//   THE SIZE. An adapter that binds stores is a few hundred bytes. One that
//   decides things is not, and the ceiling is what says so out loud.
//
//   THE EDGES. Every import this layer makes of a layer below is `import type`
//   except one, and a type edge is erased — so there is nothing below this layer
//   it could be calling. The exception is named below and has to stay named: a
//   second one is the commit where a hook started doing something.

import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";

import { build } from "esbuild";
import { describe, expect, it } from "../support/test.js";

const kSource = new URL("../../src/react/", import.meta.url);

// `hammer/react` is budgeted at 3 KB gzipped (`docs/15-tasks.md` §Phase 7),
// which is what a consumer is promised. This is a third of that, because the
// promise and the property are different claims: 3 KB is what an application may
// pay, and a kilobyte is what binding five stores costs. Raising this number is
// the review conversation the rule above exists to force, and the commit that
// raises it says what bought it.
const kCeilingGzipBytes = 1024;

// The one run-time import this layer makes of a layer below, and why it is
// allowed to exist: `useSyncExternalStore` compares snapshots by identity, so
// the state a resource has before its subscription exists has to be the SAME
// object the store itself publishes, not a copy of the same shape. A copy is an
// infinite render loop.
const kAllowedValueImports = new Set(["kResourceLoading"]);

async function bundled(): Promise<string> {
    const result = await build({
        entryPoints: [fileURLToPath(new URL("index.ts", kSource))],
        bundle: true,
        minify: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        // The peer. It is the consumer's copy and is never counted against this
        // budget — bundling it would measure React.
        external: ["react"],
        write: false,
        outdir: fileURLToPath(new URL("../../dist/never-written/", import.meta.url)),
        legalComments: "none",
        logLevel: "silent",
    });
    return result.outputFiles.map((file) => file.text).join("");
}

const kBundle = await bundled();

describe("the react adapter's size", () => {
    it("is under the ceiling that says it binds rather than decides", () => {
        const gzipped = gzipSync(Buffer.from(kBundle, "utf8"), { level: 9 }).length;
        expect(gzipped).toBeLessThanOrEqual(kCeilingGzipBytes);
    });

    it("brings nothing with it but react", () => {
        // Every specifier the built module still imports from. A bundled entry
        // point has exactly the externals in it, so anything else here is a
        // dependency an application would have to install to use the adapter —
        // which `package.json` promises it does not have.
        const specifiers = [...kBundle.matchAll(/from\s*"([^"]+)"/g)].map((match) => match[1]);
        expect(new Set(specifiers)).toEqual(new Set(["react"]));
    });
});

describe("the react adapter's edges", () => {
    const files = readdirSync(kSource).filter((name) => name.endsWith(".ts"));

    it("has source to check", () => {
        expect(files.length).toBeGreaterThan(0);
    });

    // The load-bearing one. A `import type` edge is erased, so a layer that
    // reaches below itself only through types is a layer that cannot be running
    // anything down there.
    it("imports the layers below it for their types and one constant", () => {
        const valueImports: string[] = [];

        for (const name of files) {
            const source = readFileSync(new URL(name, kSource), "utf8");
            for (const match of source.matchAll(
                /^import\s+(type\s+)?\{([^}]*)\}\s*from\s*"([^"]+)"/gm,
            )) {
                const isType = match[1] !== undefined;
                const specifier = match[3] ?? "";
                // `react` is the peer this layer exists to bind to, and its own
                // imports are not an edge into hammer.
                if (isType || specifier === "react") {
                    continue;
                }
                for (const imported of (match[2] ?? "").split(",")) {
                    const named = imported.trim().split(/\s+as\s+/)[0]?.trim() ?? "";
                    // A re-export inside the layer is not an edge out of it.
                    if (named === "" || specifier.startsWith("./")) {
                        continue;
                    }
                    valueImports.push(named);
                }
            }
        }

        expect(new Set(valueImports)).toEqual(kAllowedValueImports);
    });

    // The other direction, and the reason the layering script exists: nothing
    // below may reach up. A store that imported a hook would make `hammer/state`
    // unusable without React, which is the whole premise of the adapter being
    // separate.
    it("is imported by no layer below it", () => {
        for (const layer of ["core", "wire", "state", "dom", "chart"]) {
            const directory = new URL(`../../src/${layer}/`, import.meta.url);
            for (const name of readdirSync(directory, { recursive: true, encoding: "utf8" })) {
                if (!name.endsWith(".ts")) {
                    continue;
                }
                const source = readFileSync(new URL(name, directory), "utf8");
                expect(source).not.toMatch(/from\s*"[^"]*react/);
            }
        }
    });
});
