# 15 — Tasks

The work, decomposed to units with a stated done condition. Kept current as each phase lands;
a task is checked only when its code, its tests and its doc are all green.

**The lift is finished.** Phases 1-6 copied code out of the application anvil was extracted
from, and that framing ended with phase 7. anvil is an independent library: it has no parent
application, no source of truth outside this repository, and nothing in it may be justified by
what one consumer happens to do. Prior art is prior art — it can be read and it can corroborate
a decision, but it cannot settle one, and a name, a feature or a concept belonging to any
application has no business in these headers. Paths below are relative to `~/Code/anvil/`.

**Phase gate:** nothing in phase N+1 starts while any phase-N test is red.

**Doc numbers are allocation order, not a table of contents.** `15` and `16` are meta docs
that landed when they landed; a content doc added later takes the next number rather than
displacing a citation somebody else's code points at.

Legend: `[ ]` open · `[~]` in progress · `[x]` done

---

## Phase 0 — scaffold and plan of record

| | Task | Done when |
|---|---|---|
| [x] | `git init`, directory skeleton | `src/`, `include/anvil/`, `tests/testapp/`, `docs/`, `cmake/`, `tools/`, `ports/` exist |
| [x] | `cmake/AnvilHardening.cmake` | `anvil::flags` carries the full warning + hardening set; ASan⊥TSan is a `FATAL_ERROR`; `anvil_target()` applies include dirs and C++20 |
| [x] | Root `CMakeLists.txt` | Three targets declared; `ANVIL_CONFIG_INCLUDE_DIR` unset is a `FATAL_ERROR` outside a test build |
| [x] | `CMakePresets.json` | `debug`, `asan`, `tsan`, `release`, `dist`, `no-vips`; the tsan test preset includes `concurrency` and excludes `database` |
| [x] | `vcpkg.json` + `ports/redis-plus-plus` overlay | Manifest names every dependency; the overlay strips upstream's hardcoded `-Werror` |
| [x] | `.clang-format`, `.clang-tidy` | Carried over |
| [x] | `CLAUDE.md` | The three axes, the attribution rule, and §1 Library rules |
| [x] | `README.md` | What anvil is, how to build, how an application consumes it |
| [x] | `docs/00-architecture.md`, `docs/01-seams.md` | Layering, pools, lifecycle, error model; all seven seams with worked examples |
| [x] | `docs/15-tasks.md`, `docs/16-test-plan.md` | This file and the test plan |
| [x] | `tools/check-source-bans.sh` | Runs as a CTest entry labelled `lint`; fails on `std::regex`, bcrypt, `rand`/`srand`, `localtime`, `strcpy`; comments and string literals stripped before matching; `// ban-exempt: <reason>` honoured |
| [x] | `tools/check-db-discipline.sh` | CTest entry labelled `lint`; fails on `create_index` outside migrations, on a repository write bypassing `db/versioned.h`, and on a TTL-collection query with no explicit expiry filter |
| [x] | `tests/testapp/anvil_app_config.h` | Two locales, `en` + `ar`; compiles against `anvil/core/locale_spec.h` |
| [x] | `tests/dependency_smoke_test.cc` | Proves OpenSSL CSPRNG, `CRYPTO_memcmp`, `OPENSSL_cleanse`, Argon2id at production parameters with a non-Latin passphrase, simdutf strict rejection, ICU, xxHash and bsoncxx all link and work |
| [x] | `tests/platform_smoke_test.cc` | bsoncxx round-trips BinData/4 and does not coerce on read. **Split out of the foundation smoke test**: `anvil::foundation` does not link the driver, so the BSON assertions could not compile there — the target split caught this on the first build, which is what it is for |
| [x] | `.gitignore` | Build trees, vcpkg installs, editor scratch |
| [x] | `cmake --preset asan && ctest --preset asan` green | 13/13 on a clean checkout: 11 `unit`, 2 `lint` |

---

## Phase 1 — the seams, then foundation

Seams 1 and 2 come first: everything below depends on the shape of `core/`.

### 1a. Seam work

| | Task | Done when |
|---|---|---|
| [x] | `core/perm_set.h` | Lifted verbatim. 128 bits, 16 bytes, explicit little-endian wire format, `constexpr` throughout |
| [x] | `core/perm_catalogue.h` | `PermName`, `PermCatalogue`, `perm_mask`. `well_formed()` rejects duplicate bit, duplicate name, empty name, out-of-range bit |
| [x] | `core/locale_spec.h` | `LocaleSpec`, 40 bytes asserted. **No dependency on the app config header** — this is what breaks the include cycle |
| [x] | `core/locale.h` | `Locale` (1 byte, validating constructors only), `Localized<N>`, `kLocaleCount`, `kAllLocales` |
| [x] | `core/types.h` | `Uuid`, `UserType`, `UserStatus`, `ErrorCode`, `kMaxErrorCode`. **No `Perm`, no `Lang`, no `LocalizedView` literal pair** |
| [x] | `core/user_context.h` | `UserContext` at exactly 64 bytes, trivially copyable, both asserted |
| [x] | `core/result.h` | `Result<T>`, `Result<void>`, `Failure`, `Status`, `ok()`, `fail()` |
| [x] | Seam smoke test | `tests/testapp/perms.h` compiles; `kPerms.well_formed()` and `kPerms.size()` hold as `static_assert`s |

### 1b. Foundation modules

| | Task | Copy from | Notes |
|---|---|---|---|
| [x] | `core/uuid` | `src/lib/core/uuid.*` | v4 + v7, CSPRNG only |
| [x] | `core/thread_pools` | `src/lib/core/thread_pools.*` | `PoolSizes` keeps its four-pool shape; `hash_threads` stays derived from a memory budget |
| [x] | `crypto/*` | `src/lib/crypto/*` | All but `pii.h`: aead, base64url, digest, fast_hash, random, secret, constant_time |
| [x] | `crypto/sealed_identity` | `src/lib/crypto/pii.*` | **Rewrite.** Redaction rule and `normalise_identity` become policy hooks; the two-key envelope and blind index are unchanged **Closed as landed elsewhere.** The row below it already records the deferral, and `forms/pii` in phase 5 already records the arrival — this checkbox is the only thing that was never updated. `include/anvil/forms/pii.h` has the two-key envelope and the blind index unchanged, and `PiiPolicy` makes `normalise` and `redact` function pointers with anvil's defaults named, which is exactly what this row asked for. It is recorded rather than deleted because `grep '^| \[ \] |'` is how anyone finds open work in this document, and a row that is done but unticked makes that list lie |
| [x] | `i18n/*` | `src/lib/i18n/*` | utf8, digits, bidi, normalize. Digit folding stays i18n-generic, not Egypt-specific |
| [x] | `input/arena` | `src/lib/inputvalidation/arena.h` | `RequestArena<N>` over `pmr::monotonic_buffer_resource`; `buffer_` must precede `resource_` |
| [x] | `input/json` | `src/lib/inputvalidation/json.*` | Strings borrow into the body; limits enforced *during* the parse; duplicate keys are an error; numbers kept as source text |
| [x] | `input/schema` | `src/lib/inputvalidation/schema.*` | Unknown field is an error; `finish()` is `[[nodiscard]]`; keep the three `optional_nullable_*` arms |
| [x] | `input/fields` | `src/lib/inputvalidation/fields.*` | `TextRules` constants become caller-supplied; keep the era-arithmetic calendar |
| [x] | `input/{email,datetime,url,html,breach_filter}` | same | `url` scheme allow-list becomes caller-supplied; `breach_filter` seeds stay placeholders with the note that they are |
| [x] | `auth/token` | `src/lib/auth/token.*` | 96 bytes, tag verified before any field is trusted. Locale byte now range-checked |
| [x] | `auth/password` | `src/lib/auth/password.*` | Argon2id, NFC normalisation, `consume_dummy_time()`, `needs_rehash()` |
| [x] | `auth/epoch_cache` | `src/lib/auth/epoch_cache.*` | Direct-mapped, fixed capacity, short TTL, fails closed |
| [x] | `http/{errors,json_writer,csv_writer,origin_check,client_address}` | `src/lib/http/*` | JSON writer emits raw UTF-8, never `\uXXXX` |
| [x] | `fs/{paths,upload,sniff}` | `src/lib/filesystem/*` | `openat` walk one component at a time with `O_NOFOLLOW` on each |
| [x] | `fs/namespace` | `src/lib/filesystem/namespace.h` | **Seam.** `Ns` list and role→width table become app-supplied |
| [x] | `config/*` | `src/lib/config/*` | **Rewrite.** Ships readers and combinators; the app declares its struct |
| [x] | Docs 03, 06, 14 | — | i18n, input validation, config |

| [x] | `crypto/sealed_identity` deferred | **NOT DONE.** It has no consumer until the form vault in Phase 5, and the redaction and normalisation policy hooks want to be designed against that caller rather than guessed at now. Moved to Phase 5; `crypto/pii.*` is not lifted yet |
| [x] | `locale_egy` (optional) | `ANVIL_WITH_EGY`, default OFF. **Both configurations build and both pass 295/295** — verified, because an optional module nobody compiles is an optional module that does not compile |

**Phase 1 gate: MET.** 295/295 green under ASan + UBSan, in both the `ANVIL_WITH_EGY=ON`
and `=OFF` configurations. `anvil::foundation` links only OpenSSL, ICU, simdutf, xxHash and
argon2 — enforced by the target's link list rather than by inspection, and demonstrated when
the BSON assertions in the smoke test refused to compile against it.

Ported: `core_types` (reduced to machinery), `errors`, `uuid`, `crypto`, `utf8`, `i18n`,
`token`, `password`, `json`, `fields`, `html_sanitize`, `filesystem`, `client_address`,
`validation_fuzz`. Written fresh: `seams_test` (the seams from outside anvil) and
`config_test` (the rewritten reader machinery).

---

## Phase 2 — platform

| | Task | Notes |
|---|---|---|
| [x] | `db/mongo_pool` | Sole owner of `mongocxx::instance`; `instance_` declared before `pool_` and `-Werror=reorder` keeps it that way |
| [x] | `db/codec` | **Decoding never coerces.** `Uuid`→BinData/4, `PermSet`→BinData/0, enums range-checked. `read_localized` loops `kLocaleCount` |
| [x] | `db/collation` | `needs_collation(Locale)` and `collation_for(Locale)`, taken by both the query and the index |
| [x] | `db/server_floor` | Boot assertion: ≥7.0 and a replica set, both fatal; FCV lag is loud but not fatal |
| [x] | `db/repository` | `RepositoryBase` (no vtable), `translate()`, `guarded` / `guarded_in_transaction`, `append_not_expired()` |
| [x] | `db/versioned` | Lifted verbatim. Expected version in the filter, `$inc` in the update, `$currentDate` not a client clock |
| [x] | `db/migrations` | **Seam.** `IndexSpec`, `PartialFilter`, idempotent apply, `schema_meta` versioning. The catalogue is the app's |
| [x] | `db/query_catalogue` | **NOT DONE.** The explain contract needs a query list to explain, and anvil has none until an application layer exists to issue queries. Moved to Phase 3, where the identity and media repositories give it something real to check **Closed as landed elsewhere**, in phase 3, where the row says it was going and where its own checkbox is ticked. `query_catalogue.h`, `query_catalogue.cc` and `query_catalogue_db_test.cc` all exist and the explain contract runs against a live cluster. Same reason as the row above for ticking it rather than deleting it |
| [x] | `redis/redis_client` | Thin pooled wrapper |
| [x] | `images/*` | probe (before decode), strip (EXIF into pixels, then drop; sRGB then drop ICC), variants (always from the master), crop (parameters, never baked in) |
| [x] | `accesscontrol/decision` | Pure function, no Drogon type, no I/O. Superadmin is an explicit check, never an all-ones mask |
| [x] | `accesscontrol/{cookies,stealth,epoch_resolver}` | `__Host-` prefix; one shared 404 response object installed as the framework's own |
| [x] | `accesscontrol/access_filter` | Response sent **before** the audit row is enqueued; the denial record carries the true code |
| [x] | `accesscontrol/route_registry` | **Seam.** `policy_for` / `is_declared` take a span; `AccessControlDeps` gains `routes` |
| [x] | `accesscontrol/route_declaration` | `declared()` throws at boot on an undeclared pattern |
| [x] | `http/rate_limit` | **Seam.** Machinery ships; rules are the app's. Buckets keyed by identity+rule, never by route |
| [x] | `timer/{queue,streams,promoter}` | Consumer groups, visibility leases, dead-letter, `XAUTOCLAIM` reclaim |
| [x] | `timer/registry` | **Seam.** `JobSpec` ships; the job table is the app's |
| [x] | Docs 04, 08, 09, 10 | access control, images, mongodb, timer/jobs |

**Phase 2 gate: MET.** 389/389 under ASan + UBSan against a live `rs0` replica set and
Redis; 9/9 under TSan with the `database` label excluded.

Four more tables became seams: collections (name, lifetime-expiry field, database), the
route table, the job table and its recurrences, and the index catalogue. `PartialFilter`
became a builder function pointer rather than an enum — half the original enumerators named
one application's fields, and a pointer expresses any filter the server accepts while staying
usable in a constexpr table.

Two structural findings, both from the build rather than from review:

- The job table could not live in `anvil_app_config.h`. A handler is handed a
  `mongocxx::client`, and `anvil::foundation` includes that header for the locale table and
  links no driver. It moved to `anvil_app_jobs.h`, included only by the platform-only
  registry.
- ASan caught a **use-after-free in newly written code**: `partial_filter_expression` stores
  a view, and the builder returned by value died at the end of the statement. The lifetime is
  now explicit in the signature.

Test adaptation needed judgment three times. `core_types_test` and `codec_test` lost their
application-table assertions (permission bit indices, audit-action numbering) — those belong
to whoever declares the tables. `access_control_test` lost ~878 lines of product-route
assertions and kept the twelve structural cases that hold for any table; the reference
application carries the product-shaped ones as `static_assert`s over its own table, which is
strictly better than a test. `thread_pools_test`'s audit-isolation case is deferred to
Phase 3 rather than weakened, because it needs the service it is about.

---

## Phase 3 — identity, audit, media

| | Task | Notes |
|---|---|---|
| [x] | `identity/users` | Repository + the user schema anvil owns. The field names are PUBLISHED (`identity/user_fields.h`) because the index catalogue is the application's and cannot name a column it has to guess |
| [x] | `identity/sessions` | Refresh-token rotation as a compare-and-swap; device counts for N users in one `$in` plus `$group`, never one query per row |
| [x] | `identity/capabilities` | **Seam.** Scope table is the app's; `single_use` decides consume vs verify, so a caller cannot choose wrong |
| [x] | `identity/authz` | The three-tier epoch resolver: local cache → Redis → MongoDB. Fails closed |
| [x] | `identity/password_service` | Posts to `hash_pool`; sheds through the return value so a callback is never re-entrant |
| [x] | `identity/verification` | OTP; the row stores only a peppered digest, and the four controls that make twenty bits acceptable are enumerated in the header |
| [x] | `identity/staff` | Permission grid; every change bumps `perm_epoch`. The population guard is a shared document two demotions collide on |
| [x] | `audit/*` | **Seam.** Buffered sink on its own pool, classify → coalesce → bound, drop counts per class reported once per flush |
| [x] | `media/*` | Upload pipeline loop→cpu→db→cpu→db; `X-Accel-Redirect` serving; format negotiation with `Vary: Accept` |
| [x] | `db/query_catalogue` | **Seam.** Deferred from Phase 2 and landed here, now that there are queries to explain |
| [x] | Docs 05, 07 | auth/sessions, filesystem |

**Phase 3 gate: MET.** 566/566 green under ASan + UBSan against a live `rs0` replica set and
Redis; 9/9 under TSan with the `database` label excluded; 437/437 in the
`ANVIL_WITH_VIPS=OFF, ANVIL_WITH_EGY=OFF` configuration. Baseline at the start of the phase
was 384.

Suites added: `audit_buffer`, `media_serving`, `identity`, `seams_phase3` (unit);
`auth_db`, `audit_db`, `media_db`, `staff_db`, `verification_db`, `auth_service_db`,
`media_concurrency`, `query_catalogue_db` (database).

### What anvil owns of a user, and what it does not

`identity/users` owns exactly the fields that decide whether a credential may in and what it
may do. It owns no profile — no display name, no address, no preference beyond the locale the
access token has to carry anyway.

That is a **namespace agreement rather than a partition of the document**. Every write is a
targeted `$set` of anvil's own keys and never a whole-document replace, so an application
keeps its own fields in the same row and reads them with its own projection. Two collections
joined on a user id would cost a second round trip on every screen that renders a person.

Roles are resolved but not stored: `effective_permissions(direct, role_ids, RoleTable)` is
machinery, and where role definitions live is the application's business. That is what keeps
`perms` and `eff` two fields with two meanings rather than one field written twice.

### Three defects the build found, none of them in new code

- **`apply_migrations` failed on a fresh cluster.** `drop_if_present` tolerated `IndexNotFound`
  (27) and `NamespaceExists` (48) but not `NamespaceNotFound` (26) — which is what dropping a
  retired index from a collection that does not exist yet returns, and therefore what EVERY
  retired index returns on a cluster's first migration. Invisible on any cluster that already
  had the collection, and fatal on the one deploy where it matters. Found by the first
  database test to run against a scratch database.
- **Three dangling projections.** `options.projection(build().view())` stores a view into a
  `document::value` that died at the end of the statement; `view_or_value` takes ownership of
  a value and a bare pointer for a view, so dropping `.view()` was the whole fix. ASan caught
  it inside the driver on the *next* call, a long way from the mistake — the same hazard
  `index_options` had already documented for `partial_filter_expression`, in code written
  before that note existed.
- **The reference application's index catalogue named `drafts`' expiry field with no TTL index
  to expire it.** Naming the field without building the index gives rows filtered out of every
  read and never actually removed: a collection that grows forever while looking empty. Found
  by a new structural test that walks `kCollections` against `kIndexes`.

### Two things that turned out to be seams

`CapabilityScope` and `AuditAction` both started as enums in the lifted code and both were
entirely one application's vocabulary — sixty-eight audit actions naming bookings, hotviews
and shop stock. Neither dimensions anything anvil compiles, so both arrive as a `std::span`
handed to the owning service.

The audit one carries a second column that is not a label: `AuditClass` decides what may be
lost under load. anvil cannot infer it — "this row is the only copy of what somebody did" is a
statement about an application's own operations — and getting it wrong is a shedding policy
that discards mutations to keep denials.

### Three places the layering pushed back

- **`media/service.cc` does not call `images::unlink_variants`,** although that function needs
  nothing from libvips. It lives in a translation unit compiled only when the image subsystem
  is on, and the media ROW lifecycle must not depend on whether this build can encode: a
  deployment serving already-stored objects with no encoder still has to be able to delete
  one. The `no-vips` configuration is what proves it, and it is why that configuration is part
  of the gate rather than a courtesy.
- **The image stage is its own translation unit** (`media/pipeline.cc`) for the same reason,
  rather than a method on `MediaService`.
- **`tools/check-db-discipline.sh` covered none of the new code.** Its `repo_files()` matched
  only files named `*repository*`, and the app layer puts a repository and the service over it
  in one subsystem-named file. Extending it to every app-layer subsystem immediately found an
  undocumented unversioned write in `staff.cc` — which is the check working, one commit after
  it would have been too late to notice.

### One deliberate weakening, stated rather than hidden

`query_catalogue_is_well_formed` does not check that a `QuerySpec`'s filter is non-null, and
`collection_is_declared` exists because `collection_spec(...) == nullptr` is not foldable in a
constant expression on this compiler. A `well_formed()` that does not compile is worse than
one that checks less; the null filter is caught at runtime by `explain_query`, by name, and
asserted by the seam test.

---

## Phase 4 — sections CMS

| | Task | Notes |
|---|---|---|
| [x] | `sections/registry` | **Seam.** `FieldSpec`, `ImageSpec`, `SectionSpec`, `ct::` validators, binary search, `registry_is_sorted`, `fields_fit_buffers`, `choices_are_well_formed` |
| [x] | `sections/defaults` | **Seam.** The three structs and `defaults_match_registry()`; the content is the app's |
| [x] | `sections/content` | `SectionContent`, `merge`, `canonicalise` — a patch and a whole document are the same type |
| [x] | `sections/payload` | Registry-driven bind, validate, pre-serialise |
| [x] | `sections/codec` | Whole-subdocument writes so a removed field actually disappears |
| [x] | `sections/repository` | Two documents per key: `{k,s:0}` published, `{k,s:1}` draft |
| [x] | `sections/service` | Three-tier cache, Redis pub/sub invalidation on a dedicated thread, ETag from canonical content |
| [x] | `sections/bootstrap` | Insert-if-absent, tolerating duplicate-key so N instances converge with no coordination |
| [x] | Doc 12 | sections CMS |

**Phase 4 gate: MET.** 626/626 green under ASan + UBSan against a live `rs0` replica set and
Redis; 9/9 under TSan with the `database` label excluded; 613/613 in the `ANVIL_WITH_VIPS=OFF`
configuration. Baseline at the start of the phase was 566.

Suites added: `sections` (unit), `content_db` (database).

### Deferred, deliberately: backup, restore and reset

The snapshot taken before an overwrite, the restore from it, and the reset to shipped defaults
are a second collection with its own retention policy, and none of the eight rows above names
them. They are not in this phase and are recorded in docs/12 §10 rather than silently dropped,
along with the two properties that cost a real outage when they were learned: a restore
REPLACES rather than delete-then-inserts, and the restored version is `previous + 1` so an
in-flight edit fails instead of colliding with a reused number.

`resolve_default_content` is exposed separately from the bootstrap loop with that caller in
mind, so "what a restore produces" and "what a fresh database gets" are the same content by
construction rather than by review.

### Three tables became one, and one enum became machinery

The registry was three application-owned things wearing one name. The field list, the default
copy and the icon allow-list are all the application's; `FieldSpec`, `ImageSpec`, `SectionSpec`
and the validators are anvil's. The split is a `std::span` handed to the service, because a
section table dimensions nothing anvil compiles — the one seam rule from docs/01.

`FieldType::Icon` was the interesting case. A list of icon names is one product's furniture,
but the *shape* — a field whose value must be one of a compile-time list, with the list on the
wire so an editor renders a picker — is entirely general, and it is the only defence against a
failure that is otherwise silent in every direction. It ships as `FieldType::Choice` carrying
its own `std::span<const std::string_view>`.

`FieldSpec` is no longer 56 bytes and cannot be: the labels are `Localized<>` rather than an
`en`/`ar` pair, and the choice list is a span. Both `FieldSpec` and `ImageSpec` now assert a
FORMULA in terms of `kLocaleCount` instead of a number — the number is the application's, and
what must not change is that there is no interior padding.

### `fields_fit_buffers` was a bound with nothing behind it

Lifted verbatim, `kMaxFieldsPerSection` bounded "merge buffers and validation bitsets" that the
lifted code did not actually have — every walk was `SectionContent::find`, a linear scan, so
serialising a 36-field section cost 36 × 36 string compares.

It bounds a real array now. `index_content` builds one pointer per declared field and per
declared slot in a `std::array`, and `canonicalise`, `serialize`, `content_etag` and
`check_required` all take it, which makes each of them one pass. A check whose name describes
nothing is a check that gets deleted by the next reader.

### Two defects the build found

- **The write path serialised into tier 1 and then immediately nulled it.**
  `publish_invalidation` drops the local entry as its first act, and the write called it
  *after* storing its freshly serialised payloads — so the comment promising "the first reader
  after a write does not pay for it" was false, and the next reader went to MongoDB. Present in
  the lifted code and invisible without a test that peeks the cache after a write. The order is
  now reversed and the dependency is stated at both ends.
- **The structural index check could not express a collection under a compound primary key.**
  `EveryQueriedCollectionHasAtLeastOneIndex` assumed "covered" meant "declared in the
  catalogue", which is true of every collection phase 3 had. `sections` declares none on
  purpose — the compound `_id` is the index. Exempting the collection would have blinded the
  check; instead it now BUILDS the filter and asks whether it is a primary-key equality and
  nothing else, so a read filtering on a sub-key of `_id`, or on `_id` alongside something
  else, still fails.

### One thing the layering pushed back on

`bootstrap_sections` does not register default images. Registering a file means probing,
normalising and deriving variants for it, which needs libvips — and the section ROW lifecycle
must not depend on whether this build can encode, for exactly the reason `media/service.cc`
does not call `unlink_variants`. The file-to-id step is a callable the caller supplies;
`sections/default_images.h` ships one over `MediaService` in a translation unit compiled only
when the image subsystem is on. The `no-vips` configuration is what proves it, which is why
that configuration is part of the gate rather than a courtesy.

---

## Phase 5 — form maker

| | Task | Notes |
|---|---|---|
| [x] | `forms/field_type` | **Seam, and a rewrite.** `FieldTypeSpec` (40 B), `FieldTypeFlag`, `table_is_well_formed`, `is_dense_from_zero`, `answer_kind_of`, `bind_field_type`; `AttachmentHooks` moved to its own header so the seam stays driver-free |
| [x] | `forms/validators` | text, number, email, date, select_single, checkbox_multi, attachment_uuid. Every one checks the SHAPE before the value |
| [x] | `forms/fid` | `Fid`, 5 bytes, only `Fid::parse` constructs one — and it is `constexpr`, so the grammar is assertable at compile time |
| [x] | `forms/repository` | One collection with a discriminator, not one per form. Field names published for the index catalogue |
| [x] | `forms/service` | Definition cache, `validate_schema`, `check_edit_compatible`, `derive_has_pii`, the drop transaction and the straggler reap |
| [x] | `forms/submission_service` | Table dispatch replacing the switch; buffered counter flush; the drop/submit reap race |
| [x] | `forms/pii` | AEAD envelope + blind index; at most one identity field per form; normalisation and redaction as policy hooks. **This is the Phase-1 `crypto/sealed_identity` deferral, landed against its real caller** |
| [x] | `forms/export` | Streaming CSV with the formula-injection guard and a UTF-8 BOM. Named `export` rather than `controller`: anvil holds no routes |
| [x] | **Benchmark** | Table dispatch vs the switch: **−1.5% to −0.4%**, i.e. indistinguishable. Recorded in docs/13 §9 with the machine and the method |
| [x] | Doc 13 | dynamic forms |

**Phase 5 gate: MET.** 703/703 green under ASan + UBSan against a live `rs0` replica set and
Redis; 9/9 under TSan with the `database` label excluded; 690/690 in the `ANVIL_WITH_VIPS=OFF`
configuration. Baseline at the start of the phase was 626.

Suites added: `forms` (unit, 56 cases), `forms_db` (database, 22 cases).

### The seam is the field-type table, and it is bigger than it looks

`FieldTypeSpec` ships; the eleven field types one application had do not. What was less
obvious is how much else travelled with them. A `FieldSpec` needs to know whether its type
allows line breaks and what its code-point cap is, and the validator signature the seam
publishes hands a validator nothing but the `FieldSpec` — so those facts have to be RESOLVED
onto the field rather than looked up per answer.

`bind_field_type` is that one place, and both the create path and the decode path go through
it. The alternative — a switch on the type inside the text validator, which is what the lifted
code had — is exactly the switch this seam exists to delete.

### Three things the extension point needed that the plan did not name

- **`answer_kind_of`.** The BSON shape an answer takes was a switch on the type in the writer
  and a second switch in the reader. With the table it has to be derived in one place, or a
  custom validator filling the wrong member produces a row the decoder refuses months later
  with an integrity fault nobody can trace. The submission service now checks the kind the
  validator produced, so a mismatched table is a refused write at the moment it is wrong.
- **`AttachmentHooks`.** "The referenced media must exist, be in namespace `Form`, be owned by
  the submitter, and not already be bound" is four application facts in one sentence. It is
  three callables, and all-or-none, because a half-supplied set binds objects nothing ever
  releases and the asymmetry is invisible until the sweeper does not run.
- **`CountFlushFn`.** The lifted counter flush called `MongoPool::instance()` and
  `drogon::app().getLoop()->runEvery` directly. A library cannot do either: which pool a flush
  runs on and which loop owns the timer are the application's. The buffer stays; the two ends
  are handed in.

