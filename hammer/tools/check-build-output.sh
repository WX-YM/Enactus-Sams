#!/usr/bin/env bash
#
# What the package actually publishes, asserted against a real build.
#
# Every other script here reads `src/`. This one reads `dist/`, because two of
# the three properties below are not properties of the source at all — they are
# properties of a compiler flag, a `files` list and an `exports` map, and each of
# those is changed by someone who is not thinking about this.
#
#   NO SOURCE MAP. A published source map re-leaks everything tree-shaking
#              removed, because the map carries each module's full contents
#              (docs/01-seams.md §4.2). hammer is a library an application
#              bundles, so a map here does not stay here: a bundler that finds
#              one folds it into the application's own map, and the application's
#              bundle is the one whose holder route table was the entire point of
#              never emitting. The map is uploaded to an error reporter privately
#              or it is not built.
#
#   EVERY EXPORT RESOLVES. An `exports` subpath or a `bin` naming a file the
#              build does not emit is a package that installs and then throws on
#              the first import — discovered by a consumer, in their build, where
#              it is most expensive to diagnose.
#
#   THE CLI STAYS EXECUTABLE. `bin` is run by a shell. A CLI that lost its
#              shebang is `syntax error near unexpected token` on somebody else's
#              machine.
#
# The build is run here rather than assumed, because a stale `dist/` is exactly
# the state in which this check would otherwise pass.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

npm run build --silent

node - <<'NODE_EOF'
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";

const pkg = JSON.parse(readFileSync("package.json", "utf8"));
let failures = 0;

function* walk(dir) {
    for (const name of readdirSync(dir)) {
        const path = join(dir, name);
        if (statSync(path).isDirectory()) {
            yield* walk(path);
        } else {
            yield path;
        }
    }
}

for (const path of walk("dist")) {
    if (path.endsWith(".map")) {
        console.error(`FAIL  the build published a source map: ${path}`);
        failures++;
        continue;
    }
    if (!path.endsWith(".js") && !path.endsWith(".d.ts")) continue;

    // The comment, separately from the file. A map can be absent from the
    // package and still be named by one — which points a browser's devtools at
    // a 404 in the best case and at whatever is served from that path in the
    // worst.
    const text = readFileSync(path, "utf8");
    if (text.includes("//# sourceMappingURL=")) {
        console.error(`FAIL  ${path} names a source map`);
        failures++;
    }
}

const targets = [...Object.values(pkg.exports ?? {}), ...Object.values(pkg.bin ?? {})];
for (const target of targets) {
    try {
        statSync(target);
    } catch {
        console.error(`FAIL  ${target} is published in package.json and the build does not emit it`);
        failures++;
    }
}

for (const [name, target] of Object.entries(pkg.bin ?? {})) {
    let first = "";
    try {
        first = readFileSync(target, "utf8").split("\n", 1)[0] ?? "";
    } catch {
        continue;
    }
    if (!first.startsWith("#!")) {
        console.error(`FAIL  the "${name}" command has no shebang: ${target}`);
        failures++;
    }
}

if (failures !== 0) {
    console.error(`build output: ${failures} violation(s)`);
    process.exit(1);
}
console.log("build output: clean");
NODE_EOF
