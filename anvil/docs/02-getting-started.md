# 02 — Getting started

Standing up an application on anvil, in the order the compiler will demand it.

**Every snippet below is taken from [`tests/testapp/`](../tests/testapp/), which is compiled by
every build of the test suite.** That is deliberate: a getting-started guide made of invented
snippets is a guide that rots silently, and the first person to notice is somebody following
it. If something here stops being true, the build breaks before you read it.

---

## 1. What you are agreeing to

anvil is a **source dependency, not a prebuilt archive**. The locale table dimensions
`std::array` members inside anvil's own translation units, so the locale count has to be a
literal at the point anvil is compiled — see `docs/01-seams.md` §2 for the alternatives that
were rejected and why.

```cmake
set(ANVIL_CONFIG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/config")
add_subdirectory(external/anvil)

add_executable(myapp src/main.cc)
target_link_libraries(myapp PRIVATE anvil::app)
```

`ANVIL_CONFIG_INCLUDE_DIR` must name a directory containing `anvil_app_config.h`. Leaving it
unset is a **`FATAL_ERROR` at configure time**, not a missing include five minutes into a
build: anything an application must supply fails at configure or compile time, never at
runtime.

Three targets, and which one you link says what you get:

| Target | Contains | Links |
|---|---|---|
| `anvil::foundation` | text, crypto, validation, value types | OpenSSL, ICU, simdutf, xxHash, argon2 |
| `anvil::platform` | everything that talks to the outside world | + Drogon, mongocxx, redis++ |
| `anvil::app` | identity, audit, media, sections, forms, notifications | nothing new |

The split is not tidiness. It makes an accidental dependency from a validator onto a
repository a **link error** rather than a review comment.

---

## 2. The one header anvil includes by name

`anvil_app_config.h`. anvil's own headers include it as `<anvil_app_config.h>` rather than by
path, which is what `ANVIL_CONFIG_INCLUDE_DIR` resolves.

**Nothing in it may name a driver type.** `anvil::foundation` includes this header for the
locale table and links no database driver at all, so a `mongocxx` type reaching this file
surfaces as a missing include inside a text validator — a confusing failure a long way from
its cause. The job table hands every handler a client, which is why it lives in a separate
`anvil_app_jobs.h` included only by `anvil/timer/registry.h`.

### Locales

```cpp
inline constexpr std::array<LocaleSpec, 2> kLocales{{
    {"en", "en", false},
    {"ar", "ar", true},
}};

inline constexpr std::size_t kDefaultLocale = 0;
```

**The order is persisted.** The index is byte 3 of every access token and the value stored in
`users.lang`. Append only: never reorder, never remove. This is the same rule permission bits
live under and it fails the same way — silently, by reinterpreting rows already written.

The third field is right-to-left. Declaring a second locale with a real ICU collation is worth
doing even in a prototype: it exercises the bidi, normalisation and collation paths that a
single Latin locale leaves entirely unmeasured until the day somebody adds one.

### Databases and collections

```cpp
inline constexpr std::array<db::DatabaseSpec, 2> kDatabases{{
    {"application"},
    {"scratch"},
}};

inline constexpr std::array<db::CollectionSpec, 14> kCollections{{
    {"users",               "",           0},
    {"user_sessions",       "expires_at", 0},
    {"drafts",              "expires_at", 1},
    // …
}};
```

A database key, not a physical name: the physical name is a deployment decision read from the
environment at boot.

The middle column is **the load-bearing one**. It names the field a lifetime TTL expires on,
and it is what lets `append_not_expired` filter on it. A TTL index is a garbage collector, not
an access control — the monitor runs roughly every 60 seconds, so an expired session is still
*readable* and would still authenticate. `tools/check-db-discipline.sh` fails the build when a
query against such a collection forgets the filter.

Leave it empty when the collection has no *lifetime*. `audit_log` has a TTL index for a
400-day retention policy and names no expiry field here, because filtering its reads on
`at > now` would return nothing at all. Retention and lifetime are different things and the
empty string is how you say which one you meant.

### Storage namespaces and the variant ladder

```cpp
inline constexpr std::array<fs::NamespaceSpec, 3> kNamespaces{{
    {"content"}, {"media"}, {"guest"},
}};

inline constexpr std::array<std::uint16_t, 5> kVariantWidths{{320, 640, 1024, 1600, 2560}};

inline constexpr std::array<std::array<std::uint16_t, fs::kRoleCount>, 3> kRoleWidths{{
    {{320, 1024, 1600, 2560}},  // content
    {{320,  640, 1024, 1600}},  // media
    {{320,  640, 1024, 1600}},  // guest
}};
```

