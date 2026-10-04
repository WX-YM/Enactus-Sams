#!/usr/bin/env bash
#
# The wire rules (CLAUDE.md §6, §7), enforced where they are cheap to check.
#
#   one transport      `fetch` and `EventSource` appear in src/wire/ and nowhere
#                      else. A second call site is a second place for the
#                      credential posture, the retry policy, the idempotency key
#                      and the queue bound to be wrong — and it will be wrong in
#                      the direction of "it worked on my machine".
#   one decode site    the response envelope is decoded once. A call site that
#                      reaches into `json.error` itself is a call site that will
#                      miss the next code anvil appends.
#   no built URLs      every URL comes from the generated route builder, with
#                      path segments through encodeURIComponent and query values
#                      through URLSearchParams. A concatenated URL is an
#                      injection and an un-typed route in one line.
#   no offsets         cursor pagination only. skip(n) is O(n) server-side, and a
#                      client that can express an offset is a client that will.
#   no secrets in URLs a URL is in the address bar, the history, the Referer and
#                      every analytics payload built from location.href.
#
# `// ban-exempt: <reason>` on the line is the escape hatch, as everywhere else.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [ ! -d src ] || [ -z "$(find src -name '*.ts' -o -name '*.tsx' 2>/dev/null | head -1)" ]; then
    printf 'wire discipline: nothing to check yet\n'
    exit 0
fi

failures=0

# $1 = message, $2 = regex, $3... = paths searched
#
# String literals are NOT stripped here, unlike the other checks: half the
# patterns below are about what is inside one — a path built by concatenation, a
# credential in a query string — and a check that cannot see a string literal
# cannot see any of them.
report() {
    local name="$1"
    local pattern="$2"
    shift 2

    local hits
    hits="$(grep -rnE "$pattern" "$@" --include='*.ts' --include='*.tsx' 2>/dev/null |
            grep -v 'ban-exempt:' || true)"

    if [ -n "$hits" ]; then
        printf 'FAIL  %s:\n%s\n\n' "$name" "$hits" >&2
        failures=$((failures + 1))
    fi
}

# The same, for a pattern that names a CALL SITE rather than a string.
#
# Two kinds of pattern end up here. One is an English word that is also an
# identifier: `offset` and `skip` appear in the prose explaining why this library
# has no way to express either. The other is an identifier that appears in the
# sentence explaining where it is allowed to appear — `sendBeacon` is named in
# `state/analytics.ts` exactly once, in the comment saying the beacon lives in
# `src/wire/` and why.
#
# Both are the same defect, and it is the one tools/check-layering.sh learned
# twice: a check that fires on its own documentation is a check somebody turns
# off. A comment is not a call.
#
# Comments are stripped; string literals are not, for the reason above. A `//`
# preceded by a colon is a URL scheme rather than a comment, so it survives: the
# URL-shaped patterns run through this helper too.
report_code() {
    local name="$1"
    local pattern="$2"
    shift 2

    local hits=""
    local hit
    local code
    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        code="${hit#*:}"
        code="${code#*:}"
        code="$(printf '%s' "$code" | sed -E -e 's,(^|[^:])//.*,\1,' -e 's:^ *\*.*::')"
        if printf '%s' "$code" | grep -qE "$pattern"; then
            hits="${hits}${hit}"$'\n'
        fi
    done < <(grep -rnE "$pattern" "$@" --include='*.ts' --include='*.tsx' 2>/dev/null |
             grep -v 'ban-exempt:' || true)

    if [ -n "$hits" ]; then
        printf 'FAIL  %s:\n%s\n\n' "$name" "$hits" >&2
        failures=$((failures + 1))
    fi
}

# Everything except src/wire/. `find -prune` rather than a grep exclusion so a
# new layer directory is covered the day it is added, not the day someone
# remembers to add it here.
others=()
for dir in src/*/; do
    case "$dir" in
        src/wire/) continue ;;
        *) [ -d "$dir" ] && others+=("$dir") ;;
    esac
done

if [ ${#others[@]} -ne 0 ]; then
    report_code "fetch outside src/wire/" '(^|[^A-Za-z0-9_.])fetch\(' "${others[@]}"
    report_code "EventSource outside src/wire/" '\bnew EventSource\b' "${others[@]}"
    report_code "sendBeacon outside src/wire/" '\bsendBeacon\b' "${others[@]}"
    report_code "envelope decoded outside src/wire/" \
                '(\.error\.code|\[.error.\]|await [A-Za-z0-9_.]+\.json\(\))' "${others[@]}"
fi

# A path built from pieces, rather than by the route builder. Both spellings:
# a template literal holding an interpolation inside a path, and a string
# concatenation onto a path.
report "a URL built by concatenation" \
       '(`/[^`]*\$\{|"/[^"]*" *\+|'"'"'/[^'"'"']*'"'"' *\+)' src

report_code "offset pagination" \
            '(\boffset\b|[?&]skip=|[?&]page=|[?&]offset=|\bskip\(|\bpageNumber\b)' src

report "a credential in a URL" \
       '[?&](token|access_token|refresh_token|api_key|apikey|key|secret|password)=' src

# A credential posture stated per call site is a credential posture that drifts.
# `credentials:` belongs to the client's one fetch site; anywhere else it is a
# second opinion about whether cookies travel.
if [ ${#others[@]} -ne 0 ]; then
    report "a credentials mode outside src/wire/" 'credentials: *["'"'"']' "${others[@]}"
fi

if [ "$failures" -ne 0 ]; then
    printf 'wire discipline: %d violation(s)\n' "$failures" >&2
    exit 1
fi

printf 'wire discipline: clean\n'
