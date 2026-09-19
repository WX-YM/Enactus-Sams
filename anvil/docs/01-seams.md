# 01 — Seams

A seam is a place where anvil stops and your application starts. anvil holds no
application's data — not one route, permission, collection name, locale, section, field
type, job kind or topic — so everything of that shape arrives through one of the seams below.

Every seam obeys three rules:

1. **The table is `constexpr` and lives in `.rodata`.** It is shared by every thread and
   every request and costs nothing to initialise.
2. **A malformed table is a compile error**, not a runtime surprise. Each seam ships a
   `well_formed()` or `table_is_well_formed()` you `static_assert` on.
3. **A missing table fails at configure or compile time.** A seam discovered by a 500 at 3am
   is a design failure, not an operator error.

Two mechanisms implement them, and which one a seam uses follows one rule:

> **A compile-time seam only where a size must be a size.**

Locales dimension `std::array` members *inside anvil's own translation units*, so the locale
count has to be a literal when anvil is compiled — that seam needs a config header on the
include path. Everything else is only ever *looked up*, so a `constexpr`-initialised
`std::span` handed over at construction is enough and costs nothing.

| Seam | Mechanism | Reaches anvil by |
|---|---|---|
| Locales | config header | `#include <anvil_app_config.h>` from anvil's headers |
| Permissions | `constexpr` object | app-layer call sites; a `span` in a deps struct |
| Routes | `std::span` | `AccessControlDeps::routes` |
| Collections | `constexpr` table | repository constructors |
| Field types | `std::span` | `FormService` and `SubmissionService` constructors |
| Sections | `std::span` | `SectionService` construction |
| Jobs, rate limits | `std::span` | the owning service's constructor |
| Idempotency | `constexpr` struct | the `IdempotencyStore` constructor |
| Notification topics | `std::span` | `Publisher` and `Inbox` constructors |
| Notification templates | `std::span` | the render call, and the services that make it |
| Capability scopes | `std::span` | `CapabilityService` constructor |
| Audit actions | `std::span` | `AuditService` constructor |
| Queries to explain | `constexpr` table | the explain check, at test time |
| Metrics | `std::span` | the `MetricRegistry` constructor |
| Analytics events | `std::span` | the `EventService` constructor |
| Migration steps, collection options | `std::span` | `MigrationDeps`, at migration time |
| Route descriptions | `std::span` | `DescriptorInput`, at build time, and the session projection |
| Response shapes | `std::span` | `RouteDescription::response`, and the writer that walks it |
| Field types, sections, topics, events | the same spans they already arrive by | `DescriptorInput`, a second time, at build time |

---

## 1. Permissions

### What anvil ships

`PermSet` — 128 bits, 16 bytes, a membership check is one AND against a register. The
alternative, a `std::vector<std::string>` of permission names, is a heap block plus a string
compare per check, on every protected request.

`perm_mask(...)` is already generic and needs nothing from you:

```cpp
template <typename... Bits>
[[nodiscard]] constexpr PermSet perm_mask(Bits... bits) noexcept;
```

It `static_cast`s whatever enumerators it is handed, so your route table stays `constexpr`
and stays in `.rodata`.

`PermCatalogue` wraps your name table:

```cpp
struct PermName final { std::string_view name; std::uint8_t bit; };

class PermCatalogue final {
public:
    constexpr explicit PermCatalogue(std::span<const PermName> names) noexcept;

    [[nodiscard]] constexpr std::size_t size() const noexcept;
    [[nodiscard]] constexpr std::string_view name_for_bit(std::size_t bit) const noexcept;
    [[nodiscard]] constexpr std::optional<std::uint8_t> bit_for_name(std::string_view) const noexcept;

    template <typename Visitor>
    constexpr void for_each_name(const PermSet& held, Visitor&& visit) const;

    [[nodiscard]] constexpr PermSet all() const noexcept;
    [[nodiscard]] constexpr bool well_formed() const noexcept;
};
```

### What you write

```cpp
// perms.h
#include "anvil/core/perm_catalogue.h"

namespace myapp {

// Bit indices are stored in access tokens AND in users.perms.
// NEVER renumber. Retired bits are reserved, never reused — renumbering silently
// regrants access for every token currently in flight.
enum class Perm : std::uint8_t {
    ContentRead   = 0,
    ContentWrite  = 1,
    ContentDelete = 2,
    MediaUpload   = 8,
    MediaDelete   = 9,
    StaffManage   = 16,
    AuditRead     = 17,
};

inline constexpr std::array<anvil::PermName, 7> kPermNameTable{{
    {"ContentRead",   static_cast<std::uint8_t>(Perm::ContentRead)},
    {"ContentWrite",  static_cast<std::uint8_t>(Perm::ContentWrite)},
    {"ContentDelete", static_cast<std::uint8_t>(Perm::ContentDelete)},
    {"MediaUpload",   static_cast<std::uint8_t>(Perm::MediaUpload)},
    {"MediaDelete",   static_cast<std::uint8_t>(Perm::MediaDelete)},
    {"StaffManage",   static_cast<std::uint8_t>(Perm::StaffManage)},
    {"AuditRead",     static_cast<std::uint8_t>(Perm::AuditRead)},
}};

inline constexpr anvil::PermCatalogue kPerms{kPermNameTable};

static_assert(kPerms.well_formed(),
              "duplicate bit, duplicate name, empty name, or a bit outside PermSet");
static_assert(kPerms.size() == 7,
              "adding a permission without naming it is a build failure, not a bit the "
              "dashboard receives and cannot render");

}  // namespace myapp
```

### Notes that are not obvious

- **Names are never translated.** An investigator comparing a staff screen against a server
  log needs the same word on both.
- **`for_each_name` visits in bit order, not table order.** Two responses for the same holder
  are then byte-identical and a client can diff them.
- **`all()` is built from the names, never from `~PermSet{}`.** A reserved gap is not a
  permission the server understands, and granting one would put a control on a screen that
  authorises nothing.
- **`name_for_bit` returns empty for an undeclared bit.** Inventing a plausible name for a
  reserved gap is worse than returning nothing.
- `UserContext` holds `PermSet`, never your `Perm`, which is why it stays at exactly 64 bytes
  regardless of how many permissions you declare.

---

## 2. Locales

### Why this one is a config header

This is the seam that decides anvil's packaging, so the reasoning is worth stating in full.

`Localized<N>` holds `std::array<std::string_view, N>` — inline, no allocation. It is a member
of `FieldSpec` and `ImageSpec`, it is the return type of `read_localized` in `db/codec.cc`, and
`N` is the stride of the tier-1 section cache. All of that needs `N` as a literal, and all of
it lives inside anvil's own translation units.

Three alternatives were considered and rejected:

| Alternative | Why not |
|---|---|
| Pass a `std::span<const LocaleSpec>` at runtime | Cannot dimension `Localized<N>` — forces a heap `vector<string_view>` per localised value, in every `FieldSpec` and every decoded row. A memory cost, which outranks packaging convenience |
| Make every locale-dependent function a template | Instantiates bsoncxx- and Drogon-heavy code in every consuming translation unit, and still does not solve a cache member stored and swapped in a `.cc` |
| A hard-coded `kMaxLocales` ceiling | Wastes `16 * (kMaxLocales - N)` bytes per localised value and per registry field, *and* a whole `atomic<shared_ptr>` cache slot per section per unconfigured locale |

So: **anvil is a source dependency**, your application authors `anvil_app_config.h`, and
`ANVIL_CONFIG_INCLUDE_DIR` puts it on anvil's include path. The cost is that anvil cannot
ship as a prebuilt `.a`. For a library always built from source alongside its consumer, that
is the right trade.

### What anvil ships

```cpp
// anvil/core/locale_spec.h — deliberately has NO app dependency, which is what
// breaks the include cycle: your config header includes this one.
struct LocaleSpec final {
    std::string_view tag;        // 16  BSON subdocument key, ?lang= value, wire value
    std::string_view collation;  // 16  ICU locale for sorting. EMPTY means binary comparison
    bool             rtl;        //  1
};
static_assert(sizeof(LocaleSpec) == 40, "LocaleSpec must not grow padding");
```

```cpp
// anvil/core/locale.h — includes <anvil_app_config.h>
inline constexpr std::size_t kLocaleCount = config::kLocales.size();

class Locale final {                 // 1 byte. Only VALIDATING constructors exist, which is
public:                              // what lets Localized::get index without a bounds check
    [[nodiscard]] static constexpr std::optional<Locale> from_index(std::size_t) noexcept;
    [[nodiscard]] static constexpr std::optional<Locale> from_tag(std::string_view) noexcept;
    [[nodiscard]] constexpr std::uint8_t     index()     const noexcept;
    [[nodiscard]] constexpr std::string_view tag()       const noexcept;
    [[nodiscard]] constexpr std::string_view collation() const noexcept;
    [[nodiscard]] constexpr bool             rtl()       const noexcept;
};

template <std::size_t N = kLocaleCount>
struct Localized final {
    std::array<std::string_view, N> values;
    [[nodiscard]] constexpr std::string_view get(Locale l) const noexcept;
    [[nodiscard]] constexpr bool complete() const noexcept;   // all present AND non-empty
};
using LocalizedView = Localized<kLocaleCount>;
```

### What you write

```cpp
// anvil_app_config.h
#pragma once
#include <array>
#include <cstddef>
#include "anvil/core/locale_spec.h"

namespace anvil::config {

// THE ORDER IS PERSISTED. The index is byte 3 of every access token and the value
// in users.lang. APPEND ONLY: never reorder, never remove — exactly the rule Perm
// bit indices live under. Reordering silently reinterprets every stored row and
// every token in flight.
inline constexpr std::array<LocaleSpec, 2> kLocales{{
    {"en", "en", false},
    {"ar", "ar", true},
}};

// The locale a request naming none is answered in.
inline constexpr std::size_t kDefaultLocale = 0;

}  // namespace anvil::config
```

