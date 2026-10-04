// Bootstrap for `node --import`, standing in for vitest's resolver and its
// esbuild-powered transform (CLAUDE.md §12, docs/15-tasks.md Phase 8 B1).
//
// vitest did two things this repository's tests depend on and neither is
// built into Node: it resolved the bare `hammer/*` specifiers a real consumer
// writes to this repository's own `src/`, and it compiled `.ts`/`.tsx` on the
// fly so a test file never needed a build step to run. `module.registerHooks`
// (synchronous customisation hooks, Node >=22.15) is the platform's seam for
// exactly that, and this file is the whole of what runs through it — every
// `node:test` binding tests import lives in `test.js` instead, so a future
// swap of either half touches one file each.
//
// `registerHooks` and not `module.register`: the async, worker-thread hook
// API works on older Node, but it cannot share this process's `esbuild`
// module instance, and every hook call would cross a thread boundary for a
// transform that is pure CPU work with nothing to await. A clear failure
// naming the version required is better than silently falling back to the
// slower API and leaving the reason unwritten.

import { registerHooks } from "node:module";
import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { transformSync } from "esbuild";

if (typeof registerHooks !== "function") {
    throw new Error(
        "hammer's test runner needs module.registerHooks, added in Node 22.15 " +
            `as a stable synchronous hook API. Running Node ${process.version}. ` +
            "Upgrade to Node 22.15 or later.",
    );
}

const repoRoot = new URL("../../", import.meta.url);
const pkg = JSON.parse(readFileSync(new URL("../../package.json", import.meta.url), "utf8"));

// The bare-specifier map `hammer/*` resolves through, derived from
// package.json's own `exports` rather than duplicated by hand — the same
// reasoning `tools/check-bundle-budget.sh` gives for deriving its own alias
// map the same way. Filtering to a target ending in `/index.js` excludes the
// codegen CLI's entry (`./dist/codegen/cli.js`, a build-time binary and not
// one of `tsconfig.json`'s `paths`), and leaves exactly the six specifiers
// `tsconfig.json` and the deleted `vitest.config.ts` both mapped: `hammer`,
// `hammer/wire`, `hammer/state`, `hammer/dom`, `hammer/chart`, `hammer/react`.
//
// Longest specifier first is not needed here the way it was in the vitest
// config's regex list: this is exact-string lookup in a Map, not a series of
// prefix-matching patterns, so "hammer/state" and "hammer" never compete.
const bareSpecifiers = new Map();
for (const [subpath, target] of Object.entries(pkg.exports ?? {})) {
    if (typeof target !== "string" || !target.startsWith("./dist/") || !target.endsWith("/index.js")) {
        continue;
    }
    const specifier = subpath === "." ? pkg.name : `${pkg.name}/${subpath.slice("./".length)}`;
    const source = `./src/${target.slice("./dist/".length, -".js".length)}.ts`;
    bareSpecifiers.set(specifier, new URL(source, repoRoot).href);
}

// A `.js`-suffixed relative specifier whose target does not exist is written
// against a `.ts`/`.tsx` source the same way every consumer's bundler resolves
// one: hammer's own source imports its siblings with the extension the
// COMPILED output will have, per `verbatimModuleSyntax`, not the one the file
// on disk has right now. The query string (if any) is carried onto whichever
// extension resolves, unmodified — it is not part of the extension swap, it
// is the reason a second import of the same file is a second module instance
// (`tests/dom/sanitized.test.ts`, replacing `vi.resetModules`).
function withExtension(fileUrl, extension) {
    const { href, search } = fileUrl;
    const withoutSearch = search ? href.slice(0, -search.length) : href;
    return withoutSearch.slice(0, -".js".length) + extension + search;
}

function resolve(specifier, context, nextResolve) {
    const directTarget = bareSpecifiers.get(specifier);
    if (directTarget !== undefined) {
        return { url: directTarget, shortCircuit: true };
    }

    if ((specifier.startsWith("./") || specifier.startsWith("../")) && context.parentURL) {
        const asWritten = new URL(specifier, context.parentURL);
        if (asWritten.pathname.endsWith(".js") && !existsSync(fileURLToPath(asWritten))) {
            for (const extension of [".ts", ".tsx"]) {
                const candidate = withExtension(asWritten, extension);
                if (existsSync(fileURLToPath(candidate))) {
                    return { url: candidate, shortCircuit: true };
                }
            }
        }
    }

    return nextResolve(specifier, context);
}

function load(url, context, nextLoad) {
    const withoutSearch = url.split("?")[0];
    if (withoutSearch.endsWith(".ts") || withoutSearch.endsWith(".tsx")) {
        const path = fileURLToPath(url);
        const source = readFileSync(path, "utf8");
        // `jsx: "automatic"` matches `tsconfig.json`'s `react-jsx`, so the suite
        // and the type-check compile the same source the same way — carried
        // over from `vitest.config.ts`'s own comment to the same effect.
        const { code } = transformSync(source, {
            loader: withoutSearch.endsWith(".tsx") ? "tsx" : "ts",
            format: "esm",
            jsx: "automatic",
            target: "es2022",
            sourcefile: path,
            sourcemap: "inline",
        });
        return { format: "module", source: code, shortCircuit: true };
    }

    return nextLoad(url, context);
}

registerHooks({ resolve, load });
