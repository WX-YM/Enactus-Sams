#!/usr/bin/env bash
#
# Zero runtime dependencies is a security control, not minimalism (CLAUDE.md §5).
#
# This library runs inside an authenticated session, in the same realm as the
# cookies it is careful never to read. Every runtime dependency is an npm account
# whose compromise ships a credential-stealing patch straight into that realm,
# through a channel nobody reviews — a transitive minor version bump.
#
# It is also a performance control. A dependency arrives with its own polyfills,
# its own dead code and its own opinion about module format, and none of it is
# visible in the diff that added it.
#
# hammer and anvil ship as one SDK, and the SDK is as trustworthy as the
# least-maintained package that touched its build (CLAUDE.md §12). The tiers
# below are the policy in tools/dependency-policy.json read as code:
#
#   allowed    — meets every criterion in CLAUDE.md §12.2: justified against a
#                platform built-in, long-lived, a tree small enough to list by
#                name, pinned exactly, no install script that runs.
#   tolerated  — already in the tree, does not meet §12.2, carries its
#                replacement and its Phase 8 Part B task. Warns in development,
#                fails in `--production`. The tolerated list only shrinks.
#   (neither)  — an error in every mode. A package nothing has reviewed is not
#                a package that gets to sit quietly in devDependencies.
#
# Nine things are checked, the first three unconditionally on every run:
#   1. `dependencies` is empty.
#   2. every peer is OPTIONAL. A required peer is a dependency with extra steps.
#   3. the lockfile's tree contains no non-dev package.
#   4. every `devDependencies` key is in a tier.
#   5. every `devDependencies` spec is an exact version — no `^`, no `~`, no range.
#   6. every lockfile package reachable from an allowed root is named in that
#      root's `tree` (a glob such as "@esbuild/*" is honoured).
#   7. a package reachable only from a tolerated root: a warning naming the
#      root and its task, an error under `--production`.
#   8. an install script the policy does not list: a warning, an error under
#      `--production`.
#   9. `.npmrc` sets `ignore-scripts=true`.
#
# `--production` turns every warning into a failure, because nothing "in
# development" reaches a user — the tree that ships is the tree that is clean.
# Driven against a violation before being trusted (the Phase 0 rule): a caret
# spec, an unlisted devDependency, an unlisted transitive package under an
# allowed root, and a missing `.npmrc` were each introduced by hand and
# confirmed to fail with a message naming the exact offender, then reverted.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

production=0
for arg in "$@"; do
    if [ "$arg" = "--production" ]; then
        production=1
    fi
done

HAMMER_DEP_PRODUCTION="$production" node - <<'NODE_EOF'
import { readFileSync, existsSync } from "node:fs";

const production = process.env.HAMMER_DEP_PRODUCTION === "1";
const inCi = process.env.GITHUB_ACTIONS === "true";

const pkg = JSON.parse(readFileSync("package.json", "utf8"));
const policyPath = process.env.HAMMER_DEP_POLICY_PATH ?? "tools/dependency-policy.json";
const policy = JSON.parse(readFileSync(policyPath, "utf8"));

let errors = 0;
let warnings = 0;

function fail(message) {
    console.error(`FAIL  ${message}`);
    errors++;
}

// A "tiered" finding is an error under --production and a warning otherwise.
// Every one of these is also printed as a GitHub annotation so a warning
// left in place is visible on the pull request that introduced it, not only
// in a log nobody opens.
function tiered(message) {
    if (production) {
        console.error(`FAIL  ${message}`);
        errors++;
    } else {
        console.warn(`WARN  ${message}`);
        warnings++;
    }
    if (inCi) {
        console.log(`::warning::${message}`);
    }
}

// ---------------------------------------------------------------------------
// 1–3: the checks this script has always made. A non-empty `dependencies`, a
// required peer, or a non-dev lockfile entry are errors in every mode — they
// are runtime, and the runtime tier has never had a warning state.
// ---------------------------------------------------------------------------

const runtime = Object.keys(pkg.dependencies ?? {});
if (runtime.length !== 0) {
    fail(`dependencies must be empty, found: ${runtime.join(", ")}`);
}

for (const peer of Object.keys(pkg.peerDependencies ?? {})) {
    if (pkg.peerDependenciesMeta?.[peer]?.optional !== true) {
        fail(`peer "${peer}" is not optional — a required peer is a dependency`);
    }
}

let lock = null;
if (existsSync("package-lock.json")) {
    lock = JSON.parse(readFileSync("package-lock.json", "utf8"));
    for (const [path, entry] of Object.entries(lock.packages ?? {})) {
        if (path === "") continue;
        if (entry.dev === true || entry.devOptional === true) continue;
        fail(`lockfile carries a runtime package: ${path}`);
    }
}

// ---------------------------------------------------------------------------
// 4–5: devDependencies keys are tiered, and every spec is exact.
// ---------------------------------------------------------------------------

