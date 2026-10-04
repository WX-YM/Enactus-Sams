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
#   * the credentials it prints actually sign in (rule 2) — through the
#     client-prehash flow, the only one it serves (docs/05 §12): ask the salt
#     route, derive the credential, post that; never the password
#   * the salt route and registration disclose nothing: a missing account and a
#     real one answer in the same shape, an email answers the same salt before
#     and after it becomes an account, and a duplicate registration answers
#     byte-identically to a fresh one
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
# The client stage, for a shell: tests/testapp/prehash_credential.cc says why a
# shell cannot do it with the reference `argon2` command.
derive="${2:-}"
if [ ! -x "$binary" ] || [ ! -x "$derive" ]; then
    printf 'usage: check-reference-server.sh <anvil_reference_server> <testapp_prehash_credential>\n' >&2
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
    [ -s "$work/err" ] && { printf '%s\n' '--- server stderr ---' >&2; tail -20 "$work/err" >&2; }
    exit 1
}

# Leak detection off: the server is killed mid-run, so every live allocation it
# holds is reported as leaked. What is under test here is that it serves, and the
# sanitiser suites are where its allocations are examined.
ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" \
ANVIL_REFERENCE_DB="$database" \
    "$binary" > "$work/out" 2> "$work/err" &
server_pid=$!

# Ninety seconds. A first start applies the whole index catalogue and hashes two
# passwords with Argon2 at production parameters, which is seconds rather than
# milliseconds and is slower again under a sanitiser. Thirty was not enough
# under `ctest -j12`: beside a dozen database suites each building the same
# catalogue on the same mongod, a boot that takes eight seconds alone took
# more than thirty, and the case failed for a reason that had nothing to do
# with the server. The wait ends as soon as the URL is printed, so the margin
# costs a healthy run nothing.
for _ in $(seq 1 900); do
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

# Two refusals compared for SAMENESS, less the request id: every answer
# carries its own (http/request_scope.h), which is the one byte-level
# difference two requests are supposed to have.
same_refusal() {  # <file> <file>
    cmp -s <(sed 's/"request_id":"[^"]*"//' "$1") <(sed 's/"request_id":"[^"]*"//' "$2")
}

# The salt route's answer, as the arguments the client stage takes.
salt_args() {
    sed -n 's/.*"salt":"\([^"]*\)","memory_kib":\([0-9]*\),"iterations":\([0-9]*\),"parallelism":\([0-9]*\),"hash_bytes":32}$/\1 \2 \3 \4/p' "$1"
}

ask_salt() {  # <identifier> <output file>
    local code
    code="$(curl -sS -o "$2" -w '%{http_code}' -X POST "$url/auth/prehash" \
                 -H "Origin: $url" -H 'Content-Type: application/json' -d "{\"identifier\":\"$1\"}")"
    [ "$code" = "200" ] || fail "the salt route answered $code for '$1': $(cat "$2")"
    [ -n "$(salt_args "$2")" ] ||
        fail "the salt route's answer is not the shape docs/05 §12 describes: $(cat "$2")"
}

ask_salt "editor@reference.test" "$work/salt.editor"
# shellcheck disable=SC2046  # four words, deliberately split
credential="$(printf '%s' "$password" | "$derive" $(salt_args "$work/salt.editor"))" ||
    fail "the client stage could not derive a credential"

curl -sS -D "$work/login.h" -o "$work/login.b" -X POST "$url/login" \
     -H "Origin: $url" -H 'Content-Type: application/json' \
     -d "{\"identifier\":\"editor@reference.test\",\"credential\":\"$credential\"}" >/dev/null
grep -q '"signed_in":true' "$work/login.b" ||
    fail "signing in with the printed credentials failed: $(cat "$work/login.b")"

