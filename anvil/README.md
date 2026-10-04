# anvil

A hardened C++20 web-application library for **Drogon + MongoDB + Redis**.

anvil is the reusable half of a production backend: the parts that every web application
needs and nobody should write twice.

| | |
|---|---|
| **Access control** | A 96-byte binary access token (not a JWT), a 128-bit permission bitset checked with one AND, a compile-time route policy table that fails closed, a timing-safe stealth 404, and an epoch-based revocation channel that does not wait out a token lifetime |
| **Input validation** | An allocation-free JSON parser that borrows into the request body, a schema binder where an unknown field is an error, an HTML sanitiser, and a `std::regex`-free validator set |
| **Storage** | Content-addressed sharded files, `openat`-against-a-boot-descriptor TOCTOU defence, streaming upload with the byte cap enforced on arrival, and `X-Accel-Redirect` serving so image bytes never enter the heap |
| **Images** | libvips probe-before-decode (a decompression-bomb defence), EXIF-orientation-into-pixels normalisation, and derived variants generated once at upload |
| **MongoDB** | A BSON codec that never coerces, a versioned-write helper with no window between check and act, an index catalogue, a collection-options catalogue, a resumable data-migration runner with a leased step lock and a real `--dry-run`, and a query catalogue whose test explains every query and fails on a `COLLSCAN` |
| **Jobs** | A Redis-Streams queue with consumer groups, visibility leases, dead-lettering, and recurring declarations that fire once per bucket across N workers |
| **Identity** | Users, sessions, capability tokens, Argon2id password hashing sized as a security control, email verification, and staff management |
| **Forms** | A dynamic form builder with an extensible field-type registry, and a PII vault — AEAD envelope plus a keyed blind index so an identity number is searchable without being decryptable |
| **Sections** | A compile-time-validated CMS: a `constexpr` registry of section shapes, published-vs-draft documents, and a three-tier cache with cross-instance invalidation |
| **Notifications** | An in-app inbox as system of record, SSE with bounded per-connection rings, plus web push, SMTP and signed webhook transports |
| **Analytics** | Sharded, cache-line-padded counters whose memory cost is a compile-time constant, an OpenMetrics scrape, and a product event sink that sheds behaviour before conversions and samples whole sessions rather than half funnels |
| **Migrations** | An index catalogue with retired-index drops and a version marker, plus ordered data steps that are forward-only, resumable by an `_id` cursor, claimed under an expiring lease, and correct because they are idempotent rather than because they are locked |
| **HTML output** | Writers that escape at the point of append and emit their own attribute quotes, a raw-insertion overload that takes only a value the sanitiser produced and re-checks it on the way out, and the origin split that contains an XSS in staff-authored content — a different host, a CSP with no script source, and a path-scoped capability cookie for the render the session cookie cannot reach |

Every mechanism above exists in a specific shape for a specific reason, and
[`docs/`](docs/) says what that reason is. Read the reason before changing the mechanism.

---

## Prerequisites

| | Minimum | Notes |
|---|---|---|
| CMake | 3.24 | Presets v6 |
| Compiler | GCC 12 / Clang 15 | C++20 |
| Ninja | any | |
| vcpkg | current | `VCPKG_ROOT` must be exported |
| MongoDB | 7.0, **as a replica set** | Transactions are required; single-node `rs0` is fine locally. Asserted at boot |
| Redis | 7.0, `appendonly yes` | The default snapshot config can lose the job stream |
| libvips | 8.14 | **Not in vcpkg** — install from your distribution |

```sh
sudo pacman -S libvips          # Arch / CachyOS
sudo apt install libvips-dev    # Debian / Ubuntu
sudo dnf install vips-devel     # Fedora
```

To bootstrap without it, configure with `--preset no-vips`; the image subsystem is then
excluded and `ANVIL_HAS_VIPS` is undefined.

## Build

```sh
export VCPKG_ROOT=/path/to/vcpkg

cmake --preset asan      # first run builds all dependencies — expect 30-60 min
cmake --build --preset asan
ctest --preset asan
```

| Preset | Purpose |
|---|---|
| `asan` | **Default for development.** Debug + ASan/UBSan, tests on |
| `tsan` | Concurrency suite only. Mutually exclusive with ASan |
| `debug` | No sanitizers, for stepping through in a debugger |
| `release` | RelWithDebInfo, tests off |
| `dist` | `-O3 -DNDEBUG`, tests on, so the suite runs against ship flags |
| `no-vips` | Bootstrap without the image subsystem |

## Using anvil in an application

