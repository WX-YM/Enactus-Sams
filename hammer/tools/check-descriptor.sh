#!/usr/bin/env bash
#
# The generated client is regenerated and diffed (docs/01-seams.md §14).
#
# A client generated from last month's server is the one failure the whole
# descriptor mechanism exists to prevent, and it is not allowed to be a runtime
# surprise. There are two halves to that and this script is the build-time one:
#
#   STALENESS. The committed module under tests/testapp/api/ is regenerated into
#              a temporary directory and compared. A difference means the
#              descriptor moved and the client did not, and the fix is one
#              command — which is printed, because a failure a reader has to
#              reconstruct is a failure somebody works around.
#
#   DETERMINISM. The committed bytes were written by a previous run, in another
#              process, on another machine. Comparing against them is therefore
#              also the assertion that the generator emits the same bytes for the
#              same tables: one that iterated an unordered container would
#              produce a diff on every run and be ignored inside a week.
#
# The runtime half is the descriptor hash the module carries, checked against the
# hash the session response returns (docs/00-architecture.md §7.1).
#
# --- and the file itself ----------------------------------------------------
#
# The descriptor is a BUILD ARTEFACT and is never served. It carries every
# route's path, `visibility` included — the whole table the emission goes to
# trouble not to emit — and a copy in a published output directory hands it over
# in one request. It is an easy mistake to make because a JSON file looks exactly
# like an asset, which is why it is checked here rather than remembered.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

descriptor="tests/testapp/hammer.descriptor.json"
committed="tests/testapp/api/hammer.generated.ts"

if [ ! -f "$descriptor" ]; then
    printf 'descriptor: nothing to check yet\n'
    exit 0
fi

if [ ! -x node_modules/.bin/esbuild ]; then
    printf 'FAIL  node_modules/.bin/esbuild is missing. Run `npm ci`.\n' >&2
    exit 1
fi

failures=0

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The generator is TypeScript and CI runs the oldest Node this package supports,
# so it is bundled rather than executed through whatever type stripping that
# particular runtime happens to have. It is the same compiler the suite uses.
node_modules/.bin/esbuild src/codegen/cli.ts \
    --bundle --platform=node --format=esm --log-level=error \
    --outfile="$work/cli.mjs"

# Warnings go to stderr and are worth reading; they are not failures. A dead
# capability scope or rate-limit bucket reads as coverage, and anvil's own
# output has both.
node "$work/cli.mjs" codegen --descriptor "$descriptor" --out "$work/api" >/dev/null

if [ ! -f "$committed" ]; then
    printf 'FAIL  %s is not committed. Generate it:\n' "$committed" >&2
    printf '      hammer codegen --descriptor %s --out %s\n' "$descriptor" "$(dirname "$committed")" >&2
    failures=$((failures + 1))
elif ! diff -u "$committed" "$work/api/hammer.generated.ts"; then
    printf '\nFAIL  %s is not what the descriptor generates. Regenerate it:\n' "$committed" >&2
    printf '      hammer codegen --descriptor %s --out %s\n' "$descriptor" "$(dirname "$committed")" >&2
    failures=$((failures + 1))
fi

# A descriptor in an output directory is the whole route table, served.
while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    printf 'FAIL  a descriptor reached the build output: %s\n' "$hit" >&2
    failures=$((failures + 1))
done < <(find dist -name '*.descriptor.json' 2>/dev/null || true)

if [ "$failures" -ne 0 ]; then
    printf 'descriptor: %d violation(s)\n' "$failures" >&2
    exit 1
fi

printf 'descriptor: clean\n'
