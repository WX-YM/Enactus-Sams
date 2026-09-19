#!/usr/bin/env bash
#
# An application's vocabulary may not appear in the library (ENGINEERING_RULES.md §1).
#
# Two shapes of the same defect:
#
#   A FEATURE NAME. A comment explaining why a mechanism is shaped the way it is,
#   by naming the application it was shaped for. A reader cannot look up a feature
#   that is not in this repository, so the sentence stops carrying its reasoning
#   and the next reader learns that the application is part of the contract. The
#   treatment is never to delete the sentence — it is to replace the citation with
#   the reasoning it points at.
#
#   COPY. A user-visible sentence, in any language, anywhere in src/. hammer ships
#   no string a person reads: the words belong to whoever knows the audience and
#   the locale, which is why anvil's error responses carry a code and no message.
#   An English default is a string that ships to an Arabic user, and it ships from
#   a library, so it ships to all of them at once.
#
# --- the list is the point --------------------------------------------------
#
# The feature-name list below is not universal. It is the vocabulary of the
# applications this library has been developed against, and it is meant to be
# edited: a word is added the moment it is removed from the source.
#
# --- scope: src/ ------------------------------------------------------------
#
# What hammer SHIPS. tests/ is deliberately not scanned: the reference
# application in tests/testapp/ must have a vocabulary and a copy table of its
# own — that is what it is for.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [ ! -d src ] || [ -z "$(find src -name '*.ts' -o -name '*.tsx' 2>/dev/null | head -1)" ]; then
    printf 'vocabulary: nothing to check yet\n'
    exit 0
fi

failures=0

# $1 = what it is, $2 = extended regex, $3 = grep case flag ("-i" or ""), $4... = paths
check() {
    local name="$1"
    local pattern="$2"
    local case_flag="$3"
    shift 3

    local hits
    hits="$(grep -rn ${case_flag} -E "$pattern" "$@" \
                 --include='*.ts' --include='*.tsx' 2>/dev/null |
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
check "an application's feature name" \
      '(hotview|hv-|house ?notes?|bookings?|staff desk|yardclub)' -i src

# A dangling finding number sends a reader to a document that does not exist in
# this repository, which is worse than no citation because it reads as though it
# could be looked up.
check "a finding number from a register this repository does not have" \
      '\bF[0-9]{2,4}\b' '' src

# --- copy -------------------------------------------------------------------
#
# A string literal holding three or more words is a sentence, and a sentence is
# copy. Comments are stripped first: the prose explaining why a mechanism exists
# is the one thing in this repository that is supposed to read like English.
#
# A `throw new Error(...)` is stripped too, and it is the one exemption here.
# `throw` is reserved for PROGRAMMER error in this library (ENGINEERING_RULES.md §3.1) — a
# violated precondition, a misconfigured client — and an expected failure is in
# the return type, where it is a code the application writes the words for. So
# the audience for one of these strings is whoever is holding the stack trace,
# never the person using the application, and firing on it would push every such
# message into a wordless constant that the next reader cannot act on. It is
# narrow on purpose: the strip is anchored to the throw, on the same line, so a
# sentence anywhere else is still copy.
#
# `src/chart` is in this list for the same reason `src/dom` is, and the reason is
# not symmetry: a chart is where an axis caption, a units suffix and the header
# row of the data-table fallback want to be written, and every one of them is a
# word a person reads. The colours, the number formatting and the labels are the
# application's (`ENGINEERING_RULES.md` §9).
copy_targets=()
for dir in src/dom src/chart src/state src/react; do
    [ -d "$dir" ] && copy_targets+=("$dir")
done

if [ ${#copy_targets[@]} -ne 0 ]; then
    hits=""
    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        code="${hit#*:}"
        code="${code#*:}"
        code="$(printf '%s' "$code" |
                sed -e 's://.*::' -e 's:/\*.*\*/::' -e 's:throw new Error(.*::')"
        if printf '%s' "$code" |
           grep -qE '["'"'"'\`][A-Za-z][a-z]+ [A-Za-z][a-z]+ [A-Za-z][a-z]+'; then
            hits="${hits}${hit}"$'\n'
        fi
    done < <(grep -rnE '["'"'"'\`][A-Za-z][a-z]+ [A-Za-z][a-z]+ [A-Za-z][a-z]+' \
                  "${copy_targets[@]}" --include='*.ts' --include='*.tsx' 2>/dev/null |
             grep -v 'ban-exempt:' || true)

    if [ -n "$hits" ]; then
        printf 'FAIL  a user-visible string in the library:\n%s\n' "$hits" >&2
        printf '      Every word a person reads comes from the application, through the\n' >&2
        printf '      copy table in docs/01-seams.md §13.\n\n' >&2
        failures=$((failures + 1))
    fi
fi

if [ "$failures" -ne 0 ]; then
    printf 'vocabulary: %d violation(s). Replace the citation with the reasoning it\n' \
           "$failures" >&2
    printf 'points at — deleting the sentence loses why the code is shaped that way.\n' >&2
    exit 1
fi

printf 'vocabulary: clean\n'
