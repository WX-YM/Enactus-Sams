#!/usr/bin/env bash
#
# Bans that are absolute, and therefore worth enforcing mechanically rather than
# in review (ENGINEERING_RULES.md §5, §3).
#
#   innerHTML &co  the markup-insertion sites. There is exactly ONE in this
#                  library and it takes a SanitizedHtml whose constructor is
#                  module-private (docs/01-seams.md §8). Every other route into
#                  the parser is a raw site nobody re-reads.
#   eval/Function  a script source built at run time cannot exist under the CSP
#                  the application serves, and is an arbitrary-code sink under
#                  every other one.
#   Math.random    predictable. Every id, key, nonce and retry jitter comes from
#                  crypto.getRandomValues — jitter from a predictable source is
#                  jitter that synchronises.
#   web storage    a credential or an API response in localStorage/sessionStorage/
#                  IndexedDB outlives the cookie that authorised it. hammer holds
#                  no credential and persists no response (ENGINEERING_RULES.md §5, §2.3).
#   document.cookie  hammer never reads a cookie. The ones that matter are
#                  HttpOnly and the rest are the application's.
#   blob reads     readAsArrayBuffer/readAsDataURL/createObjectURL put a whole
#                  file in the one heap with the least room (ENGINEERING_RULES.md §2.2).
#   XMLHttpRequest  no cancellation, no streams, and a second transport is a
#                  second place for the credential rules to be wrong.
#   any/ts-ignore  a type hole is a runtime surprise with a paper trail.
#                  @ts-expect-error with a reason is the one escape hatch,
#                  because it fails when the reason stops being true.
#   x!.y           a non-null assertion is a claim the compiler was told not to
#                  check. Narrow instead.
#
# A ban that needs an exception in a specific place gets an explicit
# `// ban-exempt: <reason>` on the line, so the exception is visible in review and
# greppable afterwards.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [ ! -d src ] || [ -z "$(find src -name '*.ts' -o -name '*.tsx' 2>/dev/null | head -1)" ]; then
    printf 'source bans: nothing to check yet\n'
    exit 0
fi

failures=0

# $1 = human-readable name, $2 = extended regex, $3... = paths
#
# Line comments and string literals are stripped before matching. Every one of
# these bans is documented in the source that implements the alternative, and a
# check that fires on the sentence explaining the ban is a check people turn off.
check_ban() {
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
        code="$(printf '%s' "$code" |
                sed -e 's://.*::' -e "s:'[^']*'::g" -e 's:"[^"]*"::g')"
        if printf '%s' "$code" | grep -qE "$pattern"; then
            hits="${hits}${hit}"$'\n'
        fi
    done < <(grep -rnE "$pattern" "$@" --include='*.ts' --include='*.tsx' 2>/dev/null |
             grep -v 'ban-exempt:' || true)

    if [ -n "$hits" ]; then
        printf 'FAIL  %s is banned:\n%s\n' "$name" "$hits" >&2
        failures=$((failures + 1))
    fi
}

check_ban "raw markup insertion" \
          '(\.innerHTML|\.outerHTML|insertAdjacentHTML|document\.write|dangerouslySetInnerHTML)' src
check_ban "run-time code generation" \
          '(\beval\(|new Function\(|setTimeout\(["'"'"'\`]|setInterval\(["'"'"'\`])' src
check_ban "predictable randomness" 'Math\.random' src
check_ban "web storage"            '(localStorage|sessionStorage|indexedDB)' src
check_ban "cookie access"          'document\.cookie' src
check_ban "whole-file reads"       '(readAsArrayBuffer|readAsDataURL|createObjectURL)' src
check_ban "a second transport"     'XMLHttpRequest' src
check_ban "a type hole"            '(: *any\b|as any\b|<any>|@ts-ignore|@ts-nocheck)' src
check_ban "a non-null assertion"   '[]A-Za-z0-9_)]![.[;,)]' src

# --- a link that opens a new context hands over window.opener ---------------
#
# Checked as a pair rather than as a ban: `_blank` is legitimate, and `_blank`
# without `noopener` is the defect. rel is on the same line by convention here
# precisely so this check can be one line.
while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    printf 'FAIL  _blank without rel="noopener noreferrer":\n  %s\n' "$hit" >&2
    failures=$((failures + 1))
done < <(grep -rn '_blank' src --include='*.ts' --include='*.tsx' 2>/dev/null |
         grep -v 'ban-exempt:' | grep -v 'noopener' || true)

# --- a javascript: URL is a script source wearing a link --------------------
while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    printf 'FAIL  javascript: URL:\n  %s\n' "$hit" >&2
    failures=$((failures + 1))
done < <(grep -rniE 'javascript:' src --include='*.ts' --include='*.tsx' 2>/dev/null |
         grep -v 'ban-exempt:' || true)

if [ "$failures" -ne 0 ]; then
    printf '%d banned construct(s) found\n' "$failures" >&2
    exit 1
fi

printf 'source bans: clean\n'
