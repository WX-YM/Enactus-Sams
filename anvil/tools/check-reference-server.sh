#!/usr/bin/env bash
#
# The reference server starts, and answers a request.
#
# That sentence is one third of the phase-12 gate in docs/15-tasks.md, and it is
# a thing no unit test can express: `anvil_listener_tests` boots Drogon
# in-process and proves the path between a socket and a handler, but nothing in
# this repository had ever started the reference APPLICATION as a process, on a
# port, against a real MongoDB and a real Redis, and made a request to it.
#
# The four properties checked below are the ones a harness on the other side of
# the wire depends on, and each is one of the five rules that keep
# tests/testapp/reference_server.cc a test binary rather than a deployment:
#
#   * the first line of stdout is a loopback base URL (rule 1)
#   * the credentials it prints actually sign in (rule 2)
#   * a stealth route and a route that does not exist answer byte-identically
#   * the route table it serves is the one tests/testapp/ declares (rule 4)
#
# --- skip rather than fail where there is nothing to talk to ----------------
#
# The server exits 3 when a dependency is unreachable and 1 when it refuses for
# any other reason, which is the distinction this script is built around: "there
# is no MongoDB on this machine" is a SKIP and "you pointed me at somebody's
# data" is a failure. One non-zero exit for both would turn the second into a
# green run on a machine that happens to be missing the first.

set -uo pipefail

binary="${1:-}"
if [ ! -x "$binary" ]; then
    printf 'usage: check-reference-server.sh <path to anvil_reference_server>\n' >&2
    exit 1
fi

# CTest is told to read 77 as "skipped".
readonly SKIP=77

work="$(mktemp -d)"
server_pid=""
database="anvil_reference_check_$$_$(date +%s)"

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    # The server's own database, dropped through the server's own connection
    # string rather than through a hardcoded one.
    if command -v mongosh >/dev/null 2>&1; then
        mongosh --quiet "${ANVIL_REFERENCE_MONGODB_URI:-mongodb://127.0.0.1:27017}" --eval \
            "db.getSiblingDB('$database').dropDatabase(); \
             db.getSiblingDB('${database}_analytics').dropDatabase()" >/dev/null 2>&1 || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    printf 'FAIL  %s\n' "$1" >&2
    [ -s "$work/err" ] && { printf '--- server stderr ---\n' >&2; tail -20 "$work/err" >&2; }
    exit 1
}

# Leak detection off: the server is killed mid-run, so every live allocation it
# holds is reported as leaked. What is under test here is that it serves, and the
# sanitiser suites are where its allocations are examined.
ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" \
ANVIL_REFERENCE_DB="$database" \
    "$binary" > "$work/out" 2> "$work/err" &
server_pid=$!

# Thirty seconds. A first start applies the whole index catalogue and hashes two
# passwords with Argon2 at production parameters, which is seconds rather than
# milliseconds and is slower again under a sanitiser.
for _ in $(seq 1 300); do
    [ -s "$work/out" ] && break
    if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
    sleep 0.1
done

if ! kill -0 "$server_pid" 2>/dev/null; then
    wait "$server_pid"
    status=$?
    server_pid=""
    if [ "$status" -eq 3 ]; then
        printf 'reference server: skipped — %s\n' "$(tail -1 "$work/err")"
        exit "$SKIP"
    fi
    fail "the server exited $status before it printed a URL"
fi

url="$(head -1 "$work/out")"
case "$url" in
    http://127.0.0.1:*) ;;
    *) fail "the first line of stdout is not a loopback base URL: '$url'" ;;
esac

# RULE 2: the credentials are drawn at boot, so the password this script signs in
# with is read out of the server's own output and can be nothing else.
password="$(awk '/editor@reference.test/ {print $2}' "$work/out")"
[ -n "$password" ] || fail "the server printed no password for editor@reference.test"

# --- it answers a request ---------------------------------------------------

code="$(curl -sS -o "$work/anon" -w '%{http_code}' "$url/session")"
[ "$code" = "401" ] || fail "an unauthenticated /session answered $code, not 401"
grep -q '"code":"UNAUTHENTICATED"' "$work/anon" ||
    fail "the 401 body is not the one http::append_error_body writes: $(cat "$work/anon")"

# The stealth claim, over a real socket in a real process: a denied route and a
# route that does not exist are indistinguishable.
curl -sS -D "$work/stealth.h" -o "$work/stealth.b" "$url/audit" >/dev/null
curl -sS -D "$work/absent.h"  -o "$work/absent.b"  "$url/no-such-path-at-all" >/dev/null
cmp -s "$work/stealth.b" "$work/absent.b" ||
    fail "a stealth drop and an unmatched route answer with different bodies"
# `date` differs between two responses a second apart and is not a tell; anything
# else that differs is.
strip_headers() { grep -viE '^(date|HTTP/)' "$1" | tr -d '\r' | sort; }
diff <(strip_headers "$work/stealth.h") <(strip_headers "$work/absent.h") >/dev/null ||
    fail "a stealth drop and an unmatched route answer with different headers"

# --- it signs in, and serves the table tests/testapp declares ---------------

curl -sS -D "$work/login.h" -o "$work/login.b" -X POST "$url/login" \
     -H 'Content-Type: application/json' \
     -d "{\"email\":\"editor@reference.test\",\"password\":\"$password\"}" >/dev/null
grep -q '"signed_in":true' "$work/login.b" ||
    fail "signing in with the printed credentials failed: $(cat "$work/login.b")"

access="$(grep -i '^set-cookie: __Host-at=' "$work/login.h" |
          sed 's/.*__Host-at=\([^;]*\).*/\1/' | tr -d '\r')"
[ -n "$access" ] || fail "the login set no __Host-at cookie"

curl -sS -o "$work/session" "$url/session" -H "Cookie: __Host-at=$access" >/dev/null
# RULE 4: these ids come from tests/testapp/route_descriptions.h and from nowhere
# else, so a table the suite asserts and a table a browser run sees cannot be two
# different tables.
grep -q '"content.get":"GET /content/{id}"' "$work/session" ||
    fail "the served route table does not name the reference table's routes: $(cat "$work/session")"
# And the other half of the projection: a route this holder does not reach is
# absent, path and all.
grep -q 'audit' "$work/session" &&
    fail "a route the holder cannot reach appears in the table it was served"

printf 'reference server: started on %s, signed in, and served its own route table\n' "$url"
exit 0