A single-locale application writes `{{{"en", "", false}}}` and pays nothing for the
machinery — `Localized<1>` is one `string_view` and the section cache is one slot per section.

### Notes that are not obvious

- **`complete()` requires every locale present *and non-empty*.** A missing translation is a
  validation error, never a silent fallback to another locale — a fallback renders a blank
  heading or the wrong language and nothing reports it.
- **Collation is one string, handed to both sides.** A sort with a collation whose index was
  built without it is a `COLLSCAN`. `db/collation.h` gives the query and the index the same
  `LocaleSpec::collation`, so they cannot disagree.
- **Empty collation means binary comparison**, which is correct and fastest for locales that
  need no linguistic ordering.
- **Token decode gains one compare.** An out-of-range locale byte is now
  `TokenError::Malformed` where a 1-byte enum accepted every value. Correct, and on the
  hottest security path, so it is worth knowing it is there.
- If you ever need to reorder locales, store the **tag** in `users.lang` instead of the index
  and keep the index on the wire only. Decide this before the first row is written, and write
  the decision next to `kLocales`.

---

## 3. Routes

anvil ships the policy machinery; you ship the table. `policy_for` is a linear scan — at a
few hundred entries this beats a hash, and it is the same argument the field-type registry
makes.

```cpp
// anvil ships
enum class RouteAccess : std::uint8_t { Public, Authenticated, Permissioned, Stealth };
enum class RouteMethod : std::uint8_t { Get, Post, Put, Patch, Delete, Any };

struct RoutePolicy final {
    std::string_view pattern;
    PermSet          required;
    RouteAccess      access;
    RouteMethod      method;
};

[[nodiscard]] constexpr const RoutePolicy* policy_for(
    std::span<const RoutePolicy> routes, std::string_view pattern, RouteMethod) noexcept;
[[nodiscard]] constexpr bool is_declared(
    std::span<const RoutePolicy> routes, std::string_view pattern, RouteMethod) noexcept;
```

```cpp
// routes.h
inline constexpr std::array<anvil::RoutePolicy, 4> kRoutes{{
    {"/login",    {},                                RouteAccess::Public,       RouteMethod::Post},
    {"/me",       {},                                RouteAccess::Authenticated,RouteMethod::Get},
    {"/content",  perm_mask(Perm::ContentWrite),     RouteAccess::Permissioned, RouteMethod::Post},
    {"/audit",    perm_mask(Perm::AuditRead),        RouteAccess::Stealth,      RouteMethod::Get},
}};
```

Installed once, in `main()`:

```cpp
anvil::accesscontrol::AccessControl::init({
    .keys = keys, .epochs = &authz, .denials = &audit, .routes = kRoutes,
});
```

**A route with no entry fails closed** — the filter logs and returns the stealth 404. Do not
rely on that: `declared()` throws at boot if a handler is registered for a pattern the table
does not name, so the failure is a startup crash rather than a route that quietly 404s in
production. A method-specific entry beats an `Any` entry for the same pattern.

### Register through `register_route`, and never type the filter name

```cpp
// anvil/accesscontrol/route_registration.h
template <typename FUNCTION>
void register_route(std::span<const RoutePolicy> routes, std::string pattern,
                    drogon::HttpMethod method, FUNCTION&& handler,
                    PublicContext public_context = PublicContext::Omit);
```

```cpp
// Instead of drogon::app().registerHandler(declared(...), &h, {Get, "anvil::…::AccessFilter"})
anvil::accesscontrol::register_route(kRoutes, "/audit", drogon::Get, &audit_handler);
```

It does what `declared()` did — throws when the table names no entry that would answer for
this `(pattern, method)` — and the thing `declared()` could not: it **attaches the filter**,
chosen from the policy, so the filter name is never typed in an application.

That distinction is the whole point, and it is worth being blunt about. The registry proves a
pattern *declares* a policy. Nothing proved the filter that *enforces* it was attached, and
nothing could: `drogon::app().getHandlersInfo()` returns pattern, method and description, and
carries no filter information at all. A boot sweep comparing the framework's routes against
your table compares those same two sets, so **an unguarded `Stealth` route passes it** —
declaring a pattern is exactly what an unguarded route also does.

In plain terms: a `Stealth` route registered without the filter is a *public* route, and every
check in this library and in your application reports green. The first `/admin` anybody writes
is where that happens. `register_route` makes the unguarded spelling unreachable rather than
discouraged — the same move `append_sanitized` makes for raw markup.

`Public` is the one class the filter does not enforce, so it does not get one by default. A
public route that renders differently for a signed-in visitor passes `PublicContext::Attach`
and gets a context when a valid token happens to be present; the epoch is still not consulted,
so that costs a decode and never a round trip.

`RouteAccess::Stealth` means a denial and a nonexistent route return byte-identical
responses. Stages 2 through 7 of the request pipeline touch no database, which is what makes
that indistinguishable by timing as well as by content. See `docs/04-access-control.md`.

---

## 4. Collections

Collection names are `constexpr` and **never derived from request data** — a collection name
built from a request field is an injection primitive that no amount of validation downstream
repairs.

```cpp
// collections.h
namespace myapp::collections {
inline constexpr std::string_view kUsers       = "users";
inline constexpr std::string_view kSessions    = "user_sessions";
inline constexpr std::string_view kMedia       = "media";
inline constexpr std::string_view kAuditLog    = "audit_log";
}

// The expiry field of every TTL'd collection, so a query against one can be made to filter
// on it explicitly. A TTL index is a garbage collector, not an access control: the monitor
// runs roughly every 60 seconds, so an expired session is still readable and would still
// authenticate. tools/check-db-discipline.sh fails the build on a query that omits it.
[[nodiscard]] constexpr std::string_view lifetime_expiry_field(std::string_view collection) noexcept;
```

---

## 5. Form field types

anvil ships the registry and the universal validators; you assemble the table and own the
numbering, because you own the disk.

```cpp
using FieldTypeCode = std::int32_t;

enum class FieldTypeFlag : std::uint8_t {
    None = 0,
    Pii             = 1U << 0,  // never written to `ans`; goes to the sealed envelope
    Options         = 1U << 1,  // >= 2 options; answers checked against the SERVER's list
    Attachment      = 1U << 2,  // resolves through the application's AttachmentHooks
    Ranged          = 1U << 3,  // min_value / max_value are meaningful
    CodePointCapped = 1U << 4,  // default_code_points / max_code_points are meaningful
    MultiLine       = 1U << 5,
    MultiSelect     = 1U << 6,
};

// A validator NEVER writes a PII value into `out`. It writes the normalised identity to
// `identity` and leaves out.kind untouched, so there is no branch anywhere that could store
// one as an ordinary answer. That is the one rule a flag cannot enforce, and the reason this
// table is reviewed as a unit.
using ValidateFn = Status (*)(const FieldSpec&, const RawAnswer&, Answer&, std::string& identity);

struct FieldTypeSpec final {
    std::string_view wire_name;            // 16
    ValidateFn       validate;             //  8
    std::uint32_t    default_code_points;  //  4  CODE POINTS, never bytes
    FieldTypeCode    code;                 //  4  stored on disk
    FieldTypeFlag    flags;                //  1
};
static_assert(sizeof(FieldTypeSpec) == 40, "FieldTypeSpec must not grow padding");

inline constexpr std::size_t kMaxPiiFieldsPerForm = 1;
```

```cpp
// field_types.h
namespace f = anvil::forms;
using F = f::FieldTypeFlag;

inline constexpr std::array<f::FieldTypeSpec, 10> kFieldTypes{{
    {"TEXT_SHORT",     &f::validators::text,             200, 0, F::CodePointCapped},
    {"TEXT_LONG",      &f::validators::text,            4000, 1, F::CodePointCapped | F::MultiLine},
    {"NAME",           &f::validators::text,             120, 2, F::CodePointCapped},
    {"NUMBER",         &f::validators::number,             0, 3, F::Ranged},
    {"EMAIL",          &f::validators::email,              0, 4, F::None},
    {"DATE",           &f::validators::date,               0, 5, F::None},
    {"SELECT_SINGLE",  &f::validators::select_single,      0, 6, F::Options},
    {"CHECKBOX_MULTI", &f::validators::checkbox_multi,     0, 7, F::Options | F::MultiSelect},
    {"IMAGE_UUID",     &f::validators::attachment_uuid,    0, 8, F::Attachment},
    // Yours. An identity number's rules are a jurisdiction's rather than a library's, so
    // anvil ships no PII validator at all — this is the extension point, and it is what
    // tests/testapp/field_types.h compiles to prove it works from outside.
    {"IDENTITY",       &myapp::identity,                   0, 9, F::Pii},
}};

static_assert(f::table_is_well_formed(kFieldTypes));
static_assert(f::is_dense_from_zero(kFieldTypes), "the lookup is a direct index");
static_assert(kFieldTypes.size() == 10, "adding a field type is a deliberate act");
```

Handed to both services at construction, along with the collection names and the attachment
hooks:

```cpp
anvil::forms::FormService       forms{db, "form_definitions", "form_submissions",
                                      kFieldTypes, attachments};
anvil::forms::SubmissionService submissions{db, "form_definitions", "form_submissions",
                                            kFieldTypes, attachments, pii_keys};
```

### Notes that are not obvious

- **`code` is stored on disk and a submission records the definition version it was validated
  against.** Old rows are read back through the field types their definition still names.
  Never renumber, never reuse a retired code. A code this build does not declare decodes to
  nothing, which is the correct direction to fail during a rolling deploy.
- **One identity field per form is a library rule, not a flag.** It is a property of the
  storage format — one `pii` envelope, one `pii_index`, and one AEAD AAD binding one envelope
  to one field id — not of any field type. A per-type flag would let you declare two PII types
  and get a silently single-valued write.
- **`has_pii` on a definition is derived from the table, never accepted from a client.** A
  form that under-declares its PII gets the wrong storage treatment and nothing downstream
  notices.
