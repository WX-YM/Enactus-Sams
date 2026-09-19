#!/usr/bin/env bash
#
# The layer graph (ENGINEERING_RULES.md §1, docs/00-architecture.md §2), enforced per import.
#
# anvil gets this for free: its three targets are link targets, so an accidental
# dependency from a validator onto a repository is a LINK ERROR. TypeScript has
# no linker and a bundler will happily resolve any edge you write, so the same
# boundary has to be a script or it is a review comment — and a review comment is
# a boundary that holds until the week somebody is busy.
#
#   core    imports NOTHING. No layer, no DOM, no fetch. It is the layer that
#           stays testable with no environment at all, and it stops being that on
#           the first import.
#   wire    core
#   state   core, wire
#   dom     core, state          (a component drives a store, never a socket)
#   chart   core, state          (its own entry point: a dashboard is a minority)
#
#           Both need a document and neither may reach the GLOBAL one — it
#           arrives through the element they were asked to mount in. See the
#           globals section below.
#   react   core, wire, state, dom, chart
#   codegen core                 (build time only; never in a browser bundle)
#
# Globals are checked as well as imports: `document` in src/core/ is the same
# defect as importing the DOM layer, and no import graph can see it. Nor can it
# see a `node:` specifier, which points at no layer at all and is checked
# separately below.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [ ! -d src ] || [ -z "$(find src -name '*.ts' -o -name '*.tsx' 2>/dev/null | head -1)" ]; then
    printf 'layering: nothing to check yet\n'
    exit 0
fi

failures=0

# Normalises `dir` + a relative specifier into a path with no `.` or `..` left
# in it. Written out rather than shelled to `realpath`, because the target is a
# `.js` specifier naming a `.ts` file: the path this resolves does not exist on
# disk and a resolver that required it to would report every import as absent.
resolve() {
    local path="$1/$2"
    local -a out=()
    local part
    local IFS=/
    for part in $path; do
        case "$part" in
            ""|".") continue ;;
            "..")   [ ${#out[@]} -gt 0 ] && unset 'out[${#out[@]}-1]' ;;
            *)      out+=("$part") ;;
        esac
    done
    printf '%s' "${out[*]}"
}