const allowedRoots = policy.allowed ?? {};
const toleratedRoots = policy.tolerated ?? {};
const tieredRootNames = new Set([...Object.keys(allowedRoots), ...Object.keys(toleratedRoots)]);

const devDeps = pkg.devDependencies ?? {};
const exactVersion = /^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$/;

for (const [name, spec] of Object.entries(devDeps)) {
    if (!tieredRootNames.has(name)) {
        fail(`devDependency "${name}" is in no tier of ${policyPath}`);
    }
    if (!exactVersion.test(spec)) {
        fail(`devDependency "${name}" is not pinned exactly: "${spec}" — no ^, ~ or range`);
    }
}

for (const name of tieredRootNames) {
    if (!(name in devDeps)) {
        fail(`policy names "${name}" as a tier root but package.json does not depend on it`);
    }
}

// ---------------------------------------------------------------------------
// 6–8: lockfile reachability. A lockfile package is node's own resolution
// answer to "which file does `require("x")` from here read", so this walks
// it the same way: a dependency name is looked up in a node_modules nested
// under the parent path first, then progressively further out, exactly as
// node.js resolves it. Anything reachable from a root's dependencies,
// optionalDependencies or peerDependencies is "brought in" by that root.
// ---------------------------------------------------------------------------

function packageEntry(lockPath) {
    return lock?.packages?.[lockPath];
}

function packageName(lockPath) {
    const segments = lockPath.split("node_modules/");
    return segments[segments.length - 1].replace(/\/$/, "");
}

function resolveDependency(name, fromPath) {
    // Candidate paths, nearest first: node_modules/<name> nested under every
    // ancestor of fromPath, ending at the top level. This is node's own
    // resolution algorithm, not a shortcut — a dependency two packages deep
    // may be satisfied by a copy hoisted to the root, or shadowed by one
    // nested closer in, and only walking outward tells the two apart.
    let cursor = fromPath;
    for (;;) {
        const candidate = cursor === "" ? `node_modules/${name}` : `${cursor}/node_modules/${name}`;
        if (packageEntry(candidate)) return candidate;
        if (cursor === "") return null;
        const cut = cursor.lastIndexOf("node_modules/");
        cursor = cut === -1 ? "" : cursor.slice(0, cut).replace(/\/$/, "");
    }
}

// `boundaries` are other roots' own package paths. Reaching one is recorded —
// the root genuinely needs it present — but its subtree is not walked from
// here: that subtree is that other root's tree to declare, not a copy of it
// re-discovered through whichever root's peerDependency happened to name it
// first. Without this, a tolerated root with an optional peer on another
// tolerated root would walk straight through that peer's own dependencies and
// report them as the first root's — this repository's own vitest once had
// exactly that optional peer on happy-dom, which is the case this boundary
// was written against.
function reachableFrom(rootPath, boundaries) {
    const seen = new Set();
    const queue = [rootPath];
    while (queue.length > 0) {
        const current = queue.shift();
        if (seen.has(current)) continue;
        seen.add(current);
        if (current !== rootPath && boundaries.has(current)) continue;
        const entry = packageEntry(current);
        if (!entry) continue;
        const deps = {
            ...(entry.dependencies ?? {}),
            ...(entry.optionalDependencies ?? {}),
            ...(entry.peerDependencies ?? {}),
        };
        for (const depName of Object.keys(deps)) {
            const resolved = resolveDependency(depName, current);
            if (resolved && !seen.has(resolved)) queue.push(resolved);
        }
    }
    return seen;
}

function matchesTree(name, tree) {
    return tree.some((pattern) =>
        pattern.endsWith("/*") ? name.startsWith(pattern.slice(0, -1)) : name === pattern,
    );
}