# The password itself is not a credential, and neither is a wrong one. A wrong
# credential for a real account and any credential for no account answer with
# the same bytes.
curl -sS -o "$work/plain.b" -X POST "$url/login" -H "Origin: $url" -H 'Content-Type: application/json' \
     -d "{\"identifier\":\"editor@reference.test\",\"password\":\"$password\"}" >/dev/null
grep -q '"code":"UNAUTHENTICATED"' "$work/plain.b" ||
    fail "a plaintext password was not refused: $(cat "$work/plain.b")"
wrong="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
curl -sS -o "$work/wrong.b" -X POST "$url/login" -H "Origin: $url" -H 'Content-Type: application/json' \
     -d "{\"identifier\":\"editor@reference.test\",\"credential\":\"$wrong\"}" >/dev/null
curl -sS -o "$work/nobody.b" -X POST "$url/login" -H "Origin: $url" -H 'Content-Type: application/json' \
     -d "{\"identifier\":\"nobody@reference.test\",\"credential\":\"$wrong\"}" >/dev/null
same_refusal "$work/wrong.b" "$work/nobody.b" ||
    fail "a wrong credential and a missing account answer differently"

# The RIGHT credential from another origin, or from none, signs nobody in: a
# forged sign-in is login CSRF, which plants the attacker's session in the
# victim's browser, and the account routes are behind no filter that would
# check where a request came from. Refused before the body is read, so the
# refusal says nothing about the account either.
for origin in "Origin: https://elsewhere.test" "X-No-Origin: 1"; do
    status="$(curl -sS -D "$work/forged.h" -o "$work/forged.b" -w '%{http_code}' -X POST \
              "$url/login" -H "$origin" -H 'Content-Type: application/json' \
              -d "{\"identifier\":\"editor@reference.test\",\"credential\":\"$credential\"}")"
    [ "$status" = "403" ] || fail "a sign-in with '$origin' was not refused: $status $(cat "$work/forged.b")"
    grep -qi '^set-cookie:' "$work/forged.h" && fail "a refused sign-in with '$origin' set a cookie"
done

# --- registration discloses nothing, and enrols what the salt route serves ---

newcomer="newcomer.$$@reference.test"
ask_salt "$newcomer" "$work/salt.before"
ask_salt "$newcomer" "$work/salt.again"
cmp -s "$work/salt.before" "$work/salt.again" ||
    fail "a missing account's salt is not stable across two asks"

newcomer_password="a long passphrase with non-ASCII in it: كلمة سر"
# shellcheck disable=SC2046
newcomer_credential="$(printf '%s' "$newcomer_password" | "$derive" $(salt_args "$work/salt.before"))" ||
    fail "the client stage could not derive the newcomer's credential"
register() {  # <output file>
    curl -sS -o "$1" -w '%{http_code}' -X POST "$url/signup" -H "Origin: $url" -H 'Content-Type: application/json' \
         -d "{\"email\":\"$newcomer\",\"username\":\"newcomer$$\",\"profile\":{\"given_name\":\"ليلى\"},\"credential\":\"$newcomer_credential\"}"
}
[ "$(register "$work/signup.first")" = "202" ] ||
    fail "registration did not answer 202: $(cat "$work/signup.first")"
[ "$(register "$work/signup.again")" = "202" ] ||
    fail "a duplicate registration did not answer 202: $(cat "$work/signup.again")"
cmp -s "$work/signup.first" "$work/signup.again" ||
    fail "a duplicate registration answers differently from a fresh one"

ask_salt "$newcomer" "$work/salt.after"
cmp -s "$work/salt.before" "$work/salt.after" ||
    fail "an email's salt changed when it became an account"

# --- verification is required before a sign-in ---------------------------------
#
# The right credential for an account awaiting verification is refused exactly
# as a wrong one is — the reference login once issued a session here — and the
# code the server printed is what lets it in.