### `table_is_well_formed` cannot check the one thing it most wants to

Under `-fsanitize=undefined`, GCC 16 refuses to fold `spec.validate == nullptr` into a
constant expression — so a `static_assert` over a `table_is_well_formed` containing that check
stops compiling, in the configuration that is the gate.

This is the same finding `collection_is_declared` recorded in Phase 3, and it gets the same
answer: a `well_formed()` that does not compile is worse than one that checks less. The null
check is `validators_are_present()`, a runtime function, asserted by the seam test and callable
at boot — and the submission path refuses a null validator BY NAME rather than dereferencing
it, so the gap costs a rejected answer rather than a jump through a pointer nobody verified.

### The explain check found two indexes nothing reads, and was right to

`EveryDeclaredIndexIsCitedByAtLeastOneQuery` failed on `submissions_one_per_user` and
`submissions_media_unique`. Both are real and both are uncitable: they are UNIQUE constraints
that exist to refuse a write, and the whole point of making those rules the server's job is
that there is no read to cite them — a count-then-insert loses the race to a double-click.

The check now exempts unique indexes alongside TTL ones, with the reason stated: both do a job
no read can cite. A unique index that IS read from is still covered, because the query that
reads it covers it.

### The subsystem is split across two link targets

`fid`, `answer`, `field_type`, `validators` and `pii` are in `anvil::foundation`; the
repository, the two services and the export are in `anvil::app`. The split is not tidiness: a
locale module links nothing but OpenSSL and ICU, and shipping a field-type validator from one
is only possible if the seam's headers do not drag bsoncxx in. That is also why `db::TimeMs`
lives on the `definition.h` side of the line and `FieldSpec` does not.

---

## Phase 6 — notifications and jobs

| | Task | Notes |
|---|---|---|
| [x] | `notifications/inbox` | The system of record. The merge is the job: one cursor over two collections with two read-state models, rendered in the reader's CURRENT locale |
| [x] | `notifications/publish` | Fan-out, coalescing, and the outbox. The audience is DERIVED from the subscriptions, because the sweeper that re-runs a dispatch holds only the stored row |
| [x] | `notifications/topics` | **Seam.** Mechanism ships; the topic table is the app's |
| [x] | `notifications/templates` | **Seam.** The `{t}`/`{n}` engine ships; the messages are the app's |
| [x] | `notifications/sse` | Bounded per-connection ring, a full ring DROPS the connection, ceiling derived from `RLIMIT_NOFILE` |
| [x] | `notifications/{outbound,webpush,webhook,email}` | VAPID/ECDSA, signed webhooks with sealed secrets, implicit-TLS SMTP |
| [x] | Doc 11 | notifications |

**Phase 6 gate: MET.** 892/892 green under ASan + UBSan against a live `rs0` replica set, and
13/13 under TSan. Baseline at the start of the phase was 750.

Suites added: `sse`, `webhook`, `webpush`, `email` (unit); `sse_concurrency` (concurrency);
`publish_db`, `inbox_db`, `outbound_db` (database).

### Two defects found in already-landed code, fixed before building on them

Both were in the notification storage committed in Phase 6's first half, and both were only
reachable through the read path this phase added — which is the argument for the phase gate
rather than an exception to it.

**The broadcast read lost notifications.** The read-merge was one `$or` with one limit sorted
`{kind, subject, _id}`. The server merges the branches correctly, but in TOPIC order: the page
fills entirely from the lowest topic code, and `next_cursor` is then an id from inside that
one branch. Applied as a `$lt` to every branch on the next page it excludes every newer row of
every other topic permanently — a reader subscribed to a busy announcement topic and a quiet
one stops seeing the quiet one at all, and nothing reports it.

Sorting `{_id: -1}` fixes the order and trades the loss for a different one: it is only
correct if the planner chooses a `SORT_MERGE`, and `explain` shows it walking the primary key
with the topic predicate as a filter against a small collection. Hinting the index produces a
blocking `SORT` instead. The limit therefore moved to the branches — one
`$match`/`$sort`/`$limit` per topic, merged with `$unionWith` — and `subject` is now stored as
a nil UUID rather than null, because `{subject: null}` matches missing-or-null and so is not a
point equality the index bounds can be elided for. See docs/11 §5.

**A millisecond race in `ANewSubscriberDoesNotSeeTheHistory`.** The test set the marker to
`v7_boundary(now + 1ms)` and then required the next generated id to land in a later
millisecond; a local round trip routinely finishes inside one. It reproduced twice in eight
runs, which is the shape that costs the most to diagnose — it presents as an unrelated suite
being flaky rather than as anything to do with subscriptions.

---

## Phase 7 — close the loop

| | Task | Done when |
|---|---|---|
| [x] | Complete `tests/testapp/` | Every seam supplied and compiled by building the tests. **"Seven" was stale** — the count was seven when this line was written and is now twelve; the fix is the list in README, which is checked against `tests/testapp/` rather than counted from memory |
| [x] | `docs/02-getting-started.md` | Written **against `tests/testapp/`**, so the example is compiled and cannot rot. Every snippet is quoted from a file the suite builds, and every relative link is checked |
| [x] | Full ASan/UBSan run | `ctest --preset asan` green end to end |
| [x] | TSan run | `ctest --preset tsan` green; `tests/tsan.supp` carries a justification per suppression and a deletion condition |
| [x] | `dist` run | `ctest --preset dist` green — the suite against ship flags |
| [x] | Doc reconciliation | Every doc re-read against the code as shipped; anything that drifted is corrected, not deleted |

**Phase 7 gate: MET.** 895/895 under ASan + UBSan, 895/895 under `dist`, and 13/13 under TSan,
all against a live `rs0` replica set with nothing skipped.

### What the reconciliation found

Recorded rather than quietly fixed, because in each case the DOC was right and the code had
drifted away from it — which is the direction that is hardest to notice.

| Drift | Correction |
|---|---|
| `http/rate_limit.h` shipped **thirteen rate-limit constants**, five of them for a `booking` subsystem anvil has never contained. docs/01-seams.md §7 has always said "you ship the constants", and CLAUDE.md §1 names a rate-limit constant explicitly as something anvil must not hold | Constants removed; `rate_limit_table_is_well_formed` added so the seam fails at compile time like every other one; `tests/testapp/rate_limits.h` added as the worked example |
| The rate-limit seam had **no conformance check**, alone among the twelve. An empty bucket name, a zero window, a zero budget and two rules sharing a bucket were all runtime behaviour | `static_assert`able check, with the four cases tested from outside anvil in `seams_phase3_test.cc` |
| README claimed an application supplies **six tables**. It supplies twelve seams across fourteen tables | Table rebuilt against `tests/testapp/`'s actual contents |
| `client_address.h` cited `kSignupPerIp` by name, which no longer exists | Citation replaced with the reasoning it pointed at, per the standing rule below |
| docs/00-architecture.md's target table did not show that `forms/` and `notifications/` **straddle the foundation boundary deliberately** | Both splits documented with the reason: a locale module that links only OpenSSL and ICU has to be able to ship a field-type validator |
| **Twenty-four citations pointed at doc sections that do not exist**, carried over from yardclub's numbering. A dangling citation sends a reader looking for reasoning they cannot find | Renumbered where the section moved; where anvil had never written the reasoning down, it is written down now — docs/04 §4 (the token path's 15 µs / one-allocation budget), docs/10 §1 (Redis is not the system of record) and docs/10 §6 (backpressure rather than a MAXLEN trim) |
| `drop_scratch_databases()` was never registered, despite a comment saying it was. Every run of `anvil_db_tests` leaked two databases with fourteen collections each | Registered as a global teardown in one translation unit. The development cluster had accumulated several hundred and its mongod core-dumped mid-phase at a 7.4 GB peak |

### What the `dist` preset caught that `asan` structurally cannot

One test, and it is the argument for the preset existing. `ThePageIsNewestFirstAcrossEveryBranch`
published four rows and asserted the last came back first — which requires them to land in
different milliseconds, and nothing made them. Under ASan a round trip comfortably exceeds a
millisecond, so it passed every time; under `-O3` three inserts finish inside one, and a
UUIDv7 orders by its 74 random bits within a millisecond.

The same hazard, in a different test, is what `f894598` fixed at the start of Phase 6. It is
worth naming twice because of how it presents: an occasional unreproducible failure in a suite
that has nothing to do with time.

---

## First — what already ships wrong

Not a phase, and it had a gate anyway — the suite under both sanitizers, `dist`, and the load
gate, on every commit. It goes first for a reason that has nothing to do with size. **Phases
8-10 add capability; every row here fixes something a consuming application is already living
with.** A gap is a thing you do not have yet. A defect is a thing you have that is wrong, and
shipping more on top of it is how it becomes permanent.

Ordered by what it costs to be wrong: the first three are wrong today — a public constant that
silently does not work, a sink that discards the rows its own policy protects, and a comment
instructing a developer to set a build flag that does not exist. The next two are the residue
of the lift, names that came across with the code in phases 1-6; a library carrying an
application's vocabulary teaches every later reader that the application is part of the
contract, which is what CLAUDE.md §1 is about, applied so far only to tables. The last one
stops it happening again.

There is a second reason to start here: **none of it depends on a design that has not been
built yet.** Everything in phases 8-10 rests on documents written in advance, and the final
review of those documents found two defects by checking claims against source rather than by
re-reading prose. These rows rest on nothing. They are also the smallest change that exercises
the whole gate — warnings-as-errors, the suite, the `tools/` scripts, the commit discipline in
CLAUDE.md §9 — and that loop is better proven on a one-word fix than discovered midway through
a twenty-row phase. The last row added a third script, so the gate that proved itself here is
wider than the one it started with.

| | Task | Notes |
|---|---|---|
| [x] | `kUserContextAttribute` vs `kUserContextKey` | **A live trap, not a naming complaint, and it should be fixed first.** `core/user_context.h` declares `kUserContextKey = "anvil.ctx"` under the comment "One attribute, one key"; `accesscontrol/access_filter.h` declares `kUserContextAttribute = "yc.ctx"`; only the second is used. Code written against the public header a reader would naturally reach for gets **a null context at run time with no error**. One key, one name, anvil's prefix — and delete the other |
| [x] | `audit/service` — re-admit a refused batch | **The other bug, and it is measured rather than reasoned.** `post()` drains the buffer, tallies the classes it carries, and on a refused `try_post` counts the loss and discards the batch — so the classify-and-shed policy governs admission to the buffer and stops at its boundary, and a refusal drops `Change` rows wholesale. A load run refused 8,485 batches against a live, busy `mongod`, which is the case the comment beside that code explicitly excludes. Return the changes to the buffer and charge the traffic; the buffer is already bounded, so re-admission cannot grow without limit. **Phase 8b copies this design and must not copy it broken** |
| [x] | `src/images/probe.cc` names a flag that does not exist | **Not foreign vocabulary — a wrong instruction.** The no-vips branch is commented `-DYARDCLUB_WITH_VIPS=OFF`; this library's option is `ANVIL_WITH_VIPS`. A developer who reads it to find out how to rebuild with images sets a variable nothing reads, and nothing tells them so. One word, and it is the cheapest row here |
| [x] | `kPreviewCookieName` | `"hv-preview"` is an application's feature abbreviated into a cookie this library sets. Rename to something the mechanism describes. It is an API change **and** a stored-value change — a browser holding the old cookie stops being recognised — so it lands with the migration note CLAUDE.md §9.3 requires, and the sooner it is done the fewer there are to strand |
| [x] | Comments naming an application's features | Thirteen public headers and two sources, wider than a first look suggests: `core/uuid.h`, `input/html.h`, `input/schema.h`, `input/fields.h`, `http/json_writer.h`, `http/client_address.h`, `http/rate_limit.h`, `db/codec.h`, `db/versioned.h`, `accesscontrol/cookies.h`, `timer/queue.h`, `notifications/record.h`, and `src/input/html.cc` — citing hotviews, bookings, house notes and a staff desk. **This is the same defect phase 7 fixed twenty-four times in the docs**, and the standing rule below already prescribes the treatment: replace the citation with the reasoning it points at. Keep the reasoning; a reader cannot look up a feature that is not in this repository, and deleting the sentence loses why the code is shaped that way |
| [x] | A check that keeps it clean | The rule is worth nothing as a memory. One `tools/` grep, a CTest `lint` entry, failing on an application's vocabulary in `include/` and `src/` — with the list of banned words being the point, since the next lift will bring different ones |

**Section gate: MET.** 968/968 green under ASan + UBSan, 968/968 under `dist`, 18/18 under
TSan, 5/5 in the load gate and 955/955 in the `ANVIL_WITH_VIPS=OFF` configuration, all against
a live `rs0` replica set with nothing skipped. Seven commits, one row each, plus the phase-8
row below and three more defects the work found.

### What closing it found

| Found | Correction |
|---|---|
| Nothing in the suite had ever constructed a **Drogon request**, which is exactly why two constants could name one attribute and disagree. Every access-control case drives `evaluate()`, which sits below the attribute map | One case in `access_control_test.cc` builds a request, attaches a context under the public `kUserContextKey`, and asserts `user_context()` finds it. The file's header says why a Drogon request appears in a suite that says it needs no HTTP stack |
| Nothing in the suite had ever constructed an **`AuditService`** either. That is how an ordering rule in an application's `main()` came to stand in for a lifetime: there was nowhere the rule could be broken | `tests/audit_service_db_test.cc`, beside `auth_service_db_test.cc`. It blocks `audit_pool`'s one worker, queues a batch behind it, destroys the service on the heap, and only then lets the pool run — with the old capture that is a heap-use-after-free under ASan on every run, not when the timing is unkind |
| **The flush timer was the third owner of a `this`** neither sink had noticed. `start()` installs a repeating timer capturing `this` and stores its id; `stop()` set a flag and left the timer installed, so a destroyed sink leaves one firing on freed memory every second for the life of the loop. Same defect as the pool task, different owner, and safe today for the same reason: both sinks are `main()`-level objects | `stop()` invalidates it, before the final flush so a tick cannot drain the buffer it is about to write. Both headers now state the contract that makes that reliable: call `stop()` on the loop thread or before the loop runs, because Trantor's `invalidateTimer` is synchronous only there. **Untested** — every timer here needs a running loop, which no binary has until phase 10 |
| **The load gate drove the wrong sink.** The 8,485 refusals it exists for were AUDIT batches, and the gate asserted "no flush is refused while the database is answering" only against the analytics sink — which shipped with the re-admission correction already in it | A fifth case drives `AuditService` under the same eight producers, with Change rows, which do not coalesce. It asserts no refusal, no loss in either class, and every row on disk — the last being what separates "nothing was refused" from "nothing arrived" |
| `Tally` and `tally_of` in `AuditService` classified a batch a second time, in a second place, so that a refusal could be charged | Deleted. `AuditBuffer::readmit` charges what it drops, because the buffer is where the classification already lives. Two places that classify are two places that can disagree |

### Filed rather than fixed — and now closed

All three landed together, and the reasons they were filed are worth keeping beside what
happened to them: two were filed because the phase that would have tested them had not been
built yet, and the third because it was a behaviour change rather than a defect fix. The
second of those turned out to be the wrong reason — see *What closing the three filed rows
found* below.

| | Task | Notes |
|---|---|---|
| [x] | Citations of a test-plan register anvil does not have | The same shape as the `F` citations the sweep replaced, in a different letter: `T11`, `T13`, `T15 case 7`, `T24 case 11`, `T74 case 4`, `T78`, `T59` — in `auth/password.h`, `auth/token.h`, `auth/epoch_cache.h`, `http/client_address.h`, `fs/upload.h`, `accesscontrol/epoch_resolver.h`, `src/http/client_address.cc`, `src/fs/upload.cc`, `src/images/variants.cc`, `src/accesscontrol/access_filter.cc` and nine test files. `docs/16-test-plan.md` numbers nothing, so none of them resolves. **Deliberately not swept with the `F` ones**: the defects section names the files it covers and these were not among them, and a reader should be able to see that the two decisions were separate. `tools/check-vocabulary.sh` gains the pattern in the same commit that clears them |
| [x] | Re-admission repeats the whole flush per row while the pool is saturated | `write_async` posts a flush the moment the buffer crosses `kBatchRows`, and a refused flush now puts the rows back — so while `audit_pool`'s queue stays full, every subsequent row drains 256 rows, copies them into a task, is refused, and puts them back. Roughly 50 KB of memory traffic and three allocations per row, in the state where the process is already behind. `EventSink` has had the same shape since 8b. `BoundedThreadPool::saturated()` exists for exactly this — its comment says it "lets a caller shed BEFORE doing expensive preparatory work" — so the flush can be skipped while the queue is known full, leaving the rows in a buffer that is already bounded and already sheds. **Filed rather than done because it is a behaviour change and not a defect fix:** it makes a refusal rarer rather than cheaper, and `ARefusedFlushReturnsItsChangesToTheBuffer` would then have no deterministic way to provoke one. The coalescer is why this is a row and not a phase — traffic folds, so reaching it takes thousands of distinct CHANGE rows per second against a database that has stopped keeping up |
| [x] | A test for the flush timers | Both sinks install a repeating timer that holds `this` and both now take it out in `stop()`, and nothing proves it: every timer here needs a RUNNING event loop and no test binary has one. It belongs in `anvil_listener_tests`, which arrives with phase 10 — the assertion is that a sink destroyed after `stop()` leaves no timer behind, which is a use-after-free under ASan if it does |

### What closing the three filed rows found

| Found | Correction |
|---|---|
| **The citation row undercounted itself by an order of magnitude.** It named "nine test files"; there were eleven files in `include/` and `src/` and **twenty-two** under `tests/`, carrying about a hundred citations between them. Same shape as phase 7's "Seven was stale" and phase 11's "the twelve seams" | Swept in full, and the COUNT is no longer the thing keeping it clean: `tools/check-vocabulary.sh` now carries the `T` pattern, so the next one is a red CTest entry rather than a number somebody has to re-derive |
| **Three different files claimed `T21`**, for two unrelated subjects — `filesystem_test.cc` for path safety, `descriptor_test.cc` for the client descriptor, `validation_fuzz_test.cc` for the storage budgets. The numbering was not merely unresolvable; it had already contradicted itself | Recorded in the check's own comment, because it is the argument for the check's scope: a register citation resolves nowhere in ANY directory, which is why the two register patterns scan `tests/` and the feature-name pattern still does not |
| **The `F` check had been passing over one the whole time.** Its pattern was bounded at `\bF[0-9]{2,4}\b`, and `F3` in `http/rate_limit.h` has one digit — in a sentence the same lift had truncated mid-clause, so the paragraph about why buckets are not keyed by route had been reading as damage since phase 2 | Bound widened to `{1,4}` and the sentence completed. A lower bound chosen from the examples in front of you is a lower bound that misses the next one; the one exemption it now needs, `Fid::parse("F1")` in `forms_test.cc`, is a grammar case whose input looks like a finding number and says so |
| **`tests/` still carried an application's feature names, and one of them made a comment wrong about the code beside it.** `route_declaration_test.cc` said "`/hotview/{id}` is declared for GET only" above a case that registers `/content/{id}`, and cited `tests/deploy_smoke_test.sh`, which this repository does not contain | Both repaired where they made the prose false. The rest — `housenote` as a path literal in four suites, `booking` in a `db_fixture.h` anecdote — is a row below rather than a hunk here: it is a mechanical rename and CLAUDE.md §9.1 says those travel alone |
| **The flush storm is 257 refusals where one is correct**, measured rather than reasoned: with the latch removed, `ASaturatedPoolCostsOneRefusalAndNotOnePerRow` reports exactly `kBatchRows + 1`. Each one is a drain, a 16 KB copy, three allocations, a refusal, a re-admission AND a `LOG_WARN` — the last of which is itself the thing docs/00 §9 forbids | One refusal arms a latch; the rest are skipped while `saturated()` holds. **The reason this was filed was wrong**: the row said re-admission "makes a refusal rarer rather than cheaper, and `ARefusedFlushReturnsItsChangesToTheBuffer` would then have no deterministic way to provoke one" — which is true of a `saturated()` check placed BEFORE the first attempt, and not of a latch armed by a refusal. Paying one refusal to learn the queue is full is what keeps that case exactly as it was written, and `try_post` is the authority anyway |
| The audit suite's `drain_pool()` could not be called twice. It waits on a sentinel flag the gate sets, and a flag left set by the first call made the second return before the task it was waiting for had run | Rearmed at the top of the call, and the whole blocker moved to `tests/pool_gate.h` so the analytics sink's identical case does not get a second copy to be subtly wrong in |
| **The flush-timer row was blocked on phase 10 and was still blocked after it.** `anvil_listener_tests` existed from phase 10 but linked `anvil::platform`, and both sinks are in `anvil::app` | It links `anvil::app` now — the same link `anvil_platform_tests` already makes, for the same stated reason. docs/16's binary table said `anvil::platform` and now says what is there |
| A timer case whose only assertion is "ASan did not fire" passes on a `start()` that installed nothing | Two witnesses, both load-bearing: one row is buffered and watched out of the buffer, which proves the sink's own timer was live; and a PROBE timer on the same loop at the same interval counts its own ticks, which proves the loop was firing timers during the window the sink's would have fired in. With `invalidateTimer` removed from `stop()`, the case is a heap-use-after-free at `src/audit/service.cc:80` on every run |

**Gate for the five rows that landed together** — the three above, phase 11's limiter metric
and phase 12a's `ANY` row: **1,136/1,136 green under ASan + UBSan** against a live `rs0` replica
set and Redis, **1,136/1,136 under `dist`**, 18/18 under TSan, 5/5 in the load gate and
1,123/1,123 in the `ANVIL_WITH_VIPS=OFF, ANVIL_WITH_EGY=OFF` configuration, with nothing
skipped. Baseline was 1,125. Seven commits.

