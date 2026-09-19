#!/usr/bin/env bash
#
# Refreshes tests/fixtures/envelopes/anvil.json from a sibling anvil checkout.
#
# NOT part of `npm run check`, and it cannot be: it needs a C++ compiler and
# anvil's headers, and hammer may not make a sibling checkout a build dependency
# (docs/15-tasks.md §Cross-repo). So this is the same shape as the descriptor
# fixture — a person runs it, and nothing in this repository can tell you when it
# needs to be run.
#
# What would close that is anvil owning the emitter the way it owns
# `testapp_emit_descriptor`, and publishing its output as a release artefact a
# fetch step could name. The row is in docs/15-tasks.md.
#
#   ANVIL=~/Code/anvil tools/record-envelopes.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

anvil="${ANVIL:-$root/../anvil}"
include="$anvil/include"
out="tests/fixtures/envelopes/anvil.json"

if [ ! -f "$include/anvil/http/errors.h" ]; then
    printf 'FAIL  no anvil headers at %s. Set ANVIL to the checkout.\n' "$include" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The compiler and the standard are anvil's, not this repository's: the headers
# are C++20 and constexpr, and a recorder built against the wrong standard is a
# recorder that does not build rather than one that lies.
"${CXX:-g++}" -std=c++20 -I "$include" -o "$work/record" tools/record-envelopes.cc

mkdir -p "$(dirname "$out")"
"$work/record" > "$out"

printf 'envelopes: recorded %s from %s\n' "$out" "$include"