- **A field's flags and code-point cap are RESOLVED from the table, never stored.**
  `bind_field_type` is the one place either is written, and both `validate_schema` and the
  decoder go through it — so a field bound from a request body and the same field read back
  from the database cannot answer "is this multi-line" differently.
- **`answer_kind_of(flags)` is the contract a custom validator must meet.** It derives the
  BSON shape an answer takes, in one place, for the writer and the reader alike. The
  submission service checks the kind your validator produced against it, so a mismatch is a
  refused write rather than an unreadable row months later.
- **A null validator is checked by `validators_are_present()`, not by
  `table_is_well_formed()`.** Under `-fsanitize=undefined` this compiler will not fold a
  function-pointer null comparison into a constant expression, and a `well_formed()` that does
  not compile is worse than one that checks less — the same trade §10 makes. Call it at boot;
  the submission path also refuses a null validator by name rather than jumping through it.
- **An attachment field resolves through `AttachmentHooks`,** three callables you supply:
  `may_bind`, `bind` and `release`. anvil cannot know which storage namespace a form's
  attachments live in, nor what "owned by" means for a submitter with no account. Supply all
  three or none; a form that declares an attachment type with no hooks refuses every
  attachment, which is the safe direction.
- Writing a custom validator is the extension point: a free function matching `ValidateFn`,
  dropped into the table. Doc 13 has the rest.

---

## 6. Sections

The CMS splits along ownership, and the split is the whole design:

| | Owner | Lives in | Changes via |
|---|---|---|---|
| Registry — which sections exist, their fields, types, bounds, image sizes | Developers | `constexpr` in `.rodata` | a deploy |
| Content — the strings and image ids | Staff | MongoDB | an authenticated write |

### What anvil ships

`FieldSpec`, `ImageSpec`, `SectionSpec`, `SectionState`, the lookups, the `ct::`
constant-evaluation validators (`count_code_points`, `is_valid_utf8` rejecting overlongs and
surrogates, `is_integer_literal`, `is_bool_literal`, `is_hex_color`, `is_safe_default_url`) and
the four conformance checks. Everything takes the table as a `std::span`: a section table is
only ever looked up, and dimensions nothing anvil compiles.

### What you write

The section table and its default content, with four `static_assert`s tying them together:

```cpp
static_assert(anvil::sections::registry_is_sorted(kSections));
static_assert(anvil::sections::fields_fit_buffers(kSections));
static_assert(anvil::sections::choices_are_well_formed(kSections));
static_assert(anvil::sections::defaults_match_registry(kSections, kDefaults),
              "a default violates its FieldSpec, is missing a locale, is not valid UTF-8, "
              "or a section has no defaults at all");
```

`tests/testapp/sections.h` is the compiled worked example.

### Notes that are not obvious

**A field's label is `Localized<>`, one per declared locale.** These are labels for the STAFF
editor, not public copy — the public site renders values, never field names — but a staff
member's editor is in their language too. `FieldSpec` is therefore
`(2 + kLocaleCount) * sizeof(string_view) + 8` bytes and `ImageSpec` is
`(1 + kLocaleCount) * sizeof(string_view) + 8`; both are asserted as formulas rather than as
numbers, because the number is the application's and what must not change is that there is no
interior padding.

**`FieldType::Choice` carries its own allow-list, and anvil never ships one.** A vocabulary of
icon names, badge styles or layout variants is one product's. anvil ships the type, the binary
search and the sortedness check; the `std::span<const std::string_view>` on the field is
yours. The choices travel with the field on the registry endpoint, so an editor renders a
picker without holding a second copy that drifts.

**`FieldType::Image` in a `FieldSpec` is a build error**, caught by `fields_fit_buffers`.
Images are declared in `SectionSpec::images`; a field whose value is a media id would bypass
the `ImageSpec` dimension check entirely.

**`fields_fit_buffers` bounds a real stack array.** `index_content` builds one pointer per
declared field and per declared slot in a `std::array`, which turns canonicalisation and
serialisation from O(fields²) linear searches into one pass. `kMaxFieldsPerSection` is what
keeps that on the stack.

**`kDefaults` is asserted positionally identical to `kSections`**, so bootstrap walks both by
index rather than looking anything up. `bootstrap_sections` takes spans and re-checks it at
runtime as well: a caller can hand it two tables that were never asserted together, and walking
by index across mismatched ones would write one section's defaults into another section's
document.

**`BootstrapReport` has two numbers about images and they answer different questions.**
`images_missing` is this boot's resolver failures; `image_slots_unbound` is declared slots with
nothing bound in storage once the boot is over. They agree on a fresh database and can disagree
everywhere else, because bootstrap binds an image only into a section it CREATES. Read the
second one for the state of the site.

### The one hook this seam carries

`SectionServiceConfig::on_invalidated` — `std::function<void(std::string_view key)>`, empty by
default — is called once per invalidated section key, **after** this instance's entry for it has
been dropped, on whichever thread dropped it. It is the seam for a cache ABOVE this library's
cache: a consumer holding typed values for a renderer has no other way to hear that a key
changed, because `invalidate_local` and the Redis subscriber are both internal.

It fires on the **subscriber's** path as well as the local write path, which is the whole point
— a hook that only fired where the write happened is the workaround it replaces. It **must not
block**: on that path it runs on the one thread draining the subscription. `key` is always a key
the registry declares, and it is the registry's own `.rodata` `string_view` rather than a view
into a message buffer. An exception escaping it is caught and logged.

Doc 12 §6 has the rest of the invalidation design, and doc 12 §8 the bootstrap report.

---

## 7. Jobs, rate limits and idempotency

Three smaller seams. The first two are a `std::span` handed to the owning service at
construction; the third is a `constexpr` struct of four numbers.

**Jobs.** anvil ships the queue, the lease, the dead-letter and the recurring-bucket logic.
You ship a table binding a stable string key to a handler function pointer. The key is stored
in Redis, so it is append-only for the same reason permission bits are. Every handler must be
**idempotent** — the queue is at-least-once and a crashed worker's claim is reclaimed, not
lost.

**Rate limits.** anvil ships `RateLimitRule`, the sliding window, the shared Redis counter and
the local fallback buckets. You ship the constants, in one table, and assert it:

```cpp
inline constexpr std::array<anvil::http::RateLimitRule, 3> kRateLimits{{
    {"login",  std::chrono::seconds{60}, 20},
    {"signup", std::chrono::minutes{60},  5},
    {"media",  std::chrono::minutes{1},  20},
}};
static_assert(anvil::http::rate_limit_table_is_well_formed(kRateLimits));
```

`tests/testapp/rate_limits.h` is the compiled worked example. The check refuses an empty bucket
name, a zero window whose Redis key never expires, a zero budget that refuses the first
request, and **two rules sharing a bucket** — which makes them share a counter, so the tighter
of the two budgets is spent by the other's traffic. None of the four is visible in a review of
the table itself.

Buckets are keyed by **identity plus rule, never by route**. A per-route bucket is an
existence oracle, which defeats the stealth 404 the route table went to the trouble of
declaring — so two routes that must be indistinguishable share a bucket deliberately.

**Per-IP and per-identity are two rules and both apply.** The identity bucket stops a
distributed spray against one victim; the IP bucket stops one host spraying across many. An
identity bucket alone lets an attacker lock somebody out by failing their password on purpose.

**A refusal carries `Retry-After`.** `RateLimitVerdict::remaining` is what is left of the
window, read from Redis inside the same script as the `INCR` — no second round trip, and no
process computing it from a clock it does not share with the one that opened the window.
`retry_after_seconds()` turns it into the header: rounded **up**, never **zero**, clamped to
the rule's own window. Each of the three is a header a client honours incorrectly if it goes
the other way, and the failure is invisible — a client that comes back too early is answered
429 again and looks exactly like one that was always going to be refused.

### Idempotency

anvil ships the store; you ship the four numbers and the key the client sends.

```cpp
inline constexpr anvil::http::IdempotencyConfig kIdempotency{
    .retention      = std::chrono::minutes{15},
    .in_flight_ttl  = std::chrono::seconds{30},
    .max_body_bytes = 16 * 1024,
};
static_assert(anvil::http::idempotency_config_is_well_formed(kIdempotency));
```

It exists because `RouteDescription::idempotent` is only half an answer. For a route where
repeating is unsafe, a client that loses a response has no good option: retrying risks a
second write, and not retrying turns a dropped packet into a user-visible failure on an
operation that probably succeeded. The server is the only participant that knows which, so it
is the one that settles it — the client sends a key it chooses, the server records the
response against it, and a repeat is answered from the record rather than performed again.

