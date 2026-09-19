#!/usr/bin/env bash
#
# Static checks no unit test can make: they are about what the source is ALLOWED
# to contain, not about what it computes (ENGINEERING_RULES.md §6, §7).
#
#   1. Index creation, collection creation and collMod never appear on a request
#      path. An index build holds the collection for its duration and every
#      db_pool thread queues behind it; collMod takes a collection lock; and a
#      collection created by whichever call arrived first is a collection whose
#      one-way doors — clustered, capped, timeseries — were decided by accident.
#   2. A repository write either goes through db/versioned.h or says in one line
#      why it does not. An unversioned read-modify-write is a lost update, and it
#      is invisible in review.
#   3. A repository holding lifetime-bounded rows also filters on the expiry
#      field. A TTL index is a garbage collector, not an access control: the
#      monitor runs roughly every 60 seconds, so an expired session stays
#      readable and would still authenticate.
#   4. One transaction/pool probe per test BINARY, not per translation unit.
#      MongoPool::init throws on its second call, so a per-TU copy means the
#      first one takes the pool and every other reports "no database" and skips.
#      That failure is silent and leaves the suite green while running almost
#      nothing.
#   5. A migration step may not $inc. A lease can expire against a process that
#      is alive but stalled, so two runners can overlap — with $set the double
#      application is a no-op and with $inc it is a wrong number nobody can
#      reconstruct. The seam already makes it hard: StepContext takes FIELDS and
#      the runner wraps them in $set. This is what closes the other route, a step
#      reaching around the accumulator and writing through its read client.
#   6. A repository method taking a client_session& uses guarded_in_transaction().
#      with_transaction retries the callback when the server reports a
#      TransientTransactionError, and a write conflict on a contended document is
#      the ordinary case of that. guarded() converts every driver exception into a
#      Failure, so the helper never sees the retryable one — a routine write
#      conflict becomes a 500 and the transaction is discarded instead of retried.
#      Taking a session IS the signal that a method runs in a transaction, so the
#      rule is mechanical.
#
# Exemptions are explicit and greppable, never silent:
#   // versioned-write-exempt: <reason>
#   // ttl-filter-exempt: <reason>
#   // txn-guard-exempt: <reason>
#   // step-inc-exempt: <reason>

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

failures=0

fail() {
    printf 'FAIL  %s\n' "$1" >&2
    failures=$((failures + 1))
}

[ -d src ] || { printf 'database discipline: nothing to check yet\n'; exit 0; }

# Every file in the tree that may contain a BSON filter.
#
# anvil groups by subsystem rather than by layer, so a repository is
# `src/<subsystem>/repository.cc` — but the app layer also puts a repository and
# the service over it in one subsystem-named file (`src/identity/users.cc`), and
# a service that writes directly is under exactly the same obligation as a
# repository that does. The rule is therefore "everything in an app-layer
# subsystem", not "everything named repository": a check that covers only the
# files whose names happen to say `repository` enforces nothing about the file
# somebody adds next to them.
repo_files() {
    {
        find src -name 'repository.cc' -o -name 'repository.h' \
               -o -name '*_repository.cc' -o -name '*_repository.h' 2>/dev/null
        find src/identity src/audit src/media src/sections src/forms src/notifications \
             src/analytics src/db \
             -name '*.cc' -o -name '*.h' 2>/dev/null
    } | sort -u
}

# --- 1. schema changes are confined to the migration -----------------------
while IFS=: read -r file _; do
    [ -z "$file" ] && continue
    case "$file" in
        src/db/migrations.cc) ;;
        *) fail "$file calls create_index outside the migration — index builds must not run on a request path" ;;
    esac
done < <(grep -rn 'create_index(' src --include='*.cc' --include='*.h' 2>/dev/null || true)

while IFS=: read -r file _; do
    [ -z "$file" ] && continue
    case "$file" in
        src/db/migrations.cc|src/db/collection_options.cc) ;;
        *) fail "$file creates a collection or runs collMod outside the migration — both take a collection lock, and a collection created off the catalogue has its one-way doors chosen by accident" ;;
    esac
done < <(grep -rnE 'create_collection\(|"collMod"' src --include='*.cc' --include='*.h' 2>/dev/null || true)

# --- 2. repository writes carry a version filter ---------------------------
while IFS= read -r file; do
    [ -z "$file" ] && continue
    case "$file" in */versioned.h) continue ;; esac
    if grep -qE '\.(update_one|update_many|replace_one|find_one_and_replace)\(' "$file"; then
        if ! grep -q 'db/versioned.h' "$file" &&
           ! grep -q 'versioned-write-exempt:' "$file"; then
            fail "$file writes without db/versioned.h and without a 'versioned-write-exempt:' note"
        fi
    fi
done < <(repo_files)