**anvil is a source dependency, not a prebuilt archive.** The locale table dimensions
`std::array` members inside anvil's own translation units, so the locale count has to be a
literal at the point anvil is compiled. See [`docs/01-seams.md`](docs/01-seams.md) §2 for the
full reasoning and the alternatives that were rejected.

```cmake
set(ANVIL_CONFIG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/config")
add_subdirectory(external/anvil)

add_executable(myapp src/main.cc)
target_link_libraries(myapp PRIVATE anvil::app)
```

Your application supplies the tables below. Each one is `constexpr`, lives in `.rodata`, and
is validated by `static_assert` at compile time — a malformed table is a build failure, never
a 500 at three in the morning:

| Table | Header | What it declares |
|---|---|---|
| Locales | `anvil_app_config.h` | The locale list. **Order is persisted** — append only |
| Collections | `anvil_app_config.h` | MongoDB collection names, their database mapping and their expiry fields |
| Permissions | your `perms.h` | The `Perm` enum and its name table |
| Routes | your `routes.h` | Every route, its method, and the permissions it requires |
| Storage namespaces | your `namespaces.h` | The `Ns` list and its role→width table |
| Field types | your `field_types.h` | The form field types your application accepts |
| Sections | your `sections.h` | Your CMS section shapes and their default content |
| Jobs | your `anvil_app_jobs.h` | Background job kinds, their handlers and their recurrences |
| Rate limits | your `rate_limits.h` | The buckets, windows and budgets |
| Idempotency | your own `IdempotencyConfig` | How long a repeated request is recognised, and how much of its response is kept |
| Notification topics | your `topics.h` | Channel kinds, fan-out strategy, retention and coalescing |
| Notification templates | your `topics.h` | One message per locale, placeholders checked across all of them |
| Capability scopes | your `capabilities.h` | The short-lived grants a later request may redeem |
| Audit actions | your `audit_actions.h` | What the security log records, and how each is classified |
| Indexes and queries | your `indexes.h`, `queries.h` | Every index, and every query CI must prove rides one |
| Metrics | your `metrics.h` | The counters, gauges and histograms, and each label's entire value space |
| Analytics events | your `events.h` | What is worth recording, how each is classified, and which need consent |
| Migration steps | your `migrations.h` | The ordered data transforms, and the collection options each collection is created with |
| Collection options | your `migrations.h` | Clustered, capped, timeseries and the `$jsonSchema` validator each collection carries |

[`docs/01-seams.md`](docs/01-seams.md) is the reference for all of them, with a worked example
per seam.

[`docs/02-getting-started.md`](docs/02-getting-started.md) walks through a minimal
application end to end. [`tests/testapp/`](tests/testapp/) is that application, and it is
compiled and tested on every build — so the example cannot rot.

## Layout

```
include/anvil/   public headers
src/
  core/ crypto/ i18n/ input/ auth/ config/ http/ fs/   anvil::foundation  (pure CPU)
  analytics/counters,snapshot,openmetrics                 "     (a counter is incremented
                                                                 inside every layer)
  db/ redis/ images/ accesscontrol/ timer/             anvil::platform    (+ drivers)
  http/rate_limit, retry_after, idempotency, content_headers,
  http/client_address_drogon                               "   (the half of http/ that
                                                                 takes a Drogon request,
                                                                 a response, or Redis)
  identity/ audit/ media/ sections/ forms/ notifications/
  analytics/ (buffer, sessions, ingest, repository, rollup, query)
                                                       anvil::app         (+ the app layer)
tests/           unit + concurrency + database integration, and testapp/
docs/            the reasoning
tools/           the CI checks that turn the rules into build failures
```

Dependencies point downward only. The three-way target split is deliberate: it makes an
accidental dependency from a validator onto a repository a **link error** rather than a
review comment.

## Conventions

[`CLAUDE.md`](CLAUDE.md) is the engineering contract. `.clang-format` and `.clang-tidy` are
checked in. Three scripts run as CTest entries and fail the build rather than a review:

- `tools/check-source-bans.sh` — `std::regex`, bcrypt, `rand`, `localtime`, `strcpy`
- `tools/check-db-discipline.sh` — no `create_index`, `create_collection` or `collMod` outside
  migrations, every repository write through `versioned.h`, every TTL collection queried with an
  explicit expiry filter, and no `$inc` in a migration step
- `tools/check-vocabulary.sh` — no application's feature names in `include/` or `src/`, and no
  citation of a finding or test-plan register this repository does not have, anywhere including
  `tests/`: a feature name may be legitimate in a suite, and a number that resolves nowhere is
  not legitimate in any directory

`-Werror=reorder` is on because member initialisation order must match declaration order.