A claim returns one of five states and **each has exactly one correct response**, which is why
it is an enum and not a pair of flags: `Fresh` (do the work), `InFlight` (409, do not),
`Replay` (send the stored status and body), `Completed` (409 — the work happened once and the
body was not retained) and `Mismatch` (409 — this key was used for a *different* request, so
replaying would answer one request with another's response).

Notes that are not obvious:

- **The identity is part of the key, and an empty one is refused.** An empty identity merges
  every anonymous caller into one namespace, where the first client to use the key `1` is
  replayed to every other client that picks it. That is the worst failure this has, and it is
  the one a caller reaches by passing a default-constructed span — so it is a refusal rather
  than a default.
- **So is the route id**, and it is `RouteDescription::id` (§14). One client reusing a key
  across a create and a delete would otherwise be answered the create's 201 to its delete.
- **It does not degrade when Redis is unreachable; it fails the request.** The rate limiter
  falls back to a per-process bucket, because a weaker limit beats none. A process-local
  idempotency store answers "fresh" for every retry that reaches a different instance, which
  is not a weaker guarantee but a confidently wrong one — and what it produces is the
  duplicate write the store exists to prevent.
- **`in_flight_ttl` is the request deadline**, not a tuning knob. Longer than a request can
  run and a client is blocked from retrying work that will never finish; shorter and two
  attempts overlap. A process killed mid-request leaves its marker to expire, which is how the
  store survives a `SIGKILL`.
- **Record a failure too.** `record()` with the error response is correct whenever the handler
  failed *with* side effects: the retry learns what the first attempt learnt and the work is
  not repeated. `release()` is for the other case — nothing was written — and only for it.
- **A route protected by this needs a rate-limit rule too.** The keys are attacker-chosen, so
  the number of records is, and every one occupies Redis memory until its TTL runs out. What
  bounds it is the stage before it: the claim is stage 9 and the per-IP limiter refuses at
  stage 3, so what one caller can create in a retention window is their budget rather than
  their bandwidth. That is why the two seams are in one section.
- **Whether a request that sent no key reaches the store at all is yours to decide.** anvil
  never reads the request. "Refuse a non-idempotent route that sent no key" and "run it
  unprotected" are both defensible depending on who the clients are.
- **It makes a retry inside the retention window safe. It does not make an operation
  exactly-once forever.** Past the window a repeat is a fresh request, which is the correct
  trade: a store that never forgot, keyed by attacker-chosen strings, is a memory-exhaustion
  vector rather than a guarantee.

---

## 7a. Notification topics

A topic is a compile-time channel kind, optionally scoped to a subject resource, and it is
never derived from request data. The reason is sharper here than for a collection name:
**subscription is the disclosure.** A caller who can subscribe to another account's topic
receives a stream of their activity, so a topic a caller can name as a string is a topic a
caller can name as anything.

### What anvil ships

```cpp
using TopicCode = std::uint8_t;                    // 0..63. STORED, and a bit position
inline constexpr std::size_t kMaxTopicKinds = 64;

enum class FanOut  : std::uint8_t { Write, Read };
enum class Scope   : std::uint8_t { Global, Resource, Account };
enum class ClientType : std::uint8_t { InApp, WebPush, Email, Webhook };

struct TopicSpec final {
    std::string_view key;                // 16  never translated, never parsed from a request
    PermSet          required;           // 16  empty for a public topic
    std::uint32_t    retention_days;     //  4
    std::uint16_t    coalesce_window_s;  //  2  0 disables coalescing
    TopicCode        code;               //  1  STORED. Append only
    FanOut           fanout;             //  1
    ChannelMask      default_channels;   //  1
    Scope            scope;              //  1
    bool             stealth_on_denial;  //  1
    bool             user_optional;      //  1
};
static_assert(sizeof(TopicSpec) == 48, "TopicSpec must not grow padding");
```

plus `TopicRef` (24 bytes), the `Preferences` masks, the lookups, and `should_deliver`.

### What you write

```cpp
inline constexpr std::array<anvil::notifications::TopicSpec, 5> kTopics{{
    {"content.published", PermSet{}, 90, 0, 0, FanOut::Read,
     channels(ClientType::InApp, ClientType::WebPush), Scope::Global, false, true},
    {"session.new_device", PermSet{}, 365, 0, 3, FanOut::Write,
     channels(ClientType::InApp, ClientType::Email), Scope::Account, false, false},
    // ...
}};

static_assert(anvil::notifications::topic_table_is_well_formed(kTopics));
static_assert(anvil::notifications::topics_are_dense_from_zero(kTopics));
```

`tests/testapp/topics.h` is the compiled worked example.

### Notes that are not obvious

- **`fanout` is compile-time because a strategy chosen at runtime can flip under load.**
  Fan-out on write is O(subscribers) inserts, which for a broadcast to 20 000 is a write storm
  that stalls `db_pool`; fan-out on read is one insert and a merge at read time. Picking one
  globally is the mistake — so it is per topic, and a broadcast to 20 000 costs what a
  broadcast to three costs.
- **`user_optional: false` is a security decision, and the exemption lives in the RULE.**
  `should_deliver` ignores the stored preference mask for such a topic, so it cannot be muted
  by a client row an older build wrote or by a hand-edited document. An account that can
  silence its own "new sign-in" alert has no alert.
- **`Scope::Account` means the subject IS the reader's own id.** That is what keeps a targeted
  security topic from matching every client subscribed to the kind and mailing one person's
  sign-in to everybody. `Scope::Resource` is the one a blanket default subscription must never
  be written for, because subscribing to a permission-gated topic is the disclosure.
- **`code` is stored AND is a bit position**, which is why the ceiling is 64 and why exceeding
  it is a build error rather than a topic that silently cannot be disabled.
- **`default_channels` may be neither empty nor `kDefaultChannels`.** Empty is a topic that
  publishes into nothing while looking correct; the sentinel means "whatever the topic
  declares", so a topic declaring it declares nothing.

### The two hooks the table needs

The table declares topics; two questions about them are still the application's, so
`PublishService` takes a `PublishHooks` alongside it:

```cpp
anvil::notifications::PublishHooks hooks{};
hooks.may_receive = [&](mongocxx::client& db, const Uuid& user, const PermSet& required) {
    return authz.holds(db, user, required);
};
hooks.enqueue_transports = [&](const Uuid& id, ChannelMask channels) {
    return queue.schedule_in(kSendKind, envelope(id, channels), std::chrono::seconds{0},
                             idempotency_key_for(id));
};
```

- **`may_receive` is asked at DISPATCH, per recipient, and never trusted from publish time.**
  A staff member who lost the bit between subscribing and the notification being sent must not
  receive it — on a `stealth_on_denial` topic that row is itself the disclosure. A topic with a
  non-empty `required` and no probe to ask **fails closed**: nothing is delivered and the row
  stays in the outbox, because delivering to everyone because nobody supplied the check is the
  one outcome that cannot be walked back.
- **`enqueue_transports` gets ONE job for the whole notification**, never one per subscriber —
  the sender pages the audience itself. It must be idempotent, because the outbox sweeper
  re-runs it; derive the queue's idempotency key from the notification id.

---

## 7b. Notification templates

A notification stores a template id and parameters, **never rendered text**. Storing
`{title, body}` per row copies the same two strings once per recipient under fan-out on write
and freezes the language at send time — a reader who switches locale still reads the
notifications they already received in the old one.

### What anvil ships

```cpp
using TemplateId = std::uint8_t;                   // STORED. Append only

struct TemplateSpec final {
    Localized<>  title;        // 16 * kLocaleCount
    Localized<>  body;         // 16 * kLocaleCount
    TemplateId   id;
    std::uint8_t param_count;
};

[[nodiscard]] Result<Rendered> render(std::span<const TemplateSpec>, TemplateId, Locale,
                                      std::span<const Param>);
```

### What you write

```cpp
inline constexpr std::array<anvil::notifications::TemplateSpec, 5> kTemplates{{
    {{{"New post", "منشور جديد"}}, {{"{t} was published", "تم نشر {t}"}}, 0, 1},
    // ...
}};

static_assert(anvil::notifications::template_table_is_well_formed(kTemplates));
```

### Notes that are not obvious

- **The grammar is `{` one ASCII letter `}` and nothing else**, and anything else containing
  `{` is a *build failure* rather than text that renders literally: `{a.b}`, `{0}`, `{total}`,
  `{{t}}` and a bare `{` all refuse to compile. "It renders as literal text" would be a claim
  about the renderer that the next change to the renderer could quietly break. A stray `}` is
  prose and is left alone — it is punctuation in somebody's copy, where `{` opens the only
  construct there is.
- **The placeholder set must match across every locale, compared order-INSENSITIVELY.** Arabic
  phrasing legitimately puts `{n}` where English puts `{t}`, and demanding the same order would
  force a translator to write an unnatural sentence. What is refused is a translation that
  *drops* a placeholder, which renders a sentence with a hole in it in one language only.
- **A placeholder with no matching parameter renders as nothing, never as `{t}`.** A row whose
  params were written by an older build degrades to a shorter sentence; the alternative puts
  the template's internals into a reader's inbox.
- **Parameters are re-validated at render, not only at publish.** A stored row is data from
  another process: it gets the treatment a request would, including the bidi-override check —
  U+202E in a notification body spoofs the whole sentence and the reader cannot see that it
  did.

---

## 8. Capability scopes

A capability token is a short, scoped grant redeemed by a **later, separate** request — an
upload slot, a destructive-action confirmation, a preview of unpublished content. anvil ships
the mechanism: mint, store only a peppered digest, and redeem through one atomic operation
whose filter carries the whole binding. The LIST of scopes is yours, because a scope names one
of your operations.

### What anvil ships

```cpp
struct CapabilityScopeSpec final {
    std::string_view name;        // the audit vocabulary; never translated
    std::int32_t     value;       // STORED. Append only
    bool             single_use;  // whether redemption burns it
};

[[nodiscard]] constexpr bool capability_table_is_well_formed(
    std::span<const CapabilityScopeSpec> table) noexcept;
```

`CapabilityScope` wraps the stored `int32` in a type of its own, so a function taking a scope
cannot be handed an error code by a caller whose arguments went in the wrong order.

### What you write

```cpp
#include "anvil/identity/capability_spec.h"

namespace myapp {

// STORED as int32. APPEND ONLY — never renumber, never reuse a retired value.
enum class Scope : std::int32_t {
    ContentDelete = 1,
    MediaUpload   = 2,
    DraftPreview  = 3,
};

inline constexpr std::array<anvil::identity::CapabilityScopeSpec, 3> kScopes{{
    {"ContentDelete", 1, true},
    {"MediaUpload",   2, true},
    {"DraftPreview",  3, false},   // read-only, presented on every page load
}};

static_assert(anvil::identity::capability_table_is_well_formed(kScopes));

}  // namespace myapp
```

### Notes that are not obvious

- **There is no unscoped scope, and there must never be one.** An unscoped capability is a
  bearer token with authority over everything and a replay window as long as its lifetime.
- **`single_use` decides whether `redeem()` consumes or verifies**, and it comes from the
  table rather than from the caller. A caller that chose could choose wrong, and choosing
  "verify" for a single-use scope is precisely the double-spend the atomic consume exists to
  prevent.
- **Zero is reserved** and the well-formedness check refuses it, so a default-initialised
  value can never name a real scope.
- **A value this build does not declare redeems nothing.** During a rolling deploy that means
  a token minted by a newer process is refused by an older one, which is the correct direction
  to fail: the alternative is an older process acting on a grant whose meaning it does not
  know.

---

## 9. Audit actions

anvil ships the sink, the buffer, the shedding policy and the append-only collection. WHAT
gets audited is a list of your own operations.

### What anvil ships

```cpp
enum class AuditClass : std::uint8_t { Change, Traffic };

struct AuditActionSpec final {
    std::string_view name;   // never translated
    std::int32_t     value;  // STORED. Append only
    AuditClass       cls;
};
```

### What you write

```cpp
inline constexpr std::array<anvil::audit::AuditActionSpec, 3> kAuditActions{{
    {"LoginSucceeded",         1, anvil::audit::AuditClass::Change},
    {"AccessDenied",           2, anvil::audit::AuditClass::Traffic},
    {"StaffPermissionChanged", 3, anvil::audit::AuditClass::Change},
}};

static_assert(anvil::audit::audit_table_is_well_formed(kAuditActions));

// Which action anvil's own denial sink writes. anvil cannot guess it.
inline constexpr anvil::audit::AuditAction kDenialAction =
    anvil::audit::AuditAction::of(Action::AccessDenied);
```

`AuditService` takes both: the table, and the one action that denials are recorded under.

### Notes that are not obvious

- **`cls` is the load-bearing column, not a label.** It decides what may be lost when the sink
  cannot hold everything. A `Change` is never dropped while a `Traffic` row is buffered; the
  buffer evicts oldest-traffic-first to make room, and refuses a change only when it holds
  nothing but changes — which is a database outage rather than a load problem.
- **Classify the denial action as `Traffic`.** It is the row a flood produces, it is
  compressible, and classifying it as a change means a burst fills the buffer with identical
  denials and then discards the permission change written in the middle of them.
- **An action this build does not declare classifies as `Change`.** That default is the safe
  direction: an unknown row is treated as the only copy of something, so a rolling deploy that
  introduces a new action cannot make the older process shed it as compressible traffic.
- **Names are never translated.** An investigator comparing a screen against a server log
  needs the same word on both, and a localised action name makes the two impossible to line up
  — which is the one thing such a screen exists to do.
- **The numbering is read back long after this build is gone.** Retention is measured in
  hundreds of days, which is longer than any deploy cycle, so a reader must be able to say "I
  do not know this action" rather than invent a plausible name for it. `audit_action_name`
  returns empty for exactly that case.

---

## 10. The queries to explain

The counterpart of the index catalogue. Adding a query without adding its index in the same
commit is not allowed (ENGINEERING_RULES.md §7), and that rule is unenforceable by inspection: a
collection scan over four hundred rows in a developer's database is indistinguishable from an
index scan, and stays that way until the collection has four hundred thousand.

anvil ships the check. You ship the list of query shapes it runs against a live server.

```cpp
struct QuerySpec final {
    std::string_view name;        // for the failure message
    std::string_view collection;
    FilterFn         filter;      // a function that BUILDS the filter
    FilterFn         sort;        // nullptr when the query is unsorted
};
```

A BSON document is not a constant expression but the table has to be `constexpr`, so the
table holds function pointers — the same shape, and the same reason, as `PartialFilterFn` in
the index catalogue.

### Notes that are not obvious

- **The filter's VALUES do not matter.** A planner picks a plan from the query's SHAPE, so a
  spec built with placeholder ids proves exactly what it needs to against an empty collection.
- **The sort is part of the shape.** A filter that rides an index perfectly can still force a
  blocking in-memory sort, which is bounded by a server-side memory budget and fails outright
  past it — so a query that passes today on a small collection stops working at a size nobody
  chose. That is a separate failure with a separate fix, and it is reported separately.
- **A partial index needs its filter repeated verbatim.** The planner uses one only when it
  can prove the query is a subset of the index's filter, so a predicate that means the same
  thing in different words gets a collection scan and nothing says so.
- **A driver error is a violation, not an exception.** A check that could not reach the server
  has not passed, and reporting that as a crash loses the other twenty queries it had not
  reached yet.

---

## 11. Metrics

anvil ships the registry, the cells, the snapshot and the OpenMetrics writer. WHAT gets
counted, beyond anvil's own mechanisms, is a list of your own.

### What anvil ships

```cpp
enum class MetricKind : std::uint8_t { Counter, Gauge, Histogram };

struct LabelSpec final {
    std::string_view                  name;
    std::span<const std::string_view> values;   // the ENTIRE value space
};

struct MetricSpec final {
    std::string_view             name;      // [a-zA-Z_:][a-zA-Z0-9_:]*
    std::string_view             help;      // shown in the scrape; never empty
    std::span<const LabelSpec>   labels;    // at most kMaxLabels (3)
    std::span<const std::int64_t> buckets;  // Histogram only; <= 12, increasing
    MetricKind                   kind;
    MetricUnit                   unit;
};

[[nodiscard]] constexpr bool metric_table_is_well_formed(
    std::span<const MetricSpec> table) noexcept;
```

And `kInternalMetrics` — the one table anvil populates rather than ships, defended by name in
[`17-analytics.md`](17-analytics.md) §3.

### What you write

```cpp
namespace m = anvil::analytics;

inline constexpr std::array<std::string_view, 3> kPlanValues{"free", "pro", "team"};
inline constexpr std::array<m::LabelSpec, 1> kCheckoutLabels{{{"plan", kPlanValues}}};

inline constexpr std::array<std::int64_t, 5> kLatencyBucketsUs{1000, 10000, 50000,
                                                               250000, 1000000};

inline constexpr std::array<m::MetricSpec, 2> kMetrics{{
    // The writer appends `_total`, so the name here must NOT carry it.
    {"checkout_completed", "Checkouts that reached a paid state", kCheckoutLabels, {},
     m::MetricKind::Counter, m::MetricUnit::None},
    // A metric declaring a unit must END with it: the scrape carries a `# UNIT`
    // line, and a name that disagrees with it is a metric two collectors
    // interpret differently.
    {"checkout_latency_microseconds", "Wall time from cart to receipt", {},
     kLatencyBucketsUs, m::MetricKind::Histogram, m::MetricUnit::Microseconds},
}};

