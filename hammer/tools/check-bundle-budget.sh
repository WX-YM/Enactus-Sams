#!/usr/bin/env bash
#
# Every entry point has a declared gzipped ceiling (CLAUDE.md §2.1).
#
# A kilobyte of JavaScript is not a kilobyte of transfer. It is downloaded,
# decompressed, parsed, compiled and held, and on a mid-range phone the
# parse-and-compile alone runs at roughly 1 ms per kilobyte of UNCOMPRESSED
# script — so the two numbers printed below are two different costs and both are
# real. Three hundred kilobytes is a third of a second in which the device is
# doing nothing a person can see.
#
# The ceilings are in `tools/bundle-budget.json` because a budget on a dashboard
# is a budget nobody enforces. They are raised only in a commit whose body says
# what bought the bytes; a ceiling raised in the same commit as the code that
# exceeded it is not a ceiling.
#
# --- what is measured, and why it is measured this way -----------------------
#
# Each entry point is bundled SELF-CONTAINED: everything it imports from the
# layers below it is inlined, because that is what an application that imports it
# actually downloads. Marking the sibling entry points external would measure a
# number no consumer ever pays — `hammer/state` reaches through `hammer/wire` to
# `hammer`, and a consumer of the first ships all three.
#
# `peerDependencies` are external, and only those. A peer is installed and
# shipped by the consumer, so counting React against this library's ceiling would
# measure somebody else's bytes.
#
# gzip at level 9 rather than brotli: it is the floor every CDN and every origin
# already does, so the number is one no deployment can be worse than.
#
# --- the ceilings must cover the exports map ---------------------------------
#
# The check that matters most here is not any single number. It is that every
# subpath in `exports` is either budgeted or declared build-time only: an entry
# point added with no ceiling is an entry point shipping unmeasured, and it would
# be discovered by a p95 device rather than by this script.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [ ! -d node_modules/esbuild ]; then
    printf 'FAIL  node_modules/esbuild is missing. Run `npm ci`.\n' >&2
    exit 1
fi

node - <<'NODE_EOF'
import { readFileSync } from "node:fs";
import { gzipSync } from "node:zlib";

import { build } from "esbuild";

const pkg = JSON.parse(readFileSync("package.json", "utf8"));
const budget = JSON.parse(readFileSync("tools/bundle-budget.json", "utf8"));

const ceilings = budget.gzipCeilingBytes ?? {};
const buildTimeOnly = new Set(budget.buildTimeOnly ?? []);
const exported = Object.keys(pkg.exports ?? {});

let failures = 0;

// The budget and the exports map have to name the same set. Both directions:
// an unbudgeted entry point ships unmeasured, and a ceiling for a subpath that
// no longer exists is a number nobody is reading.
for (const subpath of exported) {
    if (subpath in ceilings || buildTimeOnly.has(subpath)) continue;
    console.error(`FAIL  "${subpath}" is in exports with no ceiling and no build-time-only note`);
    failures++;
}
for (const subpath of [...Object.keys(ceilings), ...buildTimeOnly]) {
    if (exported.includes(subpath)) continue;
    console.error(`FAIL  the budget names "${subpath}", which is not in exports`);
    failures++;
}

// `./dist/state/index.js` is what the consumer resolves; `src/state/index.ts` is
// what it was built from. Derived rather than listed, so a new entry point is
// measured by having been exported at all.
function sourceOf(target) {
    if (typeof target !== "string" || !target.startsWith("./dist/") || !target.endsWith(".js")) {
        return null;
    }
    return `src/${target.slice("./dist/".length, -".js".length)}.ts`;
}

// A peer is the consumer's to install and to ship.
const external = Object.keys(pkg.peerDependencies ?? {});

// The specifiers the entry points use on each other, mapped to source. A
// self-contained bundle needs them resolved rather than left as imports, and
// this is the same map `tsconfig.json`'s `paths` carries and
// `tests/support/register.mjs` derives from `exports` the same way.
const alias = {};
for (const [subpath, target] of Object.entries(pkg.exports ?? {})) {
    const source = sourceOf(target);
    if (source === null) continue;
    alias[subpath === "." ? pkg.name : `${pkg.name}/${subpath.slice("./".length)}`] = source;
}

const rows = [];
for (const subpath of Object.keys(ceilings)) {
    const source = sourceOf(pkg.exports?.[subpath]);
    if (source === null) {
        console.error(`FAIL  "${subpath}" does not resolve to a built source file`);
        failures++;
        continue;
    }

    const result = await build({
        entryPoints: [source],
        bundle: true,
        minify: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        write: false,
        outdir: "dist/never-written",
        alias,
        external,
        // Prose preserved from a licence banner is bytes a device downloads and
        // a parser walks past.
        legalComments: "none",
        logLevel: "silent",
    });

    const raw = Buffer.from(result.outputFiles.map((file) => file.text).join(""), "utf8");
    const gzipBytes = gzipSync(raw, { level: 9 }).length;
    const ceiling = ceilings[subpath];
    rows.push({ subpath, gzipBytes, rawBytes: raw.length, ceiling });

    if (gzipBytes > ceiling) {
        console.error(
            `FAIL  ${subpath} is ${gzipBytes} B gzipped, over its ${ceiling} B ceiling by ${gzipBytes - ceiling} B`,
        );
        failures++;
    }
}

const width = Math.max(...rows.map((row) => row.subpath.length), 4);
for (const row of rows) {
    const used = Math.round((row.gzipBytes / row.ceiling) * 100);
    // The raw size is printed beside the gzipped one because it is the number
    // the parse-and-compile cost is proportional to, and it is not the number
    // the ceiling is written against.
    console.log(
        `  ${row.subpath.padEnd(width)}  ${String(row.gzipBytes).padStart(6)} / ${String(row.ceiling).padStart(6)} B gz` +
            `  (${String(used).padStart(3)}%)  ${String(row.rawBytes).padStart(7)} B parsed`,
    );
}

if (failures !== 0) {
    console.error(`bundle budget: ${failures} violation(s)`);
    console.error("      Raise a ceiling only in a commit whose body says what bought the bytes.");
    process.exit(1);
}
console.log("bundle budget: clean");
NODE_EOF