# The layer a resolved path belongs to: the segment straight after `src/`, and
# empty for a path that is not under it.
layer_of() {
    case "$1" in
        src/*) printf '%s' "${1#src/}" | cut -d/ -f1 ;;
        *)     printf '' ;;
    esac
}

allowed_for() {
    case "$1" in
        core)    printf '' ;;
        wire)    printf 'core' ;;
        state)   printf 'core wire' ;;
        dom)     printf 'core state' ;;
        chart)   printf 'core state' ;;
        react)   printf 'core wire state dom chart' ;;
        codegen) printf 'core' ;;
        *)       printf '__unknown__' ;;
    esac
}

for dir in src/*/; do
    layer="$(basename "$dir")"
    allowed="$(allowed_for "$layer")"

    if [ "$allowed" = "__unknown__" ]; then
        printf 'FAIL  src/%s/ is not a declared layer. Add it to this script and to\n' "$layer" >&2
        printf '      docs/00-architecture.md §2 in the same commit, or it is a layer\n' >&2
        printf '      nobody agreed to.\n' >&2
        failures=$((failures + 1))
        continue
    fi

    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        file="${hit%%:*}"
        spec="$(printf '%s' "$hit" | sed -nE 's:.*from *["'"'"']([^"'"'"']+)["'"'"'].*:\1:p')"
        [ -z "$spec" ] && continue

        # Which layer does this specifier reach?
        #
        # A relative specifier is RESOLVED against the importing file rather than
        # pattern-matched, because a layer has subdirectories: `src/state/workers/`
        # reaching `../counts.js` is a hop within its own layer, and a check that
        # counted the `../` would read it as an escape from one.
        target=""
        case "$spec" in
            ./*|../*)  target="$(layer_of "$(resolve "$(dirname "$file")" "$spec")")" ;;
            hammer)    target="core" ;;
            hammer/*)  target="${spec#hammer/}"; target="${target%%/*}" ;;
            *)         continue ;;
        esac

        # A relative specifier that lands outside src/ is not a layer edge. The
        # only ones are a test importing a fixture, and this loop never sees those.
        [ -z "$target" ] && continue

        [ "$target" = "$layer" ] && continue

        if ! printf ' %s ' "$allowed" | grep -q " $target "; then
            printf 'FAIL  %s: src/%s/ may not import %s\n' "$file" "$layer" "$target" >&2
            failures=$((failures + 1))
        fi
    done < <(grep -rnE "^ *(import|export)[^;]* from *[\"']" "$dir" \
                  --include='*.ts' --include='*.tsx' 2>/dev/null || true)
done

# --- the globals an import graph cannot see --------------------------------
#
# $1 = layer, $2 = regex of globals it may not name.
# Comments and string literals are stripped before matching, for the reason
# tools/check-source-bans.sh gives: the prose explaining why `core` may not touch
# a document is the one place in this repository where the word SHOULD appear,
# and a check that fires on its own documentation is a check somebody turns off.
#
# The value-shaped globals are matched as they are USED — `document.` or
# `document[` — rather than as the bare word, and that is the same lesson from
# the other direction. `document` is this repository's own word for the thing an
# optimistic write confirms against and a versioned write carries the version of
# (ENGINEERING_RULES.md §6), so `readonly document: T` is a field name in a layer that has
# never seen a DOM. Matching the word made the check fire on the vocabulary
# rather than on the defect.
#
# What the narrowing gives up is an alias — `const d = document` — and that is
# the right trade: this catches the global being reached, which is every way it
# is reached by ACCIDENT, and a check is not a defence against somebody working
# around it on purpose.
check_globals() {
    local layer="$1"
    local pattern="$2"
    [ -d "src/$layer" ] || return 0

    local hits=""
    local hit
    local code
    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        code="${hit#*:}"
        code="${code#*:}"
        code="$(printf '%s' "$code" |
                sed -e 's://.*::' -e 's:^ *\*.*::' -e "s:'[^']*'::g" -e 's:"[^"]*"::g')"
        if printf '%s' "$code" | grep -qE "$pattern"; then
            hits="${hits}${hit}"$'\n'
        fi
    done < <(grep -rnE "$pattern" "src/$layer" --include='*.ts' --include='*.tsx' 2>/dev/null |
             grep -v 'ban-exempt:' || true)

    if [ -n "$hits" ]; then
        printf 'FAIL  src/%s/ reaches a global it may not:\n%s\n' "$layer" "$hits" >&2
        case "$layer" in
            dom|chart)
                printf '      This layer needs a document and takes it from the element it\n' >&2
                printf '      was asked to mount in — `documentOf(mount)` in dom/mount.ts.\n' >&2
                ;;
            react)
                printf '      The adapter binds stores and renders nothing. A document, a\n' >&2
                printf '      window or a fetch here is behaviour that belongs in a layer\n' >&2
                printf '      below, where it is testable without a framework.\n' >&2
                ;;
            *)
                printf '      That global belongs to a layer above this one.\n' >&2
                ;;
        esac
        printf '\n' >&2
        failures=$((failures + 1))
    fi
}

# A value the layer reaches into, and a type it annotates with, are two
# different shapes and are matched as two.
kGlobalValue='(^|[^.A-Za-z0-9_$])(document|window|navigator) *[.[]'

check_globals core  "$kGlobalValue"'|\b(EventSource|BroadcastChannel)\b|\bfetch\('
check_globals wire  "$kGlobalValue"'|\bHTMLElement\b'
check_globals state "$kGlobalValue"'|\bHTMLElement\b'

# `dom` and `chart` NEED a document, and that is the reason they are checked
# rather than the reason they are exempt.
#
# The layer above the stores may name `HTMLElement` and must build elements. What
# it may not do is reach the GLOBAL one: a component takes its document from the
# element it was asked to mount in (`dom/mount.ts`), so a test supplies its own
# instead of racing every other test in the file, and the same component renders
# into a document that is not the tab's — an editor preview, a print view, a
# frame — without a branch. That is `ENGINEERING_RULES.md` §3.3 applied to the one singleton
# this layer cannot avoid needing.
#
# `mount.ownerDocument` does not match, and deliberately: the pattern is
# case-sensitive and that is a capital D. The property is how the document is
# supposed to arrive.
check_globals dom   "$kGlobalValue"
check_globals chart "$kGlobalValue"

# The adapter is ABOVE the components and still may not reach a document: it
# binds stores to a framework's lifetime and renders nothing at all. A hook that
# reached the global one would be a hook that cannot run in a frame, a print
# view or a second document — and it would make the adapter's own claim, that it
# holds no behaviour of its own, false in the one place nobody looks.
check_globals react "$kGlobalValue"'|\bfetch\('

# --- the platform a layer is allowed to be on -------------------------------
#
# `hammer/codegen` runs at build time, reads a file from disk and writes
# TypeScript. Every other entry point runs in a browser. A `node:` import outside
# src/codegen/ is a path walker and a code emitter shipped to every device — or,
# where the bundler resolves the specifier to an empty shim instead, a module
# that builds clean and fails at the one moment it is used.
#
# The loop above cannot catch it: `node:fs` names no layer, so it is skipped
# there the way any bare specifier is.
for dir in src/*/; do
    layer="$(basename "$dir")"
    [ "$layer" = "codegen" ] && continue

    node_hits="$(grep -rnE "from *[\"']node:" "$dir" \
                      --include='*.ts' --include='*.tsx' 2>/dev/null |
                 grep -v 'ban-exempt:' || true)"

    if [ -n "$node_hits" ]; then
        printf 'FAIL  src/%s/ imports a node builtin:\n%s\n\n' "$layer" "$node_hits" >&2
        failures=$((failures + 1))
    fi
done

if [ "$failures" -ne 0 ]; then
    printf 'layering: %d violation(s). Dependencies point downward only.\n' "$failures" >&2
    exit 1
fi

printf 'layering: clean\n'