sign_in_as_newcomer() {  # <output file>
    curl -sS -o "$1" -w '%{http_code}' -X POST "$url/login" -H "Origin: $url" -H 'Content-Type: application/json' \
         -d "{\"identifier\":\"$newcomer\",\"credential\":\"$newcomer_credential\"}"
}
[ "$(sign_in_as_newcomer "$work/pending.b")" = "401" ] ||
    fail "an account awaiting verification was signed in: $(cat "$work/pending.b")"
same_refusal "$work/pending.b" "$work/wrong.b" ||
    fail "an unverified account answers differently from a wrong credential"

code=""
for _ in $(seq 1 50); do
    code="$(awk -v who="$newcomer" '$1 == "code" && $2 == "verify" && $3 == who {print $4}' "$work/out" | tail -1)"
    [ -n "$code" ] && break
    sleep 0.1
done
[ -n "$code" ] || fail "the server printed no verification code for $newcomer"
# Two registrations, two codes: the repeat replaced the first, so its owner
# was sent the new one — the LAST line is the code that works.
[ "$(awk -v who="$newcomer" '$1 == "code" && $2 == "verify" && $3 == who' "$work/out" | wc -l)" -ge 2 ] ||
    fail "a repeated registration of a pending account did not send the replacement code"

verified="$(curl -sS -o "$work/verify.b" -w '%{http_code}' -X POST "$url/auth/verify" \
                 -H "Origin: $url" -H 'Content-Type: application/json' \
                 -d "{\"identifier\":\"$newcomer\",\"code\":\"$code\"}")"
[ "$verified" = "200" ] || fail "the printed code did not verify the address: $(cat "$work/verify.b")"
[ "$(sign_in_as_newcomer "$work/verified.b")" = "200" ] ||
    fail "a verified account could not sign in: $(cat "$work/verified.b")"

# Registering an address that is now an ACTIVE account answers exactly as
# before, and its owner is told somebody tried.
[ "$(register "$work/signup.third")" = "202" ] ||
    fail "a registration of an active account did not answer 202"
cmp -s "$work/signup.first" "$work/signup.third" ||
    fail "a registration of an active account answers differently from a new one"
told=""
for _ in $(seq 1 50); do
    told="$(awk -v who="$newcomer" '$1 == "code" && $2 == "exists" && $3 == who' "$work/out")"
    [ -n "$told" ] && break
    sleep 0.1
done
[ -n "$told" ] || fail "a registration of an active account told its owner nothing"


access="$(grep -i '^set-cookie: __Host-at=' "$work/login.h" |
          sed 's/.*__Host-at=\([^;]*\).*/\1/' | tr -d '\r')"
[ -n "$access" ] || fail "the login set no __Host-at cookie"

# Signs in with a derived credential and prints the access cookie's value.
access_for() {  # <identifier> <credential>
    curl -sS -D "$work/access.h" -o /dev/null -X POST "$url/login" -H "Origin: $url" \
         -H 'Content-Type: application/json' \
         -d "{\"identifier\":\"$1\",\"credential\":\"$2\"}"
    grep -i '^set-cookie: __Host-at=' "$work/access.h" |
        sed 's/.*__Host-at=\([^;]*\).*/\1/' | tr -d '\r'
}
root_password="$(awk '/root@reference.test/ {print $2}' "$work/out")"
ask_salt "root@reference.test" "$work/salt.root"
# shellcheck disable=SC2046
root_credential="$(printf '%s' "$root_password" | "$derive" $(salt_args "$work/salt.root"))" ||
    fail "the client stage could not derive root's credential"
root_access="$(access_for "root@reference.test" "$root_credential")"
[ -n "$root_access" ] || fail "root's login set no __Host-at cookie"

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

