#!/usr/bin/env bash
#
# `npm audit` is part of the production gate, and only the production gate
# (CLAUDE.md §12.3, §12.6). It needs the registry, and `npm run lint` is the
# thing that has to work on a machine with no network at all — so this script
# is never called from `npm run lint` or from `npm run check`, only from
# `npm run check:production`.
#
# Development: a finding is a WARNING. The tolerated tier already carries an
# advisory (happy-dom) that nothing here can fix except retiring the package
# (Phase 8 Part B — B1 already retired vitest this way), so failing
# `npm run check` over it would make ordinary development red for a fact
# everybody already knows. An audit that cannot reach the registry is a
# warning too, and says so, because a machine with no network is not a
# machine with a clean tree — it is a machine that did not check.
#
# --production: any finding at ANY severity is an ERROR, and an audit that
# cannot run is an ERROR as well. "The registry was unreachable" is not a
# reason to publish; it is the one failure mode a gate that skips on it
# would reward. A release either proves the tree clean or it does not ship.
#
# npm audit fix --force is never run here or anywhere in this repository
# (CLAUDE.md §12.4): it crosses majors silently, which is a rewrite of the
# build nobody reviewed.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

production=0
for arg in "$@"; do
    if [ "$arg" = "--production" ]; then
        production=1
    fi
done

audit_json="$(npm audit --json 2>/tmp/hammer-audit-stderr.$$)"
audit_status=$?
audit_stderr="$(cat /tmp/hammer-audit-stderr.$$ 2>/dev/null || true)"
rm -f "/tmp/hammer-audit-stderr.$$"

# The report goes through a temp file rather than an environment variable —
# `npm audit --json` over a tree this size is comfortably past what some
# shells accept in a single environment value — and rather than a pipe,
# because the heredoc below already occupies node's stdin with the script
# itself.
audit_json_file="$(mktemp)"
trap 'rm -f "$audit_json_file"' EXIT
printf '%s' "$audit_json" > "$audit_json_file"

HAMMER_AUDIT_PRODUCTION="$production" \
HAMMER_AUDIT_STATUS="$audit_status" \
HAMMER_AUDIT_STDERR="$audit_stderr" \
HAMMER_AUDIT_JSON_FILE="$audit_json_file" \
node - <<'NODE_EOF'
import { readFileSync } from "node:fs";

const production = process.env.HAMMER_AUDIT_PRODUCTION === "1";
const inCi = process.env.GITHUB_ACTIONS === "true";
const stderrText = process.env.HAMMER_AUDIT_STDERR ?? "";

function tiered(message) {
    if (production) {
        console.error(`FAIL  ${message}`);
        return 1;
    }
    console.warn(`WARN  ${message}`);
    if (inCi) console.log(`::warning::${message}`);
    return 0;
}

// A gate that passes because the registry was unreachable is a gate that
// passes whenever somebody wants it to (CLAUDE.md §12.3). This branch is
// deliberately the LOUD one: an audit that could not run reports the same
// severity as an audit that ran and found something, under --production.
function auditCouldNotRun(reason) {
    let errors = tiered(`npm audit could not run: ${reason}`);
    console.log("");
    console.log(production ? "audit: NOT releasable — audit did not run" : "audit: could not verify (offline or registry error)");
    process.exit(production ? 1 : 0);
}

const raw = readFileSync(process.env.HAMMER_AUDIT_JSON_FILE, "utf8");

let report;
try {
    report = JSON.parse(raw);
} catch {
    auditCouldNotRun(stderrText.trim() || "npm audit produced no parseable JSON");
}

// npm emits {"error": {...}} instead of a report when it cannot reach the
// registry, resolve the tree, or when the lockfile and package.json disagree
// enough that it refuses to try.
if (report && typeof report === "object" && report.error) {
    const summary =
        report.message || report.error.summary || report.error.detail || report.error.code || JSON.stringify(report.error);
    auditCouldNotRun(String(summary));
}

const counts = report?.metadata?.vulnerabilities;
if (!counts || typeof counts.total !== "number") {
    auditCouldNotRun("the report carried no metadata.vulnerabilities — an unrecognised npm audit output shape");
}

let errors = 0;
let warnings = 0;

if (counts.total === 0) {
    console.log("audit: clean — 0 advisories");
} else {
    const severities = ["critical", "high", "moderate", "low", "info"];
    for (const severity of severities) {
        const n = counts[severity] ?? 0;
        if (n === 0) continue;
        const names = Object.entries(report.vulnerabilities ?? {})
            .filter(([, v]) => v.severity === severity)
            .map(([name]) => name)
            .sort();
        const message = `${n} ${severity} advisor${n === 1 ? "y" : "ies"}: ${names.join(", ")}`;
        if (production) {
            console.error(`FAIL  ${message}`);
            errors++;
        } else {
            console.warn(`WARN  ${message}`);
            warnings++;
            if (inCi) console.log(`::warning::${message}`);
        }
    }
}

console.log("");
const releasable = counts.total === 0;
console.log(
    releasable
        ? "audit: releasable — no advisory at any severity"
        : `audit: NOT releasable — ${counts.total} advisory(ies) over the tree`,
);

process.exit(errors > 0 ? 1 : 0);
NODE_EOF
node_exit=$?

exit "$node_exit"
