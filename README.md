# Enactus SAMS — Website & Management Platform

The public website of Enactus SAMS (Sadat Academy — Maadi) and the admin panel
staff use to run it: site copy and galleries, teams and rosters, recruitment
applications, forms, staff accounts and an audit log.

| Part | Built on | Where |
|---|---|---|
| Backend | [anvil](anvil/) (C++20, Drogon, MongoDB, Redis) | `src/` |
| Admin panel | [hammer](hammer/) + React, client generated from the backend's tables | `admin/` → served from `public/admin` |
| Public site | static pages reading the public API | `public/` |

anvil and hammer are vendored internal libraries; read their `docs/` before
changing how a mechanism works.

---

## 1. How it fits together

```
browser ──HTTPS──▶ nginx ──▶ enactus_backend (127.0.0.1:8085) ──▶ MongoDB (replica set)
                     │                                       └─▶ Redis
                     └─ /protected_storage/ (internal): media files the backend
                        authorises with X-Accel-Redirect
```

* **Accounts and sessions** are anvil's. The browser hashes the password with
  Argon2id (hammer, in a worker) and sends only the derived credential; the
  server stores a peppered record of it. Sessions are `__Host-` HttpOnly
  cookies with refresh-token rotation; a permission change or a disabled
  account takes effect on the next request. There is no token in JavaScript.
* **Authorisation** is one table (`src/config/routes.h`): every route is
  Public, Authenticated or Guarded by a permission, and the filter is attached
  by the registration itself. Team managers are additionally scoped to their
  own team's applications and roster.
* **Content** is anvil sections (site copy, `src/config/sections.h`) and
  entries (galleries, teams, rosters, applications; `src/config/entries.h`).
  Images go through anvil's media pipeline: sniffed, size-capped, re-encoded,
  metadata stripped, served per width from disk by nginx.
* **Forms** are anvil forms; the CSV export neutralises spreadsheet formulas.
* **Every staff change and every refused request** lands in the audit log;
  visits are counted by anvil analytics without storing addresses.

The admin panel's API client (`admin/src/api/hammer.generated.ts`) is
generated from the descriptor the backend's own build emits, so a route,
permission or content field cannot drift between the two.

## 2. Repository layout

```
src/config/        the application's tables: routes, permissions, sections,
                   entries, field types, rate limits, indexes, audit actions
src/app/           handlers and services (one file per area), legacy import
src/main.cc        the server        src/migrate.cc   deploy-time migrations
src/emit_descriptor.cc               the descriptor hammer generates from
admin/             the admin panel (React + hammer)
public/            the public website; public/admin is the built panel
deploy/            nginx.conf and a systemd unit
scripts/           generate-secrets.sh, build-admin.sh
tests/             unit tests (tables_test, legacy_test) and tests/e2e
anvil/ hammer/     the vendored libraries
```

## 3. Building

Requirements: GCC 13+ or Clang 17+, CMake 3.25+, Ninja, Node 22, and the
libraries anvil needs — Drogon/Trantor, mongo-cxx-driver, redis-plus-plus with
hiredis, OpenSSL, libargon2, simdutf, ICU, xxHash, libvips (with the AVIF
encoder, e.g. `libheif-plugin-aomenc`), and GTest for the tests. The vcpkg
presets in `CMakePresets.json` install most of them; libvips comes from the
system.

```bash
# with vcpkg (VCPKG_ROOT set):
cmake --preset debug && cmake --build --preset debug

# or against libraries already installed under $PREFIX:
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PREFIX_PATH=$PREFIX -DCMAKE_BUILD_RPATH=$PREFIX/lib
cmake --build build/dev
```

This builds `enactus_backend`, `enactus_migrate`, `emit_descriptor` and
`enactus_tests`. After any change to `src/config/` or `admin/src/`, rebuild
the admin panel (it regenerates the client from the new tables):

```bash
BUILD_DIR=build/dev scripts/build-admin.sh
```

## 4. Configuration

Everything comes from the environment; a missing or malformed value stops the
server at boot rather than running degraded.

| Variable | Meaning | Default |
|---|---|---|
| `SITE_ORIGIN` | The site's origin, e.g. `https://enactussams.org`. The only origin a state-changing request is accepted from | required |
| `MONGODB_URI` | MongoDB connection string. Must be a **replica set** (transactions) | required |
| `MONGO_DATABASE` | Database name | `application` |
| `REDIS_URL` | Redis, e.g. `tcp://127.0.0.1:6379` | `tcp://127.0.0.1:6379` |
| `DOC_ROOT` | The `public/` directory | required |
| `STORAGE_ROOT` | Where uploaded media lives. Must not be inside `DOC_ROOT` | required |
| `DEFAULTS_DIR` | Default images for a fresh database (`public/assets`) | required |
| `BIND_ADDR` / `PORT` | Listener | `127.0.0.1` / `8085` |
| `TRUSTED_PROXIES` | CIDRs whose `X-Forwarded-For` is believed, e.g. `127.0.0.1/32` behind nginx | none |
| `DB_POOL_THREADS`, `HASH_POOL_THREADS`, `HTTP_THREADS` | Pool sizes | 16, 4, 4 |
| `TOKEN_SIGNING_KEY`, `SESSION_PEPPER`, `PREHASH_PEPPER`, `PREHASH_SALT_KEY`, `CODE_PEPPER`, `ADDRESS_INDEX_KEY`, `ANALYTICS_VISITOR_PEPPER`, `PII_SEAL_KEY`, `PII_INDEX_KEY` | 32-byte secrets, unpadded base64url | required |
| `PREHASH_PEPPER_ID` | Name of the current pepper | `p1` |

