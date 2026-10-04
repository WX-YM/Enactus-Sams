#!/usr/bin/env bash
#
# The live and browser runs, against a reference server this script starts.
#
# --- why this is a script and not a paragraph in a document -----------------
#
# `docs/15-tasks.md` carried a row for four years' worth of phases saying the
# live suite had never executed, because anvil served the reference application
# on no port. anvil ships `anvil_reference_server` now, and the first execution
# found a defect 1,145 unit tests had passed over (`CHANGELOG.md`, Unreleased).
#
# A run that valuable must not depend on somebody remembering six environment
# variables. The equivalent lesson is already written in `tools/check-descriptor.sh`:
# the honest fix for "a person has to remember to run that binary" is a step that
# runs it.
#
#   tools/run-live.sh                 # the Node live suite
#   tools/run-live.sh --browser       # the browser runs as well
#
# --- what it will not do ----------------------------------------------------
#
# It does not build anvil and it never will. hammer cannot make a sibling
# checkout a build dependency (`CLAUDE.md` §1) — that is the same rule that
# keeps the descriptor a committed fixture rather than a generated one — so the
# binary is NAMED and its absence is a clear message rather than a compile.
#
# It also starts its own database, in the sense that matters: the server refuses
# to start against a database it did not create, and this script hands it a name
# of its own on every run and drops it afterwards. "I pointed the reference
# server at the wrong URI" must not be a thing only a backup recovers from.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

browser=0
for argument in "$@"; do
    case "$argument" in
        --browser) browser=1 ;;
        *) printf 'usage: run-live.sh [--browser]\n' >&2; exit 1 ;;
    esac
done

binary="${HAMMER_REFERENCE_SERVER:-}"
if [ -z "$binary" ]; then
    # The paths anvil's own presets build into, newest first. Named rather than
    # searched: a `find` over a sibling checkout is a `find` that one day picks
    # up a binary from a branch nobody is testing.
    for candidate in \
        "$root/../anvil/build/release/tests/anvil_reference_server" \
        "$root/../anvil/build/no-vips/tests/anvil_reference_server" \
        "$root/../anvil/build/asan/tests/anvil_reference_server"; do
        if [ -x "$candidate" ]; then binary="$candidate"; break; fi
    done
fi

if [ ! -x "$binary" ]; then
    printf 'FAIL  no anvil_reference_server.\n' >&2
    printf '      Build it in the anvil checkout:\n' >&2
    printf '        cmake --build build/release --target anvil_reference_server\n' >&2
    printf '      or set HAMMER_REFERENCE_SERVER to one.\n' >&2
    exit 1
fi

# Each suite gets a server, a database and a set of rate-limit budgets of its
# own. One server for both is what this script did first, and it stopped working
# the day sign-in moved to the client: a client-hashed sign-in is a salt call
# and a login call, both charged to the per-address budget of 20 a minute, and
# the live suite's account cases spend it before the browser suite's first
# login. A budget is part of the state a run must not inherit, for the same
# reason a session is.
database=""
work="$(mktemp -d)"
server_pid=""

stop_server() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=""
    fi
    if [ -n "$database" ] && command -v mongosh >/dev/null 2>&1; then
        mongosh --quiet "${ANVIL_REFERENCE_MONGODB_URI:-mongodb://127.0.0.1:27017}" --eval \
            "db.getSiblingDB('$database').dropDatabase(); \
             db.getSiblingDB('${database}_analytics').dropDatabase()" >/dev/null 2>&1 || true
    fi
    database=""
}

cleanup() {
    stop_server
    rm -rf "$work"
}
trap cleanup EXIT

start_server() {
    # A database of this suite's own, which the server claims with a marker
    # document and which nothing else can be holding.
    database="hammer_live_$$_$(date +%s)_$1"
    : > "$work/out"
    : > "$work/err"

    printf 'starting %s\n' "$binary"
    ANVIL_REFERENCE_DB="$database" "$binary" > "$work/out" 2> "$work/err" &
    server_pid=$!

    # Thirty seconds. A first start applies the whole index catalogue and hashes
    # two passwords with Argon2 at production parameters, which is seconds rather
    # than milliseconds.
    for _ in $(seq 1 300); do
        [ -s "$work/out" ] && break
        if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
        sleep 0.1
    done

    if ! kill -0 "$server_pid" 2>/dev/null; then
        wait "$server_pid"
        status=$?
        server_pid=""
        # The server exits 3 when a dependency is unreachable and 1 when it
        # refuses for any other reason. Both are failures HERE — a live suite
        # that skipped because there was no database is a suite whose green
        # means nothing, which is the same rule the suites themselves apply to a
        # missing origin — but the message has to say which, because only one of
        # them is the operator's fault.
        if [ "$status" -eq 3 ]; then
            printf 'FAIL  the reference server has nothing to talk to: %s\n' "$(tail -1 "$work/err")" >&2
            printf '      It needs a MongoDB and a Redis. See anvil docs/03-deployment.md.\n' >&2
        else
            printf 'FAIL  the reference server exited %s: %s\n' "$status" "$(tail -1 "$work/err")" >&2
        fi
        exit 1
    fi

    # RULE 1 of the reference server: the first line of stdout is the base URL,
    # on an ephemeral port, so this reads the port rather than guessing it and
    # two runs on one machine do not collide.
    origin="$(head -1 "$work/out")"
    case "$origin" in
        http://127.0.0.1:*) ;;
        *) printf 'FAIL  the first line of stdout is not a loopback base URL: %s\n' "$origin" >&2; exit 1 ;;
    esac

    # RULE 2: the credentials are drawn at boot and printed once, so what this
    # run signs in with is read out of the server's own output and can be
    # nothing else. Nothing here is a constant, and that is deliberate: a fixed
    # password in a repository is a fixed password in a deployment.
    editor_secret="$(awk '/editor@reference.test/ {print $2}' "$work/out")"
    root_secret="$(awk '/root@reference.test/ {print $2}' "$work/out")"
    if [ -z "$editor_secret" ] || [ -z "$root_secret" ]; then
        printf 'FAIL  the server printed no credentials\n' >&2
        exit 1
    fi

    export HAMMER_LIVE_ORIGIN="$origin"
    export HAMMER_LIVE_USER="editor@reference.test"
    export HAMMER_LIVE_SECRET="$editor_secret"
    export HAMMER_LIVE_SUPERADMIN="root@reference.test"
    export HAMMER_LIVE_SUPERADMIN_SECRET="$root_secret"
    # RULE 3: a verification or reset code is printed as `code <purpose>
    # <address> <code>` rather than mailed, and a suite that registers an
    # account has to read it back to complete the flow. The file, not a copy:
    # codes are printed after the server has answered, while the suite runs.
    export HAMMER_LIVE_SERVER_OUT="$work/out"

    printf 'serving on %s\n\n' "$origin"
}

failures=0

start_server live
npm run --silent test:live || failures=$((failures + 1))
stop_server

if [ "$browser" -eq 1 ]; then
    printf '\n'
    start_server browser
    npm run --silent test:browser || failures=$((failures + 1))
    stop_server
fi

exit "$failures"