# --- chat (docs/22-chat.md §9) --------------------------------------------------
#
# Two accounts and the group the server seeded for them: one sends, edits and
# revokes, the other reads and reports a receipt, and a third account who is in
# no conversation is answered as if the conversation were not there.
read -r _ group < <(grep '^chat ' "$work/out") || true
[ -n "${group:-}" ] || fail "the server printed no chat group"
conversation="/chat/conversations/$group"
chat() {  # <method> <path> <access> <output file> [body]
    curl -sS -o "$4" -w '%{http_code}' -X "$1" "$url$2" -H "Cookie: __Host-at=$3" \
         -H "Origin: $url" -H 'Content-Type: application/json' ${5:+-d "$5"}
}
# Sixteen bytes the composer mints before its first attempt (docs/22 §4.2).
cid="$(head -c 16 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=')"
message="{\"cid\":\"$cid\",\"body\":\"hello from the editor\"}"
[ "$(chat POST "$conversation/messages" "$access" "$work/chat.sent" "$message")" = "201" ] ||
    fail "a member's message was not sent: $(cat "$work/chat.sent")"
seq="$(sed -n 's/.*"seq":\([0-9]*\).*/\1/p' "$work/chat.sent")"
[ -n "$seq" ] || fail "a sent message answered no seq: $(cat "$work/chat.sent")"
# The retry after a lost response: the same client id is the same message.
[ "$(chat POST "$conversation/messages" "$access" "$work/chat.retry" "$message")" = "200" ] ||
    fail "a retried send was not answered with the first: $(cat "$work/chat.retry")"
cmp -s "$work/chat.sent" "$work/chat.retry" || fail "a retried send is a second message"
# A write from another origin stores nothing, and is refused before its body.
status="$(curl -sS -o "$work/chat.forged" -w '%{http_code}' -X POST "$url$conversation/messages" \
          -H "Cookie: __Host-at=$access" -H 'Origin: https://elsewhere.test' \
          -H 'Content-Type: application/json' -d '{"cid":"AAAAAAAAAAAAAAAAAAAAAA","body":"forged"}')"
[ "$status" = "403" ] || fail "a send from another origin was not refused: $status"

[ "$(chat GET "$conversation/messages" "$root_access" "$work/chat.read")" = "200" ] ||
    fail "the other member could not read the conversation: $(cat "$work/chat.read")"
grep -q '"body":"hello from the editor"' "$work/chat.read" ||
    fail "the other member does not see the message: $(cat "$work/chat.read")"
grep -q 'forged' "$work/chat.read" && fail "a forged send was stored"

[ "$(chat PATCH "$conversation/messages/$seq" "$access" "$work/chat.edit" '{"body":"hello, edited"}')" = "204" ] ||
    fail "the sender could not edit: $(cat "$work/chat.edit")"
[ "$(chat PATCH "$conversation/messages/$seq" "$root_access" "$work/chat.edit.other" '{"body":"not mine"}')" = "404" ] ||
    fail "a member edited somebody else's message: $(cat "$work/chat.edit.other")"
chat GET "$conversation/messages" "$root_access" "$work/chat.edited" >/dev/null
grep -q '"body":"hello, edited","reply_to":null,"edits":1' "$work/chat.edited" ||
    fail "the edit did not reach the other member: $(cat "$work/chat.edited")"

