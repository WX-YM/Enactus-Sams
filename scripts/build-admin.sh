#!/usr/bin/env bash
# Regenerates the admin panel's API client from the server's own tables, builds
# the panel, and installs it under public/admin, where the backend serves it.
# Run after any change to src/config or admin/src.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${BUILD_DIR:-$root/build/dev}"

"$build/emit_descriptor" > "$root/admin/hammer.descriptor.json"
(cd "$root/hammer" && npm run --silent build)
node "$root/hammer/dist/codegen/cli.js" codegen \
    --descriptor "$root/admin/hammer.descriptor.json" --out "$root/admin/src/api"
(cd "$root/admin" && npx tsc -b && npx vite build)

# Replace the installed copy whole: a stale precompressed index.html.gz left
# beside a new index.html would be served in its place.
rm -rf "$root/public/admin"
cp -r "$root/admin/dist" "$root/public/admin"
find "$root/public/admin" -type f \( -name '*.html' -o -name '*.js' -o -name '*.css' -o -name '*.svg' \) \
    -exec gzip -kf9 {} \; -exec sh -c 'command -v brotli >/dev/null && brotli -kf "$1" || true' _ {} \;
echo "admin panel installed in public/admin"
