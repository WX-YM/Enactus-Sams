#!/usr/bin/env bash
#
# Every TypeScript block in the documentation is a region of compiled source.
#
# `docs/02-getting-started.md` opened, for its whole life until this script, by
# saying its examples "cannot rot" because they are compiled as
# `tests/testapp/`. They were not. The testapp was compiled and the snippets were
# prose beside it, and four of them described an API this library has never had:
# a `createClient` taking a route table and three origins, a `call` taking a
# route id, a `useResource` taking a query, a `useSession` taking nothing. A
# reader following that document wrote code that did not compile, and the first
# application to consume hammer is how anyone found out.
#
# The claim was the defect. Not the drift — the drift is what happens to every
# document — but the sentence asserting it could not happen, which is exactly the
# sentence that stops anybody checking. So the claim is made TRUE here instead of
# being removed: a fenced block names a region of a file the type-checker reads,
# this script lifts that region out and diffs it, and `--write` puts it back.
#
# --- why transclusion rather than compiling the blocks ----------------------
#
# Extracting fenced blocks into a scratch file and running `tsc` over them was
# the obvious design and it is worse in the way that matters: a snippet in a
# guide is a fragment. It references a `signal` and a `locale` and a `show()`
# that the surrounding paragraph established, so making it compile on its own
# means either inventing a preamble per block — which is source nobody reads and
# nothing else checks — or writing snippets contorted into self-sufficiency,
# which is a document optimised for a build step rather than for a reader.
#
# A region is the other way round. The example is ordinary source in the
# reference consumer, with the imports and the surrounding functions it needs; it
# is compiled by `npm run typecheck` for the same reason everything else there
# is, and the document holds a copy the build refuses to let drift.
#
# --- the escape hatch, and why it has to carry a reason ---------------------
#
# A signature sketch is not a call site: `renderX(mount, options): Mounted` is
# the shape of a contract and there is no file it could be lifted from. Those are
# spelled `ts sketch: <why>`, which is the same bargain as `@ts-expect-error`
# with a reason on the line — the hatch exists, it is visible in review, and it
# costs a sentence to use. A bare ```ts fence is a failure, because a block that
# names nothing is exactly the block this whole mechanism exists to catch.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

mode="check"
if [ "${1:-}" = "--write" ]; then
    mode="write"
fi

MODE="$mode" node - <<'NODE_EOF'
import { readFileSync, writeFileSync } from "node:fs";
import { readdirSync } from "node:fs";

const write = process.env["MODE"] === "write";

// Every document a consumer reads. README.md is in it because a reader reaches
// for it before the guide, and a wrong example there is read more often than a
// wrong example anywhere else.
const documents = ["README.md", ...readdirSync("docs").filter((name) => name.endsWith(".md")).sort().map((name) => `docs/${name}`)];

const failures = [];
const notes = [];

function fail(where, message, remedy) {
    failures.push({ where, message, remedy });
}

// --- the regions a source file offers ---------------------------------------

const regions = new Map();     // "path#name" -> { lines, path, name }
const claimed = new Set();
const scanned = new Set();

// Once per file, not once per unresolved key. Re-reading would re-register every
// region it holds, and the second registration reports itself as a duplicate —
// so a document naming one region that does not exist would bury its own error
// under one spurious failure per region in the file.
function readRegions(path) {
    if (scanned.has(path)) {
        return;
    }
    const source = readFileSync(path, "utf8").split("\n");
    scanned.add(path);
    const open = [];
    for (let i = 0; i < source.length; i += 1) {
        const line = source[i] ?? "";
        const begins = /^\s*\/\/ #region (\S+)\s*$/.exec(line);
        if (begins !== null) {
            open.push({ name: begins[1] ?? "", from: i + 1 });
            continue;
        }
        if (!/^\s*\/\/ #endregion\s*$/.test(line)) {
            continue;
        }
        const started = open.pop();
        if (started === undefined) {
            fail(`${path}:${i + 1}`, "#endregion with no #region above it");
            continue;
        }
        const key = `${path}#${started.name}`;
        if (regions.has(key)) {
            // Two regions of one name is a document transcluding whichever the
            // reader happens to find first, which is not a property a check can
            // be built on.
            fail(`${path}:${i + 1}`, `a second region named ${started.name}`);
            continue;
        }
        regions.set(key, {
            path,
            name: started.name,
            lines: dedent(source.slice(started.from, i)),
        });
    }
    for (const started of open) {
        fail(`${path}:${started.from}`, `#region ${started.name} is never closed`);
    }
}