static_assert(m::metric_table_is_well_formed(kMetrics),
              "an empty, duplicate or ungrammatical name; an empty help; a counter named "
              "_total; a name that disagrees with its unit; non-increasing buckets; a "
              "duplicate label; an anvil_ prefix; or a cell count past the ceiling");
```

`tests/testapp/metrics.h` is the compiled worked example.

### Notes that are not obvious

- **A label's VALUE set is part of the declaration, not a runtime concern.** That is what lets
  `well_formed()` multiply the value spaces and refuse a table above `kMaxCells` — the
  registry's memory cost is a compile-time constant, and a cardinality explosion is a build
  failure rather than an OOM under load. See [`17-analytics.md`](17-analytics.md) §6.
- **The ceiling counts cells, not series.** A histogram series is `buckets + 3` cells — one per
  boundary, an overflow, a `_sum` and a `_count` — so twelve buckets and four label values is
  sixty cells, not four. A ceiling that counted series would not bound anything.
- **`observe()` takes label INDICES, and there is no `string_view` overload.** Not as a
  convenience, not as an escape hatch. A function that cannot be handed a request byte cannot
  be made to accept one by a refactor that was not thinking about metrics.
- **The `anvil_` prefix is enforced in both directions.** Your table may not use it; anvil's
  may not omit it. A reader of a scrape always knows which side a series came from, and the
  two can never collide.
- **`help` is never empty and never translated.** It is read by an operator against a server,
  in the same place the metric name is, and a localised one makes two scrapes from two
  processes impossible to line up.
- **Bucket boundaries are integers in the declared unit.** A floating-point boundary makes two
  processes disagree about which bucket a value fell in, and a bucket that differs by one
  between instances is indistinguishable from a real signal. There is deliberately no
  `Seconds` unit for that reason: a pool wait expressed in whole seconds has one useful
  boundary, so durations are declared in **microseconds** and the name says so.
- **The name carries the unit as its last segment**, and the check enforces it. The scrape
  emits a `# UNIT` line beside the name; a series called `checkout_latency` that declares
  microseconds is read as seconds by anything that trusts the name and as microseconds by
  anything that reads the metadata, and the two disagree silently.
- **A metric removed from the table is a series that vanishes from the scrape.** Unlike every
  other table here, nothing is persisted, so there is no renumbering hazard — but a dashboard
  is a consumer too, and it breaks silently.

---

## 12. Analytics events

anvil ships the buffer, the shedding policy, sessionisation, the rollup arithmetic and the
collections' shape. WHAT is worth recording is a list of your own product's moments.

### What anvil ships

```cpp
using EventCode = std::int32_t;                 // STORED. Append only, forever
enum class EventClass : std::uint8_t { Behaviour, Conversion };

struct DimensionSpec final {
    std::string_view                  name;
    std::span<const std::string_view> values;   // a closed set, like a metric label
};

struct EventSpec final {
    std::string_view                name;
    std::span<const DimensionSpec>  dimensions; // at most 4
    EventCode                       code;       // STORED
    EventClass                      cls;
    bool                            requires_consent;
};

[[nodiscard]] constexpr bool event_table_is_well_formed(std::span<const EventSpec>) noexcept;
[[nodiscard]] constexpr bool events_are_dense_from_zero(std::span<const EventSpec>) noexcept;
```

### What you write

