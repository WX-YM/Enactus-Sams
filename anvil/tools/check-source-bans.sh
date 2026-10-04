#!/usr/bin/env bash
#
# Bans that are absolute, and therefore worth enforcing mechanically rather than
# in review (CLAUDE.md §5, §8).
#
#   std::regex   a backtracking engine. A crafted input against an RFC 5322-style
#                pattern is catastrophic backtracking — one request pinning a core
#                for seconds. Construction alone costs tens of microseconds, and
#                its behaviour on non-ASCII bytes follows the locale, which is
#                unusable for a UTF-8 system.
#   bcrypt       truncates at 72 bytes, which for a 40-character non-Latin
#                passphrase is silent truncation mid-password. Argon2id only.
#   rand/srand   time-seeded and predictable. Every byte that matters comes from
#                RAND_bytes.
#   localtime    the server's opinion about a timezone, not a fact. The operator's
#                zone is configuration.
#   strcpy/etc   unbounded copies into fixed buffers.
#
# Plus one ban that applies to ONE directory, because that is where its failure
# mode lives:
#
#   std::to_string under src/analytics/. It is the shape of every cardinality bug
#                that has ever shipped — a label value built from a number is one
#                change away from a label value built from a request — and the
#                whole metrics design rests on a value space that is constexpr
#                (docs/17-analytics.md §6). A ban is greppable where the
#                reasoning is not.
#
# Plus two rules that exist only because anvil is a library (CLAUDE.md §1):
#
#   - a public header may not reach into src/, or a private type becomes part of
#     the ABI by accident;
#   - no `using namespace` at namespace scope in any header, because in a library
#     that injects names into every consumer's translation unit.
#
# A ban that needs an exception in a specific place gets an explicit
# `// ban-exempt: <reason>` on the line, so the exception is visible in review and
# greppable afterwards.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

failures=0

# $1 = human-readable name, $2 = extended regex, $3... = paths to search
#
# Comments and string literals are stripped before matching. Every one of these
# bans is documented in the source that implements the alternative, and a check
# that fires on the sentence explaining the ban is a check people turn off.
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
        code="$(printf '%s' "$code" | sed -e 's://.*::' -e 's:"[^"]*"::g')"
        if printf '%s' "$code" | grep -qE "$pattern"; then
            hits="${hits}${hit}"$'\n'
        fi
    done < <(grep -rnE "$pattern" "$@" --include='*.cc' --include='*.h' 2>/dev/null |
             grep -v 'ban-exempt:' || true)

    if [ -n "$hits" ]; then
        printf 'FAIL  %s is banned:\n%s\n' "$name" "$hits" >&2
        failures=$((failures + 1))
    fi
}

targets=()
[ -d src ] && targets+=(src)
[ -d include ] && targets+=(include)
# tools/ holds anvil's own programs as well as these scripts — emit_envelopes.cc
# links the library and is shipped from this repository, so it is held to the
# library's bans rather than to a test's. It was outside the scan until a
# program landed here.
[ -d tools ] && targets+=(tools)

if [ ${#targets[@]} -eq 0 ]; then
    printf 'source bans: nothing to check yet\n'
    exit 0
fi

# The request-path bans do not apply to tests: a test may legitimately name
# std::regex in a comment, and a test does not run inside a request.
check_ban "std::regex"       '(std::regex|#include <regex>)'                "${targets[@]}"
check_ban "predictable RNG"  '(std::rand\(|srand\(|std::mt19937)'           "${targets[@]}"
check_ban "localtime"        '(localtime|localtime_r|localtime_s)'          "${targets[@]}"
check_ban "bcrypt"           '[Bb]crypt'                                    "${targets[@]}" ${TESTS_DIR:-tests}
check_ban "unbounded copies" '(\bstrcpy\(|\bstrcat\(|\bsprintf\(|\bgets\()' "${targets[@]}" ${TESTS_DIR:-tests}

# --- one directory, one extra ban ------------------------------------------
analytics_targets=()
[ -d src/analytics ] && analytics_targets+=(src/analytics)
[ -d include/anvil/analytics ] && analytics_targets+=(include/anvil/analytics)
if [ ${#analytics_targets[@]} -ne 0 ]; then
    check_ban "std::to_string under analytics" 'std::to_string' "${analytics_targets[@]}"
fi

# --- library rule: a public header may not reach into src/ -----------------
if [ -d include ]; then
    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        printf 'FAIL  public header includes a private header:\n  %s\n' "$hit" >&2
        failures=$((failures + 1))
    done < <(grep -rnE '#include +"(\.\./)*src/' include --include='*.h' 2>/dev/null || true)
fi

# --- library rule: no `using namespace` at namespace scope in a header -----
#
# Matched at column zero only: a `using namespace` indented inside a function body
# is scoped to that body and is fine.
for dir in "${targets[@]}"; do
    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        printf 'FAIL  `using namespace` at namespace scope in a header:\n  %s\n' "$hit" >&2
        failures=$((failures + 1))
    done < <(grep -rnE '^using namespace ' "$dir" --include='*.h' 2>/dev/null |
             grep -v 'ban-exempt:' || true)
done

# --- the route-declaration guard is shared, not copied ---------------------
#
# In the system anvil was extracted from this was a lambda copy-pasted into twelve
# controllers, and it checked is_declared(pattern) while the filter resolves
# (pattern, method) — so a handler registered under a method the registry does not
# declare booted clean and then denied every request to itself. Twelve copies of a
# guard is twelve chances for one to be weakened; there is exactly one, and this is
# what keeps it that way.
for file in src/*/controller.cc src/*/*_controller.cc; do
    [ -e "$file" ] || continue
    if grep -q 'declared(' "$file"; then
        if ! grep -q 'accesscontrol/route_declaration.h' "$file"; then
            printf 'FAIL  %s calls declared() without including anvil/accesscontrol/route_declaration.h\n' \
                   "$file" >&2
            failures=$((failures + 1))
        fi
        if grep -qE '(auto|std::function[^=]*) declared *=' "$file"; then
            printf 'FAIL  %s defines its own declared() — use the shared guard\n' "$file" >&2
            failures=$((failures + 1))
        fi
        # Every registration must name the method it registers, or the guard is back
        # to asking only whether the pattern exists somewhere.
        if grep -nE 'declared\([^)]*\)' "$file" |
           grep -qvE 'drogon::(Get|Post|Put|Patch|Delete|Head|Options)'; then
            printf 'FAIL  %s calls declared() without a method argument\n' "$file" >&2
            failures=$((failures + 1))
        fi
    fi
done

if [ "$failures" -ne 0 ]; then
    printf '%d banned construct(s) found\n' "$failures" >&2
    exit 1
fi

printf 'source bans: clean\n'