`scripts/generate-secrets.sh` prints a fresh set of every secret. Generate them
**once** per deployment and keep them out of git. Rotating
`TOKEN_SIGNING_KEY` or `SESSION_PEPPER` signs everyone out; changing
`PREHASH_PEPPER` or `PREHASH_SALT_KEY` in place makes every stored password
unusable.

## 5. Deploying (and migrating the existing site)

On the server, with MongoDB as a replica set and Redis running locally:

```bash
# 1. a service user, the storage directory, and the configuration
sudo useradd --system --home /var/lib/enactus enactus
sudo usermod -aG enactus www-data              # nginx reads media files via the group
sudo install -d -o enactus -g enactus -m 0750 /var/lib/enactus/storage
sudo install -d -m 0750 /etc/enactus
scripts/generate-secrets.sh | sudo tee /etc/enactus/env >/dev/null
sudo tee -a /etc/enactus/env >/dev/null <<'EOF'
SITE_ORIGIN=https://enactussams.org
MONGODB_URI=mongodb://127.0.0.1:27017/?replicaSet=rs0
MONGO_DATABASE=application
DOC_ROOT=/opt/enactus/public
STORAGE_ROOT=/var/lib/enactus/storage
DEFAULTS_DIR=/opt/enactus/public/assets
TRUSTED_PROXIES=127.0.0.1/32
EOF
sudo chmod 600 /etc/enactus/env

# 2. the binaries and the site
sudo install -D build/dev/enactus_backend build/dev/enactus_migrate -t /opt/enactus/bin
sudo rsync -a --delete public/ /opt/enactus/public/

# 3. migrations — run on every deploy; safe to repeat
sudo -u enactus env $(sudo cat /etc/enactus/env | xargs) /opt/enactus/bin/enactus_migrate
```

**The first deploy over the old site** adds the one-shot import of the old
collections (`users`, `teams`, `applications`, `content`, `form_schema`,
`form_submissions` in the same database):

```bash
sudo -u enactus env $(sudo cat /etc/enactus/env | xargs) \
    /opt/enactus/bin/enactus_migrate --legacy --uploads /path/to/old/uploads
```

* Existing users **keep their passwords**: each argon2id hash is rewritten
  offline as an anvil prehash record over the same salt and parameters, so the
  password they already use is exactly what signs them in. A row still holding
  a plaintext password is hashed once during the import.
* `role: superadmin` (and `admin@enactussams.org`) become superadmins;
  permission names carry over; role and team become the staff profile.
* Site copy, galleries (images re-registered through the media pipeline),
  teams with rosters, applications, and the form with its responses are
  imported. The report lists anything skipped and why (an account without a
  password, an invalid email, a remote or unsafe image URL, a value the new
  editor would refuse).
* It runs once per database; `--force-legacy` repeats it. The old collections
  are only read — drop them yourself once you have checked the result.

To create a superadmin from scratch instead (the password is read from stdin):

```bash
sudo -u enactus env $(sudo cat /etc/enactus/env | xargs) \
    /opt/enactus/bin/enactus_migrate --create-superadmin you@enactussams.org
```

```bash
# 4. the service and nginx
sudo cp deploy/enactus.service /etc/systemd/system/ && sudo systemctl enable --now enactus
sudo cp deploy/nginx.conf /etc/nginx/sites-available/enactus   # edit paths and server_name
sudo ln -s ../sites-available/enactus /etc/nginx/sites-enabled/ && sudo nginx -t && sudo systemctl reload nginx
```

nginx must serve `/protected_storage/` from `STORAGE_ROOT` as an `internal`
location (see `deploy/nginx.conf`); without it no image loads.

## 6. Tests

```bash
build/dev/enactus_tests          # unit tests: tables, legacy mappings, password migration
tests/e2e/run.sh                 # end-to-end: api, public, admin (or name one)
```

`tests/e2e/run.sh` starts the built backend behind nginx with a private Redis
and a throwaway database on your MongoDB replica set, seeds data shaped like
the old site, runs `enactus_migrate --legacy`, and drives the API, the public
pages and the admin panel (in Chromium; set `CHROMIUM` if it is not under
`/opt/pw-browsers`). It needs `nginx`, `redis-server`, Python with `pymongo`
and `argon2-cffi`, and Node.

anvil's and hammer's own suites live in their directories (`anvil/tests`,
`npm run check` in `hammer/`).

## 7. License

This repository is proprietary software belonging to **Enactus SAMS**. All rights reserved.