```cpp
namespace a = anvil::analytics;

inline constexpr std::array<std::string_view, 3> kSurfaceValues{"web", "ios", "android"};
inline constexpr std::array<a::DimensionSpec, 1> kSurface{{{"surface", kSurfaceValues}}};

inline constexpr std::array<a::EventSpec, 3> kEvents{{
    {"PageViewed",       kSurface, 0, a::EventClass::Behaviour,  true},
    {"SignupStarted",    kSurface, 1, a::EventClass::Behaviour,  true},
    {"SignupCompleted",  kSurface, 2, a::EventClass::Conversion, false},
}};

static_assert(a::event_table_is_well_formed(kEvents));
static_assert(a::events_are_dense_from_zero(kEvents),
              "the lookup is a direct index; a sparse table turns it into a scan per event");
static_assert(kEvents.size() == 3,
              "adding an event is a deliberate act: the code is stored on disk and can never "
              "be renumbered or reused");
```

`tests/testapp/events.h` is the compiled worked example.

### Notes that are not obvious

- **`cls` is the load-bearing column, not a label** — the same sentence §9 writes about
  `AuditClass`, and it is true here for the same reason. A `Conversion` is never dropped while
  a `Behaviour` row is buffered. Classify a page view as a conversion and a refresh storm
  evicts the signup it was hiding.
- **`requires_consent` is refused at the door, not filtered later.** An event declaring it is
  never buffered and never written without consent. "Recorded and then excluded from queries"
  is a policy one forgotten `$match` away from being no policy at all.
- **`code` and a dimension VALUE INDEX are both stored.** They join the locale index, the
  permission bit, the namespace index, the field-type code, the audit action value and the
  notification template id on the list of numbers this library can never renumber. Rollups
  computed months ago carry them.
- **A dimension is a closed set for the same reason a metric label is.** An event carrying a
  free-text dimension is a collection whose index cardinality is chosen by a visitor. At most
  **255 values**, because the index is one byte on the wire and in memory — `0xFF` is the
  absent slot — and a closed set larger than that is a dimension that wants to be an event.
- **An unknown `code` fails in two directions, and both record less.** A code this build does
  not declare classifies as `Behaviour`, so a row an older process cannot interpret can never
  evict one it can; and it is treated as *requiring consent*, so a rolling deploy does not
  record what the newer process would have refused. Both matter during a rolling deploy and
  nowhere else, which is exactly when nobody is watching.
- **The three collections are yours to declare**, in `config::kCollections`, like every other
  collection. anvil names none of them. Two of them want the high-churn database, and
  [`17-analytics.md`](17-analytics.md) §15 says why.

---

## 13. Migration steps and collection options

anvil ships the runner, the cursor, the batching, the ledger, the lock and the dry run. WHAT
moves is a list of your own transforms. This is the second half of the index catalogue in §10:
that one declares the indexes, this one declares the documents.

### What anvil ships

```cpp
enum class Cursor  : std::uint8_t { IdRange, WholeCollection };
enum class StepOutcome : std::uint8_t { Ok, Failed };

// The client is for READS. Writes are accumulated and the RUNNER decides whether
// to execute them, which is what makes --dry-run a property of anvil rather than
// a promise each step makes individually.
//
// set() and unset() take the FIELDS a document should carry, never an update
// document — the runner is what wraps them in $set / $unset. So `$inc` is not
// something a step is able to say, which is the idempotence argument as a type
// rather than as a rule.
class StepContext final;

using StepFn = StepOutcome (*)(StepContext&,
                               std::span<const bsoncxx::document::view>) noexcept;

struct MigrationStep final {
    std::string_view name;               // STORED as the ledger _id
    std::string_view collection;
    StepFn           apply;
    std::uint32_t    batch_size;
    std::int32_t     min_schema_version; // refuses to run before its indexes exist
    Cursor           cursor;
};

[[nodiscard]] constexpr bool step_table_is_well_formed(
    std::span<const MigrationStep> table) noexcept;
```

```cpp
enum class Granularity : std::uint8_t { None, Seconds, Minutes, Hours };
using ValidatorFn = bsoncxx::document::value (*)();

struct CollectionOptionsSpec final {
    std::string_view collection;
    std::string_view timeseries_time_field;
    std::string_view timeseries_meta_field;
    std::int64_t     capped_size_bytes;     // <= 0 = not capped
    std::int64_t     capped_max_documents;
    ValidatorFn      validator;             // nullptr = none
    bool             clustered_on_id;
    Granularity      granularity;
};

[[nodiscard]] constexpr bool collection_options_are_well_formed(
    std::span<const CollectionOptionsSpec> specs) noexcept;
```

`CollectionOptionsSpec` is in `db/collection_options.h` rather than beside `MigrationStep`, and
there is no separate `ValidatorSpec`: a validator is one `ValidatorFn` on the collection's own
entry, which is the shape `PartialFilterFn` and `FilterFn` already use and one type fewer to
learn. The two tables still arrive in the same `MigrationDeps` bundle, because one invocation
applies all three.

### What you write

```cpp
// `mig` rather than `m`: the metrics table in §11 already aliases `m` to
// anvil::analytics, and two aliases for one name in one namespace is a hard
// error the moment an application declares both — which is the ordinary case.
namespace mig = anvil::db;

// Defined in migrations.cc, so the table stays constexpr — the shape jobs.cc uses.
mig::StepOutcome backfill_display_name(mig::StepContext&,
                                       std::span<const bsoncxx::document::view>) noexcept;

inline constexpr std::array<mig::MigrationStep, 1> kSteps{{
    {"2024_06_backfill_display_name", "users", &backfill_display_name,
     500, 3, mig::Cursor::IdRange},
}};

static_assert(mig::step_table_is_well_formed(kSteps),
              "an empty, duplicate or ungrammatical name; a collection that is not declared "
              "in config::kCollections; a zero batch size; a negative schema precondition");
```

The name's grammar is lowercase ASCII, digits and single underscores, starting and ending on
an alphanumeric — `step_name_is_well_formed` is the predicate. It is a grammar rather than a
non-empty check because the name is a stored key that an operator types back into `--only`
while recovering a half-applied migration, and `a__b` and `a_b_` are both reproduced from
memory as `a_b`.

```cpp
[[nodiscard]] bsoncxx::document::value draft_shape();   // defined in migrations.cc too

inline constexpr std::array<mig::CollectionOptionsSpec, 1> kCollectionOptions{{
    {"drafts", {}, {}, 0, 0, &draft_shape, true, mig::Granularity::None},
}};

static_assert(mig::collection_options_are_well_formed(kCollectionOptions),
              "a collection that is not declared, two entries for one collection, or a "
              "combination the server refuses");
```

`tests/testapp/migrations.h` is the compiled worked example, and it carries two steps on
purpose: one over a UUIDv7 collection and one over `sections`' compound `_id`, because a cursor
proved against one `_id` shape is a cursor that breaks on the other.

### Notes that are not obvious

- **`name` is stored, and a renamed step re-runs from scratch** on every cluster that already
  applied it. It is the ledger's `_id`; there is nothing else to match on. Date-prefix the name
  and then leave it alone — including when the step's code is later corrected.
- **The table is a `std::span`, not a third config header.** `<anvil_app_jobs.h>` exists because
  `kMaxLeaseSeconds` is derived from that table inside anvil's own compilation — the "a size
  must be a size" exception — and migrations derive no such constant. `migration_step.h`
  includes `<mongocxx/client-fwd.hpp>` rather than the client, so the driver still never
  reaches `anvil::foundation`. It does include the bsoncxx **value** headers, and it has to:
  `StepFn`'s own signature names `bsoncxx::document::view`, and `std::span` requires a complete
  element type. "Client-fwd and nothing more" was written before the signature was, and the
  part that was load-bearing — no connection type in a header the low layer can see — is intact.
- **A step may not `$inc`**, and `tools/check-db-discipline.sh` fails the build over it. A lease
  can expire against a process that is alive but stalled, so two runners can overlap; with
  `$set` the double application is a no-op and with `$inc` it is a wrong number nobody can
  reconstruct. See [`18-data-migrations.md`](18-data-migrations.md) §4.
- **`min_schema_version` is the step's precondition, not its position.** A step that reads
  through an index declares the version that created it, and the runner refuses rather than
  collection-scanning production because the deploy order slipped.
- **A step that filters on anything but `_id` needs an index and a `QuerySpec` entry**, like any
  other query. The plain `_id` walk needs neither, which is why the exception is worth naming.
- **`collection_options_are_well_formed` reads `config::kCollections`**, so a capped collection
  whose rows have a declared lifetime is a build failure. Capped forbids deletes and forbids a
  TTL index, so those rows would be readable forever with no path that could remove them — a
  contradiction between two tables, caught where both are visible.
- **Options are applied in two phases and the order is the deployment order.** `Create` runs
  before the index catalogue, because `createIndexes` creates a missing collection implicitly
  and one created that way is not clustered, not capped and not a timeseries. `Validate` runs
  after the data steps, because adding a validator to a collection that already holds documents
  rejects the writes that would have made them conform
  ([`18-data-migrations.md`](18-data-migrations.md) §12). A collection this run creates carries
  its validator immediately — there is nothing in it that could fail one.
- **The null `apply` check is `steps_are_present()`, at runtime.** Not because it failed to
  fold, but because the two tables beside it had already settled the question: neither
  `catalogue_is_well_formed` nor `query_catalogue_is_well_formed` compares a function pointer
  against `nullptr` in a constant expression, for the reason those headers state. A third table
  doing it differently would be the odd one out, and a `well_formed()` that does not compile
  on somebody's compiler is worse than one that checks less. The runner refuses a null `apply`
  as a catalogue inconsistency — exit 3 — rather than dereferencing it.

---

## 14. Route descriptions, and the client descriptor

### What anvil ships

A web client holds the same tables this server does — the routes, the permission bits, the
error vocabulary, the limits. Written twice they drift, and the drift is silent in the
direction that matters: a renumbered permission bit is not a missing feature, it is a **wrong
authority check** rendered to a user as an affordance that should not exist.

So anvil ships the emitter, and an application's tables are written once — here, where a
`static_assert` already validates them.

