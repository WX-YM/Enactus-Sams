#!/usr/bin/env bash
#
# Zero runtime dependencies is a security control, not minimalism (ENGINEERING_RULES.md §5).
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
# Three things are checked:
#   1. `dependencies` is empty.
#   2. every peer is OPTIONAL. A required peer is a dependency with extra steps —
#      the consumer installs it, ships it, and pays for it.
#   3. the lockfile's tree contains no non-dev package. This is the one that
#      catches the real case: a dev dependency that quietly moved.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

node - <<'NODE_EOF'
import { readFileSync, existsSync } from "node:fs";

const pkg = JSON.parse(readFileSync("package.json", "utf8"));
let failures = 0;

const runtime = Object.keys(pkg.dependencies ?? {});
if (runtime.length !== 0) {
    console.error(`FAIL  dependencies must be empty, found: ${runtime.join(", ")}`);
    failures++;
}

for (const peer of Object.keys(pkg.peerDependencies ?? {})) {
    if (pkg.peerDependenciesMeta?.[peer]?.optional !== true) {
        console.error(`FAIL  peer "${peer}" is not optional — a required peer is a dependency`);
        failures++;
    }
}

if (existsSync("package-lock.json")) {
    const lock = JSON.parse(readFileSync("package-lock.json", "utf8"));
    for (const [path, entry] of Object.entries(lock.packages ?? {})) {
        if (path === "") continue;
        if (entry.dev === true || entry.devOptional === true) continue;
        console.error(`FAIL  lockfile carries a runtime package: ${path}`);
        failures++;
    }
}

if (failures !== 0) {
    console.error(`dependencies: ${failures} violation(s)`);
    process.exit(1);
}
console.log("dependencies: clean");
NODE_EOF