The eleven new cases are three for `descriptions_match` (all `static_assert`s over deliberately
wrong tables, which is the form phase 12's gate asks for), three for the limiter's two sources,
two for the audit sink's storm and its recovery, one for the analytics sink's, and two for the
flush timers. Two of them were run against the defect they describe before being run against
the fix — the storm reports 257 refusals where one is correct, and the timer case is a
heap-use-after-free at `src/audit/service.cc:80` — because a case that has only ever been green
is a case nobody has proved is a case.

### Filed by the sweep, in turn

| | Task | Notes |
|---|---|---|
| [x] | An application's feature names, still in `tests/` | `housenote` as a path literal in `filesystem_test.cc`, `html_sanitize_test.cc` and `access_control_test.cc`; `booking` and `shop` in a `db_fixture.h` anecdote; `hotview` in an `html_sanitize_test.cc` comment. `tools/check-vocabulary.sh` has never scanned `tests/` for these, and its stated reason is sound for `tests/testapp/` — the reference application has to have a vocabulary of its own — but it is not sound for a suite naming a product anvil has never contained. **Filed rather than swept with the register citations because it is a mechanical rename and CLAUDE.md §9.1 says those travel alone**, and because unlike a dangling `T74` these are test DATA rather than prose: renaming them changes strings a case asserts on, which is a different kind of diff to review. The two places where the vocabulary had made a comment FALSE about the code beside it were repaired with the citations, because that is the same defect. What this row costs if it is left: the next reader of `access_control_test.cc` learns that `/housenote/edit` is a route shape anvil knows about **Closed.** Ten sites across four files: `content` in place of the foreign name in `filesystem_test.cc` — which already spelled it that way twelve lines above — and in `html_sanitize_test.cc`'s media origin, `/content/edit` for the route shape in `access_control_test.cc`, and `db_fixture.h`'s anecdote replaced by what the number actually rests on. The embedded-NUL traversal candidate moved with the name and its byte count moved with it, because the count IS that case. `tools/check-vocabulary.sh` now scans `tests/` for feature names and excludes `tests/testapp/` by path. Two things beyond the row. The exclusion is a per-check parameter rather than a line in the shared helper, because folding it into the body would have quietly exempted `testapp/` from the REGISTER checks too — which is the opposite of what those say and would have stayed invisible until somebody wrote an `F12` in there. And the three scopes are each proved by planting a violation and watching the script fail, then planting the other two and watching it pass and fail respectively: a checker nobody has seen refuse something is a checker that passes on any codebase in the world |

---

## Phase 8 — count what happens

Three subsystems follow phase 7, in this order and for these reasons.

**Analytics first** because [`00-architecture.md`](00-architecture.md) §9 has named its
counters since phase 0 and shipped none of them — the oldest documented gap in the library.
Landing it first means the two phases after it ship instrumented rather than retrofitted.
**Data migrations second** because it extends an existing seam rather than opening a
subsystem, and because the collections this phase introduces — high volume, TTL'd,
rollup-backed — are the first real customer of a resumable batched step. That is the same
deferral `db/query_catalogue` took from phase 2 to phase 3, now that there are queries to
explain. **Rendering last** because it is the largest new surface and the only one whose
defects are an XSS class, and because it consumes both of the others: its cache wants the
counters, and its boot-time load failure wants a counter rather than a log line.

### What a load run against the consuming application already measured

Two of the numbers phase 8 exists to produce were obtained the expensive way — from a
`release-check` run of an application built on anvil, against a live cluster on a shared
sixteen-core box. **Both measure anvil's own mechanisms**, which is what makes them usable
here: a figure about a consumer's code would be an anecdote, and these are not.

| Measured | What it means for anvil |
|---|---|
| **8,485 audit batches refused**, 8,485 drop reports | `audit_pool` saturated against a **live** `mongod` at 8–11 cores. The comment in `src/audit/service.cc` reasons that a refusal means the database has been unreachable for minutes; it is wrong, and the gap it hides is the second row of the section above |
| **393,116 transactions aborted** | `with_transaction` retries a `TransientTransactionError` with no backoff of anvil's own, so a contended document produces a retry storm. docs/00 §9 names this counter and says to alert when it stops tracking request volume — which requires the counter to exist first |
| 1,088 of 1,092 tests green; the four failures were in consumer code | Neither number above is visible to a passing suite. That is the argument for the load gate row, not a reason to distrust the suite |

### 8a. The metrics core

| | Task | Notes |
|---|---|---|
| [x] | `analytics/metric_spec` | **Seam.** `MetricSpec`, `LabelSpec`, `metric_table_is_well_formed`. A label's VALUE set is `constexpr`, so the series count is a compile-time number and a cardinality explosion is a build failure rather than an OOM under load |
| [x] | `analytics/internal_metrics` | The counters docs/00 §9 has named since phase 0. A `constexpr` table anvil POPULATES, and the one it is entitled to: every name describes a mechanism in this repository. The `anvil_` prefix is enforced in both directions |
| [x] | `analytics/counters` | Sharded, cache-line-padded cells; an increment is one `fetch_add(relaxed)` on a line no other thread writes. ONE heap allocation at construction, sized from the two tables — the `SectionService::cache_` shape. `observe()` takes label INDICES and has no `string_view` overload |
| [x] | `analytics/snapshot` | Aggregate on read, into one caller-owned reserved buffer. Summing shards on read is what lets a pull scrape need no periodic task at all |
| [x] | `analytics/openmetrics` | Append into one reserved string, as `json_writer` and `csv_writer` do. Two scrapes of an unchanged registry are BYTE-IDENTICAL, including series order — that is what lets an operator diff them. `ScrapePolicy` ships; the route is the app's, because anvil registers none |
| [x] | Instrument the named counters | authz cache hit rate, `hash_pool` / `cpu_pool` queue depth, `mongocxx::pool` wait, TTL collection sizes, orphan-sweep results, stealth 404s per source network, audit rows dropped, transactions aborted. **The list in docs/00 §9 is the acceptance bar, not a suggestion.** Two of them have measured targets rather than a guess — see the table above — and `anvil_transactions_aborted_total` wants a label for the retry outcome, because 393,116 aborts that all eventually committed and 393,116 that did not are different incidents. **Two of the eight landed under different names than this list gives** — see the findings table below |
| [x] | `db/repository` — bound the retry storm | Falls out of the counter above. `with_transaction` retries a labelled transient immediately and for up to 120 s; anvil adds no backoff, so a contended document turns N workers into a tight loop against the one document they are all waiting for. Establish whether a bounded jittered backoff belongs in `guarded_in_transaction` or is genuinely the caller's, and **write the answer down either way** — the current silence reads as a decision nobody made |
| [x] | `tests/testapp/metrics.h` | The worked example, `static_assert`ed from outside anvil |

### 8b. Event ingest

| | Task | Notes |
|---|---|---|
| [x] | `core/thread_pools` | `analytics_pool`, a fifth pool. **`audit_pool` is not reused**: it exists so a saturated request path cannot starve the forensic record, and a second writer re-creates exactly what it was made to prevent. `PoolSizes` gains two members — an API and aggregate-initialisation break, called out per CLAUDE.md §9.3 |
| [x] | `analytics/event_spec` | **Seam.** `EventCode` is a STORED int32 and append-only, joining the locale index, the perm bit, the namespace index, the field-type code, the audit action and the template id. So is a dimension's value index |
| [x] | `analytics/event`, `analytics/buffer` | classify → coalesce → bound: `audit/buffer.h`'s policy a second time, as a SECOND TYPE rather than a shared one, because an analytics flood must never be able to evict an audit row |
| [x] | `analytics/sessions` | A visitor is a peppered, day-rotating HMAC over the packed address. The address never reaches a row; the pepper is what makes a 32-bit input space unreversible from a dump. The `(visitor, day)` pair IS the sessionisation — no read-then-write. Landed as the document's compound `_id` rather than as a unique secondary index: a compound `_id` is already unique, so the collection carries no secondary index at all |
| [x] | `analytics/ingest` | Consent is refused at the door, not filtered later. Sampling is deterministic per SESSION above the high-water mark, so a funnel is whole or absent — never half, which is worse than missing. The sampled-out count is itself a counter |
| [x] | `analytics/repository` | Three collections, all named by the app: `analytics_events`, `analytics_sessions`, `analytics_rollups`. The first two go in the second database, which is what that database was declared for |
| [x] | `analytics/rollup` | A recurring job. The rollup's `_id` IS its identity tuple, so a re-run is an upsert with `$set`. **A rollup that `$inc`s is not re-runnable**, and every queue here is at-least-once |
| [x] | `analytics/query`, `analytics/erasure` | Reads hit rollups only, bounded and cursor-paginated. Erasure is one `delete_many` against a partial index anonymous rows never enter |
| [x] | `tests/testapp/{events,indexes,queries}.h` | The table, its indexes and its six query shapes — in the same commit, per CLAUDE.md §7. **Six indexes and not seven**: the seventh would have been over the rollup's dimension slots, which are an array, and the explain check caught that the planner will not use it (findings table below) |
| [x] | `tools/check-source-bans.sh`, `tools/check-db-discipline.sh` | `std::to_string` banned under `src/analytics/`; `src/analytics` added to `repo_files()` |
| [x] | A load gate | **Scoped deliberately: not a benchmark suite.** One CTest entry, excluded from the default presets, that drives the pools past saturation and asserts the properties only load can falsify — no batch is refused while the database is answering, a full queue sheds rather than grows, peak RSS stays inside the registry's stated ceiling, and the two counters above are non-zero only where they should be. Both findings in the table above were invisible to 1,088 green tests, and neither is a timing assertion, so neither is the flaky kind docs/16 rules out |
| [x] | Doc 17; docs/00 §1, §2, §3, §9; docs/01 §11–12; docs/14 §3; README | Plus docs/09 §5.1 and docs/16, neither of which was on this list and both of which the work made wrong |

**Phase 8 gate: MET.** 958/958 green under ASan + UBSan, 958/958 under `dist`, 18/18 under
TSan and 4/4 in the load gate, all against a live `rs0` replica set with nothing skipped.

### What the phase found that the design documents did not

Recorded rather than quietly fixed, because in each case the design was written in advance and
the code is what proved it wrong. That is the opposite direction from phase 7's reconciliation
and it is worth keeping the two distinguishable.

| Found | Correction |
|---|---|
| `anvil_mongo_pool_wait_seconds` cannot exist. Bucket boundaries are integers in the declared unit (doc 17 §5 bans a floating-point one), and a pool wait in whole seconds has exactly one useful boundary | Renamed `anvil_mongo_pool_wait_microseconds`, and `MetricUnit` ships no `Seconds` at all so the next duration metric cannot repeat it. docs/00 §9 and docs/17 §3 corrected |
| `anvil_stealth_denials_total` "labelled by coarsened source network" is the cardinality explosion doc 17 §6 exists to refuse — the value space of that label is the internet | Labelled by the true reason and by whether the client was stealthed. The network stays on the audit ROW, where a retention window bounds it instead of resident memory |
| **`EventSink::post` captured `this`** in the task it handed to `analytics_pool`. A task the pool has accepted runs after `post()` returns and nothing keeps the sink alive until then | The task captures copies of the two repositories, the retention and the batch, and touches no member. **Found by the load gate on its first run**, as a stack-use-after-return, which is the entire argument for that row existing |
| The three analytics services took ONE database name, and two of the three collections are declared in a different database from the third | Every service takes `db::DatabaseNames` and resolves per collection. Found by `rollup_db_test`, which read an empty collection and counted zero |
| A rollup index over the dimension slots is **multikey**, and a multikey index cannot provide a sort on a key that follows it — which is what the bucket ordering needs | The index is not shipped; the dimensions are a residual over a range that is already bounded. Found by `query_catalogue_db_test`, which asserts every declared index is ridden by a declared query |
| `anvil_ttl_collection_rows` needs a label whose values are the APPLICATION's collections, which anvil cannot name | Derived at compile time from `config::kCollections`, filtered to the collections with a lifetime. The value space stays `constexpr`, so the series count stays a compile-time number |
| docs/16 said the Structure table does not change. It gained an eighth binary | `anvil_load_tests`, labelled `load` and excluded from every default preset. It sizes its own pools: `app_fixture.h`'s are deliberately tiny so that shedding is observable, which is the opposite of what "no batch is refused while the database is answering" needs to mean anything |

### What the `dist` preset caught again

One test, and the same shape as phase 7's. `ASnapshotTakenDuringWritesIsBetweenFirstAndLast`
bumped its witness counter AFTER the registry's, which makes the witness a lower bound with a
window; a snapshot taken inside that window reads a value the witness has not reached yet.
Under ASan the window never opened. Under `-O3` it does, on the first run.

It is worth naming a third time because of how it presents: a failure in a suite that has
nothing to do with the thing that is actually wrong.

### Opened by this phase, closed with the defects section

| | Task | Notes |
|---|---|---|
| [x] | `AuditService::post` captures `this` too | The same shape the load gate found in `EventSink`, in the sink phase 8b was told not to copy broken. It was safe only because an `AuditService` is a `main()`-level object that outlives `Pools::shutdown()` — an ORDERING rule rather than a structural one. Landed with the audit re-admission row above, and it took a test file with it: nothing in the suite had ever built an `AuditService`, which is why an ordering rule could stand in for a lifetime |

Two more of the same family came out with it, and both are recorded in that section: the flush
**timer** holds a `this` that `stop()` never took out, in BOTH sinks; and the load gate drove
only the analytics sink, so the assertion the 8,485 audit refusals motivated was being proved
against the half that was never wrong.

---

## Phase 9 — move the data

| | Task | Notes |
|---|---|---|
| [x] | `db/migration_step` | **Seam.** `MigrationStep`, `StepContext`, `Cursor`. A `std::span` in `MigrationDeps`, NOT a third config header. **"`<mongocxx/client-fwd.hpp>` and nothing more" turned out to be impossible** — see the findings table — but the part that was load-bearing holds: no connection type in a header the low layer can see. `StepContext` takes the FIELDS a document should carry and the runner wraps them, so `$inc` is not something a step is able to say |
| [x] | `db/migration_ledger` | `anvil_migration_ledger`, beside `anvil_schema_meta`: the mechanism's own storage. `_id` is the step NAME, which is why a renamed step re-runs from scratch on every cluster that already applied it. The schema version is NOT shared, and a test asserts `applied_schema_version()` answers after a data run exactly what it answered before |
| [x] | The step lock | One `find_one_and_update` claim against an expiring lease, in MongoDB and not Redis. "Free" is a lease in the past rather than a null owner, which is what makes the filter upsertable — see the findings table. A runner that loses reports *held* and exits 2 rather than waiting |
| [x] | `db/data_migrations` | Ordered, forward-only, resumable by an `_id` range cursor, never `skip(n)`. The runner owns the cursor and the batching; the step owns only the transform. A failed step blocks every step after it, and the blocked step is REPORTED as blocked rather than merely absent |
| [x] | Dry run | `StepContext` accumulates and the RUNNER decides. It writes nothing at all — not the documents, not the schema, and not the ledger: a dry run that took the lock could block the real one, and one that recorded a cursor would make the real run skip what it had only pretended to do |
| [x] | `db/migrate_cli` | Three functions rather than one, so the flags are testable without a cluster and the body is testable inside a binary whose `MongoPool` is already initialised. Stable exit codes, `static_assert`ed to BE `RunOutcome`'s values rather than transcribed. **Exit 3 also covers an invocation that was not understood** — see the findings table |
| [x] | `db/migrations` — `IndexSpec` grows | `collation` and `hidden`, both appended and both with a default member initialiser (the findings table says why). `hidden` is reconciled with `collMod` against the live index, because `createIndexes` refuses a name that exists with a different specification — which is the right answer for every other field and the wrong one for this one |
| [x] | `db/collection_options` | **Seam.** Clustered, capped, timeseries and `$jsonSchema` validators. An existing collection whose options differ is an ERROR. Applied in two phases, and the order is the deployment order: creation before the indexes, validators after the data steps |
| [x] | `verify_retired`, `report_undeclared` | Both REPORT rather than act; they are `--status` output. Each test asserts the EMPTY result first, because a check that reports nothing passes on any cluster in the world |
| [x] | `tests/testapp/migrations.h` | Two steps: one over a UUIDv7 collection and one over `sections`' compound `_id`. Plus three collection-options entries, including `analytics_events` clustered — which is what doc 18 §11 argued for and nothing had yet declared |
| [x] | `tools/check-db-discipline.sh` | `src/db` joins `repo_files()`; a step may not `$inc`; `create_collection` and `collMod` are confined to the migration alongside `create_index` |
| [x] | Doc 18; docs/09 §7, §7.1; docs/00 §7; docs/01 §13; README | |

**Phase 9 gate: MET.** Every suite green under ASan + UBSan against a live replica set, with
nothing skipped.

### What the phase found that the design documents did not

| Found | Correction |
|---|---|
| **`migration_step.h` cannot include "`<mongocxx/client-fwd.hpp>` and nothing more".** `StepFn`'s own signature names `bsoncxx::document::view`, and `std::span` requires a complete element type | The bsoncxx value headers are included by necessity. The sentence was written before the signature was; what it was protecting — no connection type in a header `anvil::foundation` can see — is intact, and docs/01 §13 and docs/18 §3 now say so |
| **A claim filter spelled as `$or` over three ways of being free cannot upsert.** MongoDB derives an upserted document from a query's EQUALITY conditions, and `$or` contributes none | "Free" is a lease in the past rather than a null owner, so the filter is one equality and one range. A held row then fails the filter and the upsert collides on `_id` — and that duplicate key IS the contention signal, from one atomic operation |
| **The `$inc` ban's first lint fired on the sentence explaining the ban.** The banned thing is a string literal, so the comment-stripping every other check does is exactly backwards here | Comments are stripped and string literals are kept, which is the opposite of `check-source-bans.sh` and correct for this one rule. Keying the check on the seam's INCLUDE also missed every step body, because a body is in a `.cc` that includes its own header; it keys on the type names now |
| **The appended `IndexSpec` fields compiled and emitted 78 warnings per translation unit.** Aggregate-initialising an existing catalogue is exactly what `-Wmissing-field-initializers` is for | A default member initialiser on both, which says what the appending was meant to say: absent means no collation and not hidden |
| **`RunOptions::label` bound to a temporary `std::string`** in the CLI — the dangling view across a call boundary CLAUDE.md §2.2 names as the most likely crash in code built on this library | A named local, and the hazard stated on the field. **Found by ASan on the first run of the CLI's own test**, which is the argument for that test existing at all |
| **A report that carried the previous run's totals.** An already-finished step reported the documents and batches of the run that did the work, so an invocation that did nothing claimed to have moved thousands of rows | The report describes THIS invocation; the historical totals stay on the ledger row, where `--status` reads them. Found by the second-invocation test |
| A counter per batch, as docs/00 §9 asks of everything else | `migrate` is a process that exits, so nothing is ever there to scrape a counter it incremented. The counts go to the ledger row and the run's report. doc 18 §8 corrected |
| `uuid::v7_boundary` as a time-window bound on an `IdRange` cursor | Not shipped: no flag selects one, and an unused parameter on a migration's hot path is a parameter nobody has tested. What `IdRange` buys instead is a REFUSAL — a collection whose `_id` is not a 16-byte uuid — plus a `cursor_at` on the ledger, which is the only progress figure an operator can read without knowing the collection |
| Exit code 3 was "the catalogue is inconsistent with the cluster", and a mistyped flag is neither that nor 0, 1 or 2 | Broadened to "the run could not be attempted as described". A fourth code would change a contract deploy scripts already branch on, and both cases mean the same thing to one |
| docs/01 §13's worked example aliased `anvil::db` to `m`, which `metrics.h` already aliases to `anvil::analytics` in the same namespace | `mig`. An application declaring both tables would not have compiled, and the reference application is where that was found |

### What the phase found beside itself

`FormDb.TheListingIsPagedByTheIdAloneAndInOneLocale` compared a descending `_id` page against
INSERTION order. Two UUIDv7 ids minted in the same millisecond order by their random tails —
which `core/uuid.h` says outright — and five creations in a tight loop share a millisecond
routinely. It passed every time it was run alone and failed on a loaded machine, with a diff of
two sixteen-byte arrays sharing their first seven bytes. The expectation is built from the ids
now; the property under test was never the creation order.

## Phase 10 — emit the page

Scoped down from what this phase first contained, and the reason is recorded rather than
quietly applied. The original plan was a template engine — grammar, parser, boot loader, view
catalogue, page cache, a `lint` entry to check the markup and a seam for the catalogue. It was
designed before anyone read the only application that has ever needed server-rendered HTML,
whose entire footprint is **one page**: about seventy lines appending escaped fields into a
reserved buffer, no template, no cache, one raw insertion site.

So this phase ships the part that was load-bearing and deletes the part that was speculative.
**It adds no seam and no new module** — the writers sit beside `json_writer` and `csv_writer`
in `http/`, because that is what they are. The engine is deferred with its argument intact in
doc 19 §8.

What it does add is `anvil_listener_tests`, which docs/16's table has listed since phase 0 and
which nothing had ever created. That is where the row that is not optional goes.

| | Task | Notes |
|---|---|---|
| [x] | `http/html_writer` | `append_html_text`, `append_html_attr` (emitting the NAME, the `=` and both quotes — see the findings table for why the name came with it), and `append_url_attr`, which **delegates to `input::is_safe_link_target` rather than implementing a URL rule** — that function already exists, is already exercised by the sanitiser, and already rejects the protocol-relative `//evil.test/x` that a fresh implementation forgets. **No JS and no CSS context**: they cannot be written correctly, so they are removed rather than approximated |
| [x] | Move `append_html_escaped` | Out of `json_writer.h`, where it has sat since phase 1. The comment citing "the hotview renderer" — a thing anvil has never contained, in a sentence truncated mid-clause — was repaired by the defects section above, so what is left is the move itself: it belongs beside the other HTML writers, and the sentence about "the other place this process writes untrusted text into a document" names one only once they are together. **Mechanical, and it travels alone** |
| [x] | Close `input::SanitizedHtml` | **Do this before `append_sanitized`, not after.** It is an aggregate with public members today, so `SanitizedHtml{attacker_controlled, Ok}` compiles and any guarantee built on it is decoration. Private constructor, `sanitize_rich_text` the only producer — the shape `CapabilityScope` already uses. An API change under CLAUDE.md §9.3, and a source break only for aggregate initialisation, which nothing outside the sanitiser does |
| [x] | `append_sanitized` | The ONLY overload that appends markup verbatim, and **it does not take a string** — it takes an `input::SanitizedHtml`. Once the row above lands, a raw site stops being a discipline anybody has to remember and becomes a call the compiler checks. That is what replaces the `AllowsRaw` flag, the raw-site table and the boot-time raw count, all three of which were machinery for auditing the rule by hand |
| [x] | Re-sanitise at render | A verdict that is not `Ok` emits `<!-- content withheld: failed re-sanitisation -->` rather than the markup. Sanitising on write and trusting on emit is what the first draft of doc 19 argued, and it is wrong for one reason: **a stored value that fails the second pass means something bypassed the write path.** One bounded allow-list pass against stored XSS aimed at an admin session is not a close call |
| [x] | `http/content_headers` | The CSP with **no `script-src` at all** — a review page needs no JavaScript, so the strongest policy is also the correct one, and a policy with no script source needs no nonce. Plus `no-store`, `noindex, nofollow`, `no-referrer`, `nosniff`. The content is unpublished and the URL is a capability; a shared cache holding either is the leak the envelope exists to prevent |
| [x] | `CONTENT_ORIGIN` boot check | `require_origin_split`: the shape of both and the separation between them, in one call, comparing the HOST rather than the origin — see the findings table. Both failure directions are silent — a deployment with the two set equal still works and has lost the isolation — so it is a boot failure, not a warning |
| [x] | An end-to-end test for the credential path | **The one row here that is not optional.** The preview route's authentication has failed in production before and no unit test saw it: the render path wanted a `UserContext` from `__Host-at`, which is host-only and so never reaches the content origin, and every test drove the repository or the service, both correct. A page whose security rests on an origin split has an auth path that is end-to-end or unverified. `anvil_listener_tests` is where it goes |
| [x] | Doc 19; docs/00 §2, §7; docs/14 §4; README | Plus docs/16, whose binary table had listed `anvil_listener_tests` since phase 0 and whose `anvil_app_tests` never existed |

**Phase 10 gate: MET.** 1,046 tests green under ASan + UBSan against a live replica set, with
nothing skipped, and the same 1,046 green under the `dist` preset — which is where phases 7 and
9 each found a defect the sanitiser's timing had hidden. The three `tools/` scripts are clean.

### What the phase found that the design documents did not

| Found | Correction |
|---|---|
| **A value-only `append_url_attr` cannot refuse anything usefully.** Doc 19 described both attribute writers as taking a value and emitting its quotes. Written that way, a caller has already appended `<a href=` before the writer gets to say no, and "emits nothing" leaves a dangling `=` in the markup | Both take the attribute NAME and emit ` name="value"` whole. A refusal then removes the attribute rather than half of it, and no call shape can forget the `=` either. The name brought its own hazard with it — a name from request data is an injection that escaping does not fix, since `onload` is well-formed — so a name outside `[A-Za-z0-9-]` emits nothing |
| **`is_safe_link_target` accepts `/x" onmouseover=alert(1)`.** It is a site-relative path, which is exactly what the function is meant to accept; the plausible reading of "delegates to `is_safe_link_target`" is that the value is then safe to emit, and it is not | Delegating the DECISION is not delegating the ESCAPE. An accepted URL still goes through the attribute escape set, and that has a case of its own with this exact input, because the mistake is one a reader of the paragraph would make rather than one a careless author would |
| **`append_html_escaped` could not survive alongside the new writers.** It escapes both quote characters and emits neither, which is precisely the unquoted-attribute call shape doc 19 §2 exists to remove — a public function whose signature invites the bug the module is about | Moved, then made internal: it is the attribute escaper now. Element text escapes `&`, `<` and `>` only, which is what makes doc 19's "the text set plus both quote characters" a real distinction rather than a description of one function called twice |
| **`require_distinct` is the wrong comparison for the origin split.** Cookies are not port-scoped, so `https://x.test` and `https://x.test:8443` are byte-distinct origins and ONE host to every browser holding a `__Host-` cookie. A deployment split only by port would have passed the check and lost the isolation entirely | `require_origin_split` compares the host with the scheme and port removed, and takes both shape checks with it so a boot site cannot make one and forget the other. It does not check REGISTRABLE domains, which needs a public-suffix list anvil does not ship — stated in the header rather than implied |
| **Drogon does not keep `Set-Cookie` in the response header map**, the same special case the access filter found for `Cookie` on the request side. The first version of the redirect case asserted on `getHeader("Set-Cookie")` and read an empty string | The cookie is built as a `drogon::Cookie` and read back through `getCookie`, so what is asserted is what a client makes of the attributes rather than a substring of a header. `SameSite` is converted from `kPreviewCookieSameSite` rather than named again, so the attribute that lands is the constant anvil declares |
| A page cannot be asserted "byte-identical" on its status and body alone | `same_response` compares the status, the body, the content type AND the whole header map. A stealth 404 that acquired one extra header would otherwise pass the test whose entire subject is that it has not |
| The plan said "no new binary" | `anvil_listener_tests` is new only in the sense that nothing had created it: docs/16's table has listed it since phase 0. The one case that has to exist here cannot be a fixture in another binary, because `drogon::app()` is a process singleton whose `run()` does not return until `quit()` |

### What the phase found beside itself

`docs/16`'s binary table described eight binaries. Six existed, one of the six linked a
different target from the one the table gave it, and `anvil_app_tests` had never been created at
all — nothing in `anvil::app` needs a process that `anvil::platform`'s tests must not share, so
`anvil_platform_tests` links `anvil::app` and always did. The table now says what is there.

---

## Phase 11 — hand the client its tables

A web client built against this server holds the same tables it does: the routes, the
permission bits, the error vocabulary, the limits. Written twice they drift, and the drift is
silent in the direction that matters — a renumbered permission bit is a wrong authority check
rendered as an affordance, not a missing feature. So they are written once, here, and the
client is generated from what this phase emits.

| | Task | Notes |
|---|---|---|
| [x] | `wire_name(input::Reason)` | **A gap, not a feature.** `docs/00-architecture.md` §8 has shown `{"email":"INVALID_FORMAT"}` since phase 0 and nothing in the library produced that string, so every application invented its own spelling of anvil's own enum. `kMaxReason` comes with it, for the reason `kMaxErrorCode` exists |
| [x] | `descriptor/route_description.h` | **Seam.** The build-time half of the route table: id, capability, rate bucket, cursor field, ceiling, idempotence. A SECOND table rather than six more fields on `RoutePolicy`, which `policy_for` scans on every protected request |
| [x] | `descriptor/descriptor.h`, `src/descriptor/emit.cc` | The document, hashed over its `tables` object so an app version bump does not invalidate every client. Lives in `anvil::foundation`: generating a client needs no driver, no event loop and no database |
| [x] | `accesscontrol/route_projection.h` | The run-time half. A non-public path is never compiled into a bundle; it is projected here, filtered by `satisfies()` — the same function the filter calls — to the holder asking |
| [x] | `tests/testapp/route_descriptions.h`, `emit_descriptor.cc` | The reference application's table and its emitter, thirty lines, built by the suite. A seam that cannot be satisfied from outside anvil fails here |
| [x] | `tests/descriptor_test.cc`, the projection cases | Determinism, the hash boundary, the public/holder split, and the one that matters: the projection agrees with `satisfies()` over every route and a range of holders |
| [x] | `docs/01-seams.md` §14 | The seam, with the reasoning for the two-table split and for the id never being the path |
| [x] | The content tables | `field_types`, `sections`, `topics` and `events` arrive through `DescriptorInput` as the SAME spans their owning services already take — a second reader of a table the application declares once, not a second table. The media table is not among them, because it is not a span seam: it is read from `<anvil_app_config.h>` where the locale table is |
| [x] | The media table, and the `srcset` argument | The disagreement the phase's first half recorded and deferred, settled in the direction it pointed at: the objection is to a client CONSTRUCTING a path, not to it knowing a number. A width is emitted attached to the ROLE it belongs to, and no ladder, format list or file extension is emitted at all — so a client can write `320w` beside a role URL and still cannot assemble `w640.avif` |
| [x] | An idempotency store | `http/idempotency.h`. Five states, because each has exactly one correct response and a pair of flags does not. Keyed by route id, identity and the client's key together, with the request's own fingerprint separating a retry from a key reused for something else. **It does not degrade when Redis is unreachable** — the one place in this library where that is the right answer, and the reason is in the header |
| [x] | `Retry-After` on a shed response | The limiter now returns the window Redis holds, read inside the same script as the `INCR`, so it is still one round trip. `retry_after_seconds()` rounds up, never answers zero and clamps to the rule — three rules, each of which is a header a client honours incorrectly if it goes the other way |

| [x] | `/auth/refresh` in the reference tables | `route_registry.h` settled the route in prose — Public, because its credential is the refresh cookie and the access token is expired at exactly the moment it is called — and no table declared it, so nothing checked the claim and a generated client had no way to spell the one call that recovers a session. Described NOT idempotent: repeating it is safe only inside `rotation_grace`, which exists so two racing tabs both succeed and not so a client may retry |
| [x] | The holder-scoped table, served | `append_reachable_routes` had a suite and no caller, so the response it travels in existed only as a paragraph in its own header. `tests/session_listener_test.cc` serves it over a real listener through the real access filter, and the recorded bytes are what a client generator is built from rather than what a hand-written fixture believes the server sends |
| [x] | `tests/listener_fixture.h` | Prised out of `preview_listener_test.cc`, which owned the listener inside its own constructor. `drogon::app()` is a singleton whose `run()` blocks, so a second listener file could neither boot one nor reach into the first — every file now installs its routes through a registrar and the listener boots from the first case that asks for a port |

| [x] | A metric for the limiter's degrade path | `anvil_rate_limit_decisions`, labelled by the counter that ANSWERED — `shared` for the Redis one every instance counts into, `local` for the per-process bucket — and by what it decided. Two labels rather than two metrics, because the question an operator has is a RATIO: how much of what this deployment allowed or refused was decided by a bucket only one process can see, which is to say by limits that are quietly N times looser. The bucket is deliberately not a label: unlike `anvil_ttl_collection_rows`, the rules arrive as a `std::span` handed to a service rather than from a config header, so the value space could not be closed at compile time even if the names were anvil's to use. The `LOG_WARN` survives as ONE line per transition into the degraded state and one on the way out — a counter cannot carry the reason for an outage and a log line must not carry the rate — and the transition is asserted by capturing Trantor's output, because "one line per outage rather than one per request" is a property about lines and there is no other way to state it |

**Phase 11 gate: MET.** 1,094 tests green under ASan + UBSan against a live `rs0` replica set
and Redis, and the same 1,094 under the `dist` preset. Baseline at the start of the second half
was 1,064: thirteen idempotency cases, nine for the window and the header, and eight for the
content tables — plus ten compile-time assertions that do not appear in the count at all,
five over the `Retry-After` arithmetic and five over the store's configuration, because a
number that ships as a store which does not store is a build failure rather than a test.

**Amended: 1,104**, under both presets — ASan + UBSan against a live `rs0` and Redis, and `dist`.
Ten cases for the served table, in `anvil_listener_tests` beside the preview ones and sharing
their listener. The three rows above them were opened by the client generator
rather than by this phase — a resolver cannot be built against a response nobody has emitted —
and they are recorded here because they close the phase's own seam rather than the generator's.

### What the phase found beside itself

**The reference application could not include two of its own headers at once.** `topics.h` had
`using n::Scope;` at namespace scope, and `capabilities.h` declares an unrelated
`testapp::Scope` for its capability scopes. Nothing had ever included both until the descriptor
emitter needed every table in one translation unit, and then it was a hard error with eleven
diagnostics. The rule CLAUDE.md §1 states for anvil's headers — no `using namespace` at
namespace scope — is the same rule, and the reference consumer is where it was worth finding:
an application that hit this would have hit it in its own build with no idea the collision was
avoidable.

**The window script could not repair a key that lost its expiry.** The old form set the TTL
under `if hits == 1`, and a counter above one can never be at one again — so a key that lost
its expiry, by a `PERSIST`, a restore from a dump, or an operator, refused that identity
permanently and nothing would ever have reset it. Driving the expiry from the `PTTL` instead is
the same cost and covers both the fresh key and the broken one; the case that reaches that
state deliberately is in the suite, because in production it is reached by a failure nobody
watches for.

**`std::to_string` on a shed path is an allocation charged to the refusal.** The
`Retry-After` value is formatted with `std::to_chars` into a ten-byte stack array instead —
the request that must cost the least is the one being refused, and it is the one an attacker
sends most.

**The idempotency store's first draft logged one line per request during a Redis outage**,
which is precisely what docs/00 §9 forbids by name: under the load that makes such a line
fire, the line is itself the outage. It counts now, as a sixth `unavailable` value on
`anvil_idempotency_claims` — the one label value that is not a state a claim can return.
`record` and `release` still log, and that is safe only because of the ordering: an outage
refuses every claim, so nothing reaches them at request rate.

**`RateLimiter::check` has the same shape and is NOT fixed here.** Its degrade path writes a
`LOG_WARN` on every request for as long as Redis is unreachable, and `RateLimitVerdict::degraded`
says in its own comment that the degradation is "surfaced so it appears in metrics" — while no
metric consumes it. The fix is a counter of its own, which is a decision about what a limiter
reports rather than a line to add to a commit about `Retry-After`, so it is a row here and not
a hunk in `1b5f6e4`.

**`LocaleSpec` carries no digit system.** A client shaping Arabic-Indic digits for display
derives it from the tag through its own platform's internationalisation, and a second table
here would be a second thing to disagree with it.

**The ETag the projection documents is keyed to half of what it needs.**
`route_projection.h` says the tag is keyed to `perm_epoch` by the caller, and that is the half
that moves with authority. The other half is the table itself: a deploy that adds a route, moves
one or re-authorises one changes what a holder reaches while their epoch stands still, so an
epoch-only tag answers `304` and leaves a long-lived tab calling yesterday's map until somebody
happens to edit that holder's permissions. The reference handler folds in one hash over the
table taken at boot, and the header and `docs/01-seams.md` §14 now say so. Whether anvil should
SHIP that identity rather than have every application re-derive it is a public-header question
and therefore a semver one, which is why it is written down here rather than added quietly:
the argument for shipping it is that an application that gets it wrong gets a stale
authorisation map, and the argument against is that anvil would then be choosing an ETag format
for every deployment at once.

**The listener fixture handed out a port before anything was listening on it.**
Drogon fires beginning advices before `startListening()`, and the two are different events:
`createListeners()` has already bound the socket by the time an advice runs — which is the only
reason the kernel's ephemeral port is readable there — but nothing calls `accept` until every
advice has returned. The fixture resolved its promise inside the advice, so a client could
connect into that window and be refused, and the case then failed with a transport error that
looked nothing like the property it was asserting. It survived eight cases in eight processes
and did not survive eighteen: under `ctest -j` roughly one case failed per run, a different one
each time. The constructor now waits for a connection to be ACCEPTED rather than for a port to
be known, and throws if that never happens — a fixture that gave up quietly would report the
failure as whichever case ran first.

**The limiter's own tests could not run beside each other.** `unique_address()` numbered its
addresses from a process-local counter, and `gtest_discover_tests` gives every case its own
process — so all three `RateLimiter` cases took `fd00::1` and counted into one Redis key under
`ctest -j`. It passed alone and failed in the full run, which is the worst shape a test can fail
in: the run nobody can reproduce is the one CI reports. Fifteen CSPRNG bytes, and the helper now
does what its own comment already claimed.

---

## Phase 12 — the addresses, the failure, and somewhere to point a client

Phase 11 emitted the tables and its second half served one of them. What proved them was not a
test in this repository: it was a client generator built against the descriptor in a sibling
checkout (`hammer`), which turns every unstated assumption into a file that will not compile or
a call that cannot be spelled. Ten things survived that build, and each one was reported here
rather than worked around — each has a fallback on the generator's side that was left
deliberately uncomfortable, a path written by hand, a type declared by hand, a key tested for
before it is read, because **a workaround in a client generator is a workaround in every
application generated by it**. Nine earlier rows of the same list are closed, and five of those
closed only because they were written down instead of routed around, which is the whole case
for this phase existing.

They are four shapes, and the first is the largest. **Five are an address the descriptor
withholds or never carried**: a client that cannot spell a call writes the path itself, and a
path written in an application is a second copy of something the route table owns. **Two are the
bytes of a failure** — [`00-architecture.md`](00-architecture.md) §8 has published
`{"error":{"code","request_id","fields"}}` since phase 0, and the one place a body is assembled
does it with a string concatenation inside the access filter — and they need a third thing
neither of them names, because the string `request_id` appears in no writer anywhere in this
repository. **Two are that there is no anvil to point a client at**: this library is a set of
test executables, so the runs only an end-to-end harness can make have nowhere to run. **One is
the response half of the descriptor**, which has never existed at all.

Nothing here is a new subsystem. Most of it is a field, a rule or an entry on a table that
already ships, or a writer beside one; two are programs, one is a release step, and one — the
response binder — is the only genuinely new machinery in the phase.

### 12a. The addresses the descriptor withholds

| | Task | Notes |
|---|---|---|
| [x] | A description may not say `ANY` | `descriptions_match` refuses `Any` in a description, and it pairs through `policy_for` rather than through an equality loop of its own — so a description naming `POST` against an `Any` policy pairs the way the filter would actually answer it, and the resolution exists in one place instead of two that can disagree. The reference application's `auth.logout` now says `POST`; its POLICY stays `Any`, because one handler dispatching internally is a real shape and `routes.h` exists to exercise it. An API change under CLAUDE.md §9.3, source-breaking for any table that used `Any` in a description — which is the point: a build failure rather than a route nobody can reach. **Resolving through `policy_for` opened a hole the plan did not name, and the one-description-per-route rule is what closes it** — see the findings below |
| [x] | `RouteDescription::bootstrap` | `session.current` is `Authenticated`, so `path_is_public` refuses its path, the generator withholds it the way it withholds every holder route's — and **the address of the session arrives with the session**, which on a cold load does not exist. The first read of the whole system is unspellable, so the application writes `/session` by hand. What the table is missing is not authority, it is disclosure: see *Two questions one predicate was answering* below. `bootstrap` says the path may be compiled into a bundle; `access` still says what the filter demands, so the route keeps its real `401` — the answer a client needs to tell "re-authenticate" from "route gone", and exactly what declaring `/session` Public with a handler-side check would have destroyed. Guarded at compile time: `descriptions_match` refuses `bootstrap` on a policy that requires a bit or whose class is `Guarded` or `Stealth` **Closed.** `RouteDescription::bootstrap`, and `path_is_public(access)` became `path_in_bundle(description, access)` — one predicate called by the emitter and by the projection, so "the client already has this path" is decided in one place. `session.current` is the reference application's one bootstrap route and the emitter now says two true things about it: `visibility:"public"` and `access:"authenticated"`. Three decisions beyond the row. `bootstrap` is refused on a PUBLIC policy too, not just on Guarded and Stealth: it is merely redundant there rather than dangerous, but a table where both reasons can be true is one a reader cannot interrogate, and refusing it makes the two arms of `path_in_bundle` provably exclusive — which has its own case over the reference table. The guard sits in the per-ROUTE loop of `descriptions_match` rather than the per-description one, because it needs the policy and resolving one up there means comparing `policy_for(...)` against `nullptr`, which GCC will not fold when the route table has external linkage — the same wall the description count already walks around, hit for a fourth time. And `include_public` became `include_bundled`, because what it overrides is no longer a question about Public. Two existing cases changed and both changed for the right reason: the projection's agreement case had to start skipping bundled paths rather than public ones, and the listener case that asserted `"session.current":"GET /session"` in the session body now asserts its absence — the table does not name the route that produced it. API change under CLAUDE.md §9.3: `path_is_public` is gone, `append_reachable_routes` renamed a parameter, and `RouteDescription` grew a trailing field that older tables omit and get `false` for |
| [x] | `media.object` in the reference tables | §14 of [`01-seams.md`](01-seams.md) publishes the role and its width and leaves the address to the route builder, [`08-images.md`](08-images.md) §4 settles the grammar as `GET /media/{ns}/{id}/{role}`, and no table declares it — so the pattern is written in the application. It does not become a field in the `media` object: the media table says what a role IS, the route table says where a route LIVES, and an address in two tables is two things to keep in agreement. So it joins `tests/testapp/routes.h` and its description like any other route, which is where a claim of this kind gets checked. The `{role}` segment's value set is the media table's, so a generated client gets an enumeration rather than a string, and the route builder still does the encoding: the hazard [`08-images.md`](08-images.md) §4 is about — a client assembling `w640.avif` — stays closed, because nothing here tells it either half **Closed.** `GET /media/{ns}/{id}/{role}` is a row in `tests/testapp/routes.h` with `media.object` beside it in the descriptions, and the `media` object is unchanged. Two decisions the row left open. It is PUBLIC, by the same mechanism `/preview/{id}` is: it is served from the media origin, where the host-only session cookie never arrives, so the filter has no credential to read and any other class would describe a check that does not happen — `Cache-Control: private` on the response is about the HANDLER's authorisation, and docs/08 §4 calling this "the public grammar" is the same statement from the client's side. And it counts into NO rate bucket: the `media` rule is 20 a minute and is sized for uploads at one libvips decode each, so a gallery page of thirty images would take a 429 on an ordinary render and the symptom would be images vanishing above the fold. A case pins each, including the one not taken. Beyond the row: the `{ns}`/`{role}` join a generated client makes is a naming convention nothing checked, so a case now WALKS the pattern, skips `{id}` as the one segment no table enumerates, and asserts every other segment is a key the media object supplies values for — renaming `{role}` to `{size}` fails it, which is how it was checked |
| [x] | The types a namespace accepts | The descriptor carries `upload_max_bytes`, the roles and the widths, and nothing that says which types a namespace will take, so the accept list is the application's to supply — a second copy of what `fs/sniff.h` already enforces, and the copy that drifts is the one offering a file picker a format the server rejects after the bytes are uploaded. The allow-list is anvil's, because it is the pipeline's capability; NARROWING it is the application's, because that is a fact about a namespace. `NamespaceSpec` grows an accept mask over `Mime` defaulting to everything the pipeline decodes, the upload path checks that mask rather than the global list, and the emitter writes `"accepts":["image/jpeg",…]` beside the roles — the emitted list and the enforced list being one table read twice. A global list would close the copy and not the case that reopens it: the moment one namespace takes no AVIF, the narrower list is written in the client by hand again. SVG never appears in an emitted list, because it is refused explicitly rather than by omission. It costs `static_assert(sizeof(NamespaceSpec) == sizeof(std::string_view))`, an ABI change under CLAUDE.md §9.3, stated here rather than discovered **Closed.** `NamespaceSpec::accepts` is a `MimeMask` defaulting to everything the pipeline decodes, `UploadSink` enforces it with its own reason `upload.ns_type`, and the emitter writes `"accepts":[…]` beside each namespace's roles from the same table. The `sizeof(NamespaceSpec) == sizeof(std::string_view)` assert is gone as the row predicted, replaced by one that states the padding and why it is affordable here. Three decisions beyond the row. The default is DERIVED by folding over the `Mime` enum rather than listed, so adding a format widens it by itself and the default never becomes the second list this mechanism exists to remove. The sink takes its namespace at `open()` and `publish()` lost its parameter — the namespace decides which types an upload may be, so a sink that learned it only once the bytes were on disk could not refuse one, and it was the one fact being passed twice. And `Mime::Unknown` deliberately has NO bit, so `mime_accepted` fails closed on an unsniffable file even against an all-ones mask, with no special case at the call site. The reference table narrows `guest` to JPEG and PNG, because it is the only namespace fed by unauthenticated input. One existing assertion changed and the change is the finding: `EXPECT_FALSE(contains(doc, "avif"))` had to become precise, because `image/avif` is now published as a fact about what a namespace TAKES while `avif` the extension stays unpublished as a fact about what the server WRITES — and a variant filename is not URL-addressable in any case. ABI change under CLAUDE.md §9.3, plus an API change to `UploadSink::open` and `publish` |
| [x] | `append_holder_authority` | `satisfies()` short-circuits on `UserType::SuperAdmin`, and a superadmin's permission set is deliberately **not** all-ones — so the one account that reaches everything counts zero bits, and a client rendering a non-route affordance from bits hides every one of them from exactly that account. The route table is unaffected: the server built it with `satisfies()`, so it already contains what a superadmin reaches. What is missing is the answer for an affordance that is not a route. The writer lives beside `satisfies()` and emits `"superadmin":<bool>` from the same enum the short-circuit reads, plus the held permission names in BIT order, the order the descriptor emits them in. Not an all-ones mask: `core/types.h` keeps "is superadmin" and "holds every bit" distinguishable so that no bit-fiddling can synthesise the first, and sending all-ones would put that conflation on the wire for every client to un-conflate. **It is an affordance hint and never an authority** — the server re-derives both on every request. The generator's fallback today is to read `superadmin` from the session payload and treat its absence as `false`, which hides more rather than less, and is waiting on the reference controller sending it **Closed.** `accesscontrol::append_holder_authority` emits `{"superadmin":<bool>,"perms":[…]}`, and the reference session handler puts it in the envelope beside `routes` — the sibling key `route_descriptions.h` said that envelope existed for. Two decisions beyond the row. The short-circuit became a named `is_superadmin(UserType)` next to `satisfies()`, which now calls it, so the flag a client is told and the branch the filter takes are one expression rather than two that can drift — and a case asserts that equality over every `UserType` rather than against an expected string. And the writer lives in `route_projection.*` rather than in `decision.h`: the predicate belongs beside `satisfies()`, but a JSON writer belongs beside the other one that fills the same response, and putting it in the constexpr decision header would have pulled a writer into every translation unit that asks an authority question. A case also pins that a bit no permission declares — the reserved gaps between an application's blocks — reaches no client, which is the second reason this is not `~PermSet{}` and the one the row did not name |

#### What the `ANY` row found that its own design did not

| Found | Correction |
|---|---|
| **Pairing through `policy_for` opens a hole that size equality used to close.** An `Any` policy answers for every method, so two descriptions naming two methods of one such policy BOTH pair — sizes still equal, ids still unique, no `(pattern, method)` repeated — and the route they left undescribed is simply absent from the descriptor. The old equality loop refused that table, for the wrong reason: neither description matched anything | "Every route described exactly once" as a count over the ROUTES, which subsumes the pairing check and also catches a description naming a route that does not exist |
| **The obvious spelling of that check does not compile**, and the diagnosis is sharper than the two times this repository has hit the wall before. `policy_for(...) == nullptr` inside a `constexpr` function is not foldable — not because of the sanitiser, and not because GCC is strict about null, but because the pointer points INTO AN OBJECT WITH EXTERNAL LINKAGE, which is exactly what `inline constexpr std::array<RoutePolicy, N>` in an application's header is. Comparing the same possibly-null pointer against `&route` folds fine | The count is expressed that way, and the finding is written down properly at last: `collection_is_declared` (phase 3) and `validators_are_present` (phase 5) both recorded it as a sanitiser quirk, and it is not one. Reproduced in fourteen lines |
| The count is O(routes × descriptions × routes), and nobody had measured what that costs a real table | Measured rather than guessed: a synthetic table of 256 routes evaluates inside GCC's default `-fconstexpr-ops-limit` and one of 512 does not. An application past that raises the limit; it does not get a weaker check. Stated in the header |
| **The served route table already carried the defect, in the recorded bytes a client generator is built from.** `session_listener_test.cc` asserted `"auth.logout":"ANY /session/logout"` — a real response, over a real listener, naming a call no generated client could make | The line asserts `POST` now, and a second one asserts that `ANY` appears nowhere in the body at all |

### 12b. The failure both documents publish and nothing writes

| | Task | Notes |
|---|---|---|
| [x] | `http/request_id.h` | **Landed:** the type, the mint and the rendering, with the encoding pinned by hand-computed vectors at both extremes — 128 bits do not divide into fives, so two zero bits lead and an all-ones id renders `7ZZZ…` rather than `ZZZZ…`. A distinct type and not a `Uuid`, although both are sixteen bytes: a `Uuid` here names a THING and several of them are capabilities, so the two must not be assignable by accident. **Left:** every part of the wiring — minting in the pre-routing advice, the `RequestScope`, `X-Request-Id`, and moving `plain_error` onto the writer. Until that lands a handler can write a correct body and nothing mints an id to pass it. §8's envelope and §9's log line have both named `request_id` since phase 0, and the string appeared in no writer here — so neither contract is one this library keeps. Sixteen bytes: a 48-bit millisecond timestamp and 80 CSPRNG bits, rendered as 26 Crockford base32 characters, which is the `01J…` §8 has always shown. Time-ordered, so a log file sorts into occurrence order without parsing; no MAC and no host identity, which is why it is not UUIDv1; transcription-safe alphabet, because the id's entire purpose is that a person reads it back to support. It identifies a log line and authorises nothing. Minted in the earliest pre-routing advice, because a request that fails before the filter still needs one, and carried in the SINGLE request attribute `UserContext` already occupies — a `RequestScope` with the context first at offset 0 and its `sizeof == 64` assertions intact, so the filter's read still pulls one cache line and the id sits in the next, untouched on the hot path. The advice creates the scope with the context zeroed and the filter fills it before any handler runs, on the loop thread that owns the request; readers are handed `const UserContext&`. A second attribute would be a second string hash, a second map insert and a second control-block allocation on every request, which is the rule `core/user_context.h` already argues **Closed.** `http::install_request_scope()` registers both halves — a pre-routing OBSERVER that mints the id into an `http::RequestScope`, and a pre-sending advice that sends it back as `X-Request-Id` — and they install together because half the pair is a half-kept contract in either direction. `RequestScope` is 88 bytes with the context at offset 0 and the id at 64, both asserted; `kUserContextKey` is GONE and the key is declared beside the scope, with no second spelling left behind, because a renamed alias here would be the two-constants trap rebuilt by hand. `plain_error` goes through `append_error_body`, which reverses the comment that used to justify it. Four decisions beyond the row. `attach_user_context` REFUSES a second fill and the filter logs when it does: the two ways it can fail are "no advice was installed" and "something already established an authority", and both are silent boot mistakes. The context reader returns an ALIASING `shared_ptr` into the scope rather than a copy, so a context crossing a thread-pool boundary keeps the id reachable and costs no second allocation. `RequestScope` carries an explicit `has_context` rather than deriving it from `is_nil(user_id)`, because that derivation believes no token can name the nil UUID, which is a property of an application's minting and not of this library. And the header is withheld from EVERY 404 rather than from the shared not-found object by address — which is where deleting the guard found something the design had not: Drogon caches that object's RENDERED form, so two consecutive stealth drops came back carrying the first request's id. A per-request value written there is one visitor's correlation id handed to every later 404 for the life of the process. API change under CLAUDE.md §9.3: `anvil::kUserContextKey` is removed |
| [x] | `http::append_error_body` | Landed with one deliberate departure from the design below: `fields` is written whenever `carries_field_detail()` says so, **including when the span is empty**, so the body's shape is decided by the code alone and a client parses one thing rather than two. The `X-Request-Id` half of this row moved to the wiring row above, because nothing mints an id yet. One writer, in `http/errors.h` beside the status table it already owns, taking the code, the id and `std::span<const input::FieldError>` — the type `input/fields.h` already defines, so the map's keys are schema constants and never keys taken from the request. `fields` is written only where `carries_field_detail()` says so, which keeps "only `ValidationFailed` explains itself" a property of one function rather than of every call site; a `409` that explains which version it saw is a disclosure. **The stealth body does not go through it.** `kNotFoundBody` stays the `constexpr` it is: an id on that path is a correlatable value a genuine 404 does not have, which is one of the tells `stealth.h` enumerates beside `WWW-Authenticate` and `Set-Cookie`. The filter's own denials DO carry one, which reverses the comment in `plain_error` — its premise is that "a 401 from this filter carries no server-side detail worth correlating", and `deny()` has written an audit row for every denial since phase 3. The id is what joins a user's "I cannot get in" to that row. `X-Request-Id` on every non-stealth response with it, so a client holding a body it cannot parse still has the id to quote |
| [x] | `anvil_emit_envelopes` | The generator's recorder compiles against these headers from a sibling checkout and reads `wire_name`, `http_status`, `is_stealth_hidden` and `kNotFoundBody` — real provenance for everything except the two lines of body ASSEMBLY, which it copies from `access_filter.cc` and `stealth.cc` because anvil has no function that returns one, and a copy in a client is a copy that goes stale silently. Once the row above lands the recorder is anvil's: one program linking `foundation` and nothing else, the shape `testapp_emit_descriptor` already has, writing every body the library can produce — one per `ErrorCode`, a `ValidationFailed` with a two-field map, and the stealth body verbatim. Deterministic, so the artefact diffs: the recorded id is a documented placeholder rather than 26 fresh characters per run, because a file that changes every run is a check nobody keeps. No string in the recorder is a body. The distinction from the descriptor is worth keeping straight — that one is the application's TABLES, this one is the library's BYTES, and only one of them changes when an application does **Closed.** `tools/emit_envelopes.cc`, one program linking `anvil::foundation` and nothing else, built whether or not the tests are — its output is a release artefact and needs no reference application, which is the library/application line showing through the build. No string in it is a body: every `body` comes out of `append_error_body` or is `kNotFoundBody` verbatim, and the `ErrorCode` list carries the same `static_assert` against `kMaxErrorCode` the descriptor's does. Three decisions beyond the row. The placeholder id is ALL-ONES, rendering `7ZZZ…`, rather than all-zero: zero is what `request_id_of` returns for a request with no scope and already means "no id was minted", while all-ones cannot be mistaken for a mint and exercises the encoder's hard case, so a recorder that produced 26 `Z`s would be visibly wrong in the artefact. The worked `fields` map has TWO entries, because a client that has only seen a single-field map has never seen the separator. And it records NO headers but the name the id goes out under: the stealth response's header set — including the three it deliberately omits — is built in `anvil::platform` and needs Drogon, and a header set typed in here would be exactly the copy this program exists to delete. `tools/` is now scanned by `check-source-bans.sh`, because a program anvil ships is held to the library's bans |

### 12c. Somewhere to point a client

| | Task | Notes |
|---|---|---|
| [x] | `anvil_reference_server` | anvil is a library plus test executables: `anvil_listener_tests` starts Drogon in-process and exits, `testapp_emit_descriptor` writes a file, and nothing serves the reference application on a port. On the generator's side that is four written suites — a live contract run and three browser runs, the two-tab credential run among them — that fail rather than skip and have nowhere to run. The evidence it is worth closing is the browser run that DID execute, because it needed no server: it found a Trusted Types sink that all 1,064 of that side's own unit tests had passed over. Five rules keep it a test binary rather than a deployment, and they are in *What keeps the reference server a test binary* below **Closed.** `tests/testapp/reference_server.cc`, serving all twelve reference routes through `register_route`, the real `AccessFilter`, the real `SessionService` and the real `AuthzService` — a stand-in for any of those would demonstrate a system this library does not have. All five rules are kept and each is marked in the code where it is kept. Three decisions beyond the row. The boot verdict has THREE outcomes rather than two: exit 3 for "a dependency is unreachable" and exit 1 for "you pointed me at somebody's data", because one non-zero exit for both turns the second into a green CI run on a machine that is missing the first — `tools/check-reference-server.sh` reads 3 as a CTest skip. Trantor's log output is redirected to stderr before anything else runs, because rule 1 says the first line of STDOUT is the base URL and one framework line would make that a lie. And the static location takes `allowAll = false`, so only files with a recognised extension are served: a directory a harness drops a bundle into is a directory somebody eventually drops something else into. The marker collection is deliberately NOT in `config::kCollections` — that table is application DATA and every entry of it gets indexes and options, and the marker has to be written before any of that runs. One finding: the first seeding call passed the permission mask as `direct` and an empty `effective`, which stores an account whose grid shows permissions and whose TOKEN carries none, and the symptom was a signed-in editor handed a route table with the content routes missing |
| [x] | The reference descriptor as a release artefact | The generator's fixture is current, and refreshing it is a person remembering to run a binary in a sibling checkout; its own staleness check regenerates the CLIENT from the committed DESCRIPTOR, so it is green by construction whatever release that descriptor came from. It cannot be closed from that side — a sibling checkout is not a build dependency. So anvil publishes `anvil-<tag>-reference-descriptor.json` from `testapp_emit_descriptor` at the tag, beside `anvil-<tag>-envelopes.json` from the row above, and a fetch step can name a release instead of a note saying to run something. The release step regenerates and diffs rather than trusting the file, which determinism is what makes possible — the same tables produce the same bytes, as §14 already requires for the hash to mean anything. **Only the reference descriptor is ever published this way**: §14's "the descriptor is a build artefact and is never served" is about an APPLICATION's, which carries every admin path. `testapp` has no deployment and its paths protect nothing, and the two files look identical, so the exception is written down **Closed.** `tools/release-artefacts.sh`, which emits both files twice and diffs before it keeps either, and publishes the FIRST run rather than the one that happened to look right. It runs as a CTest entry in `--check` mode rather than only at a tag, and that is the decision beyond the row: determinism is not a packaging detail, it is what makes the descriptor's hash answer "is this client built from this server" at all, and a tag is the worst moment to discover it does not. The exception is ENFORCED rather than written down twice — the script refuses any descriptor whose `app.name` is not the reference application's, so the rule cannot be applied to the wrong file once. Both refusals are exercised by hand against a deliberately wrong emitter rather than assumed |

### 12d. The response half of the descriptor

| | Task | Notes |
|---|---|---|
| [x] | A response binder | anvil writes responses by hand, so nothing describes a response SHAPE, and a generated client declares its own types — hand-written, unverified, and the one part of a generated client the descriptor does not underwrite. The tempting fix is a `ResponseSpec` table emitted into the descriptor while handlers keep appending by hand, and it is wrong for the reason this document keeps repeating: that is a second copy of the response shape, in the place least able to check itself. So the spec IS the writer. `ResponseWriter` walks the declared fields in order, and a call naming a key the spec does not have next is a build error wherever the call sequence is straight-line — which is what makes the emitted schema a description of the bytes rather than a claim about them. It lands scoped: flat objects and arrays of one declared shape, with the reference application's own responses as the consumer that proves it. A route with no spec emits `"response":null` and a client falls back to its own declaration, which is today's behaviour — so adoption is per route rather than a flag day. Anything richer stays hand-written and undescribed, said plainly rather than approximated **Closed.** `http/response_spec.h` is the vocabulary and `http/response_writer.h` is the writer, split so that `route_description.h` — included by every table an application declares — does not pull in a template it never instantiates. `RouteDescription` grew `response` and `response_is_array`, both DEFAULTED, which is what makes adoption per route: the eleven undescribed reference routes simply stop writing values and get `"response":null`. Four decisions beyond the row. The two new members are APPENDED rather than ordered by alignment, against CLAUDE.md §3.2 — the packing is exact either way, and appending is what keeps an application's existing table compiling, which is the "per route, not a flag day" property the row demands; the departure is argued in the header rather than left to be discovered. The row's own claim that a described body "may not be assembled under a runtime branch" is WRONG and is corrected in the header: a writer is a value holding one reference, so both arms of a branch start from the same statically-known position and each is checked there — which is how a nullable field is written — and what genuinely cannot be expressed is a loop picking a key by a runtime index. `descriptions_match` refuses a duplicated key (JSON does not forbid one and every parser resolves it differently) and an `is_array` with no shape. And `append_json_uuid` moved onto `std::span<const std::uint8_t, 16>`, because it took a reference to a C array and `Uuid` is a `std::array` — it had no caller, so it could only ever have been called through a cast. The reference application describes `identity.me` and deliberately leaves `session.current` undescribed, which is the instructive entry: its body is two nested objects and the honest answer is null. Descriptor format 3; ABI change under CLAUDE.md §9.3 |

**Phase 12 gate:** the suite green under ASan + UBSan against a live `rs0` and Redis and green
under `dist`, as every phase gate is, plus three things a test count cannot express. The
reference server starts in CI and answers a request. The generator's contract suite passes
against the published artefacts rather than against a fixture somebody refreshed. And every
table shape this phase makes illegal — a description saying `ANY`, `bootstrap` on a permissioned
route, a namespace whose accept mask is empty — fails at compile time, proved

**Met, with one half of one clause outside this repository.** `tools/check-reference-server.sh`
is a CTest entry: it starts the server against a live MongoDB and Redis, reads the base URL off
the first line of stdout, signs in with the password the server drew at boot, and asserts that
the table it is served is the one `tests/testapp/` declares — and that a stealth drop and an
unmatched route agree in body *and* in headers, which a body comparison cannot see. It exits 77
where there is no database, so a machine without one skips rather than going green on nothing.
`tools/release-artefacts.sh --check` runs in the same suite, so determinism is a standing
property rather than something a tag discovers. The illegal table shapes are `static_assert`s
over deliberately wrong tables and appear in no test count, and this phase added two more — a
response shape with a duplicated key, and an array of no declared shape.

What this repository cannot assert is the *generator's* contract suite passing, because a
sibling checkout is not a build dependency and a check that shells out to one passes on exactly
one machine. What it can do is publish the artefacts that suite needs, which is what the row
above closed.
the way phase 11's configuration assertions were: a `static_assert` over a deliberately wrong
table, which does not appear in the test count at all.

### Two questions one predicate was answering

`path_is_public(access)` answers "may this path be compiled into a bundle" by asking "is this
route reachable with no credential". Those are the same question for every route this library
had until `/session`, and they come apart there: the route demands a credential, and its address
must be public anyway, because **a client cannot be told where to ask for the table until it has
asked**. `/auth/refresh` escaped the problem by being genuinely Public — its credential is a
cookie the filter does not read — and that is a coincidence of that route rather than a pattern
to copy. Declaring `/session` Public to get its address published would move an authentication
check out of the filter and into a handler, which is how a route stops failing closed.

So the two questions get two fields. `access` stays the authority answer and is emitted
unchanged; `bootstrap` is the disclosure answer, and `path_is_public(access)` becomes
`path_in_bundle(description, access)` — **one predicate, called by both the emitter and the
projection**, so "the client already has this path" is decided in one place. That has a second
consequence worth having: a bootstrap path drops out of the holder-scoped projection for the
same reason a public one already does, which is that resending a path the bundle contains is
bytes on a response every signed-in tab asks for.

The generated client is then told two true things about `session.current` rather than one false
one: its address is public, and calling it without a session is a `401`.

### What keeps the reference server a test binary

A reference application that serves is a thing people deploy. Five rules, each of which exists
because of the way it fails without them:

- **It binds loopback and prints its base URL as the first line of stdout**, so a harness reads
  the port rather than guessing it, and an ephemeral port is the default — a fixed port is a
  process nobody can run twice.
- **Every credential is drawn at boot and printed.** A fixed password in a repository is a fixed
  password in a deployment, and this binary exists to be copied from.
- **It refuses to start against a database it did not create**, by a marker document it writes on
  first use. "I pointed the reference server at the wrong URI" must not be a thing only a backup
  recovers from.
- **It shares every table with `tests/testapp/`.** What a browser run sees and what the suite
  asserts then cannot disagree, which is the entire value of the reference application being the
  proof (CLAUDE.md §1).
- **It is never `install()`ed**, and it serves exactly one static directory at one path — the
  deliberate exception to the component map in [`00-architecture.md`](00-architecture.md) §1,
  where bytes on disk are the edge's job. That exception is the reason the row exists:
  `SameSite=Lax` cookies are not sent cross-site, so a harness fulfilling its bundle from another
  origin is testing a cookie policy no deployment has.

### What the cross-repo build settled, and what it deferred

Three arguments were settled by being built against rather than by either side giving way, and
all three are already recorded where they belong: the `srcset` resolution in §14 of
[`01-seams.md`](01-seams.md), `auth.refresh` being described as not idempotent in phase 11 above,
and `LocaleSpec` carrying no digit system because a client derives digit shaping from the tag and
a second table would be a second thing to disagree with it.

Six things the generator deferred **deliberately**, recorded here so that anvil does not ship a
seam for one of them on the assumption that it was an oversight:

- **Offline persistence**, because a persisted API response outlives the cookie that authorised
  it — private data readable after the session ended, on a shared device. It returns as a
  per-resource opt-in naming its own eviction and threat model, not as a default.
- **A service worker**, which is a second origin-scoped program with its own update lifecycle and
  makes the stale-bundle problem strictly worse before it makes anything better.
- **A second framework adapter.** The core is framework-agnostic and the adapter is thin on
  purpose; the second one is written when a consumer needs it, and its existence is the proof the
  boundary held.
- **A template or view layer**, for the reasons [doc 19](19-server-side-rendering.md) §1 gives
  for retiring anvil's own, which apply on a client twice over.
- **Request coalescing and a normalised entity cache.** Both real wins, both large, and neither
  worth designing before there is a consumer whose screens show what gets coalesced.
- **Client-side sampling of analytics.** The server samples whole sessions deterministically, and
  a second sampler produces a compound rate nobody can reason about.

---

## Phase 13 — what the first consumer hit

Phase 12's list came from a client *generator*, which fails loudly: a call it cannot spell is a
file that will not compile. This one came from the first **application**, which does not — every
item below shipped, served traffic, and reported green while doing the wrong thing. That is the
whole character of the phase and the reason its rows read differently: there is no build error
to work back from, only a symptom somebody eventually noticed.

Four were reported, and a fifth row was opened beside them to record a question rather than a
task. All six are now closed, and each is recorded here because the *decision* in it is what a
reader six months out will need, not the diff. They share a shape: a function whose two halves
are individually correct and collectively wrong, or a number an operator is told to read that
answers a different question from the one they are asking.

| | Task | Notes |
|---|---|---|
| [x] | `accesscontrol/route_registration.h` | The registry proved a pattern DECLARES a policy; nothing proved the filter that ENFORCES it was attached, and nothing could — `getHandlersInfo()` returns pattern, method and description and carries no filter at all, so an unguarded `Stealth` route passed the boot guard and the coverage test alike and was a **public admin route with every check green**. `register_route` builds the constraint list from the policy, so the name is never typed in an application. Two decisions beyond the report: `Authenticated` is enforced too, because `evaluate_token` denies an absent token on it by the same branch and leaving it out would have been the same defect one class over; and `Public` gets no filter by default, because attaching one there is a question about CONTEXT rather than authority — `PublicContext::Attach` is the opt-in for a page that renders differently when signed in. `route_constraints` is exposed because the choice is otherwise untestable, and one case pins `kAccessFilterName` against `AccessFilter::classTypeName()`, which is the half of the hole that was a runtime string |
| [x] | `media::resolve_role` | Searched one format and fell straight to the master. On a build whose libvips has no libheif that served a multi-megabyte PNG to a phone in place of a 90 KB WebP, with a 200 and a correct `Content-Type` and no symptom but page weight. The master is now LAST. Two findings beyond the report: falling to the master was itself a failure upward and a larger one than any variant, so the "never fails upward" rule was self-defeating and a role narrower than every written variant now takes the narrowest variant; and degradation must be **directional** — `negotiate_format` answers `Webp` precisely for clients that never claimed AVIF, so a symmetric "try the other format" hands the oldest clients an image they cannot render. Behaviour change to a published function under CLAUDE.md §9.3 |
| [x] | `bootstrap_sections` counts bindings | `images_missing` counts RESOLVER failures, and bootstrap binds an image only into a section it CREATES. Both are consequences of insert-if-absent and both are correct; together, a database bootstrapped before the defaults tree existed reports **`images_missing == 0` on every subsequent boot while every slot stays empty**. The resolver did its work, the files were registered and pinned, nothing was wrong, and no section points at any of them — and an operator reading the number this document tells them to read is told everything is fine. It must NOT be fixed by having bootstrap update an existing section: a boot path that overwrote stored content would make every deploy a content wipe, which is the whole reason insert-if-absent is the rule. So the report grows a second count taken from the STORED state — sections declared with an image slot whose `media` map is empty — which is the question an operator actually has, answered by a cheap indexed read at boot. The existing field keeps its meaning and its name rather than being redefined under a reader who already believes something about it **Closed.** `BootstrapReport::image_slots_unbound`, taken from the stored documents after the insert loop by `SectionRepository::count_unbound_image_slots` — one `$in` over the `_id` index with a `media`-only projection, so the whole registry is one round trip and one index seek per section that declares a slot. `images_missing` keeps its name and its meaning. Three decisions beyond the report. It counts SLOTS and not sections: the row said "sections whose `media` map is empty", and a section declaring three slots with one bound is two gaps on the page that such a count calls zero — the same shape of wrong answer the row is about, one case narrower. A slot counts as bound only if it holds BINARY data, because the 36-character string form of an id is something no renderer resolves and counting presence would call it bound. And a failure of the read fails the whole bootstrap rather than reporting zero: a boot that cannot see what it just wrote has not finished, and a newest number that silently reads zero is the defect it exists to close. Public header change under CLAUDE.md §9.3 — a field appended to `BootstrapReport` |
| [x] | A `tel:` URL a validator produced | The SSR allow-list is site-relative, `https:` and `mailto:`, and `append_url_attr` fails SILENTLY BY DESIGN — it emits nothing and returns `false`. A handler passing a `tel:` URL and ignoring the return value renders an anchor with no `href`: not a link, not focusable, not announced as one, and on a contact page the symptom is that a phone number stops being tappable on the device every visitor is holding. Do NOT add a scheme to the list and do NOT let an application write a second URL rule — the second implementation is the one that drifts. Apply §3's shape to a scheme instead: take a type only a validator can produce, build the value from it, and emit through `append_html_attr`, which still owns the quoting and the escape set. `input::PhoneEgy::e164` is thirteen bytes a scanner wrote and can be nothing else, so the function cannot be handed a request byte by a refactor that was not thinking about it. The Egypt-specific type is the reason this is not simply `append_tel_attr(std::string_view)`, and the general form waits for a second validator rather than being guessed at **Closed.** `http::append_tel_attr(std::string&, const input::PhoneEgy&)` in `anvil/locale_egy/phone_html.h`. The allow-list is unchanged and a case pins that it still refuses a `tel:` URL, so the decision not taken is asserted rather than remembered. Two things beyond the report. It takes NO attribute name — a `tel:` URI is an `href` and nothing else, and a name parameter would reintroduce the exact failure being fixed, a call that emits nothing while reporting success. And it RE-CHECKS the thirteen bytes at the point of emission rather than trusting the type: `PhoneEgy` is an aggregate behind an out-parameter API, so `PhoneEgy phone{}` compiles and has to, and a caller who ignored the `Reason` holds thirteen NUL bytes of a perfectly well-typed number. That is `append_sanitized` re-running the verdict at render, applied one type over. Beside it: the cases for these — and every case already written for the Egyptian validators — were compiled by no preset at all, so nothing but a hand-passed `-D` ever ran one; `ANVIL_WITH_EGY` is now ON in `asan` and `dist` |
| [x] | `SectionServiceConfig` invalidation hook | A cache ABOVE this library's cache cannot be told a key changed: `invalidate_local` and the Redis subscriber are both internal to `SectionService`. Both workarounds are worse than a hook — dropping and refilling from the application's own write handler misses writes from every other instance, and subscribing to the same Redis channel separately is a second listener thread doing what one already does. A `std::function<void(std::string_view key)>` invoked wherever `invalidate_local` already is. It must be called on the SUBSCRIBER's path and not only on the local write, or it reproduces the first workaround inside the library; it must not block, because it runs on the thread draining the subscription; and it is called AFTER the local cache is dropped, so a consumer that re-reads on the callback cannot read the stale value it was just told about. Makes "the three-tier shape is reusable" true for a derived cache as well as for this one **Closed.** `SectionServiceConfig::on_invalidated`, called from `invalidate_local` — which is what puts it on the subscriber's path as well as the local write's, rather than bolted to `write()` where it would be the workaround with anvil's name on it. Called after the drop, and a case asserts the ordering by `peek()`ing from inside the callback rather than by counting calls, because a hook called first passes a counting test and hands the consumer back the stale value. Two things beyond the report. The REGISTRY's `string_view` is what goes out, not the caller's: on the subscriber's path the caller's is a view into a message buffer that dies with the callback, and the registry's is `.rodata`. And an exception is caught and logged — `invalidate_local` is `noexcept` and the subscriber thread calls it, so one consumer's bug in a callback would be `std::terminate` and every other instance's invalidations stopping with the process |
| [x] | `SectionService` typed read for SSR | `peek()` returns pre-serialised API JSON — bytes shaped for the editor endpoint, not typed values shaped for a renderer — so an SSR path built on it re-parses JSON per render, and the alternative, `read_document()`, blocks and belongs on `db_pool`. The first consumer's answer was to hold its own `SectionContent` snapshot ABOVE this library's cache, which worked and needed no change here, so this row is **a question before it is a task**: does the second consumer do the same thing? If it does, the duplication is anvil's to absorb and the shape is a typed accessor beside `peek()` sharing its three-tier cache. If it does not, this closes as "the consumer's snapshot is the right layer" and the row exists to record that it was asked. Do not build it on one data point **Closed as asked-and-answered, with the answer recorded rather than the feature built.** There is still exactly one consumer, so the data point count has not changed and the row's own instruction — do not build it on one — stands. What DID change is the standing of the consumer's snapshot. Before the row above, holding a `SectionContent` above this library's cache was a workaround with a correctness hole in it: nothing told that snapshot about a write on another instance, so it drifted exactly the way tier 1 would without the Redis channel. With `on_invalidated` it is a supported layer with an invalidation channel under it, which makes "the consumer's snapshot is the right layer" an answer rather than a concession. What would reopen this: a second consumer that writes the same typed accessor over `read_document()`. If that happens the duplication is anvil's to absorb and the shape is a typed read beside `peek()` sharing its three-tier cache |

**Phase 13 gate:** the suite green as every phase gate requires, plus one thing a test count
cannot express — each row that changed code landed with a case that FAILS against the code as
it stood before it. Every item in this phase shipped green, so a test that passes before the
fix is a test that was never going to catch it.

**Met.** `image_slots_unbound` is a number the old report could not produce, and the case that
reads it also asserts `images_missing == 0` beside it, so the two answers are pinned as
different rather than as one renamed. `append_tel_attr` is exercised beside a case asserting
that `append_url_attr` still refuses the same URL. The hook's ordering case `peek()`s from
inside the callback, which a hook called before the drop fails and a call-counting case does
not. And one thing the gate did not ask for and got: the Egyptian validators' cases were
compiled by no preset at all, so nothing but a hand-passed `-D` ever ran one.

---

## Phase 14 — shedding, tracing, and a second transport

Three designs written in full before any of them was built, and kept out of the numbered docs
until they were. All three have shipped, so `docs/proposals/` is empty and gone: the storm
breaker is [`11-notifications.md`](11-notifications.md) §12, the trace context is
[`17-analytics.md`](17-analytics.md) §18, and the upgrade contract is
[`04-access-control.md`](04-access-control.md) §8 — in every case the section that used to
record the gap now records the mechanism. A reader who finds the refusal finds the design in
its place rather than beside it, which is the difference this gate was about.

They are unrelated to one another and share no ordering. What they share is that each was
previously refused or deferred for a reason that is still half right, and the design's job in
every case was to take the half that is cheap and leave the half that is not.

| | Task | Notes |
|---|---|---|
| [x] | `notifications/shedding.h` | Shedding is not absent — `try_post` refuses a full queue — it is UNPRIORITISED, so a password reset published during a like-storm is refused exactly as readily as a like. Two gates, because a publish is two things with different loss semantics: admission, before the inbox row is written, DROPS and is legal only for a `user_optional` topic; dispatch deferral, after the row is committed, only DELAYS, because it leaves `dispatched_at` unset — byte-for-byte the state a crashed process leaves — and `sweep_outbox()` already finishes exactly that. The property to assert over the whole topic table: a topic the reader may not silence is one the breaker may not drop. `user_optional` is the discriminator and no new field, because `topic_spec.h` already argues the two must be the same flag and a second bit would let them drift. Watermarks are a FRACTION of queue capacity, which is why a default is defensible where docs/11 §12 refused to guess an absolute rate. Ships with its counters and an outbox-depth GAUGE or it does not ship: gate 2 converts queue pressure into backlog, which is the failure mode the design introduces and is invisible without it **Closed.** `shedding.h` is `ShedPolicy`, `ShedVerdict` and a pure `shed_verdict`; both gates live in the non-transactional `publish()` and share ONE pressure sample, because two samples a few microseconds apart could disagree and a publish admitted under one number and deferred under another is a decision nobody made. The table property is a loop over `kTopics` at nine pressures, plus a case asserting the table HAS an unsilenceable topic — without which the loop passes by testing nothing. Three decisions beyond the design. The metric is `anvil_notifications_shed_total` labelled by OUTCOME and not by topic: the design asked for `{topic}` and the metric seam cannot close that label space, because a topic table arrives as a span rather than from the config header, which is the identical refusal the rate limiter's `{bucket}` already got. The transactional overload is NOT shed, because admission control that runs after the caller already holds a transaction and a pooled client has nothing left to save. And a NaN pressure or a probe that throws both Proceed — the safe answer to a number nobody can read is never to throw a notification away, and it is not logged, because the storm that makes a probe misbehave is exactly the traffic under which a line per publish is itself the outage. ABI change under CLAUDE.md §9.3: a field on `PublishHooks` and a defaulted constructor parameter |
| [x] | `http/trace_context.h` | docs/17 §16 refused a tracing model, a propagation format and a wire protocol; only one of the three is expensive. The line is that **anvil owns context and the application owns export**. `traceparent` only — fixed at 55 bytes, parsed with a length compare and a hex table, the same shape as `auth::decode`. `tracestate` is dropped on the floor and not forwarded: it is the only variable-length attacker-controlled part of the standard and nothing here would read a byte of it. Parsed into 24 bytes of binary and never carried as the string, which is what leaves no injection surface downstream. **Nothing is ever written back on a response** — `stealth.h` forbids `X-Request-Id` for exactly the reason an echoed `traceparent` would be an existence oracle, and the rule is absolute rather than conditional because "echo it except on stealth routes" survives until the first handler that sets it directly. A job LINKS to the enqueuing trace and mints a new root; every queue here is at-least-once and a span covering the publish and Thursday's retry is not a trace. Off by default, because the second request attribute is the real cost — and it must NOT be folded into `UserContext`, whose 64 bytes and one cache line are asserted | **Closed.** Three commits, and the design it landed as is [`17-analytics.md`](17-analytics.md) §18. Two corrections to the design. The first: the proposal argued at length that `tracestate` is the attacker-controlled part of the standard and then read `traceparent` without asking who sent it — both are headers, and on a request that reaches the process directly `traceparent` is whatever the client typed. A chosen id collides with a real trace, a chosen `sampled` bit forces export, and a fresh id per request is unbounded cardinality; none of those is a hole in THIS process, which is why the question is easy to skip, and every one of them is a hole in the thing the traces are for. So ingest is the rule `client_address.h` already applies to `X-Forwarded-For` — believed only from a peer in the trusted-proxy list — with no third policy for "believe any client", because a deployment whose callers are its own services has them behind that list already. The second: "off by default, because the SECOND request attribute is the real cost" was written before `RequestScope` consolidated the attribute map down to one entry. There is no second attribute to pay for now; the 24 bytes are in an allocation that already happens, 88 to 112, and the default stays `Off` on the parse cost and the trust question rather than on an allocation that no longer exists. The job link forced a durable wire format change, so the envelope has two versions and version 2 is written ONLY when there is a link — an untraced deployment emits the same 32 bytes it always did, because an older worker ACKs and DROPS an envelope whose version it does not know, and a lost job is not a retried one. Two cases were written twice: the `guarded()` restore cases passed against a build with the restore deleted, because `guarded()` installs unconditionally and the observer overwrote the stale value before reading it — they now read the worker through an unguarded post, which is the only spelling that can fail. Not done: the trace id on every log line, which the proposal put in a redacting logger this repository does not have |
| [x] | `OriginRequirement::Always` | **Worth landing whether or not the transport is ever built.** `is_state_changing` returns false for `GET`, a WebSocket handshake IS a `GET`, so `check_origin` answers `NotRequired` for every upgrade — and same-origin policy does not constrain WebSockets while the browser attaches the cookie anyway, so on an upgrade the origin check is not defence in depth, it is the only CSRF defence there is. It fails OPEN and says nothing: the verdict is a pass, not an error, so there is no log line, no metric and no failing test. Same failure shape as the `getHeader("cookie")` bug — a control that is present, called, and answering the wrong thing. A defaulted parameter, so no call site and no consumer's build changes. Not exploitable today because no WebSocket route exists; it arms itself silently on the first one | **Closed**, exactly as designed and with nothing added: a `ByMethod`/`Always` enum, a defaulted parameter on `check_origin`, and `is_state_changing` untouched so it stays a statement about methods rather than one about transports. Two cases, and the second is the one that is easy to skip — `Always` narrows the requirement and never widens it, and the default is still what every existing call site had. A default that had silently become `Always` would turn every GET in every consuming application into a rejection, which is the failure mode of fixing this the other way round. The paragraph in docs/04 §7 that recorded the gap now records the control. It landed on its own, ahead of the transport in the row below, because the hole it closes is in shipped code and the cost of closing it was a defaulted enum — and because the wrong answer is a pass, so it would have armed itself silently on the first upgrade route somebody added
| [x] | A WebSocket upgrade contract | Everything else here authorizes a REQUEST. A socket authorizes once and lives for hours, which makes the epoch's 10 s TTL meaningless for an open connection and lets an access token's expiry pass underneath one. So the connection re-runs the filter's own check on the cache's own period — faster cannot produce a different answer — and the token's expiry bounds the connection, which is SSE's "a dropped client reconnects" applied unchanged. In-band re-authentication is rejected: parsing a credential out of a socket frame is a second authentication path with a second set of bugs, to avoid a reconnect the client already implements. Frames are not requests and the rate-limit table never sees them, so a per-connection frame budget closes rather than answering `429`. Reuse `sse.h`'s ring and its drop-the-connection policy rather than inventing a second one. **Verify before building on it:** the filter keys on `getMatchedPathPattern()`, and whether Drogon's WebSocket router populates it is not something the constraint signature promises — an empty pattern sends every such route down the `policy == nullptr` branch, which is closed, silent, and indistinguishable from the feature not working | **Closed**, and the design it landed as is [`04-access-control.md`](04-access-control.md) §8. The verification the row demanded came back in the design's favour and is now a case rather than a belief: a real handshake on a raw socket asserts that the filter chain runs for an upgrade and that `getMatchedPathPattern()` carries the route. It had to be a raw socket — `drogon::HttpClient` sets `Connection` itself, so `Connection: Upgrade` never reaches the wire, `isWebSocket` answers false, and the case returns the framework 404 while appearing to have tested a handshake. Two things the design did not ask about came with it. The pattern is the LOWERCASED path, which is specific to WebSocket routes and which `register_websocket_route` now refuses rather than letting a route table be where it is discovered; and the filter is named by its TYPE, because `DrObject<T>::alloc_` is instantiated only when something ODR-uses it — a string literal leaves the filter's object file out of the link, and Drogon's answer to a middleware it cannot find is a log line and a chain that runs without it. **One real leak, found by asserting bytes rather than headers.** A refused stealth upgrade came back as the byte-identical 404 followed by `88 02 03 e8`, a WebSocket close frame that `WebSocketConnectionImpl`'s destructor writes onto a still-connected socket — the existence oracle stealth exists to close, arriving after the response where no assertion about headers or bodies could see it. Refusing with a fresh response that marks the connection closing removes the deterministic half; the residual is a race on both paths, measured at 0/2/1 against 12/5/3 over forty handshakes, and anvil cannot close it because the connection object is constructed before any filter runs. Stated with the numbers in docs/04 §8.3 rather than left for somebody to find. **Three departures from the design.** `still_authorized` returns a verdict rather than a `bool`, because the third answer is "the authority is not cached" and the design's own batched resolve cannot be asked for through a `bool`; it drops the `token_epoch` parameter, because `ctx.perm_epoch` IS it; and it is NOT WebSocket-specific, because `sse.h` already holds connections open across the same gap. **And one refusal**: no connection registry and no outbound ring. SSE's ring exists because `publish()` is a producer inside this library that must deliver into it; a WebSocket carries the application's own messages and anvil has no producer, so a ring here is a container with no writer in this library, permanently in the ABI. The descriptor budget IS anvil's, because it is one process-wide number two subsystems would otherwise each spend in full, and it moved out of `sse.h` into `core/descriptor_budget.h` where the shares sum under a `static_assert`

| [x] | The four bytes after a refused upgrade | The one thing the row above could not close, recorded there as a residual anvil had no point of control over: `~WebSocketConnectionImpl` writes `88 02 03 e8` onto a still-connected socket, every filter refusal happens after that object exists, and the frame is an existence oracle arriving after the response. **The premise was wrong by one step.** `HttpServer::onRequests` calls `passSyncAdvices` BEFORE `make_shared<WebSocketConnectionImpl>`, and an advice that returns a response short-circuits the branch — the object is never built, so there is no destructor and no frame. `accesscontrol/upgrade_gate.h` takes exactly the refusals that have to be indistinguishable: a path no `register_websocket_route` registered, and every synchronous refusal on a `Stealth` route. Everything else keeps its filter, its real code and its request id, because an advice runs before an id has been minted and a `401` answered from there would lose the value that joins a complaint to the row explaining it. It is not a second decision — same `evaluate_token`, same table, answering only on `Step::Deny`, which is the branch the filter would take a moment later. **And the measurement that closed the row above was wrong in a way worth keeping.** 0/2/1 against 12/5/3 was one behaviour read two ways: `raw_handshake` read the socket once and a single `recv` sees the frame only when TCP coalesced it into the same segment. Reading until the peer goes quiet says the frame follows EVERY filter refusal and NONE the gate makes, and that `setCloseConnection(true)` — added to suppress it — never suppressed anything; the call is gone and the test now asserts the whole byte stream, exactly, which the previous note called the flaky kind. **A second leak, larger than the frame and in the response itself.** Reading the whole stream showed the refusal carrying a `Connection: close` the unmatched answer did not — Drogon sets that flag on the per-IO-thread COPY of the shared 404, so whether an unmatched upgrade carried it depended on what that thread had served before. Two refusals whose headers differ are separable by reading the response, with no timing and no trailing bytes involved. It was invisible because `gtest_discover_tests` gives every case its own process, so the pair never shared a listener; run as one process, the mainline fails. Two more things fell out: an upgrade to an ordinary route is now the unmatched answer rather than Drogon's router's, which also makes unreachable a branch that mutates the per-IO-thread copy of the shared 404 and gave every later 404 from that thread a `Connection: close` no other thread's carried; and a WebSocket controller registered outside this library is unreachable rather than unguarded |

**Phase 14 gate:** each row lands with the case that would have caught what it fixes, and the
proposal document it came from moves out of `docs/proposals/` into the numbered doc that
refused it — a design that shipped and a design that is still a proposal must not look the same
to the next reader.

**Five of five.** The storm breaker and the trace context landed;
`proposals/storm-breaker.md` and `proposals/trace-context.md` are gone, their designs now being
[`11-notifications.md`](11-notifications.md) §12 and [`17-analytics.md`](17-analytics.md) §18 —
in both cases the section that used to refuse the thing. `OriginRequirement::Always` landed with
no route to use it and docs/04 §7 records the control where it recorded the gap; the upgrade
contract landed on top of it as docs/04 §8, and the residual that contract recorded is closed by
the row after it — the fifth, which is not a design from `docs/proposals/` but the thing the
fourth one learned it could not do and turned out to be able to.

`docs/proposals/` is empty and removed, which is the state this gate was written to produce.

---

## Phase 15 — sections that repeat

The first consumer's portfolio was three case studies, and each was two sections, a page
enumerator and an analytics value, all fixed at compile time. Adding a project was a deploy.
The request was general: one mechanism for a portfolio, a feed and a forum, configured by the
application, with the shape still `constexpr`.

| | Task | Notes |
|---|---|---|
| [x] | `verify_images` as a free function | Mechanical. A second store of section-shaped content has to check images with the same code, not a copy |
| [x] | `append_shape_json` | Mechanical. The per-section element `serialize_registry` writes, public, so an entry kind's shape reaches an editor as the same bytes |
| [x] | `anvil/entries` | Kind table, service, repository, binders, serialisers, seeding. **One document per entry**, unlike sections, because placement (slug, flags, position, child count) belongs to neither copy; the draft is kept from readers by one projection, a decoder that ignores it, and a test over the serialised bytes. `sc` folds kind and parent so one four-key index answers every listing with no blocking sort, which the explain check asserts. A child's capacity is exact, a conditional increment of the parent; a root kind's is a count and is documented as able to overshoot under a race. Flags are one `$bit` update and unversioned, so a pin does not stale an open editor. Seeding is **claimed once per kind**, because insert-if-absent would restore every seed staff deleted; the first run of the test found that a claim lost to a duplicate key must unwind the transaction, or the driver retries the commit of an aborted transaction for two minutes. ABI: new public headers only, plus the two above |

Open, each waiting for an application that needs it: a one-locale public JSON serialiser, listing
by author (with its index), and seeding a child kind.

---

## Phase 16 — an entity analytics dimension

Phase 15 gave an application runtime entries — a project no longer needs a deploy to exist. The
analytics dimension for "which project" was still a `constexpr` list of slugs, so a project added
at runtime went uncounted until the next release appended it, and a renamed slug would relabel
its own history. The request: a dimension kind whose value is an id rather than a name from a
table, admitted one at a time rather than bounded by one.

| | Task | Notes |
|---|---|---|
| [x] | `analytics/event_spec` — `DimensionKind`, `entity_dimension_of` | **Seam.** `Enum` is the closed set every dimension has always been; `Entity` carries an application-minted UUID and an empty `values` — the two disagreeing is refused by `event_table_is_well_formed` alongside every other malformed table. At most one `Entity` dimension per event, because the value has one storage slot, not an array |
| [x] | `analytics/event` — `Event::entity`, `RollupRow::entity` | `Uuid`, `kNilUuid` for "none", stored as BSON `BinData` subtype 4 and OMITTED from the wire form when nil — so a row that never uses one, and every row written before this field existed, costs and reads exactly as before. `Event` 44→60 bytes, `EventRow` 56→72; `EventFoldKey` gained the id so two different entities never fold into one repeat count |
| [x] | `analytics/ingest` — `EntityAdmission`, `Outcome::RefusedEntity` | A new gate, second of five: consent, then entity admission, then visitor, sampling, buffer. Bounded cardinality moves from a `constexpr` value space (impossible here, since the space is not fixed at compile time) to an application hook consulted at `offer()` — in-memory only, since `offer()` runs on a Trantor event-loop thread and must not block. **Unset refuses every id** — deny by default, because a runtime hook being missing is not a fact `static_assert` can see |
| [x] | `analytics/repository`, `analytics/rollup` | The rollup's identity tuple gained the entity id, grouped exactly as an enum combination; a rollup already on disk before this feature keeps the exact `_id` shape it always had, so a re-run still finds and `$set`s it. `AggregateKey` and its hash gained the id |
| [x] | `analytics/query` — `counts_over_time_for_entity`, `counts_by_entity` | The narrowed/grouped pair every enum dimension has an equivalent question for, except `counts_by_entity` is new in KIND — "top projects" has no enum-dimension analogue, because a small enum space needs no discovery query. Returned highest-count-first, ties broken by id, so two reads of an unchanged window agree byte for byte |
| [x] | `descriptor/emit` | Every dimension now carries `"kind"` (`"enum"` or `"entity"`); an entity dimension's `"values"` stays present and empty rather than omitted, so a client need not infer the kind from the field's absence |
| [x] | `tests/testapp/events.h` — `ProjectViewed` | The reference application's worked example, alongside the enum-dimension ones it has always had |

ABI: `Offer`, `Event`, `EventRow`, `EventFoldKey`, `RollupRow`, `RollupQuery`, `DimensionSpec` all
grew a field, and `IngestConfig` grew `entity_admission`. Every growth is APPENDED after the
existing members — most with a default member initialiser — so a positional aggregate-init
written against the old member count keeps compiling; the new state resolves to "none" or "deny"
without the caller doing anything, which is what let every existing test in phases 8 and 9 pass
unmodified against the wider structs. Called out per CLAUDE.md §9.3 regardless, because the
struct shapes are still a public-header change.

What stayed exactly as it was: an enum dimension's stored index, its `values` table, its rollup
grouping, and every existing query. The regression proof is `tests/rollup_db_test.cc` and
`tests/analytics_db_test.cc` passing unchanged — not a copy, the same files, because a dimension
kind an event does not declare must cost that event nothing.

---

## Phase 17 — edit an image

The request: crop, resize, rotate and draw on an image that is already stored. The design is
[`21-image-edits.md`](21-image-edits.md), and the client half is hammer's
`docs/04-image-edits.md` §Phase 9. **An edit is a canonical recipe, rendered once into a new
object that holds a reference on its source.** The master is never written and an edit is
never chained.

The phase opens with three defects rather than a feature. Reading `images::apply_crop`
against `07-filesystem.md` found that it publishes cropped variants under the source's id,
where namespace deduplication shares them with every other binding of those bytes. It also
mixes old and new variants while it runs, and on a mid-set failure it unlinks files the row
still lists (21 §1). It has no caller outside `images_test.cc`, so nothing in production is
broken today, and the design replaces it rather than patching it.

| | Task | Notes |
|---|---|---|
| [x] | Pin the `apply_crop` defects before removing it | **Closed** as properties of the replacement rather than as failing cases of the removed function: `AnEditWritesOnlyUnderItsOwnIdAndLeavesTheSourceByteIdentical` stores a source with its variants, edits it twice, and asserts every byte of the source's master and variants is unchanged. A red commit for a function about to be deleted would have broken bisect for nothing |
| [x] | The `{ns, sha}` index — unique or not? | **Answered: not unique** (`media_ns_hash`, `unique = false`). §3.3 is a filter change, `find_by_hash` gains `src` not existing, and there is no migration. `media_dedup` in the query catalogue carries the same predicate |
| [x] | `images/recipe` — decode, canonical re-encode, validate | **Closed.** In the foundation, so a build without libvips still validates a recipe. The thirty golden vectors came from a third implementation written from the document and are `tests/testapp/edit_vectors.h`; `testapp_emit_edit_vectors` prints them as hammer's fixture, and hammer's codec passed all thirty on its first run. The fault reaches the wire through the existing `fields` map (`field_error`) rather than as a second vocabulary. The fuzz target is its own row, below |
| [x] | `images/stroke_raster` | **Closed.** One stroke's mask at a time, each row visiting only the band round the segment's line, a deadline every 4096 rows. Cases pin a round dot, overlap painted once, a hairline drawn as a pixel, sRGB source-over with straight alpha, and a mask no larger than the clipped box. Stroke width is capped at an eighth of the short edge, which is what bounds a stroke's work; the cap is in the recipe and was not in the first design |
| [x] | `images/render_edit` | **Closed.** Found by its own test: the deadline is armed on the loaded image, and drawing replaces the pipeline with an image over plain memory that references nothing upstream — so the loaded image was freed mid-function and the deadline's destructor disconnected a handler from it. The loaded image now holds its own reference. `save_master` moved into the private `detail.h` so a derived master is encoded exactly as an upload's |
| [x] | `media/record`, `media/repository` | **Closed.** `insert_edit` inserts and `$inc`s the source in the caller's transaction; `delete_if_unreferenced` and `claim_unreferenced` release an edit's source in the same transaction as the claim. A claimed row that does not decode still commits, as before, so a corrupt row is not handed to the sweeper on every pass forever |
| [x] | `media/service` — the stages | **Closed**, as `prepare_edit`, `media::render` and `record_edit`. Ten database cases, including two attempts that both rendered and raced the unique index: the loser answers with the winner, its files are gone, and its reference never committed |
| [x] | Remove `apply_crop`, `CropRect`, `crop.h` | **Closed.** Removal from the published surface under CLAUDE.md §9.3, stated in the commit that made it. `08-images.md` §5 is a pointer to 21 |
| [x] | `descriptor/emit` — `limits.edit`; `01-seams.md` | **Closed**, and the seam is §17 rather than §14, because it is two routes, a budget and an index as well as the descriptor block |
| [x] | `tests/testapp` — the routes | **Closed**, under `/media-edits/{ns}/{id}` rather than `/media/{ns}/{id}/edits`: every GET under the latter is a role on the public object route. The state route replaced widening the metadata route, which in the reference application is a list route. The reference server opens storage of its own per run, seeds one generated picture through the upload stages, prints it as `media <ns> <id>`, removes the storage on exit, and `check-reference-server.sh` edits it |
| [x] | The sweeper and the purge see derived rows | **Closed.** `TheSweeperCollectsAnEditAndThenItsSource`: the first pass collects the edit and releases the source, and the second collects the source. `DeletingAnEditReleasesItsSource` for the purge |
| [x] | A fuzz target over `decode_recipe` | **Closed.** `tests/recipe_fuzz_test.cc`, in `anvil_alloc_tests`: 200 000 inputs of shaped noise and 200 000 mutations of the thirty vectors, each accepted recipe checked to re-encode to its own bytes and to plan inside its frame against five masters. Removing the tail rule and the canonical comparison together fails both cases at once. A stroke claiming the full point bound with no points behind it costs one allocation (the stroke record), and a stroke count over the bound costs none. Clean under `asan` |
| [x] | An edit reaches the application's audit log | **Closed.** Found by the first consumer: cubit audits every upload from its own handler, and an edit, whose handler is anvil's, wrote no row at all. `EditRoutes::on_edit` is called once per answered edit, after the response, on whichever thread answered. Every answer goes through one `Reply`, so no stage can answer without reporting or report twice. The reference server prints each report, and `check-reference-server.sh` asserts the six rows its run produces, in order: two refused origins, the created edit, the same edit found again, a chained edit refused, and a missing source |
| [x] | The account routes check `Origin` | **Closed.** Found beside the edit route's own check: no account route checked where a request came from, so any page could sign a visitor in to the page's own account (login CSRF). Every role now checks first, before the body is read. `check-reference-server.sh` sends the editor's correct credential from another origin and from none, and expects `403` with no cookie set; with the check removed, the forged sign-in answered `200 {"signed_in":true}` |
| [x] | The preview matches the render, measured | **Closed.** `testapp_emit_edit_renders` renders six recipes over a generated 640×480 gradient — red runs with x, green with y, so a wrong turn or flip is a whole-picture difference — and writes each derived master as a PNG with a manifest; hammer commits them as `tests/edit/parity/` and screenshots its real preview at each output size (hammer `tests/edit/parity.test.ts`). Turn, flip and crop agree to the byte; strokes to a mean of 0.02 levels; the resize to 0.19; everything at once to 0.28, its outliers along a stroke's edge. A control with the flip left out scores 42. The one convention that differs is the outermost pixel of a resized picture: the browser filters the edge against transparency and libvips extends it, so the comparison insets one pixel |

ABI and API: `MediaRecord` and `NewMedia` grow three optional fields, appended; `crop.h` is
removed. Both are called out under CLAUDE.md §9.3. Nothing on the serving path changes: a
derived object is an object, and `resolve_role` never learns the difference.

---

## Phase 18 — conversations

The request: a general-purpose chat subsystem (direct, group and channel conversations, media,
receipts, live delivery, opt-in end-to-end encryption), configured by the application, with
WhatsApp as the reference. The design is [`22-chat.md`](22-chat.md). It is three phases
because each is usable without the next: this one is a complete plaintext messenger that
clients poll, 19 makes it live, and 20 encrypts it.

**A conversation is an ordered log with one writer of order, the server's sequence number,**
and receipts, unread counts, sync, visibility and expiry are all comparisons against it.

The phase opens with changes to `fs`/`media`, not to chat. Storage is already never public:
every byte leaves through a handler that chose to issue `X-Accel-Redirect`. What a private
namespace adds is a handler on the media origin, where no session arrives, that still has to
know whether to agree (22 §6).

| | Task | Notes |
|---|---|---|
| [x] | `crypto/siv` — deterministic AEAD | AES-256-SIV through OpenSSL's `EVP_aes_256_siv`, in the foundation, with RFC 5297's vectors. Deterministic so a grant for the same object in the same bucket is the same URL and the browser cache works; a random-IV AEAD would make every mint a new URL and every scroll a re-download. Equality is the only thing it reveals, and equality is what a cache needs to see **Closed.** Wycheproof's 512-bit-key AES-SIV vectors rather than RFC 5297's, which are AES-128 only. One associated-data component, always, even when empty, because RFC 5297 treats an empty component and no component as different inputs. Found beside it: OpenSSL's SIV provider reads a null input pointer as "finalise" and a null output pointer as "associated data", and an empty span may carry a null `data()`, so every update is handed a non-null pointer |
| [x] | Media grants, and `NamespaceSpec::visibility` | `Public` or `Private`, default `Public`, so every existing table keeps its behaviour. A grant is `ns ‖ id ‖ expiry` sealed under SIV, with expiry bucketed to the next ten-minute boundary, minted on the site origin after the caller's access was checked and opened on the media origin with no session. For a `Private` namespace `accel_redirect_response` takes a `MediaGrant`, which only `open_grant` produces, so an application's own handler cannot serve a private object by accident. The handler refuses a row with no references left, which makes delete-for-everyone immediate. A cookie on the media origin is refused, not overlooked: an `<img>` on the site is a cross-site subresource request and gets no `SameSite=Lax` cookie. ABI change under CLAUDE.md §9.3 **Closed**, minus the route, which lands with the chat media row because the first route that needs it is chat's. The grant carries a cleartext key id ahead of the sealed body, the shape `auth::TokenKeys` uses, so rotation picks a key instead of trying both, and the purpose and key id are the associated data, so the byte that chose the key is authenticated too. `kGrantChars` is exact (51) and refused on length before anything decodes. The id-only overload of `accel_redirect_response` answers a `Private` namespace with the stealth 404 at run time, because the namespace is a run-time value. The overload that takes a `MediaGrant` is the only way to serve one, and only `open_grant` makes a `MediaGrant`. The descriptor does not mark private namespaces yet; that lands with `limits.chat`. The `chat` and `sealed` namespaces are `Private` |
| [x] | Upload handles | An upload answers with a sealed `ns ‖ id ‖ uploader ‖ expiry`, valid for an hour, and **nothing else**: no id, hash, size, owner or created-vs-found flag (22 §6.2). Only a send from the same account can redeem it. Without it, an object id learned anywhere could be attached to a conversation with yourself and turned into a grant. A case spends another account's handle and is refused **Closed**, as the primitive: `mint_upload_handle` and `open_upload_handle`, beside the grant and sealed the same way under a separate purpose, so neither ever opens as the other. The 72-character handle is everything the upload route will answer; the route itself is chat's (`install_chat_routes`). A handle opened by an account other than the uploader fails exactly as a forgery does, because the comparison happens inside the authenticated body |
| [x] | `NamespaceSpec::dedupe` — `Namespace`, `Owner`, `None` | Needed **even though** the response now says nothing about a hit: a hit skips probe, normalise and transcode, so the clock answers in milliseconds where a new file takes seconds, and that is a confirmation-of-file oracle that no response shape can hide. Padding to a constant time is refused: it holds a request open for nothing, and under load it is never actually constant. `find_by_hash` gains the owner for `Owner` and is skipped for `None`; the `{ns, sha}` index gains the owner in the same commit (CLAUDE.md §7: a query and its index together). Default `Namespace`; a case pins that two owners uploading the same bytes into an `Owner` namespace get two objects **Closed.** The scope is applied inside `MediaRepository::find_by_hash`, which now takes the uploader, so a caller cannot forget to narrow; `None` answers without a round trip. **The index was NOT widened**, which departs from this row: in an `Owner` namespace the rows sharing a hash are one per owner who uploaded those bytes, so the owner is a cheap residual predicate on `{ns, sha}`, and changing a declared index is a migration every consuming application would have to run for nothing. The reference application gains the `chat` (`Owner`) and `sealed` (`None`) namespaces now, because the cases need both and phases 18 and 20 use them. API change under CLAUDE.md §9.3: `find_duplicate` and `find_by_hash` take the owner |
| [x] | Widen `MimeMask` to `u16` | Mechanical, on its own (CLAUDE.md §9.1), ahead of the row that needs it. The existing `static_assert` says a ninth `Mime` needs a wider mask and not a wrap, and the `File` class brings five. The descriptor emits the mask as a number, so its width is not on the wire. ABI change **Closed**, alone and with no behaviour change. `NamespaceSpec` stays 24 bytes: the second byte of the mask lands in padding that was already there |
| [x] | The `File` object class | MP4, WebM, Ogg/Opus, M4A, PDF: sniffed against the closed list, **never decoded or transcoded**, no variants. Audio and video are served `inline` (nginx answers `Range` itself behind `X-Accel-Redirect`), and everything else `attachment`, PDF included, because a PDF viewer runs script. Transcoding is refused rather than deferred: an ffmpeg parser for every container on the upload path is a far larger surface than the four image decoders 07 §5 weighs. Each signature is added with a fuzzed sniff case, and an SVG-in-PDF-clothing case **Closed.** `fs::mime_class` splits the closed list into Image and File; `media::store_file` publishes a file as sent and refuses an image, and `media::process` refuses a file, so neither stage can be handed the other's input. Two things beyond the row. `kDecodableMimes` was derived from the WHOLE enum, so adding five values would have widened every namespace that states no list; the default is now the image class and a namespace names `kFileMimes` to take files. And a file response carries `default-src 'none'; media-src 'self'; sandbox` as well as a disposition, so a document opened from the URL has no script and no origin whatever the bytes are. QuickTime, HEIC, Matroska and Ogg Vorbis are refused by name in the cases, and a PDF header is accepted only at offset zero. 07 §5 records the classes |
| [x] | `chat/kind_spec.h` — the seam | `ConversationKindSpec`, `Right`, `RoleRights`, `kinds_are_well_formed` with every refusal in 22 §2.3 as its own `static_assert` case, layout asserted by `sizeof`. `tests/testapp/chat_kinds.h` declares direct, group, announcement group and channel, the WhatsApp-shaped defaults as an example. `01-seams.md` §18 in the same commit, since a seam lands with its entry **Closed**, with 01 §18 in the same commit. Three departures from 22 §2, which now shows the struct as built. Timers are a span of seconds the application declares, not a mask over a ladder anvil would ship: a ladder of durations is a product's data. A kind carries its own `max_text_code_points` (at most 4 096) and `mentions_break_mute`, which the design mentioned only in passing. And both namespaces are `std::optional<fs::Ns>`, because `fs::Ns` deliberately has no default and a kind may take no attachments at all. The table is indexed by its stored code, so `kind_from_stored` is one comparison. The storage checks go further than 22 §2.3 asked: a `media_ns` that is public or deduplicates namespace-wide is refused at compile time, which makes the two media rows above impossible to forget in a table. The reference application gains `ChatCreateGroup` and `ChatCreateChannel` (bits 32 and 33) |
| [x] | `chat/text` — message validation | In the foundation, beside the form validators: strict UTF-8, a code-point bound, C0/C1 refused except `\n` `\t`, bidi overrides and isolates refused rather than stripped (11 §11), mention spans as `{offset, length, user}` in code points and inside the text, link-preview fields with the URL through the HTML writer's scheme rule. A fuzz target, as the recipe decoder had **Closed**, in `anvil::foundation`. Four judgment calls beyond the row, each recorded in 22 §4.3. Isolates are ALLOWED, because `TextClass::Prose` already allows them for mixed-script prose and a second policy would drift from it. `\n` is the only line break. NFC is REQUIRED rather than applied, because mention offsets are code points into the client's text and normalising shifts them. And blank means White_Space or Default_Ignorable, so zero-width filler is not content. The reaction check is one ICU grapheme cluster behind a 32-byte bound, with one break iterator per thread; the fuzz's ICU allocation counter caught a fresh `UText` costing 480 bytes per call, so it is reused, and every path then allocates nothing. One change outside chat came with it: `has_dangerous_scheme` folded the whole URL into a `std::string` inside a `noexcept` predicate, so an allocation failure was `std::terminate`. It now skips control bytes in place, with the accepted and refused sets unchanged |
| [x] | `chat/repository` — rows, codecs, indexes | The six collections of 22 §3.1, names from the application (01 §4). Every index in 22 §9.1 declared by the testapp and asserted in the query catalogue, `COLLSCAN`-free against an empty collection as well as a populated one (11 §5's reason). UUIDs as subtype 4, and `cid` as 16 raw bytes **Closed**, as one commit because an index and its query land together (CLAUDE.md §7): `chat/record.h` publishes the field names and rows, `ChatRepository` is every query and nothing else, and the reference application declares six collections, twelve indexes and eighteen explained queries. Three things beyond the row. `{c, att}` was not built, because with grants nothing asks it (22 §9.1 now says so). Every optional part of a message is OMITTED rather than written empty, because a message is the highest-volume row in the module. And the first run of the suite found the transaction trap entries had already met: a write refused inside a transaction has aborted it on the server, and a callback that returns normally makes the driver retry committing an aborted transaction for two minutes. So a failure inside a chat transaction must THROW out of the callback; the service follows that convention and the cases' helper does too |
| [x] | Conversations and membership | Direct find-or-create by the `dpk` upsert, with the encryption bit in the key; group and channel creation behind `create_requires`; add, remove, leave, role change and rename, each one a **system message** in the same transaction as the member row and the `mv` bump; succession of the last owner by `js`. Cases: two concurrent opens make one direct conversation; a membership transaction racing a send retries and the send never waits; a non-member refusal is byte-identical to an unknown conversation **Closed**, as `ChatService` (`create`, `open_direct`, `state`, `update_info`, `members`, `add_members`, `remove_member`, `set_role`, `follow`), with the transactional rule stated in the header: every membership transaction `$inc`s the conversation's seq and `mv`, so two of them on one conversation conflict and serialise, and the actor's rights and the member count are read INSIDE the transaction and are exact. Invite links are their own row. Four decisions beyond the row. A direct conversation's two rows are made by `ensure_member` on every open, idempotently, so the opener who lost the upsert race, or one whose first attempt died halfway, finishes the conversation; there is no step only the creator can do. `ls` is the seq of the message announcing the departure, so the leaver never sees it. Nobody removes somebody of a higher role, and only an owner hands ownership on. A channel follow writes no system message and counts no followers: counting a six-figure membership per follow costs more than the bound protects. And the case suite found a block oracle in its own first draft: checking reachability before the caller's membership answered a stranger NotAllowed where somebody had blocked them and NotFound where nobody had, so membership is checked first; a case pins it. `ChatHooks::may_reach` refuses everyone when unset. Validation failures carry their reason in the new `Failure::detail` |
| [x] | Sending and history | The four steps of 22 §4.1, with the seq `$inc` **outside** the transaction and the gap it can leave stated in the contract. `{c, u, cid}` makes a retry a read. The visible range `js ≤ s < ls` is the only visibility rule, and history paging is by seq cursor and capped. Cases: a burnt seq (crash simulated between steps) leaves a gap that sync crosses; a retry after a lost response returns the first seq; a removed member reads up to `ls` and not past it; a non-member never burns a seq **Closed**, as `send`, `history` and `catch_up`, with each case the row asked for plus four. A non-member's refusal is asserted to leave the conversation's seq untouched. Four sends racing one client id are all answered with the one stored message: the unique index lets one in, and each loser's seq is burnt and its answer is the winner's row. Plaintext into an encrypted conversation is refused, because storing it would put on the server what the conversation exists to keep off it. And a message is stamped with the earlier of its conversation's timer and its kind's retention AT SEND, so changing a timer later does not reach back into what was already said. Attachments, replies and forwards arrive with their own rows. The activity bump goes through `ChatHooks::claim_activity_bump`; unset, every send bumps, and a failed bump is logged rather than failing a send that was stored |
| [x] | Edits, revocation, replies | Windows from the kind; `RevokeAny` for moderators; a revoked row keeps its seq with the content and attachments gone in one transaction, references released; edits are not chained. Replies must target a seq inside the sender's own range, or a reply becomes a way to quote a message from before you joined **Closed**: `edit`, `revoke`, and `SendMessage::reply_to`. The service now takes the `MediaService`, because a revoke releases each attachment's reference in its own transaction, with the reactions on that message. Two decisions beyond the row. A moderator's `RevokeAny` reaches only messages inside the moderator's OWN visible range: an admin who joined later still did not see what came before, and a right is not a window into history. And a release that finds the object row already gone does not abort the revoke, because there is no count left to move; any other failure aborts, because a count committed apart from the message it describes is a leak. Edits answer NotFound for "not yours", "too old" and "gone" alike |
| [x] | Application message kinds | A stored code plus an `input/schema.h` field schema, bound like a form submission. Plaintext only: inside ciphertext the server validates nothing, and the doc says so **Closed**, as `anvil/chat/card_spec.h` and `MessageKind::Card`, with a poll as the reference application's example (`tests/testapp/chat_cards.h`) and 01 §18 extended. One departure: the row imagined a declarative field schema, and the input layer binds imperatively, so a card kind is a key, a stored code and a BINDER — a function pointer answering canonical JSON or a refusal with field and reason. anvil does not trust the binder either: what it answers is re-parsed and refused unless it is a JSON object within 4 KiB. The service now takes one `ChatServiceDeps` struct, because the constructor had reached seven arguments. A revoke takes the card's code and body together, which the first run of the case found: removing only the body left a row the decoder refused |
| [x] | Receipts, unread counts, the chat list | `dlv`/`rd` watermarks advanced with `$max`; the read-receipt privacy flag; unread as subtraction, capped at 999; the list as a range scan on `{u, arch, pin, act}` plus one `$in` per page. The **coalesced activity bump**: a Redis `SET NX` two-second debounce per conversation that fails open. A load case measures the send path on a 1 024-member group with and without the debounce, and the number goes into the commit body **Closed**: `receipts`, `read_by`, `chat_list`, `preferences`, and `chat::redis_activity_gate`. The load case this row asked for moves to the phase gate, where the hot-conversation throughput is measured once for the whole send path rather than for one part of it. Three findings beyond the row. A receipt is CLAMPED to the conversation's head before the `$max`, because a client claiming to have read seq 10^9 would otherwise appear in "read by" for every message not yet written; a case pins it. A joiner's watermarks start AT the message announcing them, which the first run of the unread case found: a founding member was shown their own arrival as unread. And pins are bounded at five with a count read before the write, so two racing pins can reach six, which the header states as a bound on a list a person curates by hand rather than a security property. The gate fails open and logs, and a case runs it against the live Redis |
| [x] | Reactions | A row per (message, user), one grapheme cluster of at most 8 code points; a page fetches its reactions with one `$in` over its seqs **Closed**: `react` and `reactions`. One departure: the read is TALLIED on the server (`$group` by seq and reaction, with whether the viewer is among them), not a row per reaction, because a page of a 1 024-member group could carry a row per person per message and what a client draws is a count. The `$match` is the `{c, s, u}` index's prefix. Seqs outside the caller's range are dropped before the query, so a tally is never a way to learn about a message they cannot see, and a reaction on a revoked or system message is NotFound |
| [x] | Disappearing messages and retention | Timer as a system message; `expires_at_utc` stamped at send; **a sweeper and no TTL index**, because the TTL monitor deletes a message without releasing its media references and leaves a leak the media sweeper cannot tell from a live reference. Every read filters the expiry explicitly, and `check-db-discipline.sh` learns the field. A case puts an attachment on an expiring message and asserts the reference count returns to zero **Closed**: `set_timer`, `sweep_expired` (for a recurring job the application registers, as it does the media sweeper), and the timer and retention stamped at send by the sending row. The sweep is one bounded pass of at most `batch` claims, each its own transaction with its attachments' references and its reactions, and the case asserts the reference count drops to zero and the reactions go with it. `check-db-discipline.sh` now covers `chat/repository`, whose expiry filter is `append_not_expired_or_timerless`, because a message with no timer has no expiry to compare. Decoded messages now carry their conversation id, which the sweep needs for the reactions |
| [x] | Invites and blocks | Invite tokens stored as `SHA-256(token ‖ pepper)`, multi-use under a cap with one conditional `find_one_and_update`; unknown, expired, revoked and full links all the same `404`. Blocks stop direct creation and direct delivery silently and touch no group; `may_start_direct` is asked after them **Closed**: `create_invite`, `revoke_invite`, `join`, `block`, `unblock`, and the direct-send rule. Three decisions beyond the row. A member following a link spends nothing: the redeem's `$inc` is in the same transaction, which aborts when the joiner is already in, so the use comes back. The service takes the invite pepper at construction and copies it into a `SecretBuffer`. And a blocked sender's direct message is STORED with `hid` and shown only to its sender, so they are not told; the blocker's history and catch-up filter it after the page is cut (a shorter page rather than an unbounded refill), it does not move their chat list, and their approximate unread count can include it, which the header states rather than hides. Whoever did the blocking is refused outright when they write to that person, because they already know |
| [x] | Chat media | Every response that shows an attachment carries a **grant** per attachment, minted after the history read's own membership and range check, and never an id. Pages send `Referrer-Policy: no-referrer`. Forwarding names a visible `(c, seq, index)`, never an object. Attach in the send transaction; release on revoke, expiry and deletion. Composer facts (dimensions, duration, file name) are message fields the sender supplies, not storage metadata the server derives. A case asserts that no chat response body contains a 16-byte object id in any encoding **The send half is built**: `OutgoingAttachment` names a fresh upload by its handle or a forward by `(c, seq, index)`, never an object id; a handle opens only for its uploader and only in the kind's `media_ns`, a forward only from a message the sender can see and only within one namespace, and every reference is `$inc`ed in the send's own transaction. The stored type comes from the object row, never the client. **Open**: minting a grant per attachment in every response, and `Referrer-Policy`, which are the routes' **Closed** with the routes. History mints one grant per attachment after the service's own membership and range check, pages carry `Referrer-Policy: no-referrer` and `private, no-store`, and a listener case asserts that neither the send's answer nor the history page contains the object id as a UUID, as hex or as base64url, and that the grant opens to the object. The grant is SPENT by `install_media_grant_route` (`media/grant_route.h`), a Public route that opens the grant on the loop thread — a length check and an AEAD open, so a forged URL costs no pool slot — and reads the row on `db_pool`, refusing with the stealth 404 a row nothing holds any more: a revoke that released the last reference stops a grant already handed out at its next use, which a case asserts. Found while building it: the grant overload of `accel_redirect_response` sent the one-year `immutable` every public object gets, which would have kept an object on a removed member's device long past the stated 10–20 minutes. `MediaGrant` now carries its expiry and the route caches `private, max-age` up to it and no further. Wakes carry no grant yet; they arrive with the hub |
| [x] | `install_chat_routes`, shapes, descriptor | Handlers on the stages and pools of 22 §4.1; `Origin` checked on every write before the body; `on_message` and `on_membership` after the response; response shapes in `chat/shapes.h`; `limits.chat` in the descriptor. testapp routes, the reference server seeding two accounts and a group, and `check-reference-server.sh` sending, editing, revoking and reading receipts across them **Closed.** 24 handlers as named functions over one `Call`, installed from a constexpr table that a `static_assert` ties to `ChatRouteIds`, so an id added to the struct and not the table fails the build. The listener cases, in a database-labelled listener binary of their own, found three defects in the first draft: the send budget pointed into a `shared_ptr` only a boot-time lambda held, so every send read a freed rule and answered 500 (the rule now travels by value — 32 trivially copyable bytes); an edit ran the send binder, which requires a client id an edit never has, so every edit's mentions were silently dropped; and the budget was a Redis round trip on the loop thread, which now runs on `db_pool` ahead of the parse. Receipts and preferences had no budget and now count into the send and write buckets. **Not built: `chat/shapes.h`.** A message nests attachments, mentions, a card and a system event, which the flat response grammar cannot express, and a body approximated into a schema is one a generated client would trust; the chat responses are undescribed, said so in `chat/routes.h`, as `session.current` is. The hooks run after the commit on the `db_pool` thread and before the response is written, not after it, and 22 §9 now says so; it also named a `may_start_direct` that was built as `may_reach`. `limits.chat` publishes anvil's bounds and, per kind, every enum by name and each role's rights by name (`chat::kRightNames`, asserted complete against `kAllRights`), with attachments as a boolean and no namespace; `"chat":null` without chat, descriptor format 4. Found beside it: `media/edit_routes.cc` makes the same blocking budget call on the loop thread, which is a row of its own and not fixed here. ABI change under CLAUDE.md §9.3: `chat/routes.h` is new, `ChatService` gains `cards()` and `grants()`, `DescriptorInput` gains a defaulted `chat_kinds` |
| [x] | The image edit budget off the loop thread | Found while closing the routes row: `media/edit_routes.cc` charged its per-account budget, a Redis round trip, on the event-loop thread that accepted the request, against CLAUDE.md §4, and the chat routes it was the model for had copied it **Closed.** The charge moves to the head of the `db_pool` stage, still after the body is known to be well formed and still before any lookup, and 21 §6's stage table says so. Nothing tested the refusal at all; `check-reference-server.sh` now repeats one edit until the `media` bucket answers `429`, and asserts the refusal reaches the application's observer as an answer like any other |

**Phase 18 gate: MET.** 1,646 tests green under ASan + UBSan against a live `rs0` replica set
and Redis, with no sanitiser report, and 1,644 of 1,644 under `debug` once the reference
server was given ninety seconds rather than thirty to boot beside a dozen suites building the
same index catalogue. The reference-server run plays editor and root against the group it seeds:
a send and its retry are one message, a forged-origin send stores nothing, only the sender
edits, a read receipt moves and the sender is told who read, a revoke reaches the other member
with the text gone, and an account in no conversation is answered with the bytes and headers
of an unmatched route.

**The number phase 19 is measured against: 214–262 sends a second** into one sixteen-member
group from eight concurrent senders, RelWithDebInfo against a one-member MongoDB 7.0 replica
set and the Redis activity gate (`ChatLoad.OneHotConversationLosesNothingAndRecordsItsThroughput`,
which records it and asserts only that nothing failed, nothing was stored twice and each
sender's seqs ascend). About 35 ms a send at eight writers, and it is round trips rather than
contention: the send transaction writes only the message row, so the hot conversation document
sees one `$inc` per send and no write conflicts, and the cost is the eight or so round trips a
send makes, two of them majority writes.

---

## Phase 19 — live

Phase 18's clients poll. This phase pushes, without making the push the record: **a lost
wake costs latency, never a message**, because the device's next sync asks `seq > cursor` and
MongoDB answers.

| | Task | Notes |
|---|---|---|
| [x] | The wake channel | Redis 7 **sharded** pub/sub keyed by user: `SPUBLISH` from the sending process after the commit, `SSUBSCRIBE` on a user's first local socket and unsubscribe on the last. Wakes carry `{c, seq}`, plus the message when it is at most 2 KiB. Rejected: a per-process channel, which needs a presence registry consulted on every send and wrong for as long as it lags a reconnect. Verify before building on it that redis-plus-plus exposes sharded subscribe; if it does not, that is a finding for this row's notes and not a reason to fall back silently **Transport closed**, by a parallel worker, as `anvil/chat/wakes.h` (`WakePublisher`, `WakeSubscriber`); wiring it to the send path lands with the hub. The verification the row demanded came back yes, with two findings worth keeping. redis-plus-plus 1.3.15 has `ssubscribe`/`spublish`, but its pipeline has no `spublish` (the raw command is queued instead), and `Subscriber::sunsubscribe()` and `sunsubscribe(StringView)` are DECLARED BUT NEVER DEFINED in the library, a link error, so the template range overload is used with a range of one. Sharded pub/sub here is correct on one Redis or a primary with replicas; Redis Cluster would need a cluster publisher and a subscriber per shard, and each channel is subscribed by its own command so that change stays inside `wakes.cc`. Every payload is decoded as a downstream frame on arrival and dropped and counted if it is not one, because a Redis channel is not a trusted boundary. A `WakeSubscribed` callback fires when Redis confirms a subscription, including after every reconnect, so the hub can make that user's sockets sync across the window in which wakes were lost |
| [x] | The member cache | Per process, keyed by conversation, validated by the `mv` the send's `$inc` already returns, so it costs no round trip. Bounded in **bytes**: a 1 024-member entry is 16 KiB. Case: a member removed on another process gets no wake for the next message **Closed** as `anvil/chat/member_cache.h`, with 22 §5.4 updated. The case runs the removal through a second `ChatService`, which is all a second process is to the cache, and asks with the version a real `allocate` returns. Validation is **equality**, not "at least": an entry newer than the send's allocate can hold a joiner and an older one a leaver, and a slow send at an older version reads for itself without replacing the newer entry. **One hole the row did not name, closed**: a miss reads the list after the allocate, so it can contain somebody who joined in between, and under `FromJoin` a wake carrying the message inline would show it to them. The read is filtered to `js ≤ seq`, which keeps an entry at 16 bytes a member. The loader is `ChatRepository::member_ids`, projected to `u` and `js` on the existing current-members shape. A conversation over `kMaxMembers` is refused and never kept, so channels wake nobody (recorded in 22 §5.4). LRU by bytes, with a 160-byte per-entry overhead counted so the bookkeeping cannot overrun the bound; an entry larger than the whole bound is answered and not kept |
| [x] | `chat/hub` — registry and ring | **Reverses 04 §8.5 for chat**, for the reason that section gave: anvil now has a producer. SSE's policy, a full ring drops the connection; slots out of `kUpgradeShare`; one socket per device, where a second socket closes the first. No I/O under the registry lock and the stream lock never taken under it (11 §6). 04 §8.5 gains a paragraph saying which half of the refusal still stands **Closed** as `anvil/chat/hub.h` (`ChatHub`, `ChatSocket`), with 04 §8.5 and 22 §8.2 updated; `http/upgrade.h`'s refusal now names chat as the exception. The paragraph says the GENERIC refusal stands: an application's own socket still gets the budget, the re-check and the share and nothing else, and the hub is not shaped to become a general registry (its ring holds chat frames, its key is a device, its one writer is the wake subscriber). Beyond the row: the ring is 8 KiB of **bytes**, length-prefixed, because frames run from 2 to 2 076 bytes and a ring of slots sized for the largest would be 66 KiB a socket; a push wakes the writer only on empty-to-non-empty, so a burst is one posted drain; the device is the token's **session**, and a replacement takes the slot it frees so a stuck socket can be replaced on a full process. The subscriber's subscribe and unsubscribe run UNDER the registry lock, on purpose and with the reason in the header: they touch no network, and after the unlock two loops could deliver one user's first-open and last-close reversed. The case that proves it opens and closes on four threads over five users with replacements and asserts the count balanced, under TSan. The hub cannot see Trantor's output buffer (Drogon exposes no write-buffer level), so what waits beyond the ring is bounded by the socket's liveness deadline instead, which lands with the socket row. `Sync`, the frame a confirmed subscription sends, and `Pong` were added to the codec in their own commit, with golden bytes and fuzz coverage |
| [x] | The socket and its frame grammar | `register_websocket_route`, inheriting the gate, `Origin`, the re-check and the budget. Binary, versioned frames with a hand-written decoder in the foundation and a fuzz target. **No durable write over the socket**: every write stays an HTTP route, because the rate limiter, idempotency, the stealth filter and the audit hooks see requests and never frames (04 §8.4) **Closed** as `anvil/chat/socket.h` (`install_chat_socket`) over `anvil/chat/live.h` (`ChatLive`), with 22 §8.2 and 01 §18 updated. The grammar and its fuzz target were already `chat/frames.h` (`chat_frames_fuzz_test.cc`); this row serves it. `ChatServiceDeps::live` is the one wire: null is Phase 18's polling chat, set and every committed send publishes its wakes, **before** the activity bump, because the bump can be an `update_many` over a thousand rows and the wake is what a reader waits for. Wakes go through Redis even to sockets on the sending process, one path rather than two. The inline message is the history route's own writer, moved to a private `render.h` in its own commit so a wake and a fetch cannot come to differ; it carries grants, never ids, and a revoked message never rides a wake. A message hidden past a block wakes the sender only. Close codes are the client's contract: 4001 replaced (do NOT auto-reconnect, or two tabs of one device take the socket from each other for ever), 4002 overflow, 4003 silent, 4004 full, 4005 reauthenticate, 4006 bad frame. Ping every 20 s, closed after 45 s silent, on one timer per socket at `kRecheckPeriod` (10 s): Drogon exposes no write-buffer level, so the silence deadline is what bounds a client that stopped reading. The handshake re-decodes the cookie once, for the expiry the re-check needs and the context does not carry. Ten listener cases over a real socket and the real Redis channel: the wake reaches every member with the bytes history writes, the sender's other devices and no stranger, an attachment as a grant, an oversized message as fetch-it, past a block the sender only, a removed member not at all, a second socket from one device closes the first as replaced, a ping is answered, a text or malformed frame closes, and a handshake without a cookie or from a foreign origin is refused. `ClientTyping` is accepted and not relayed until the typing row |
| [x] | SSE fallback | The same wakes on the existing `sse.h` stream, for clients behind a proxy that breaks upgrades. The routes are unchanged, so the fallback costs no second code path for writes **Closed** as `ChatLiveConfig::sse` and `ChatLive::follow_on_stream`, with 22 §8.1 and 01 §18 updated. The wake rides the reader's existing notification stream as two new `SseEventKind`s, `ChatWake` (its `notification` field is the conversation) and `ChatSync`. **Content-light on purpose**: no message and no seq, because an `SseEvent` is 32 bytes for every stream in the process and growing it for chat grows all of them; the client catches the named conversation up from its cursor. A stream opts in by holding a `StreamLease` for its life, which subscribes the reader's wake channel exactly as a socket does, so the subscriber's reference count covers both and a reader with a socket and a stream is one subscription. A stream WITHOUT a lease carries no chat even when the reader's channel is live for a socket. The lease's subscribe and unsubscribe are taken under its own mutex, for the hub's ordering reason. No typing on the fallback. Without a configured `sse`, `follow_on_stream` throws: a lease that silently carried nothing would hide the wiring fault. Three listener cases: a wake reaches a leased stream as the conversation after a `ChatSync`, a stream without a lease gets nothing while the same reader's socket is woken, and releasing the last lease stops the stream's wakes while the socket keeps them |
| [x] | Typing | Ephemeral: relayed as a wake to members with live sockets, three seconds per connection per conversation, never stored, none on channels **Closed** as `ChatLive::typing` and the socket's admission, with 22 §8.3 updated. Per socket, `kTypingInterval` (3 s) per conversation over `kTypingSlots` (8) remembered conversations, oldest replaced: a frame inside the interval is dropped and not refused, and a client cycling through more than eight is still inside the frame budget. The relay is a membership read, so it is posted to `db_pool` and dropped when the pool is full: typing is the one thing in chat whose loss nobody can see. Refused silently, never by a close the typist could time: a non-member, a channel, a direct conversation with a block either way. Membership is asked FIRST so the cost of the silence does not depend on who blocked whom. The read of the conversation gives the member cache the same version and head a send's allocate gives it. The `Typing` frame goes to the other current members through their wake channels and never to the typist's own devices. Not on the SSE fallback. Three listener cases: typing reaches the other member and not the typist's phone, three frames in a burst are one relay while a second conversation has its own budget, and a non-member's and a blocked typist's frames reach nobody while the stranger's socket stays open and answers a ping |
| [x] | Presence | **Off unless the application turns it on**, and then answered per viewer through `may_see_presence`. Online is a Redis key with a TTL refreshed by the socket; last seen is written to MongoDB at most every five minutes and only when the hook allows anyone to read it **Closed** as `anvil/chat/presence.h` (`PresenceTracker`) behind `ChatLiveConfig::presence`, and the route `GET /chat/presence/{user}` (`ChatRouteIds::presence`, so 25 routes), with 22 §8.3 and §9 and 01 §18 updated. Off means off: no key, no row, and the route answers the stealth 404. One Redis key per account, `anvil:chat:seen:<hex>`, a signed ms timestamp (+ online heartbeat, − offline since), TTL 1 h. **One pipelined round trip per pass** for every account the process holds, not one per socket, so ten thousand sockets cost one Redis call every 20 s. The first and last socket mark the change at once through the hub's callbacks, which only record it, so they are safe under the hub's lock. Online is a value fresher than 45 s, so an account whose process was SIGKILLed goes offline by itself. Last seen goes to MongoDB as a `$max` (exempt from versioning for that reason: two writers racing cannot lose a later time) at most every 300 s per account, at most 1 000 rows a pass, and **only when `may_see(nil viewer, subject)`** says anybody may see it: a value nobody is shown is never stored. The hook lives in `ChatLiveConfig::presence`, not `ChatHooks`, because a service without live delivery has no presence to answer. A Redis key is not a trusted input: a value that does not parse is no answer. Withheld and never-seen are one answer, `{"online":false,"last_seen":null}`. **Not done, deliberately**: the `Presence` frame stays in the grammar and is not sent, because pushing a change to everyone allowed to see it needs a watcher list per account, a presence registry by another name; clients poll the route for the accounts on screen. Seven database cases against live Redis and MongoDB (`chat_presence_db_test.cc`) and one listener case: a socket makes its owner read online with a last seen, an account the hook hides reads with the exact bytes of one never seen, and the hidden account still sees itself |
| [x] | Push nudges | One job per message and never one per recipient (11 §7), coalesced per (account, conversation) through the job's idempotency key, to the `WebPush` clients notifications already holds. No notification rows: the chat list is the inbox, and two read states for one fact disagree. Mute honoured; mentions break a mute only where the kind allows it. Plaintext wording through the application's template, rendered per recipient and locale **Closed** as `anvil/chat/push.h` (`ChatPush`) behind `ChatServiceDeps::push`, with 22 §8.4 and 01 §18 updated. Every send asks the application's queue for one job keyed by (conversation, window), so a window's sends are one job, and the job tells each recipient how many messages wait: one push per (account, conversation) per window, saying forty for forty. **Who is pushed, decided**: a current member with a message they can see, from somebody else, past their DELIVERED watermark, at least one of them sent in the job's window, and no mute (a mention breaks one where `mentions_break_mute` is set). The watermark rather than the hub, because `has_socket()` is one process's answer: a device that holds a message says so with a receipt through whichever process it reached, the job runs a grace period after its window so it can, and the failure mode is a duplicate push to a device that has not said so yet, never a miss. The job is asked for BEFORE the commit, so a SIGKILL after the commit has already asked; a commit within a second of its job's due time asks again under its own key. The wiring is the application's: a job kind, a WebPush-only topic nothing is published to (so the transport can label the push and a device can turn it off), and two templates, with and without the text; the reference application's are `chat.push`, `chat.message` and templates 5 and 6. The reference server runs a real `JobQueue`, and `check-reference-server.sh` waits for root's browser to be pushed about a message root's receipt does not cover. Two queries, both in the explain catalogue: the newest hundred messages projected to seven fields with the text cut to 120 code points by `$substrCP` on the server, and a page's endpoints in one `$in` on `clients_owner_type`. A verdict costs an endpoint what a notification's would, through `notifications::record_verdict`, factored out of `OutboundSender` for it. Not pushed: channels, as they are not woken; a message hidden past a block; encrypted conversations until phase 20 adds the `{c, seq}` payload, for which the job's arguments carry a version byte. Six database cases (`chat_push_db_test.cc`): forty sends are forty asks, one job and one push saying forty that writes no notification or inbox row and never reaches the sender; a member whose device posted delivered is not pushed; mute; a mention through a group's mute and not through a direct one's; and the wording per locale and preview setting |
| [x] | Two processes, one conversation | A listener case with **two** processes sharing Redis and MongoDB: a send on A wakes a socket on B; killing A mid-send leaves B's client to recover by sync. Run as one process as well, for the reason `listener_in_one_process` exists (16, "One case per process hides a class of defect") **Closed** as `tests/chat_peers_listener_test.cc`, the binary `anvil_chat_peer_tests` with its own `main`: given `--chat-peer` it is a chat server (the listener fixture's stack behind the real access filter) that prints its port and serves until its stdin closes; otherwise it is the test, and each case spawns two of those. They share exactly what a deployment's instances share: the MongoDB database (`ANVIL_TEST_SCRATCH_DB`, now honoured by the fixture) and the cookie key (`ANVIL_CHAT_TOKEN_KEY`), both set by the parent's `main` before it spawns; nothing else, so hub, member cache, subscriber and publisher are separate. Two cases. A send through A's HTTP route wakes the reader's socket on B with the seq A answered. Eight senders burst into A, A is SIGKILLed after forty answers, and the reader's device on B, which synced on opening and then only read wakes, syncs again from the cursor it held: the seqs it then holds equal exactly the seqs MongoDB holds for the conversation, every wake named a committed message, and every answered send is committed. A send through B after the kill is the sentinel that ends the wait for wakes. A peer stops its `ChatLive` before `main` returns, and B's exit status is asserted to be 0, so a LeakSanitizer report in the child fails the case. Under `asan`, of thirteen runs of the kill case, two killed A with a message committed and never answered or woken, which only the sync recovered; the rest killed it between commits. Also run in one process (`chat_peers_in_one_process`) |

**Phase 19 gate:** the two-process case green under `asan`, and a load case with a thousand
idle sockets and a busy group recording wake latency at p50 and p99. The commit records the
number, not "fast".

**Phase 19 gate: MET.** 1,701 tests green under ASan + UBSan against a live `rs0` replica set and
Redis, with no sanitiser report, the two-process cases and their one-process run among them: two
chat servers spawned from the test binary, sharing only MongoDB, Redis and the cookie key, where a
send on one wakes a socket on the other, and where one is SIGKILLed mid-burst and the other's
client, syncing from the cursor it held, ends holding exactly the seqs MongoDB holds.
Each child stops its live delivery before `main` returns, and its exit status is asserted, so
LeakSanitizer reads the children too.

**The wake numbers: commit to wake p50 0.25–0.31 ms, p99 0.44–0.61 ms, at 252–260 sends a
second**, over six runs of `ChatWakeLoad.ABusyGroupBesideAThousandIdleSocketsRecordsItsWakeLatency`
(`chat_wake_load_test.cc`, labels `load;database`): eight senders into one sixteen-member group
whose every member holds a socket, beside a thousand idle sockets of other accounts, 2,000 sends
and 32,000 wake arrivals a run, with every wake received once and none of the sockets closed.
RelWithDebInfo (`release` preset), on an i7-12650H (16 threads, 15 GiB) running Linux
7.2.6-cachyos, against a one-member MongoDB 7.0.43 replica set and Valkey 9.1.2, with `ulimit -n`
at 1,048,576. The interval is measured in one process on one steady clock: from `on_message`, which
the service now calls at the commit and before the wake, to the frame being read off the member's
socket. It therefore covers the member cache, the render, `SPUBLISH`, the subscriber's thread, the
hub's ring, the loop's write and the kernel, and none of the transaction. Throughput is the phase 18
baseline's (214–262), and the phase 18 case measured 250–259 the same afternoon in the same build, so
live delivery costs the send path nothing it can see: the send is round trips to MongoDB, and the wake
is one pipelined round trip to Redis beside them. One earlier run measured 1,485 sends a second, p50
0.19 ms, p99 0.44 ms; six later runs, alone and beside the phase 18 case, did not reproduce it, and
it is recorded as unexplained rather than averaged in.

---

## Phase 20 — end-to-end encryption

The server's half of 22 §7: a device directory whose additions need a signature from an
existing device, a single-use key directory, a device-set fence on every encrypted send,
per-device queues, and sealed media. **The server never holds a key that decrypts a message.**
Everything that encrypts or decrypts is hammer's, and is **our own implementation, written
from Signal's published specifications with libsignal as the reference and test oracle**,
never a translation of its source, because libsignal is AGPL-3.0 and a port carries the
licence with it (22 §7.2). hammer needs its own design document before row 1 starts.

| | Task | Notes |
|---|---|---|
| [ ] | hammer's design, and the wire both sides build | A precondition, not code in this repository. hammer's document fixes the suite: X25519 for agreement, a separate Ed25519 signing key instead of XEdDSA (which WebCrypto lacks and must not be hand-written), and no PQXDH in version 1 for want of an ML-KEM in WebCrypto. It also fixes the libsignal oracle harness, which is a test tool and never shipped. This repository gets golden vectors for every byte the server checks (key encodings, the link signature's input, the send envelope), printed by a testapp emitter the way the edit recipe's were **Open: hammer's document is not written.** This repository's half is done: `tests/testapp/chat_vectors.h`, produced the way the edit recipe's were, by a third implementation (a throwaway script over Python's `cryptography`, concatenating each signed message from 22 §7.3.1's table without reading either side's code). Two devices with fixed private halves, their four derived public keys, both prekey messages and signatures each, the 119-byte link message and the approver's signature, five keys the server must refuse with the field it names (two low-order X25519 points, a non-canonical X25519 spelling, the Ed25519 identity under which OpenSSL accepts a forgery, a non-canonical Ed25519 y), and an encrypted push payload. `chat_vectors_test.cc` holds anvil to every byte and passed on its first run; `testapp_emit_chat_vectors` prints them as hammer's fixture, every key in base64url under the names the device routes bind, the private halves in hex, and an encrypted send body as the send route takes it |
| [x] | `crypto/x25519`, `crypto/ed25519` | OpenSSL `EVP_PKEY_X25519`/`ED25519` in the foundation: length checks, low-order point refusal, signature verification. RFC 8032 and RFC 7748 vectors, for the same reason the HKDF test uses RFC 5869's (11 §8) **Closed**, with two findings. **OpenSSL accepts a forgery under the identity key**: with the identity point as the public key, `(R = identity, S = 0)` verifies for every message, and its decoder silently reduces a non-canonical `y`. So `ed25519_public_key_is_valid` applies RFC 8032 §5.1.3 strictly with BIGNUM and refuses the small-order points under both signs, and `ed25519_verify` runs it first; a case asserts that OpenSSL accepts the forgery and ours does not. It does NOT exclude mixed-order keys, which needs a scalar multiplication OpenSSL does not expose. `x25519_public_key_is_valid` refuses the low-order u-coordinates AND every non-canonical spelling (bit 255 set, or u ≥ p), beyond RFC 7748, because a key with several byte spellings breaks byte comparison and safety numbers, and WebCrypto never exports one. Signing, keypair generation and `x25519_derive` exist for tests and the vector emitter; nothing on a request path signs or agrees a key. Small-order tables from libsodium's blocklists, re-derived independently |
| [x] | Devices and the link chain | `chat_identities`, at most `max_devices`; the first device after fresh primary authentication only; every later device admitted by a verified Ed25519 signature from an existing one. Cases: a valid session with no approving signature cannot add a device; a signature by an unlinked device is refused; revoking a session unlinks the device it registered; the idle sweeper unlinks **Closed**, by a parallel worker, as `anvil/chat/devices.h` (`DeviceDirectory`), and 22 §7.3.1 records it with the signed-message byte layouts. Beyond the row: the client proposes the device id, because the approver signs over it before the server sees anything; `gone`, the last sixteen unlinked ids, stops a stolen device its owner unlinked from replaying its own link inside the ±5-minute window; and each device records the session that registered it, so `unlink_session` ends it. The case that revoking a session unlinks its device is covered at the directory; calling it from session revocation lands with the routes, as does calling `discard` after an unlink |
| [x] | `dsv` propagation | `$max` of the change's millisecond on every conversation of the account, recorded as an intent on the identity document and finished by a sweeper (11 §3's outbox, unchanged), plus a "devices changed" system message per encrypted direct conversation. Cases: a crash between the identity write and the propagation is finished by the sweeper; two racing device changes leave `dsv` at the later one **Closed** as `ChatService::propagate_devices` and `sweep_device_changes` behind `ChatServiceDeps::devices`, with 22 §7.4 and 01 §18 updated. **`dsv` rises to `max(dsv + 1, change)`, not to `$max` of the change**: the millisecond is one process's clock, and a change stamped by a process running behind a version another process wrote would leave the fence where a sender who read the old set passes it. So the second case asserts the version ends AT OR PAST the later change and moved for the earlier one too, finishing the later first and then four propagations racing on threads. The cost is a spurious 409 on a re-run, never a stale sender let through; the update is one pipeline write. **Beyond the row: a membership change raises `dsv` too**, inside the transaction that already writes `seq` and `mv`, because a joiner or a leaver changes the set a message must be encrypted for exactly as a device does. The walk pages by conversation id over a new `{u, c}` index on members, since the chat list's activity order moves under a cursor; the sweeper scans a new `{pend}` partial index, oldest first, and leaves a change younger than `kDeviceChangeGrace` (60 s) to the request that made it. The DevicesChanged message (`SystemEvent` 9) is written once per change and conversation: its client id is SHA-256 of the change under the invite pepper, so a re-run finds it through `{c, u, cid}`, and it is peppered because its sender is the account itself, which could otherwise send under the id first and suppress its own notice. The pending mark is cleared only while it still holds the pushed change. Six database cases (`chat_e2ee_db_test.cc`) with real key stores: every current conversation and no other is raised and the mark cleared; the crash, swept only past the grace; the race; one notice per change in an encrypted direct conversation and none in a plaintext one or a group, four concurrent runs included; a join and a removal each move the fence; and no directory refuses rather than pretends |
| [x] | The key directory | Batches of up to 100, at most 1 000 per device; claim as one `find_one_and_delete`; the last-resort key when empty, plus a low-water flag. Claims budgeted per claimer **and** per target, because exhaustion is how an attacker pushes a victim's new sessions onto the reusable key. Case: two concurrent claims never return the same key **Closed**, by a parallel worker, as `anvil/chat/prekeys.h` (`PrekeyDirectory`), recorded in 22 §7.5. The per-device bound is EXACT, not a count read before an insert: a `pk` counter reserved with `$inc` under a filter, in one transaction with the batch, so six uploads racing for two free batches store exactly two, and a claim that empties a pool resets the counter and heals drift. Claim budgets per claimer and per target stay with the routes row |
| [x] | Encrypted send | Common ciphertext and/or a per-device map; the `dsv` check inside the existing `$inc` filter, refused as `409 chat.devices_stale` with the current lists; the map's key set exactly the current devices minus the sender's (an extra one is refused, not ignored); at most 64 entries. Per-device rows in `chat_device_queue` with a TTL at 30 days, a filtered read, and ack-to-delete **Closed** as `ChatService::send_encrypted` over `anvil/chat/device_queue.h` (`DeviceQueue`), behind `ChatServiceDeps::queue`, with 22 §7.6 and §9.1 and 01 §18 updated. `MessageKind::Encrypted` (3) stores the common ciphertext as `ct` and the sending device as `sd`; every rendered message now carries `ciphertext` and `device`, null on plaintext, so a client parses one shape. The send route tells the bodies apart by `dsv`; the 409 carries the first page of the conversation's device lists, written by the same writer as the device listing, its `dsv` read before the lists. **The row's exactness and the design's paging contradicted each other**, and `page: true` reconciles them: a group's sender-key distribution may be a subset, per-device only, nothing extra, and the fence catches a stale page. **Defect found beside it and fixed in its own commit**: `create` and `open_direct` answered the `dsv` from BEFORE their own first membership transaction raised it, so a client's first encrypted send was stale by construction. Beyond the row: the sending device is named and must be the actor's current one; a retry is answered with the stored message even after the fence moved, because the client id is checked first; past a block the peer's rows are never written; a per-device row expires no later than its message, and a revoke or the expiry sweeper deletes its rows; the member read happens only for a send with a map, projected to device ids; sealed attachments resolve in the kind's `sealed_ns` with no name or dimensions beside them; `ChatService` refuses at construction a directory without a queue. Encrypted edits and reactions are messages with their `ref` inside the ciphertext and are not served by the plaintext routes. Eleven database cases (`chat_e2ee_db_test.cc`) and one listener case: the common ciphertext kept and one row per other device, each read only by its own device and acknowledged away; a stale fence spends no seq; a missing device is stale and an extra, a self-addressed or a doubled one refused; the sending device the actor's own and current; each mode refusing the other's body; a retry after the fence moved; past a block; revoke; paging; expiry with the message; the device page with its version and a member with none; and over HTTP, the 409's page and a send at the version it named |
| [x] | The `Sealed` object class | `Private` visibility, served only on a grant, uploaded for a handle (both phase 18). No sniff, no dedupe, no variants; the declared SHA-256 compared with the hash the stream already computes; `application/octet-stream` and `attachment`, always, on the content origin only; a per-account **byte** budget, because a namespace that cannot inspect content is otherwise free file hosting. `kinds_are_well_formed` refuses an encrypted kind without a `Sealed` namespace **Closed**, by a parallel worker, in five commits, with 07 §5 and 22 §2.3 and §6.4 updated. `Mime::Sealed` is chosen by the namespace and never by the bytes; a sealed namespace takes only that class, never deduplicates and is Private, asserted at compile time by `fs::namespace_is_well_formed`; `finish_sealed` compares the declared SHA-256 with the stream's own digest, and `finish` and `finish_sealed` each refuse the other kind of namespace. Serving takes the type, disposition and file from the namespace rather than the row, under `default-src 'none'; sandbox` with no `media-src`. **One defect found beside it and fixed in its own commit**: `UploadSink::publish()` checked only that a finish had RUN, and `finish` marks the file synced before checking anything, so a caller ignoring a refusal could publish a refused file. `publish()` now requires a finish that accepted the bytes. The per-account byte budget is the chat upload route's, which can refuse before the sink opens |
| [x] | Device, key and queue routes | Added to the phase as it went: the directory, the keys and the per-device queue had no route, and the rows below need one. Eight notes across the rows above deferred something to "the routes row", which the plan never had **Closed** as nine route ids over new `ChatService` methods (`my_devices`, `register_device`, `link_device`, `unlink_device`, `session_ended`, `sweep_idle_devices`, `upload_prekeys`, `claim_bundles`, `device_queue`, `acknowledge_queue`), behind `ChatServiceDeps::prekeys`, with 22 §7.3.1 and §9 and 01 §18 updated. `Actor` carries the session, so a device records the one that registered it. **The claim is scoped to an encrypted conversation both people are in**, a stealth 404 for anybody else, and **the per-target budget is spent only after both memberships hold**: spent first, a stranger asking about an account would lock every member out of claiming against it. A first device needs the new `ChatHooks::authenticated_at`, unset refusing; `ChatHooks::on_device` reports each change. An unlink discards the device's keys and queue and propagates. Reading, sending and uploading touch the device, so the idle sweeper never takes one in use. The reference application answers `authenticated_at` from its v7 session id and declares the routes and two claim budgets (`chat-claim` 120, `chat-claim-target` 30 a minute). Four listener cases and three database cases: register, a refused second first device, a link by signature, the listing with the chain and no session, an idempotent unlink; a first device on a session ten minutes old refused; claims refused to a stranger without spending the target's budget, three keys handed out once each, and the fourth claim 429 with a key left in the pool; the queue read and acknowledged by its own account only; an ended session taking its device, keys, queue and fence position with it; the idle sweeper ending a device the same way; and a service without the hook refusing a first device |
| [x] | Session revocation reaches the device directory | `ChatService::session_ended` ends the device a session registered, and nothing in anvil calls it: `SessionService::revoke_all` and `revoke_others` revoke with one `update_many` and never learn which sessions they ended. An `on_revoked(user, sessions)` hook there, fed by reading the ids inside the same filter first, and the reference application wiring it to `session_ended`. Case: signing out everywhere unlinks every device those sessions registered **Closed** as `identity::SessionsRevoked`, the last constructor argument of `SessionService` and of `StaffService`, with 01 §16 and §18, 05 and 22 §7.3 updated. **Every revoking path is told, not only the two the row named**: a sign-out, a reset, a change, an eviction past the cap, a replayed refresh token, a refresh that finds the account inactive, and a disable. The disable revokes through its own repository and would otherwise have been the one path that leaves a disabled account's devices linked. The hook is asked after the epoch bump, because the access token is what stops the session being used, and is best effort: a throw is logged and swallowed, and a device whose session died unnamed can no longer be touched, so the idle sweeper ends it. **The repository's bulk revocations now answer the ids, not a count** (`SessionRepository::revoke_all` and `revoke_all_except`), reading 64 ids under the old filter and revoking exactly those, until a read finds none. A plain read followed by the old `update_many` would revoke a session signed in between them without naming it. Past sixteen pages, which the cap makes unreachable, the rest are revoked unnamed rather than left live. The page read is `sessions_to_revoke` in the explain catalogue, on the listing's index. The reference application forwards every revoked session to `session_ended`. Five cases: the repository names all seventy sessions across two pages and no other account's; the service names a sign-out's one, an eviction's oldest, a change's others and a sign-out-everywhere's rest, and nothing for nothing; a throwing hook fails no revocation; a disable names both sessions and an enable none; and, with a real `SessionService` wired to a real `ChatService`, a password change on the phone unlinks the laptop and moves the fence, and signing out everywhere unlinks the phone |
| [x] | Push carries nothing | Encrypted conversations push `{c, seq}` only. A case asserts the payload, decrypted with the subscription's private key as 11 §8's tests do, contains no message bytes **Closed** as `chat::encrypted_push_payload` and the encrypted branch of `ChatPush::deliver`, with 22 §7.8 and §8.4 and 01 §18 updated. An encrypted send now asks for its window's job before the commit and once more after a slow one, exactly as a plaintext send does, through the same job kind: the version byte the arguments carry was not needed, because the job reads whether the conversation is encrypted. The delivery's title is empty and its body is the payload, verbatim, the newest seq waiting for that recipient. **Beyond the row**: not the sender's name, the title or the count either, although the server holds all three, and neither wording hook is asked, so nothing an application binds can reach the push. A mention is inside the ciphertext, so nothing breaks a mute. One database case (`chat_push_db_test.cc`): three encrypted sends are one push to the other member and none to the sender, its body is exactly `{c, seq}` at the newest seq, and sealed to a browser's P-256 subscription and opened with its private key it holds neither ciphertext in base64url, nor a word of the plaintext the ciphertext stood for, nor the sender's name |
| [x] | Removal and rotation | The server stops serving a removed member at `ls` on every route, the socket included, and the membership system message is the signal for the remaining clients to rotate sender keys. A case per route that a removed member's device is refused **Closed**, with 22 §3.3 updated. The audit of every conversation route found the log itself already bounded by the visibility range, and **two leaks beside it, both closed**: the member listing answered a past member, so a removed member could still read who remained in an encrypted group, the set the removal rotated the keys away from; and a past member was shown the conversation's live head, so how far it went on after them. The listing now needs a current member, and a past member is shown the head at `ls − 1` in the conversation and the chat list. Phase 18's case that pinned the old listing is reversed, with the reason in it. The fence closes the rest: a removal raises `dsv`, so a send encrypted for a set that still held the leaver is refused, and no per-device row can be written for them after `ls`. One listener case over an encrypted group of three: before the removal the leaver reads the message and his own queue row; after it, history, catch-up, the head and the queue all stop before the next message; the remaining member's log carries `member_removed` at its position; the member listing, the device page and "read by" answer byte for byte as an unmatched route; twelve writes (an encrypted and a plaintext send, edit, revoke, react, receipts, a claim, title, timer, add, remove, invite) answer 404; and his own preferences stay his. The socket's half is the existing `ChatSocketListener` case that a removed member is not woken, since the removal moves `mv` |
| [ ] | External review | Both halves, before any application ships an encrypted kind. This is the one row in the plan that is not closed by a green suite: the protocol is the riskiest code in either repository, and a review's findings get rows of their own here |

**Phase 20 gate:** every row closed, the review included. Encrypted kinds are compiled and
tested before that, but the seam's documentation says plainly that they are not for
production until the review row is checked.

**Phase 20 gate: NOT MET.** Every row this repository can close is closed, at 1,739 tests
green under ASan + UBSan against a live `rs0` replica set and Redis. Two rows are open, and
neither is code this repository can finish alone: hammer's design document (the precondition
of the first row, whose repository half, the golden vectors, is done), and the external
review. 01 §18 says that an encrypted kind is not for production until the review row is
checked, as this gate requires.

