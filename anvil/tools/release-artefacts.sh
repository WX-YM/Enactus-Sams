#!/usr/bin/env bash
#
# The two files a release publishes, and the check that they mean anything.
#
#   anvil-<tag>-envelopes.json            anvil's own failure BYTES
#   anvil-<tag>-reference-descriptor.json the reference application's TABLES
#
# --- why a release publishes them at all ------------------------------------
#
# A client generator built against this repository has to be built against
# SOMETHING, and until this script that something was a fixture a person
# remembered to refresh by running a binary in a sibling checkout. That fixture's
# own staleness check regenerates the client from the committed descriptor, so it
# is green by construction whatever release the descriptor came from — the check
# cannot see that it is a release behind, because nothing in it names a release.
#
# It cannot be closed from the generator's side either: a sibling checkout is not
# a build dependency, and a check that shells out to one is a check that passes on
# exactly one machine. So anvil publishes the files at the tag, and a fetch step
# names a release instead of naming a note that says to run something.
#
# --- why it regenerates and diffs rather than trusting the file --------------
#
# A published artefact that nobody re-derives is a file somebody edited once. Both
# emitters are deterministic — the same tables produce the same bytes, which
# docs/01-seams.md §14 already requires or the descriptor's hash means nothing —
# so "emit it twice and compare" is a real check and costs milliseconds. If it
# ever fails, the artefact is not the problem: something in the emitter has
# started iterating an unordered container, and every hash it has ever published
# is suspect.
#
# --- the one exception this script exists to bound ---------------------------
#
# docs/01-seams.md §14: "the descriptor is a build artefact and is never served."
# That rule is about an APPLICATION's descriptor, which carries every route's
# path including the administrative ones the stealth 404 exists to withhold.
#
# `testapp` has no deployment and its paths protect nothing, so publishing ITS
# descriptor discloses nothing — but the two files look identical, and a rule
# whose exception lives in somebody's head is a rule that gets applied to the
# wrong file once. So the exception is enforced here: this script refuses to
# publish any descriptor whose `app.name` is not the reference application's.

set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage:
  release-artefacts.sh --check <emit_envelopes> <emit_descriptor>
      Emit both artefacts twice into a temporary directory and diff. Publishes
      nothing. This is the form CTest runs.

  release-artefacts.sh --tag <tag> [--out <dir>] <emit_envelopes> <emit_descriptor>
      Write the two release files into <dir> (default: dist/), each regenerated
      and diffed before it is kept.
USAGE
    exit 2
}

mode="check"
tag=""
out="dist"

while [ $# -gt 0 ]; do
    case "$1" in
        --check) mode="check"; shift ;;
        --tag)   mode="publish"; tag="${2:-}"; shift 2 ;;
        --out)   out="${2:-}"; shift 2 ;;
        --help|-h) usage ;;
        --*)     printf 'unknown option: %s\n' "$1" >&2; usage ;;
        *)       break ;;
    esac
done

[ $# -eq 2 ] || usage
envelopes_bin="$1"
descriptor_bin="$2"

for binary in "$envelopes_bin" "$descriptor_bin"; do
    if [ ! -x "$binary" ]; then
        printf 'FAIL  not an executable: %s\n' "$binary" >&2
        printf '      build anvil_emit_envelopes and testapp_emit_descriptor first\n' >&2
        exit 1
    fi
done

# The reference application's name, as its own emitter writes it. Matched as the
# exact key so that an application called `testapp-something` does not slip past.
readonly REFERENCE_APP='"app":{"name":"testapp"'

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Emit twice and compare. The SECOND run is the check; the first is what gets
# published, so a non-deterministic emitter can never publish the run that
# happened to look right.
emit_twice() {
    local binary="$1"
    local name="$2"

    "$binary" > "$work/$name.1"
    "$binary" > "$work/$name.2"

    if ! cmp -s "$work/$name.1" "$work/$name.2"; then
        printf 'FAIL  %s is not deterministic — two runs, two files.\n' "$name" >&2
        printf '      Something in the emitter iterates an unordered container, and every\n' >&2
        printf '      hash it has published is suspect (docs/01-seams.md §14).\n' >&2
        diff <(head -c 2000 "$work/$name.1") <(head -c 2000 "$work/$name.2") >&2 || true
        return 1
    fi
}

failures=0

emit_twice "$envelopes_bin" "envelopes" || failures=$((failures + 1))
emit_twice "$descriptor_bin" "reference-descriptor" || failures=$((failures + 1))

# The exception, enforced rather than remembered.
if [ -s "$work/reference-descriptor.1" ] &&
   ! grep -qF "$REFERENCE_APP" "$work/reference-descriptor.1"; then
    printf 'FAIL  that descriptor is not the reference application'"'"'s.\n' >&2
    printf '      An application descriptor carries every route path, including the\n' >&2
    printf '      administrative ones the stealth 404 exists to withhold, and it is a\n' >&2
    printf '      build artefact that is never published (docs/01-seams.md §14). Only\n' >&2
    printf '      testapp'"'"'s is, because it has no deployment and its paths protect\n' >&2
    printf '      nothing.\n' >&2
    failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    exit 1
fi

if [ "$mode" = "check" ]; then
    printf 'release artefacts: deterministic, and the descriptor is the reference one\n'
    exit 0
fi

if [ -z "$tag" ]; then
    printf 'FAIL  --tag names the release the artefacts belong to\n' >&2
    exit 2
fi

mkdir -p "$out"
cp "$work/envelopes.1"             "$out/anvil-$tag-envelopes.json"
cp "$work/reference-descriptor.1"  "$out/anvil-$tag-reference-descriptor.json"

printf 'wrote %s/anvil-%s-envelopes.json\n' "$out" "$tag"
printf 'wrote %s/anvil-%s-reference-descriptor.json\n' "$out" "$tag"
