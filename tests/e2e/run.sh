#!/usr/bin/env bash
# End-to-end suites against a real stack: the built backend behind nginx, a
# private Redis, and a throwaway database on your MongoDB replica set.
#
#   tests/e2e/run.sh            # all suites
#   tests/e2e/run.sh api        # one of: api, public, admin
#
# Each suite starts from a fresh legacy-shaped database that has been through
# `enactus_migrate --legacy`, so the migration of existing users is exercised
# every run. Requires: a build in build/dev (or BUILD_DIR), nginx, redis-server,
# python3 with pymongo and argon2-cffi, node, and a Chromium (CHROMIUM).
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${BUILD_DIR:-$repo/build/dev}"
mongo="${MONGODB_URI:-mongodb://127.0.0.1:27017/?replicaSet=rs0}"
port="${E2E_PORT:-8090}"
backend_port="${E2E_BACKEND_PORT:-8095}"
redis_port="${E2E_REDIS_PORT:-6390}"
db="enactus_e2e_$$"
if [ $# -eq 0 ]; then suites=(api public admin); else suites=("$@"); fi

export REPO="$repo"
export BASE="http://127.0.0.1:$port"
export CHROMIUM="${CHROMIUM:-$(ls -d /opt/pw-browsers/chromium-*/chrome-linux/chrome 2>/dev/null | head -1)}"
export PLAYWRIGHT_CORE="${PLAYWRIGHT_CORE:-$repo/hammer/node_modules/playwright-core/index.mjs}"

work="$(mktemp -d)"
export SHOTS="$work/shots"
mkdir -p "$work/storage" "$work/nginx" "$SHOTS"
backend_pid=""
redis_pid=""

cleanup() {
    [ -n "$backend_pid" ] && kill "$backend_pid" 2>/dev/null || true
    [ -f "$work/nginx/nginx.pid" ] && nginx -c "$work/nginx/nginx.conf" -s stop 2>/dev/null || true
    [ -n "$redis_pid" ] && kill "$redis_pid" 2>/dev/null || true
    python3 -c "import pymongo,sys; pymongo.MongoClient(sys.argv[1]).drop_database(sys.argv[2])" "$mongo" "$db" || true
    echo "logs and screenshots: $work"
}
trap cleanup EXIT

wait_for() { for _ in $(seq 1 60); do curl -s -o /dev/null "$1" && return 0; sleep 0.5; done; echo "timed out waiting for $1" >&2; return 1; }

# --- configuration: fresh secrets every run ---------------------------------
{
    "$repo/scripts/generate-secrets.sh"
    cat <<EOF
BIND_ADDR=127.0.0.1
PORT=$backend_port
DOC_ROOT=$repo/public
STORAGE_ROOT=$work/storage
DEFAULTS_DIR=$repo/public/assets
MONGODB_URI=$mongo
MONGO_DATABASE=$db
REDIS_URL=tcp://127.0.0.1:$redis_port
SITE_ORIGIN=$BASE
TRUSTED_PROXIES=127.0.0.1/32
EOF
} > "$work/env"
chmod 600 "$work/env"

# --- nginx: proxy plus the internal media location --------------------------
user_line=""
[ "$(id -u)" -eq 0 ] && user_line="user root;"
cat > "$work/nginx/nginx.conf" <<EOF
$user_line
worker_processes 1;
pid $work/nginx/nginx.pid;
error_log $work/nginx/error.log;
events { worker_connections 256; }
http {
    access_log $work/nginx/access.log;
    client_body_temp_path $work/nginx/body;
    proxy_temp_path $work/nginx/proxy;
    fastcgi_temp_path $work/nginx/fcgi; uwsgi_temp_path $work/nginx/uwsgi; scgi_temp_path $work/nginx/scgi;
    server {
        listen 127.0.0.1:$port;
        server_tokens off;
        client_max_body_size 26m;
        location /protected_storage/ { internal; alias $work/storage/; add_header X-Content-Type-Options nosniff always; }
        location / {
            proxy_pass http://127.0.0.1:$backend_port; proxy_http_version 1.1; proxy_set_header Connection "";
            proxy_set_header Host \$host; proxy_set_header X-Forwarded-For \$remote_addr;
        }
    }
}
EOF
nginx -c "$work/nginx/nginx.conf"

reset_stack() {
    [ -n "$backend_pid" ] && { kill "$backend_pid"; wait "$backend_pid" 2>/dev/null || true; backend_pid=""; }
    [ -n "$redis_pid" ] && { kill "$redis_pid"; wait "$redis_pid" 2>/dev/null || true; }
    redis-server --port "$redis_port" --bind 127.0.0.1 --save "" --appendonly no >"$work/redis.log" 2>&1 &
    redis_pid=$!
    rm -rf "$work/storage"/*
    python3 "$repo/tests/e2e/seed_legacy.py" "$mongo" "$db" >/dev/null
    set -a; . "$work/env"; set +a
    printf 'superadmin-pass-123\n' | "$build/enactus_migrate" --legacy --uploads "$repo/uploads" \
        --create-superadmin owner@enactussams.org >"$work/migrate.log" 2>&1
    "$build/enactus_backend" >"$work/backend.log" 2>&1 &
    backend_pid=$!
    wait_for "http://127.0.0.1:$backend_port/api/site"
}

status=0
for suite in "${suites[@]}"; do
    echo "== $suite"
    reset_stack
    case "$suite" in
        api)    python3 "$repo/tests/e2e/api_test.py" || status=1 ;;
        public) node "$repo/tests/e2e/public_ui.mjs" || status=1 ;;
        admin)  node "$repo/tests/e2e/admin_ui.mjs" || status=1 ;;
        *)      echo "unknown suite: $suite" >&2; status=2 ;;
    esac
done
exit "$status"
