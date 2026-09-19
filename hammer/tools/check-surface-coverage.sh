#!/usr/bin/env bash
#
# Every published value is named by the suite.
#
# `check-public-surface.sh` answers "what does this library export"; this one
# answers the question nobody was asking, which is whether anything has ever
# used it. The two are not the same and the gap between them is where the
# expensive defects live: a name that is exported, documented, type-checked and
# never called is a name whose first caller is an application, and an
# application's build is the most expensive place to find out it does not work.
#
# --- what this is, in one example -------------------------------------------
#
# `Client.upload` was exported for four phases. `wire/upload.ts` had tests for
# the three functions it is assembled from and every one of them passed. Nothing
# anywhere called the method. When something finally did, the first run found
# that a retried upload re-sent an empty stream — a defect none of the three
# unit suites could see, because it was not in any of the three.
#
# That is the whole argument. A suite proves the parts work; only a caller
# proves the thing made of them does, and a published name with no caller has
# never been the thing made of them.
#
# --- values, not types ------------------------------------------------------
#
# A type is exercised by being written in an annotation, and to a grep that is
# indistinguishable from being mentioned in a comment. So the check is over
# values, where "named" and "run" are close enough to be worth failing a build
# over. A type that nothing uses is caught by the review of the surface diff
# instead, which is where a person is already looking.
#
# --- what it cannot tell you ------------------------------------------------
#
# It is a grep, so a name mentioned in a comment satisfies it. That is a real
# weakness and it is the right trade: the alternative is coverage instrumentation
# over a suite that deliberately runs in three environments, to catch a case
# nobody has ever hit. What this catches is the case that HAS been hit, which is
# a name no test file so much as mentions — and a reviewer reading a diff that
# adds a name to a comment to silence a build is a reviewer who will say so.
#
# --- why there is no allow-list ---------------------------------------------
#
# The obvious escape hatch is a list of names exempted with a reason, and it is
# the wrong shape here: the reason would always be "this one is hard to test",
# which is the property that makes it worth testing. A constant is named by
# asserting it is the value it claims to be, which is one line and catches the
# typo that ships to every consumer at once. If a name genuinely should not be
# published, the fix is to stop publishing it.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

committed="tools/public-surface.txt"

if [ ! -f "$committed" ]; then
    printf 'FAIL  %s is not committed. Run tools/check-public-surface.sh --write.\n' "$committed" >&2
    exit 1
fi

# The snapshot is diffed against the real surface by the check that runs before
# this one, so reading it rather than the compiler costs nothing and keeps this
# script free of a TypeScript dependency.
missing=0
entry=""

while read -r kind name; do
    case "$kind" in
        "##") entry="$name"; continue ;;
        both|value) ;;
        *) continue ;;
    esac

    if ! grep -rqw -- "$name" tests/ 2>/dev/null; then
        if [ "$missing" -eq 0 ]; then
            printf 'FAIL  a published value that nothing in tests/ names:\n' >&2
        fi
        printf '      %-14s %s\n' "$entry" "$name" >&2
        missing=$((missing + 1))
    fi
done < <(grep -E '^(## |both |value )' "$committed")

if [ "$missing" -ne 0 ]; then
    printf '\n      %d in total. Each one is a name an application can import and\n' "$missing" >&2
    printf '      nothing here has ever called. Write the test, or stop exporting it.\n' >&2
    exit 1
fi

printf 'surface coverage: clean\n'