Also persisted — the namespace index is stored as an int32 on every media row. The directory
name is *also* the `{ns}` segment of a media URL, so there is one string for both rather than
two that can disagree.

Every role must name a width `kVariantWidths` actually contains, and
`anvil/fs/namespace.h` `static_assert`s exactly that. A role pointing at a width nothing
writes resolves to the next rung down for every object in the system, and the table looks
entirely correct while doing it.

---

## 3. Permissions, and why the enum is not the table

```cpp
enum class Perm : std::uint8_t {
    ContentRead   = 0,
    ContentWrite  = 1,
    ContentDelete = 2,

    MediaUpload   = 8,
    MediaDelete   = 9,
    // …
    SystemAnnounce = 120,
};

inline constexpr std::array<anvil::PermName, 11> kPermNameTable{{
    {"ContentRead",  static_cast<std::uint8_t>(Perm::ContentRead)},
    // …
}};
static_assert(kPerms.well_formed());
```

Bit indices are stored in access tokens **and** in the user row. Never renumber: retired bits
are reserved, never reused, because renumbering silently regrants access for every token
currently in flight.

The gaps are deliberate. Leaving room between groups means adding a permission to an existing
group does not push every later bit along — which would be exactly the renumbering above.

`well_formed()` rejects a duplicate bit, a duplicate name, an empty name and an out-of-range
bit, and it is a `static_assert`, so all four are build failures.

---

## 4. Routes fail closed

```cpp
inline constexpr std::array<ac::RoutePolicy, 8> kRoutes{{
    {anvil::PermSet{}, "/login",
     ac::RouteAccess::Public,        ac::RouteMethod::Post},

    {anvil::PermSet{}, "/session/logout",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Any},
    // …
}};
```

A route with no entry is **denied**, not allowed. A test enumerates every registered route and
asserts it declares a policy — that test is the mechanism, and it is why the table cannot
quietly fall behind the handlers.

`RouteAccess::Stealth` answers a denial with the same 404 a nonexistent route produces, in the
same time. Use it wherever the *existence* of the route is itself the secret; a staff-only
endpoint that answers 403 has confirmed it exists.

---

## 5. Wiring it up

`main.cc`, in the order that matters:

```cpp
int main() {
    // 1. The driver instance, exactly once, before any pool. MongoPool is its
    //    sole owner, and it must outlive every pool and every client.
    anvil::db::MongoPool::init(uri, max_pool_size);

    // 2. Pools, sized per workload class. There is no default: every field is
    //    a deployment decision, and hash_threads is a SECURITY control rather
    //    than a tuning knob — Argon2id at 64 MiB x 32 threads is 2 GiB of RSS
    //    from one attacker, so the thread count IS the memory cap.
    anvil::Pools::init(anvil::PoolSizes{.db_threads = 16,
                                        .db_queue = 256,
                                        .cpu_threads = 8,
                                        .cpu_queue = 64,
                                        .hash_threads = 8,     // 8 x 64 MiB
                                        .hash_queue = 32,
                                        .audit_threads = 1,
                                        .audit_queue = 32,
                                        .analytics_threads = 1,
                                        .analytics_queue = 32});

    // 3. Services, constructed with the tables as spans into .rodata.
    // 4. Drogon's loop, last.
}
```

**No migration here.** Boot does not create collections, build indexes or move documents: an
index build holds its collection for the duration and every `db_pool` thread queues behind it,
so a rolling deploy that migrated per instance would stall every request the new instance
accepted ([`00-architecture.md`](00-architecture.md) §7). That work is a second binary, run
once, from one place:

```cpp
// migrate.cc — the whole of it.
int main(int argc, char** argv) {
    const anvil::config::Config config = load(anvil::config::system_environment());
    const anvil::db::MigrationDeps deps{resolve_database_names(config),
                                        config.mongodb_uri,
                                        testapp::kIndexes,
                                        testapp::kRetiredIndexes,
                                        testapp::kSteps,
                                        testapp::kCollectionOptions,
                                        testapp::kSchemaVersion};
    return anvil::db::migrate_main(argc, argv, deps);
}
```

anvil ships no `main` and no routes, but it ships the body of this one — because five
applications spelling `--dry-run` five ways is an operator running the wrong one against
production. [`18-data-migrations.md`](18-data-migrations.md) is the whole of it.

`db_threads` matches the `mongocxx::pool` size — a pool thread that cannot get a connection is
a thread waiting on a mutex. `cpu_threads` follows hardware concurrency. `audit_threads` is
separate and small so a saturated request path cannot starve the record of what saturated it;
its queue holds *batches* of up to 256 rows rather than rows, so a bound of a few dozen is
thousands of rows of headroom. `analytics_threads` is separate from `audit_threads` for the
same reason in the other direction: the event sink is the one that floods, because an
analytics event fires on requests that are not interesting enough to audit, and putting it on
`audit_pool` would starve the forensic record exactly as a request path would
([`17-analytics.md`](17-analytics.md) §10).