```cpp
// anvil/descriptor/route_description.h
struct RouteDescription final {
    std::string_view id;             // opaque, stable, and never the path
    std::string_view pattern;        // must equal one RoutePolicy's, under this method
    std::string_view capability;     // empty when the route consumes none
    std::string_view rate_bucket;    // empty when no rule names it
    std::string_view cursor_field;   // empty when it is not a list route
    std::uint32_t    limit_max;      // 0 when it is not a list route
    accesscontrol::RouteMethod method;
    bool             idempotent;     // whether REPEATING it is safe
    bool             bootstrap = false;           // whether its PATH may be in a bundle
    bool             response_is_array = false;   // a list of the shape, or one of it
    // The declared shape of the success body, EMPTY when it is undescribed. The
    // last three are defaulted, so adopting either claim is per route.
    std::span<const http::ResponseField> response = {};
};

// Every route described EXACTLY ONCE, every description naming a route that
// exists and a method a client can send, every id unique, a list route carrying
// both a cursor and a ceiling or neither, and no `bootstrap` claim on anything
// but an unpermissioned Authenticated policy.
//
// The pairing resolves through accesscontrol::policy_for, which is the function
// the filter itself uses — so a description naming POST against an `Any` policy
// pairs the way a request to it would actually be answered, rather than through
// an equality loop of this header's own that could disagree with the filter.
[[nodiscard]] constexpr bool descriptions_match(std::span<const accesscontrol::RoutePolicy>,
                                                std::span<const RouteDescription>) noexcept;
```

```cpp
// anvil/descriptor/descriptor.h
void append_descriptor(std::string& out, const DescriptorInput& input);
[[nodiscard]] std::string emit_descriptor(const DescriptorInput& input);
```

```cpp
// anvil/accesscontrol/route_projection.h — the run-time half
void append_reachable_routes(std::string& out, std::span<const RoutePolicy> routes,
                             std::span<const descriptor::RouteDescription> descriptions,
                             const PermSet& held, UserType type,
                             bool include_bundled = false);
```

```cpp
// anvil/http/response_spec.h — the vocabulary a shape is declared in
enum class FieldKind : std::uint8_t { String, Int, Bool, Uuid, Time, Strings };
struct ResponseField final { std::string_view name; FieldKind kind; bool nullable; };
[[nodiscard]] constexpr bool response_shape_is_well_formed(std::span<const ResponseField>);

// anvil/http/response_writer.h — the writer that IS the declaration
template <const auto& Fields> ObjectWriter<Fields, 0> write_object(std::string& out);
template <const auto& Fields> class ArrayWriter;
```

### What you write

A description per route, and a program that prints the document:

```cpp
// route_descriptions.h
inline constexpr std::array<anvil::descriptor::RouteDescription, 2> kRouteDescriptions{{
    {"auth.login", "/login", "", "login", "", 0, ac::RouteMethod::Post, false},
    {"content.get", "/content/{id}", "", "", "", 0, ac::RouteMethod::Get, true},
}};
// A trailing field may be omitted and is then value-initialised, so every table
// written before `bootstrap` existed still compiles and every route in it still
// says `false` — which is the safe answer.

static_assert(anvil::descriptor::descriptions_match(kRoutes, kRouteDescriptions));
```

`tests/testapp/emit_descriptor.cc` is that program, in full, and it is thirty lines.

A route whose success body is flat may also declare its shape, and then write it through the
writer that walks that declaration:

```cpp
// responses.h
inline constexpr std::array<anvil::http::ResponseField, 3> kMeResponse{{
    {"id",          anvil::http::FieldKind::Uuid,    false},
    {"locale",      anvil::http::FieldKind::String,  false},
    {"permissions", anvil::http::FieldKind::Strings, true},
}};
static_assert(anvil::http::response_shape_is_well_formed(kMeResponse));

// route_descriptions.h — the two trailing values are the whole cost of adopting it
{"identity.me", "/me", "", "", "", 0, ac::RouteMethod::Get, true, false, false, kMeResponse},

// the handler
anvil::http::write_object<kMeResponse>(body)
    .uuid<"id">(ctx->user_id)
    .text<"locale">(ctx->locale.tag())
    .strings<"permissions">(held)
    .done();
```

### Notes that are not obvious

- **Two tables rather than one wider one, and the reason is where each is read.**
  `policy_for()` scans the route table linearly on **every protected request**, so every byte
  added to `RoutePolicy` is a byte pulled through L1 on a path whose entire design is that it
  does no work. A `RouteDescription` is read once, by a program that exits. Fusing them would
  put build-time metadata into the request path's cache lines to save an application one
  `static_assert`.
- **The id is never the path.** The id is what a client names the route by, so it is compiled
  into whichever bundle calls it — and a client that names routes by path publishes the path.
  The path of an administrative route is the map [`04-access-control.md`](04-access-control.md)
  §3's stealth 404 exists to withhold.
- **A path the bundle does not already have is not emitted into one; it is sent with the
  session.** `path_in_bundle(description, access)` is the split. Everything else is projected
  at run time by `append_reachable_routes`, filtered to the holder asking. A client bundle is a
  public file and a lazily-loaded chunk is a public URL; neither becomes private by being
  split, which is why the filtering is the server's and is per **holder** rather than per
  audience. A content editor is never handed the paths of the routes above them.
- **`bootstrap` exists because that predicate was answering two questions.** "May this path be
  in the bundle" and "is this route reachable with no credential" are the same question for
  every route until the one that RETURNS the table: it demands a credential, and its address
  has to be public anyway, because a client cannot be told where to ask for the table until it
  has asked. `access` stays the authority answer and is emitted unchanged; `bootstrap` is the
  disclosure answer. The generated client is then told two true things about `session.current`
  — its address is public, and calling it without a session is a `401` — rather than one false
  one.

  It is legal **only** on an `Authenticated` policy requiring no bit, and `descriptions_match`
  refuses everything else at compile time. Not Guarded or Stealth, because those paths are the
  map the 404 exists to withhold and a table is the wrong place to take that claim on trust;
  and not Public either, which is merely redundant — refusing it keeps exactly one reason per
  route for a path being in the bundle, so a reader can always tell which.

  A bootstrap path also drops out of the **holder-scoped projection**, for the same reason a
  public one already does: resending a path the bundle contains is bytes on the one response
  every signed-in tab asks for. The route is still reachable; it is just not worth sending.
- **The projection calls `satisfies()` — the same function the filter calls.** A second
  implementation of "may this holder reach this route" is a second one to keep in agreement,
  and the one that drifts is the one no attacker is reading. Drift is a defect in both
  directions: a route listed but denied is an affordance that fails, and a route withheld but
  allowed is a feature that has silently disappeared for somebody entitled to it. The suite
  asserts the two agree over every route and a range of holders rather than asserting the
  projection's output shape.
- **The response the projection travels in is the caller's, and three of its headers are not
  optional.** `tests/session_listener_test.cc` is the worked example, served over a real
  listener through the real access filter, and its response is what a generated client parses:
  `{"routes":{"<id>":"<METHOD> <path>", …}}`, `application/json; charset=utf-8`,
  `Cache-Control: private, no-cache`, `Vary: Cookie`, and an `ETag`. The caching pair is a
  security control rather than a tuning choice — the document is per **holder**, so a shared
  cache holding one copy serves one holder's map to another, which is the disclosure this whole
  section exists to prevent, reintroduced one layer downstream of it. `no-cache` rather than
  `no-store`, because a response that may not be stored may not be revalidated either, and then
  the ETag buys nothing.
- **Key the ETag to `perm_epoch` AND to the table's own identity.** The epoch is the half that
  moves with authority: it bumps on every grant, so a new route reaches a long-lived tab at the
  next revalidation rather than at the next login. It is not the whole key, and the gap is a
  DEPLOY — a build that adds a route, moves one or re-authorises one changes what a holder
  reaches while their epoch stands still, so an epoch-only tag answers `304` and leaves the
  client calling yesterday's map until somebody happens to edit that holder's permissions. One
  hash over the table at boot closes it. Hashing the RESPONSE instead would also close it and
  costs the projection on every revalidation, which is the work the revalidation exists to skip.
- **`idempotent` is a property of the route, not of the method.** A `POST` that is safe to
  repeat says so and needs no idempotency key; a `PUT` that is not says so too. Deriving it
  from the verb is how a client's retry becomes a duplicate write.
- **A list route carries both a cursor field and a ceiling, or neither**, and
  `descriptions_match` refuses one without the other: a cursor with no ceiling is an unbounded
  result set, and a ceiling with no cursor is a page nobody can advance. Handing a client a
  cursor field is also what makes an offset unspellable in a generated client rather than
  merely discouraged — `skip(n)` is O(n) server-side.
- **The descriptor is a build artefact and is never served.** It carries every route's path,
  including the ones `path_in_bundle` refuses to compile into a client, so a copy of it under a
  document root hands over in one request the map the rest of this section withholds. It also
  carries nothing about storage — no collection name, no index, no query, no migration — and
  the suite asserts that by searching the emitted bytes for those words.

  **One descriptor is published anyway, and only one: the reference application's.** anvil
  releases `anvil-<tag>-reference-descriptor.json` beside `anvil-<tag>-envelopes.json`, so a
  client generator can fetch a release instead of being told to run a binary in a sibling
  checkout — which is a note somebody has to remember, and the fixture that comes of it is
  green by construction whatever release it came from. The exception is safe because `testapp`
  has no deployment and its paths protect nothing; it is dangerous because the two files look
  identical. So it is enforced rather than remembered: `tools/release-artefacts.sh` refuses to
  publish a descriptor whose `app.name` is not the reference application's.

  The same script regenerates and diffs rather than trusting either file, which is what
  determinism buys — and it runs as a CTest entry rather than only at a tag, because a tag is
  the worst moment to discover that a hash covers bytes that are not reproducible.
- **The hash covers the tables and not the metadata.** `hash` is SHA-256 over the `tables`
  object exactly as emitted, so a version bump that changes no table does not invalidate every
  client that is running perfectly good code, while a table change invalidates all of them.
  That makes determinism load-bearing: the same tables must produce the same bytes on every
  run, or the staleness check reports drift that is not there and is ignored inside a week.
  Nothing in the emitter iterates an unordered container, and permission names are emitted in
  **bit order** rather than table order for the same reason.