Waiting for an application, recorded in 22 §10: calls, device history transfer, plaintext
search, verifiable reports (franking), forward limits, view-once, sealed sender, an MLS suite,
resumable uploads, video transcoding, stories.

---

## Phase 21 — what the client half needed

hammer's chat design (hammer `docs/05-chat.md` §13) found thirteen things the server half did
not give a client, each filed in hammer's `15-tasks.md` §Cross-repo with a fallback that was
deliberately worse. The rows are those thirteen, in the client's priority order. The first is
the only one that blocks: a refusal a client cannot tell from an expired token signs a person
out of every tab.

| | Task | Notes |
|---|---|---|
| [x] | A fresh-authentication refusal that is not `401` | A first device on a stale sign-in answers `401 UNAUTHENTICATED` with no field, which a client reads as an expired access token: it refreshes, replays, is refused `401` again, and signs the person out of every tab. A code that means "prove yourself again", with the field named **Closed** as `428 CAPABILITY_REQUIRED`, from both `DeviceDirectory::register_first_device` and `ChatService::register_device` (the hook unset or silent), with `"reason":"chat.fresh_authentication","field":"authenticated_at"` beside the error. 428 rather than a reasoned 403 because it is already anvil's word for "a second, deliberate act is needed first" (a capability token is the other one), and a client's error table already treats it so; a 403 says "you may not", which is false. The 409 stale send's `reason` now goes through the same writer. 22 §7.3.1 and 01 §18 updated. Cases: the directory and the service answer the code with the field; over HTTP a session signed in ten minutes ago is answered 428 with the reason and field, never `UNAUTHENTICATED`, and no device is stored |
| [x] | A catch-up for mutations | A plaintext edit, revoke or reaction changes a row without a seq and wakes nobody, so a client syncing `after=` never learns of it, and 22 §4.5's claim that "the next sync" tells it is false. A per-conversation mutation counter stamped on the changed row, a catch-up by it, and a wake when it moves **Closed** in two commits, with 22 §4.5, §8.2, §9 and §9.1 and 01 §18 corrected. The counter (`mut` on the conversation, `mu` on the message) is allocated **inside** the mutation's transaction, first, not outside it as a send's seq is: a cursor moves past every number it reads, so a lower number committing after a higher one was read would be lost for good, and two transactions writing the conversation commit in the order they numbered. The same reaction twice and a refused edit allocate nothing. `GET …/messages?changed_after=<n>` answers the history shape, oldest change first; every message renders `mutation` and every conversation `mutations`. A `Mutation` frame (`0x09`, 26 bytes, the conversation and the counter) wakes the current members, or only the sender past a block; on the stream it is `ChatWake`. A past member's counter and catch-up stop at `lmu`, written with `ls`. **Found beside it**: the seq catch-up has the race this counter was built to avoid (a send's seq is allocated outside its transaction, so a device catching up between a higher seq's commit and a lower one's moves its cursor past the lower), and it is recorded here rather than fixed, because its fix is a change to §4.1's send path. Index `{c, mu}` partial, in the testapp catalogue and the explain catalogue as `chat_changed`. Four database cases (catch-up by counter and not by seq; nothing that changes nothing moves it; a past member stops where they left; eight racing reactions numbered one to eight, one each) and three listener cases (the counter on the conversation and `changed_after=` over HTTP, a stranger's 404 byte for byte; the frame for an edit, a reaction and a revoke on the other member's and the author's sockets; the frame on the stream fallback as `ChatWake`) |
| [x] | Another member's delivered watermark | The `Receipt` frame is never sent and no route reads another member's `dlv`, so a sender can be shown read and never delivered **Closed** as a second list on the readers route, `{"read_by":[…]|null,"delivered_to":[…]}`, over a new `{c, dlv}` index, with 22 §5.1, §9 and §9.1 and 01 §18 updated. **The route and not the frame**, on cost: a frame per watermark move is a publish per member per receipt batch, so a thousand-member group whose members all post receipts is on the order of a million publishes per message read, where the route is one indexed range read per sender per look, and both watermarks mean one call for the newest message answers every older tick. The frame stays in the grammar, unsent. `read_by` is null rather than empty for a `Delivered` kind, which this route used to refuse, so "nobody read it" is never said by accident; an `Off` kind still answers 403. Delivered is not withheld by private reads, as a delivered tick never was. Three database cases (a `Delivered` kind with no reads and one holder; one ask covering an older message; `Off` refused) and the two existing ones extended; one listener case over the route's bytes |
| [ ] | A chat upload route, plaintext and sealed | No handler exists, so no attachment can be sent end to end. Plaintext answers `media::mint_upload_handle`; sealed takes the client's declared SHA-256 and spends the per-account byte budget before the sink opens |
| [x] | A batch prekey claim | A claim names one account, so a group's first encrypted send is one request per member: a thousand members at 120 a minute is eight minutes of requests **Closed** as `{"users":[…]}`, at most `kMaxClaimBatch` = 32 accounts (duplicates once), answered `{"claims":[{"user","bundles":[…]\|null,"refused":null\|"NOT_FOUND"\|"RATE_LIMITED","retry_after":null\|s}]}` in the order asked. 32 is the response bound: about 100 KiB at five devices, 320 KiB at the sixteen-device ceiling. The caller's own membership and the conversation being encrypted refuse the whole request as before; the targets' memberships are one `$in` (`ChatRepository::current_among`, the existing `chat_members_in` shape) read before any budget, and the per-target budget is spent only for a member. Refusals are per account because the others' keys are already spent. The per-claimer budget is spent once per request, which is safe because the per-target rule is what bounds a victim's drain and it counts every claim. **The single `{user}` form is retired**, deliberately: encryption is not for production, nothing was written against it, and one shape is one decoder; a body naming `user` is `400 users REQUIRED`. 22 §7.5, §9 and 01 §18 updated. The listener case is extended: a batch of four naming bob (throttled, with `retry_after`), alice's own other devices, a stranger (`NOT_FOUND`) and bob again answers three entries in order, and no key leaves bob's pool; the old form and a batch of 33 are refused by name |
| [x] | A batch presence read | One account per call is one request per chat-list row **Closed** as a route id of its own, `presence_many`, `GET /chat/presence?users=<id>,<id>,…` in the reference application, answering `{"presence":[{"user","online","last_seen"}]}` in the order asked, a duplicate once. Bound `kMaxPresenceBatch` = 100, a chat-list page at its ceiling, the one screen that shows many accounts; past it `400 users TOO_LONG`, refused as the list is parsed so a long query is never held. `PresenceTracker::view_many` asks `may_see` per account and reads only the allowed ones, in one `MGET` and at most one `$in` for keys that have gone, so withheld and never-seen are the same bytes and the same cost. Presence off is the stealth 404, as the single read. 22 §8.3, §9 and 01 §18 updated. One listener case: an online account, a withheld one, an unknown one and a duplicate, in order, the withheld entry identical to the unknown's; a page and one refused by name, a malformed id refused |
| [x] | A mute duration | `muted_until` is an instant a client can only compute from its device clock **Closed** as `mute_for_s` (seconds, zero unmutes, at most `kMaxMuteDuration`, 366 days) and `mute_indefinitely: true` on the preferences route, the instant computed from the server's clock; indefinitely is stored as `kMutedIndefinitely`, 9999-12-31T23:59:59.999Z, so every push comparison already reads it as muted and no second representation exists. `muted_until` stays; naming two of the three is `400 mute_for_s NOT_ALLOWED`, and a longer duration `OUT_OF_RANGE`. **Beyond the row**: the route answered `204`, which left a client holding a duration it could not turn into the instant to show, so it now answers `200` with the membership after, as receipts do; `ChatService::preferences` returns the `MemberRecord`. 22 §5.1 and 01 §18 updated. One listener case: eight hours lands within a minute of the server's now plus eight hours and is answered as an instant, indefinitely answers the sentinel, zero unmutes, two ways and a year and a day are refused by name |
| [x] | A client id on `chat.create` | A retried create makes a second group, so a client cannot retry one **Closed** as a required `cid` on the create body (16 bytes, unpadded base64url; missing, malformed or nil is `400 cid REQUIRED`), stored on the conversation under a unique `{by, cid}` index partial on `cid` existing. A retry reads the first attempt after the kind, permission and title checks (so it is refused as the first would have been) and is answered with it and `200`; two attempts racing past the read collide on the index and the loser answers with the winner, as a send's do. `chat.create` is described idempotent. `ChatService::create` returns `CreatedConversation {conversation, membership, created}`; a caller that never retries (the reference server's seed) may pass no key, which the route never allows. 22 §3.1, §9, §9.1 and 01 §18 updated; the explain catalogue gains `chat_created`. One database case (four racing creates with one key make one group, and the same key from another creator is theirs) and one listener case (no key refused by name; a retry `200` with the first id) |
| [x] | Which device is mine | A client whose key store was evicted cannot find its orphaned device to unlink it **Closed** as `"mine"` on every device of the owner's listing, true exactly on the one whose recorded session is the caller's, written last in each device object (`…,"low":false,"mine":true}`). A boolean and never the session, which the listing has never carried. 22 §7.3.1 and 01 §18 updated. One listener case: the registering session sees `true`, another session of the account sees `false`, and neither body holds the session id |
| [x] | A relay for the link approval | The new device posts its own link, so the approver's signature has to travel back to it, and a desktop browser usually cannot scan a code **Closed** as a mailbox in a new `DeviceCollections::links` collection behind four route ids (all POST, tokens in bodies): `request_link` `{device_id, agreement_key, signing_key}` → `201 {"token","expires_in_s":600}`; `read_link_request` `{token}` → `{device_id, agreement_key, signing_key, expires_in_s}`; `approve_link_request` `{token, approver, timestamp, link_signature}` → `204`; `collect_link_approval` `{token}` → `{"approval":null}` while pending, once `{"approval":{approver, timestamp, link_signature}}`, then 404. Only the peppered digest of the 256-bit token is stored (as invites); one row per requesting session, replaced by its next request; `kLinkRequestLifetime` ten minutes, filtered on every read, TTL as collector. Same account only, and the collect only by the requesting session; anything else is the stealth 404. The approval is verified when it is LEFT, under the approver's stored key over the 119-byte link message, so a bad signature is the approver's `403` and not the new device's surprise; a second approval is `409`. Only public material is held. Two indexes (`{u, sid}`, TTL `{exp}`), one catalogue query. 22 §7.3.1, §9, §9.1 and 01 §18 updated. One listener case end to end: pending to the asker and 404 byte for byte to a stranger and to another session; read; a forged signature refused; approved once, a second 409; collected once, then 404; and the collected approval links the device |
| [x] | Signed-prekey and last-resort rotation | 22 §7.3.1: "a signed prekey cannot be rotated yet" **Closed** as `DeviceDirectory::rotate_prekeys` and `ChatService::rotate_prekeys` behind the `rotate_prekeys` route id, `PUT /chat/devices/{device}/keys` with `{signed_prekey, signed_prekey_signature}`, `{last_resort_key, last_resort_signature}` or both (`204`). Each new key is checked as a key and its signature verified under the device's STORED signing key over 22 §7.3.1's `anvil-chat-spk` / `anvil-chat-lrk` messages; the write's filter restates that signing key, so a device unlinked meanwhile changes nothing. Refusals: neither pair `400 signed_prekey REQUIRED`, a bad key or signature `400` naming it, not the caller's current device `403 device_id`. **No overlap is kept server-side**, deliberately: the old bundle was already handed to whoever claimed it, and completing a session begun from it needs the old private half, which is the device's store's to keep for its overlap. dv and dsv do not move. The device is touched. 22 §7.3.1, §9 and 01 §18 updated. Two database cases (both rotated and claimed at once, one alone; another device's signature, a signed prekey offered as last resort, another account, neither) and one listener case |
| [x] | Frame and text vectors as printed fixtures | `chat_frames_test.cc` and `chat_text_test.cc` are the contract a client's codec and validator are written against, and neither is printed **Closed** as two vector headers and two emitters, each header held by the existing test file. `tests/testapp/chat_frame_vectors.h`: the thirteen golden frames (the `Mutation` frame included) and the largest wake, and `refusals()`, every named refusal case of the test file plus two families over every golden, each short prefix (`frame.length`) and one trailing byte (`frame.length`, or `frame.size` past ClientTyping), 309 in all, each with its fault name and error code. `testapp_emit_chat_frames` prints `{format, frame_version, inline_wake_bytes, max_downstream_bytes, max_upstream_bytes, goldens:[{name, direction, hex, frame:{type,…fields}}], refusals:[{name, direction, hex, fault, code}]}`. `tests/testapp/chat_text_vectors.h`: 109 cases over the message, mention, preview and reaction validators, built in C++ (a bound of 4 096 Arabic letters is not spelled out), and `testapp_emit_chat_text` prints `{format, max_message_code_points, cases:[{name, validator, text_hex\|url_hex, max_code_points?, title_hex?, description_hex?, mentions?, expect:{accepted:true}\|{field, reason}}]}`, texts in hex because some are not UTF-8. Both emitters refuse to print a vector anvil does not answer as written. The refusal fields for body, mentions and reaction are `body`, `mentions` and `reaction`, the names the routes use. 22 §8.2 and 01 §18 updated. Two foundation cases hold anvil to every printed entry |
| [x] | The descriptor's page ceiling below one of its routes | Added during the phase: hammer's generator refused the reference descriptor, whose `limits.page_limit_max` was 100 while `chat.members` declared `limit_max` 200, and a client reads the global value as the server's ceiling **Closed** by lowering the member page to 100 (`kMaxMemberPage`, the route's and the service's clamp), not by raising the global to 200. Every other list here pages to 100 or less, a client sized for one ceiling is the point of having one, and the cost is a 1 024-member listing taking eleven pages instead of six, on a screen nobody scrolls to the end of. `descriptor::page_ceiling_covers` (constexpr) is asserted by the reference emitter, and `emit_descriptor` throws on a descriptor that breaks it, so the contradiction cannot be emitted again. 01 §14 and 22 §9 updated. One unit case: the reference tables pass, and a ceiling of 64 is refused by the predicate and by the emitter |
| [x] | A staff read of a conversation, and reports | Staff reviewing a reported dispute need a way into a conversation they are not in, audited on every read; a member needs a way to point them at a message **Closed** as a kind knob, four optional route ids and a report collection, with 22 §9.2 (new), §9.1 and 01 §18 updated. `ConversationKindSpec::reviewable` (default false; refused with `E2ee::Required`; published in `limits.chat`): per kind, so a kind promised private is unreadable by any permission, and visible in the table. `GET /chat/review/{c}` → `{conversation, members:[{user, role}], next}` and `GET /chat/review/{c}/messages?before=` → the history shape over the whole retained log, both `Stealth` behind the application's own bit (`ChatReview` = 34 here), anvil naming none. Every read writes an audit row synchronously first — `ChatServiceDeps::review = {AuditService*, AuditAction}`, actor the reader, subject the conversation, the client address — and is `503` if it cannot; an encrypted or non-reviewable conversation is `403 "reason":"chat.not_reviewable"`. No watermark moves and no receipt names the reader. `POST /chat/conversations/{c}/reports {from, to?, note?}` (≤ 500 messages, inside the reporter's range and the head; note ≤ 1 000 code points) → `201 {"id"}`, a retry over the same range `200` with the first (unique `{c, by, f, to}`); `ChatHooks::on_report`; `GET /chat/reports?after=` newest first. The four ids install all or none. The reference application declares the bit, the audit action (`ChatConversationReviewed` = 13), the `chat_reports` collection, the direct and group kinds reviewable, and an `AuditService` for the reads. Two listener cases: two staff reads audited twice, no watermark moved, absent from receipts, a member's attempt byte-identical to an unmatched route, an encrypted conversation refused with the reason; a report filed, retried, refused past the head and to a stranger, listed to staff and absent to a member |

---

## Standing rules for every task

- **Copy the comments too.** The reasoning is the reason this code is worth reusing. A
  mechanism whose justification was dropped gets undone by the next reader.
- **Where a comment cites a yardclub finding number**, replace the citation with the reasoning
  it points at. `anvil` has no finding register and a dangling `(F237)` is worse than no
  citation.
- **A `static_assert` that came across is not optional.** Each one turns a future mistake into
  a build error; dropping it to make something compile is the mistake.
- **No task is done without its test.** Test-suite parity per module is the acceptance bar.