# Receipts: the reader's watermark moves, and the sender sees who read.
[ "$(chat POST "$conversation/receipts" "$root_access" "$work/chat.receipt" "{\"read\":$seq}")" = "200" ] ||
    fail "a read receipt was refused: $(cat "$work/chat.receipt")"
reader="$(sed -n 's/^{"user":"\([^"]*\)".*/\1/p' "$work/chat.receipt")"
grep -q "\"read\":$seq" "$work/chat.receipt" || fail "the receipt did not move: $(cat "$work/chat.receipt")"
chat GET "$conversation/messages/$seq/readers" "$access" "$work/chat.readers" >/dev/null
grep -q "\"$reader\"" "$work/chat.readers" ||
    fail "the sender is not told who read: $(cat "$work/chat.readers")"

[ "$(chat DELETE "$conversation/messages/$seq" "$access" "$work/chat.revoke")" = "204" ] ||
    fail "the sender could not revoke: $(cat "$work/chat.revoke")"
chat GET "$conversation/messages" "$root_access" "$work/chat.revoked" >/dev/null
grep -q "\"seq\":$seq,[^}]*\"revoked\":true,\"body\":\"\"" "$work/chat.revoked" ||
    fail "the revoke did not reach the other member: $(cat "$work/chat.revoked")"
grep -q 'hello, edited' "$work/chat.revoked" && fail "a revoked message still carries its text"

# A push nudge (docs/22-chat.md §8.4), through a real job queue. Root's receipt
# covers the first message, so the next one is something root's devices do not
# hold, and root's browser — the endpoint the server registered — is told about
# it once the second's window and the second's grace have passed.
cid="$(head -c 16 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=')"
[ "$(chat POST "$conversation/messages" "$access" "$work/chat.nudged" \
      "{\"cid\":\"$cid\",\"body\":\"ping for push\"}")" = "201" ] ||
    fail "the message to push about was not sent: $(cat "$work/chat.nudged")"
nudge='^push https://push\.reference\.test/root [0-9]* Reference|editor: ping for push$'
for _ in $(seq 1 200); do
    grep -q "$nudge" "$work/out" && break
    sleep 0.1
done
grep -q "$nudge" "$work/out" ||
    fail "the other member was not pushed about the message: $(grep '^push ' "$work/out")"

# Somebody in no conversation: the same bytes as a route that does not exist.
newcomer_access="$(access_for "$newcomer" "$newcomer_credential")"
[ -n "$newcomer_access" ] || fail "the newcomer's login set no __Host-at cookie"
curl -sS -D "$work/chat.stranger.h" -o "$work/chat.stranger.b" "$url$conversation/messages" \
     -H "Cookie: __Host-at=$newcomer_access" >/dev/null
cmp -s "$work/chat.stranger.b" "$work/absent.b" ||
    fail "a stranger reading a conversation is answered differently from an unmatched route"
diff <(strip_headers "$work/chat.stranger.h") <(strip_headers "$work/absent.h") >/dev/null ||
    fail "a stranger reading a conversation is answered with different headers"

# --- image edits (docs/21-image-edits.md §6) ------------------------------------
#
# Only when this build can render: a server built without libvips prints no
# source, and its edit routes answer as missing.
read -r _ media_ns media_source < <(grep '^media ' "$work/out") || true
if [ -n "${media_source:-}" ]; then
    edit() {  # <id> <body> <output file>
        curl -sS -o "$3" -w '%{http_code}' -X POST "$url/media-edits/$media_ns/$1" \
             -H "Cookie: __Host-at=$root_access" -H "Origin: $url" \
             -H 'Content-Type: application/json' -d "$2"
    }
    # A write from another origin, or from none, is refused before anything
    # else happens: the Origin compare is the CSRF control, and an edit is a
    # write that spends a render.
    for origin in "Origin: https://elsewhere.test" "X-No-Origin: 1"; do
        status="$(curl -sS -o "$work/edit.forged" -w '%{http_code}' -X POST \
                  "$url/media-edits/$media_ns/$media_source" \
                  -H "Cookie: __Host-at=$root_access" -H "$origin" \
                  -H 'Content-Type: application/json' -d '{"recipe":"AQhAAEAAgACAAAA"}')"
        [ "$status" = "403" ] || fail "an edit with '$origin' was not refused: $status $(cat "$work/edit.forged")"
    done
    # A centre crop of the seeded 1600 × 1200 picture, the vector the codecs
    # share: 201 and 800 × 600 the first time, the SAME object with 200 after.
    [ "$(edit "$media_source" '{"recipe":"AQhAAEAAgACAAAA"}' "$work/edit.first")" = "201" ] ||
        fail "an edit was not created: $(cat "$work/edit.first")"
    grep -q '"width":800,"height":600' "$work/edit.first" ||
        fail "the edit is not the planned size: $(cat "$work/edit.first")"
    [ "$(edit "$media_source" '{"recipe":"AQhAAEAAgACAAAA"}' "$work/edit.again")" = "200" ] ||
        fail "the same edit of the same source was not found again: $(cat "$work/edit.again")"
    cmp -s "$work/edit.first" "$work/edit.again" ||
        fail "the same edit of the same source is two objects"
    edited="$(sed -n 's/.*"id":"\([^"]*\)".*/\1/p' "$work/edit.first")"
    curl -sS -o "$work/edit.state" "$url/media-edits/$media_ns/$edited" \
         -H "Cookie: __Host-at=$root_access"
    grep -q "\"source\":\"$media_source\",\"width\":1600,\"height\":1200,\"recipe\":\"AQhAAEAAgACAAAA\"" \
         "$work/edit.state" || fail "an edit does not reopen on its source: $(cat "$work/edit.state")"
    [ "$(edit "$edited" '{"recipe":"AQQA"}' "$work/edit.chained")" = "400" ] ||
        fail "an edit of an edit was not refused: $(cat "$work/edit.chained")"
    # A source that does not exist is the shared 404, byte for byte.
    edit "00000000-0000-4000-8000-000000000000" '{"recipe":"AQQA"}' "$work/edit.missing" >/dev/null
    cmp -s "$work/edit.missing" "$work/absent.b" ||
        fail "a missing edit source answers differently from an unmatched route"
    # One audit row per answer, in order, each naming what it was answered: the
    # application's observer (media/edit_routes.h) is the only way an edit
    # reaches an audit log. The last answer is reported after it is sent, so
    # the line may trail the response by a moment.
    for _ in $(seq 1 50); do
        [ "$(grep -c '^edited ' "$work/out")" -ge 6 ] && break
        sleep 0.1
    done
    expected="$(printf '%s\n' \
        "edited FORBIDDEN $media_source - -" \
        "edited FORBIDDEN $media_source - -" \
        "edited OK $media_source $edited created" \
        "edited OK $media_source $edited -" \
        "edited VALIDATION_FAILED $edited - -" \
        "edited NOT_FOUND 00000000-0000-4000-8000-000000000000 - -")"
    [ "$(grep '^edited ' "$work/out")" = "$expected" ] ||
        fail "the edit audit rows are not one per answer: $(grep '^edited ' "$work/out")"

    # The render budget still refuses, now that it is spent on db_pool rather
    # than on the loop thread. Twenty a minute in the `media` bucket, four of
    # them spent above: the same edit again is found rather than rendered, so
    # the loop is cheap until the budget runs out, and then it is a 429 that
    # reaches the application's observer like any other answer.
    limited=""
    for _ in $(seq 1 25); do
        status="$(edit "$media_source" '{"recipe":"AQhAAEAAgACAAAA"}' "$work/edit.budget")"
        [ "$status" = "429" ] && { limited=1; break; }
        [ "$status" = "200" ] || fail "a repeated edit answered $status: $(cat "$work/edit.budget")"
    done
    [ -n "$limited" ] || fail "twenty-five edits in a minute were never refused"
    grep -q '"code":"RATE_LIMITED"' "$work/edit.budget" ||
        fail "the refusal is not the rate-limit error body: $(cat "$work/edit.budget")"
    for _ in $(seq 1 50); do
        [ "$(grep '^edited ' "$work/out" | tail -1)" = "edited RATE_LIMITED $media_source - -" ] && break
        sleep 0.1
    done
    [ "$(grep '^edited ' "$work/out" | tail -1)" = "edited RATE_LIMITED $media_source - -" ] ||
        fail "a refused edit did not reach the observer: $(grep '^edited ' "$work/out" | tail -1)"
fi

printf 'reference server: started on %s, signed in through the salt route, registered, verified, served its own route table, refused forged writes, chatted across two accounts, and edited an image within its budget\n' "$url"
exit 0