# --- 3. lifetime-bounded collections are queried with an expiry filter -----
#
# Keyed on the SUBSYSTEM rather than on a collection name: in anvil the names are
# supplied by the application, so there is no literal to grep for. These four are
# the subsystems whose rows anvil itself gives a lifetime to.
driver_calls='\.(find|find_one|find_one_and_update|find_one_and_delete|find_one_and_replace|update_one|update_many|delete_one|delete_many|count_documents)\('
for subsystem in identity/sessions identity/capabilities identity/verification \
                 notifications/repository notifications/inbox analytics/repository; do
    for file in "src/${subsystem}"*.cc "src/${subsystem}"*.h; do
        [ -e "$file" ] || continue
        grep -qE "$driver_calls" "$file" || continue
        if ! grep -q 'append_not_expired' "$file" &&
           ! grep -q 'ttl-filter-exempt:' "$file"; then
            fail "$file queries lifetime-bounded rows without append_not_expired and without a 'ttl-filter-exempt:' note"
        fi
    done
done

# --- 4. one transaction/pool probe per test binary -------------------------
if [ -d tests ]; then
    while IFS=: read -r file _; do
        [ -z "$file" ] && continue
        case "$file" in
            tests/db_fixture.h) ;;
            *) fail "$file defines its own transaction/pool probe — use tests/db_fixture.h, or the binary gets one probe per translation unit" ;;
        esac
    done < <(grep -rnE '^\[\[nodiscard\]\].*\b(transactions_available|pool_ready)\(\)' \
                  tests --include='*.cc' --include='*.h' 2>/dev/null || true)
fi

# --- 5. a migration step may not $inc --------------------------------------
#
# Keyed on the seam's TYPE NAMES rather than on a directory or on the include,
# because steps are the application's: they live wherever that application puts
# them, and the body is routinely in a .cc that includes its own header rather
# than anvil's. Matching the include found the table and missed every step body,
# which is where a $inc would actually be written.
#
# COMMENTS are stripped and string literals are NOT, which is the opposite of
# tools/check-source-bans.sh and is right for this rule: the banned thing IS a
# string literal — `kvp("$inc", …)` — while the sentence explaining the ban is a
# comment, and a check that fires on its own explanation is a check people turn
# off. It fired on the reference step bodies' own header comment the first time
# it ran.
while IFS= read -r finding; do
    [ -z "$finding" ] && continue
    fail "$finding"
done < <(python3 - <<'STEPINC'
import os, re

for root in ('src', 'tests'):
    for base, _, names in sorted(os.walk(root)):
        for name in sorted(names):
            if not name.endswith(('.cc', '.h')):
                continue
            path = os.path.join(base, name)
            with open(path) as handle:
                text = handle.read()
            if not re.search(r'\b(MigrationStep|StepContext|StepOutcome)\b', text):
                continue
            if 'step-inc-exempt:' in text:
                continue
            code = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
            code = re.sub(r'//[^\n]*', ' ', code)
            if '$inc' in code:
                print(f"{path} declares migration steps and names $inc — a step "
                      f"writes what a document should be, never a delta, because "
                      f"two runners can overlap")
STEPINC
)

# --- 6. a method taking a session must let transient errors propagate ------
while IFS= read -r finding; do
    [ -z "$finding" ] && continue
    fail "$finding"
done < <(python3 - <<'PY'
import glob, re

paths = sorted(set(glob.glob('src/*/repository.cc') + glob.glob('src/*/*_repository.cc') +
                   glob.glob('src/identity/*.cc') + glob.glob('src/audit/*.cc') +
                   glob.glob('src/media/*.cc') + glob.glob('src/sections/*.cc') +
                   glob.glob('src/forms/*.cc') + glob.glob('src/notifications/*.cc')))
# Repository AND service methods alike: taking a client_session& is the signal
# that a method runs inside somebody's transaction, whichever layer it is in.
pattern = r'\n((?:[\w:<>,\s\*&]+?)\s+\w*(?:Repository|Service)::(\w+)\s*\([^{;]*?\)\s*(?:const\s*)?\{)'
for path in paths:
    with open(path) as handle:
        src = handle.read()
    for match in re.finditer(pattern, src):
        if 'client_session&' not in match.group(1):
            continue
        body = src[match.end():match.end() + 400]
        if 'guarded_in_transaction(' in body or 'txn-guard-exempt:' in body:
            continue
        if 'guarded(' in body:
            print(f"{path}: {match.group(2)}() takes a client_session& but uses "
                  f"guarded() — a transient write conflict there becomes a 500 "
                  f"instead of a retry; use guarded_in_transaction()")
PY
)

if [ "$failures" -ne 0 ]; then
    printf '\n%d database-discipline violation(s)\n' "$failures" >&2
    exit 1
fi

printf 'database discipline: clean\n'