The ordering rules that are not obvious:

- **`mongocxx::instance` is constructed exactly once and outlives every pool and client.**
  A client drawn from the pool is *not* thread-safe and must never cross a thread boundary.
- **Nothing blocking ever runs on a Trantor event-loop thread** — not mongocxx, not disk I/O,
  not Argon2, not libvips. One blocking call on a loop thread stalls every connection that
  loop owns.
- **Separate pools per workload class, each with a bounded queue.** One shared pool means one
  image-upload burst starves every database query. A full queue sheds `503`; it never queues
  unboundedly.

---

## 6. The tables the subsystems need

You only supply the table for a subsystem you use. Each is a `std::span` handed to the owning
service at construction, and each is `static_assert`ed at its declaration.

| Using | Supply | Worked example |
|---|---|---|
| Media | `namespaces.h` | [`tests/testapp/namespaces.h`](../tests/testapp/namespaces.h) |
| Any query at all | `indexes.h`, `queries.h` | [`indexes.h`](../tests/testapp/indexes.h), [`queries.h`](../tests/testapp/queries.h) |
| Forms | `field_types.h` | [`field_types.h`](../tests/testapp/field_types.h) |
| Sections | `sections.h` | [`sections.h`](../tests/testapp/sections.h) |
| Jobs | `anvil_app_jobs.h` + the bodies | [`anvil_app_jobs.h`](../tests/testapp/anvil_app_jobs.h), [`jobs.cc`](../tests/testapp/jobs.cc) |
| Rate limiting | `rate_limits.h` | [`rate_limits.h`](../tests/testapp/rate_limits.h) |
| Notifications | `topics.h` | [`topics.h`](../tests/testapp/topics.h) |
| Capability tokens | `capabilities.h` | [`capabilities.h`](../tests/testapp/capabilities.h) |
| The audit log | `audit_actions.h` | [`audit_actions.h`](../tests/testapp/audit_actions.h) |

`docs/01-seams.md` is the reference for all of them. Two are worth calling out here because
their failure mode is silence rather than an error:

**A query without its index is not allowed to land.** Adding a query means adding its index in
the same commit; CI asserts no `COLLSCAN` via `explain`, and `queries.h` is the list it
explains. A query nothing declares is a query nothing checks.

**A notification topic's fan-out strategy is compile-time.** A strategy chosen at runtime is
one that can flip under load, and the whole point of having two is that a broadcast to 20 000
subscribers costs what a broadcast to three costs. See `docs/11-notifications.md` §2.

---

## 7. Running the suite against your own tables

`tests/testapp/` is the reference consumer **and the proof**. Every seam is exercised by
building the tests, so a seam that cannot be satisfied from outside anvil fails there — which
is the only place it can fail cheaply.

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

| Preset | What it is for |
|---|---|
| `asan` | The default. Debug + ASan/UBSan, the whole suite |
| `tsan` | The concurrency suite. Mutually exclusive with ASan, which is a `FATAL_ERROR` |
| `dist` | `-O3 -DNDEBUG`, tests on — the suite against the flags you will actually ship |

The `database`-labelled suites need a MongoDB **replica set**; a single-node `rs0` is fine
locally. They skip cleanly when there is no server rather than failing, so a first build works
before you have one.

Run `tools/check-source-bans.sh`, `tools/check-db-discipline.sh` and
`tools/check-vocabulary.sh` too. They run as CTest entries labelled `lint`, so `ctest` already
does — but the first two are also the ones that tell you something about *your* code rather
than anvil's. The third is about anvil's own prose: it fails on an application's vocabulary
reaching a public header, which is how the vocabulary of the application this library was
lifted out of got there.

---

## 8. Where to read next

| Doc | Read it when |
|---|---|
| `01-seams.md` | Always. Everything you supply is here, with a worked example each |
| `00-architecture.md` | Before wiring `main.cc` — layering, pools, lifecycle, the error model |
| `04-access-control.md` | Before writing a second route |
| `06-input-validation.md` | Before accepting a request body |
| `09-mongodb.md` | Before writing a query |
| `14-config.md` | When your config struct outgrows three fields |

Every mechanism in this library exists in a specific shape for a specific reason, and `docs/`
says what the reason is. **Read the reason before changing the mechanism** — most of them are
there because something failed once, and the reasoning is the first thing lost to a refactor
that does not know it.
