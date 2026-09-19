#!/usr/bin/env bash
#
# An application's vocabulary may not appear in the library (ENGINEERING_RULES.md §1).
#
# anvil was lifted out of one application, and prose travels further than code:
# thirteen public headers and two sources reached the library still citing that
# application's features — its previews, its bookings, its house notes, its staff
# desk — in comments explaining why anvil's own mechanisms are shaped the way
# they are. A reader cannot look up a feature that is not in this repository, so
# the sentence that was carrying the reasoning stops carrying anything, and the
# next reader learns that the application is part of the contract.
#
# The treatment is never to delete the sentence. It is to replace the citation
# with the reasoning it points at, which is the part worth keeping.
#
# --- the list is the point --------------------------------------------------
#
# These words are not universal. They are the vocabulary of the ONE application
# anvil came from, and the next lift will bring a different set. This file is
# meant to be edited: a word is added here the moment it is removed from the
# source, and nothing is served by trying to guess the next list in advance.
#
# --- three scopes, because the rules are not the same rule ------------------
#
# A FEATURE NAME is checked in include/, src/ AND tests/ — everywhere except
# tests/testapp/. The reference application has to have a vocabulary of its own,
# so a word banned from a public header is a word IT may need; a suite around it
# is a different case, and the first version of this check conflated the two.
# Scanning only what anvil ships left ten of them in the suite for a phase:
# `ns/housenote/...` as a traversal candidate in filesystem_test.cc, a route
# shape `/housenote/edit` in access_control_test.cc, a media origin in
# html_sanitize_test.cc, and an anecdote in db_fixture.h about two suites that
# have never existed in this repository. None of them is prose a reader can look
# up, and the route one is worse than unresolvable — it teaches the next reader
# that `/housenote/edit` is a shape anvil knows about.
#
# A CITATION INTO A REGISTER is checked in tests/ as well, because there is no
# directory in which it resolves. docs/16-test-plan.md numbers nothing and anvil
# has no finding register, so `T74 case 4` and `(F237)` point at the same
# nothing wherever they sit — and the first sweep, which scanned only what anvil
# ships, left eighty-odd of them in tests/ for a phase. Three files claimed T21,
# for two unrelated subjects, which is the evidence that the numbering was not
# merely unresolvable but already inconsistent with itself.
#
# A line that needs the word gets an explicit `# ban-exempt: <reason>` or
# `// ban-exempt: <reason>`, the same convention as tools/check-source-bans.sh,
# so the exception is visible in review and greppable afterwards. The one in
# tests/forms_test.cc is the shape to copy: `Fid::parse("F1")` is a grammar case
# whose input happens to look like a finding number.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

shipped=()
[ -d include ] && shipped+=(include)
[ -d src ] && shipped+=(src)

everywhere=("${shipped[@]}")
[ -d tests ] && everywhere+=(tests)

if [ ${#shipped[@]} -eq 0 ]; then
    printf 'vocabulary: nothing to check yet\n'
    exit 0
fi

failures=0

# $1 = what it is, $2 = extended regex, $3 = grep case flag ("-i" or ""),
# $4 = extra grep flags, empty for none, $5... = the directories to scan
#
# The fourth parameter exists because the two rules do NOT share a scope, and
# folding the exclusion into the function body would have quietly exempted
# tests/testapp/ from the register checks as well — which is the opposite of
# what those say, and would have been invisible for as long as nobody wrote an
# `F12` in there.
check() {
    local name="$1"
    local pattern="$2"
    local case_flag="$3"
    local extra="$4"
    shift 4

    local hits
    hits="$(grep -rn ${case_flag} ${extra} -E "$pattern" "$@" \
                 --include='*.h' --include='*.cc' 2>/dev/null |
            grep -v 'ban-exempt:' || true)"

    if [ -n "$hits" ]; then
        printf 'FAIL  %s:\n%s\n\n' "$name" "$hits" >&2
        failures=$((failures + 1))
    fi
}

# One application's features. Matched in comments as well as in code, which is
# the whole reason this is a separate check from tools/check-source-bans.sh:
# that one strips comments before matching, and comments are where this defect
# lives.
#
# tests/testapp/ is excluded by PATH rather than by scanning a narrower set of
# directories, because the reference application's files sit inside the same
# tree as the suites that must not name a product anvil has never contained.
check "an application's feature name" \
      '(hotview|hv-|house ?notes?|housenote|bookings?|staff desk|yardclub)' -i \
      '--exclude-dir=testapp' "${everywhere[@]}"

# A dangling finding number is the same defect in a different spelling. anvil
# has no finding register, so `(F237)` sends a reader to a document that does
# not exist in this repository — which is worse than no citation, because it
# reads as though it could be looked up. Replace it with the reasoning.
#
# One digit, not two: the first version of this bound the number at {2,4} and
# `F3` in http/rate_limit.h sat under it for a whole phase, in a sentence the
# same lift had truncated mid-clause. A lower bound chosen from the examples in
# front of you is a lower bound that misses the next one.
check "a finding number from a register anvil does not have" \
      '\bF[0-9]{1,4}\b' '' '' "${everywhere[@]}"

# And a test-plan number is the third spelling of it. docs/16-test-plan.md
# numbers nothing, so `T74 case 4` resolves nowhere — it reads as a pointer into
# a register that would tell you which case pins the behaviour, and there is no
# such register and never was.
#
# It is the same treatment and the same rule: keep the sentence, drop the
# pointer, and where the pointer was carrying the only statement of why, write
# the why down instead.
check "a test-plan number from a register anvil does not have" \
      '\bT[0-9]{1,3}\b' '' '' "${everywhere[@]}"

if [ "$failures" -ne 0 ]; then
    printf 'vocabulary: %d violation(s). Replace the citation with the reasoning it\n' \
           "$failures" >&2
    printf 'points at — deleting the sentence loses why the code is shaped that way.\n' >&2
    exit 1
fi

printf 'vocabulary: clean\n'