// The body as the document should show it: the common indentation removed, so a
// region taken out of a function reads as source rather than as an excerpt.
// Blank lines do not count towards the common prefix — a trailing-whitespace
// convention would otherwise decide how far every block is indented.
function dedent(lines) {
    let shortest = Infinity;
    for (const line of lines) {
        if (line.trim().length === 0) {
            continue;
        }
        shortest = Math.min(shortest, line.length - line.trimStart().length);
    }
    if (!Number.isFinite(shortest) || shortest === 0) {
        return trimEnds(lines);
    }
    return trimEnds(lines.map((line) => (line.trim().length === 0 ? "" : line.slice(shortest))));
}

function trimEnds(lines) {
    let first = 0;
    let last = lines.length;
    while (first < last && (lines[first] ?? "").trim().length === 0) first += 1;
    while (last > first && (lines[last - 1] ?? "").trim().length === 0) last -= 1;
    return lines.slice(first, last);
}

// --- the blocks a document holds ---------------------------------------------

// The fence, its info string, and the lines between it and its close. Only the
// TypeScript ones are this script's business: a `sh` block is a command, a
// `jsonc` block is a descriptor fragment, and neither has a compiled source to
// be lifted from.
const kTypeScript = new Set(["ts", "tsx"]);

function blocksIn(text) {
    const lines = text.split("\n");
    const found = [];
    let open = null;
    for (let i = 0; i < lines.length; i += 1) {
        const line = lines[i] ?? "";
        if (open !== null) {
            if (line === "```") {
                found.push({ ...open, bodyTo: i });
                open = null;
            }
            continue;
        }
        const fence = /^```(\S+)(?: +(.*))?$/.exec(line);
        if (fence === null) {
            continue;
        }
        open = {
            language: fence[1] ?? "",
            info: (fence[2] ?? "").trim(),
            fenceAt: i,
            bodyFrom: i + 1,
        };
    }
    return { lines, found };
}

for (const document of documents) {
    const text = readFileSync(document, "utf8");
    const { lines, found } = blocksIn(text);
    const rewrites = [];

    for (const block of found) {
        if (!kTypeScript.has(block.language)) {
            continue;
        }
        const at = `${document}:${block.fenceAt + 1}`;

        if (block.info.length === 0) {
            fail(
                at,
                "a TypeScript block naming no source",
                'name the region it comes from — ```ts path/to/file.ts#region — or, for a signature rather than a call site, ```ts sketch: <why>',
            );
            continue;
        }

        if (block.info.startsWith("sketch:")) {
            if (block.info.slice("sketch:".length).trim().length === 0) {
                fail(at, "a sketch with no reason", "say what it is a sketch of");
            }
            continue;
        }

        const named = /^(\S+)#(\S+)$/.exec(block.info);
        if (named === null) {
            fail(at, `an info string this script cannot read: ${block.info}`, "expected path/to/file.ts#region");
            continue;
        }

        const path = named[1] ?? "";
        const key = `${path}#${named[2] ?? ""}`;
        try {
            readRegions(path);
        } catch {
            fail(at, `${path} does not exist`);
            continue;
        }
        const region = regions.get(key);
        if (region === undefined) {
            fail(at, `${path} has no region named ${named[2] ?? ""}`, `add // #region ${named[2] ?? ""} around it`);
            continue;
        }
        claimed.add(key);

        const shown = lines.slice(block.bodyFrom, block.bodyTo);
        if (shown.join("\n") === region.lines.join("\n")) {
            continue;
        }
        if (write) {
            rewrites.push({ from: block.bodyFrom, to: block.bodyTo, lines: region.lines });
            continue;
        }
        fail(
            at,
            `has drifted from ${key}`,
            "tools/check-docs.sh --write",
        );
    }

    if (write && rewrites.length !== 0) {
        // Back to front, so an earlier rewrite does not move a later one's
        // offsets.
        let out = lines;
        for (const rewrite of rewrites.reverse()) {
            out = [...out.slice(0, rewrite.from), ...rewrite.lines, ...out.slice(rewrite.to)];
        }
        writeFileSync(document, out.join("\n"));
        notes.push(`${document}: ${rewrites.length} block(s) rewritten`);
    }
}

// A region nothing shows is not a failure — it folds in an editor and it may be
// waiting for a document to be written — but it is worth saying, because the
// alternative is a file slowly accumulating markers for nobody.
for (const key of regions.keys()) {
    if (!claimed.has(key)) {
        notes.push(`unused region: ${key}`);
    }
}

for (const note of notes) {
    process.stderr.write(`note  ${note}\n`);
}

if (failures.length !== 0) {
    for (const failure of failures) {
        process.stderr.write(`FAIL  ${failure.where}: ${failure.message}\n`);
        if (failure.remedy !== undefined) {
            process.stderr.write(`      ${failure.remedy}\n`);
        }
    }
    process.stderr.write(`docs: ${failures.length} violation(s)\n`);
    process.exit(1);
}

process.stdout.write(write ? "docs: written\n" : "docs: clean\n");
NODE_EOF