- **`descriptor` is the format version, `hash` is the content, and `app.version` is whose.**
  Three questions — "can the generator read this file", "is this client built from this
  server", "whose tables are these" — and one number cannot answer three.
- **Every `ErrorCode` and every `input::Reason` is listed once inside the emitter**, with a
  `static_assert` tying each list to its `kMax`. A range-for over an enum is not a thing C++
  has, and casting `0..kMax` emits a name for a value that stops being an enumerator the
  moment the enum grows a gap. Appending a code without listing it is a build failure rather
  than a client with a hole in its vocabulary.

### The content tables

The four tables above are how a client CALLS. These are what it RENDERS, and they arrive
through `DescriptorInput` as the same spans the owning services already take — a second reader
of a table an application declares once, not a second table:

```cpp
.field_types = kFieldTypes,   // §5
.sections    = kSections,     // §6
.topics      = kTopics,       // §7a
.events      = kEvents,       // §12
```

The media table is not among them, because it is not a span seam: namespaces and the role
ladder are declared in `<anvil_app_config.h>` and dimension arrays inside anvil's own
translation units, so the emitter reads them where everything else does (§2) — the same reason
the locale table is not a parameter either.

Each is emitted in the vocabulary a client already speaks rather than the one the server
stores:

- **Field-type flags are named booleans, not the byte.** A client handed `"flags":17` needs a
  copy of anvil's enum to read it, and a copy of an enum is the second table this document
  exists to remove. The JSON shape an answer takes is emitted alongside, derived by
  `answer_kind_of` in the one place the submission service derives it — and it is `null` for a
  PII type, which produces no answer at all. A form renderer that expected one there would
  render a field that has silently emptied itself.
- **A section carries its staff labels, one per declared locale, in the locale table's
  order** — the order the stored one-byte index means. Without them the only string a
  generated editor has is the key, so the control reads `cta_href`. An image slot with no
  aspect constraint emits `"aspect":null` and never `{"num":0,"den":0}`: an object is always
  truthy, so the zeroed form renders "shaped 0:0" beside the control, which a staff member can
  neither satisfy nor recognise as nothing being asked.
- **A topic carries a `visibility`, for the same reason a route's path does.** Subscription is
  the disclosure: a topic gated by a permission is one whose existence is part of what the
  permission protects, so its name may not be compiled into a bundle. Channels are emitted by
  name so a client needs no copy of the `ClientType` bit layout to render a preference mask.
- **An event carries its CLOSED dimension values**, because the row stores the index into
  them, and `requires_consent`, because that event is refused at the door rather than filtered
  later — a client that offers it without consent is reporting into a refusal it cannot see.

### The media table, and the `srcset` argument it settles

Phase 11 recorded a disagreement it did not resolve: [`07-filesystem.md`](07-filesystem.md)
keeps the width ladder server-side — "a client that knows the ladder is a client that will
start building paths from it again" — and the public grammar is a role, while a responsive
`srcset` needs width descriptors or the browser cannot choose between the sources it is given.

Both hold, and this is where. **The objection is to a client constructing a path, not to it
knowing a number**, so a width is emitted *attached to the role it belongs to*, and nothing
else is emitted at all — no bare ladder, no format list, no file extension:

```json
"media": {"default_role": "card",
          "namespaces": [{"ns": "content",
                          "accepts": ["image/jpeg", "image/png",
                                      "image/webp", "image/avif"],
                          "roles": [{"role": "thumb", "width": 320},
                                    {"role": "card",  "width": 1024}]}]}
```

A client can then write `srcset="/media/content/{id}/thumb 320w, /media/content/{id}/card
1024w"`, where every URL is still a role and every number is still the server's. What it
cannot do is assemble `w640.avif`, because it has never been told either half. The role→width
mapping still changes without a client release: a client re-fetching the descriptor gets new
numbers against the same role names, and the paths it already holds keep working while it
does.

### What the seam carries now that it did not

Six things an application used to write by hand, and one it still does — deliberately, and
with the boundary stated rather than left for each application to guess at.

Six are closed. **A description may no longer say `ANY`:** `descriptions_match` refuses one,
so naming a method in every description — even where the policy is `Any` — is a build rule
rather than advice. The policy stays the thing that accepts every verb, and the description is
what a client calls with. `method_name` still spells `"ANY"` because the switch has to be total,
but nothing can reach it from the emitter any more, and that is the point: failing loudly in a
generated client put the failure on whoever generated the client, when the table is where the
mistake was. An application that wants two methods callable declares two policies, which is
already what the route table demands of two methods that differ in authority.

**And an authenticated route's path may now be published without its authority being:**
`RouteDescription::bootstrap` says the path may be compiled into a bundle, `access` still says
what the filter demands, and `path_is_public(access)` has become
`path_in_bundle(description, access)` — one predicate called by the emitter and the projection
alike. `session.current` is the case that forced it: the address of the session arrives *with*
the session, which on a cold load does not exist, so the first read of the whole system was
unspellable and every application wrote `/session` in by hand. The tempting alternative,
declaring the route Public and checking the credential in the handler, moves an authentication
check out of the filter, which is how a route stops failing closed. The flag is refused at
compile time on anything but an unpermissioned `Authenticated` policy.

**And the media grammar is a route like any other.** `GET /media/{ns}/{id}/{role}` is settled
in [`08-images.md`](08-images.md) §4 and was declared by no table, so every application wrote
the pattern by hand — one address in an application and another in a route table, which is two
things to keep in agreement. It is now a row in the route table with a description beside it,
which is where a claim of this kind gets checked.

It did **not** become a field in the `media` object. That table says what a role *is*; the route
table says where a route *lives*. A generated client joins the two **by segment name**: `{ns}`
and `{role}` are the keys the `media` object already publishes value sets for, so both come out
as enumerations rather than free strings, and a case asserts that join rather than leaving it as
a convention — rename a segment and it fails. The encoding stays the route builder's, so a
client still cannot assemble `w640.avif`, because nothing tells it either half.

**And each namespace publishes the types it accepts.** `"accepts":["image/jpeg",…]` sits beside
that namespace's roles, read from `NamespaceSpec::accepts` — the same mask `UploadSink` enforces
on the way in, so a file picker and a refusal cannot disagree. The allow-list stays anvil's,
because it is the pipeline's capability; **narrowing** it is the application's, because which
types a namespace should take is a fact about that namespace. It is per namespace rather than
global for the reason a global one would not close: the moment one namespace takes no AVIF, the
narrower list gets written into the client by hand again.

SVG never appears in an emitted list, and not by omission — there is no `Mime` for it at all,
because [`07-filesystem.md`](07-filesystem.md) §5 refuses it explicitly so that an upload
attempt is auditable as the probe it is.

**And the session response carries the holder's authority.**
`accesscontrol::append_holder_authority` appends
`{"superadmin":<bool>,"perms":["…"]}`, which is what a client renders the affordances that are
*not* routes from. `satisfies()` short-circuits on user type and a superadmin's permission set
is deliberately not all-ones, so counting bits answered **zero** for the single account that
reaches everything — and a client rendering from bits hid every control from exactly them,
which looks like a missing feature rather than a bug. The route table was never affected,
because the server builds it with `satisfies()`.

The boolean is read from `is_superadmin()`, the same predicate the filter's short-circuit
reads, so there is one expression of that fact rather than two. `perms` carries the bits the
holder actually holds — never `~PermSet{}`, which would put the conflation `core/types.h` keeps
apart onto the wire and hand out the reserved gaps between an application's blocks as though
they meant something. Names are in bit order, the order the descriptor emits them in, so two
responses for one holder are byte-identical and the session ETag keeps meaning something.

**It is an affordance hint and never an authority.** The server re-derives both halves on every
request. A client that hides a control it should have is cosmetic; a server that skips a check
is not.

**And a route may declare the SHAPE of its answer.** The descriptor described calls and not
answers, so a generated client declared its own response types — hand-written, unverified, and
the one part of a generated client this document did not underwrite.

The tempting fix is a `ResponseSpec` table emitted into the descriptor while handlers keep
appending by hand, and it is worse than no table: that is a second copy of the response shape,
in the place least able to check itself, and it would be believed. A handler that stopped
writing a key would leave the schema saying it still does.

**So the spec is the writer.** `write_object<kShape>(body)` returns a writer positioned at the
first declared field, every call returns the writer for the next one, and a call naming a key
that is not next, a call of the wrong type for the field that is, and a `done()` before the last
field are all `static_assert`s. The emitted schema is therefore a description of the bytes
rather than a claim about them, and the two cannot drift because there is only one of them.

The position is a template parameter, so a described body has to be assembled where the call
sequence is statically known. A runtime **branch** is fine — both arms start from the same
position and each is checked there, which is how a nullable field is written — and what cannot
be expressed is a loop that picks a key by a runtime index. That is refused on purpose: the
alternative is a runtime cursor, which turns a build error into a 500.

- **It covers flat objects and arrays of one declared shape, and nothing else.** A
  nested-object grammar here would be a schema language, and a schema language that describes
  80% of the bodies is one a client still cannot trust. A route with no declared shape emits
  `"response":null` and a generated client falls back to its own declaration, which is what
  every route did before this existed — so **adoption is per route rather than a flag day**.
  The reference application describes `identity.me` and deliberately leaves `session.current`
  undescribed: its body is `{"routes":{…},"authority":{…}}`, which this grammar cannot express,
  and saying so is better than approximating it.

### What the seam does not carry, and is not going to

- **Anything richer than a flat body stays hand-written and undescribed.** That is the honest
  boundary rather than a gap waiting to be closed: the value of a declared shape is that it
  cannot lie, and a grammar stretched to cover nested documents would stop being checkable at
  the point where it started being useful.