if (lock) {
    const allEntries = Object.keys(lock.packages ?? {}).filter((p) => p !== "");

    // Every declared root's own package path, used as a boundary: reaching
    // ANOTHER root through a peerDependency is real (react-dom does need
    // react present) but that root's own subtree is that root's to declare,
    // not a copy re-discovered through whoever's peerDependency named it.
    const allRootNames = [...Object.keys(allowedRoots), ...Object.keys(toleratedRoots)];
    const allRootPaths = new Set(allRootNames.map((name) => `node_modules/${name}`));

    // The union of every allowed root's declared tree. A package that two
    // allowed roots both reach — react-dom's peer on react, @types/react-dom's
    // peer on @types/react — is declared once, under the root that owns it,
    // and is not tolerated debt a second time because another root's peer
    // dependency happens to resolve to it too.
    const allowedTreeUnion = new Set();
    for (const spec of Object.values(allowedRoots)) {
        for (const pattern of spec.tree ?? []) allowedTreeUnion.add(pattern);
    }
    const allowedTreePatterns = [...allowedTreeUnion];

    // Every allowed root's own reachable set, checked against the union above.
    const allowedReachable = new Set();
    for (const rootName of Object.keys(allowedRoots)) {
        const rootPath = `node_modules/${rootName}`;
        if (!packageEntry(rootPath)) continue; // reported above as a missing devDependency
        const boundaries = new Set([...allRootPaths].filter((p) => p !== rootPath));
        for (const lockPath of reachableFrom(rootPath, boundaries)) {
            allowedReachable.add(lockPath);
            const name = packageName(lockPath);
            if (!matchesTree(name, allowedTreePatterns)) {
                fail(
                    `"${lockPath}" is reachable from allowed root "${rootName}" but is not in any allowed tree (${policyPath})`,
                );
            }
        }
    }

    // Every tolerated root's reachable set, minus anything already accounted
    // for as allowed — react-dom's peer on react is real, and react already
    // has its own tree entry, so it is not tolerated debt a second time. A
    // root is always attributed to itself first (pre-seeded below), so a
    // package that is itself a declared tolerated root is never blamed on
    // whichever OTHER root's peerDependency happens to name it too.
    const toleratedReachedBy = new Map(); // lockPath -> root name
    for (const rootName of Object.keys(toleratedRoots)) {
        const rootPath = `node_modules/${rootName}`;
        if (packageEntry(rootPath)) toleratedReachedBy.set(rootPath, rootName);
    }
    for (const rootName of Object.keys(toleratedRoots)) {
        const rootPath = `node_modules/${rootName}`;
        if (!packageEntry(rootPath)) continue;
        const boundaries = new Set([...allRootPaths].filter((p) => p !== rootPath));
        for (const lockPath of reachableFrom(rootPath, boundaries)) {
            if (allowedReachable.has(lockPath)) continue;
            if (!toleratedReachedBy.has(lockPath)) {
                toleratedReachedBy.set(lockPath, rootName);
            }
        }
    }

    // One line per tolerated ROOT, not per package. The debt is the root: it is
    // what a Part B row retires, and its subtree goes with it. Ninety-six lines
    // on every `npm run check` is output people learn to scroll past, which is
    // the opposite of what a warning is for.
    const subtreeOf = new Map(); // root name -> lockfile packages it alone brings
    for (const rootName of toleratedReachedBy.values()) {
        subtreeOf.set(rootName, (subtreeOf.get(rootName) ?? 0) + 1);
    }
    for (const [rootName, count] of [...subtreeOf.entries()].sort()) {
        const task = toleratedRoots[rootName]?.task ?? "no task recorded";
        tiered(`tolerated root "${rootName}" brings ${count} package(s) into the tree (${task})`);
    }

    // Every lockfile package must be accounted for by SOME root, allowed or
    // tolerated. One that is neither is drift: something the lockfile
    // carries that no root in the policy claims, which is exactly the case
    // this script exists to make impossible to miss.
    const accountedFor = new Set([...allowedReachable, ...toleratedReachedBy.keys()]);
    for (const lockPath of allEntries) {
        if (!accountedFor.has(lockPath)) {
            fail(`"${lockPath}" is not reachable from any allowed or tolerated root`);
        }
    }

    // Install scripts. `ignore-scripts=true` means none of these actually
    // RUN — this is a record of which ones exist and why running nothing is
    // safe, so a new one arriving silently is a warning rather than a fact
    // nobody notices until an upgrade needs the script it disabled.
    const installScripts = policy.installScripts ?? {};
    for (const lockPath of allEntries) {
        const entry = packageEntry(lockPath);
        if (entry?.hasInstallScript !== true) continue;
        if (!(lockPath in installScripts)) {
            tiered(`"${lockPath}" has an install script not listed in ${policyPath}`);
        }
    }
    for (const lockPath of Object.keys(installScripts)) {
        if (!packageEntry(lockPath)) {
            fail(`${policyPath} lists an install script for "${lockPath}", which is not in the lockfile`);
        }
    }
}

// ---------------------------------------------------------------------------
// 9: .npmrc. `ignore-scripts=true` is the setting the esbuild and fsevents
// entries above are harmless UNDER — remove it and this script's own
// reasoning about them stops being true.
// ---------------------------------------------------------------------------

if (!existsSync(".npmrc")) {
    fail(".npmrc is missing — ignore-scripts=true is required (CLAUDE.md §12.2)");
} else {
    const npmrc = readFileSync(".npmrc", "utf8");
    const hasIgnoreScripts = npmrc
        .split("\n")
        .map((line) => line.trim())
        .some((line) => line === "ignore-scripts=true");
    if (!hasIgnoreScripts) {
        fail(".npmrc does not set ignore-scripts=true");
    }
}

// ---------------------------------------------------------------------------
// Summary. Always printed, in every mode: a tree with an open warning is not
// releasable even when `npm run check` is green, and the only way anybody
// finds that out without reading every line above is one line that says so.
// ---------------------------------------------------------------------------

const releasable = errors === 0 && warnings === 0;
console.log("");
console.log(
    releasable
        ? "dependencies: releasable — no error, no warning"
        : `dependencies: NOT releasable — ${errors} error(s), ${warnings} warning(s)`,
);

if (errors !== 0) process.exit(1);
NODE_EOF
