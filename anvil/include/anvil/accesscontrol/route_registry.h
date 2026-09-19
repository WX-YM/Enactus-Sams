#pragma once

// The authorization policy for every route an application exposes.
//
// anvil ships the machinery and the lookups; the TABLE is the application's and
// arrives as a std::span (docs/01-seams.md §3).
//
// --- Why a registry rather than a per-route filter object ---
//
// The obvious design is `AccessFilter(PermSet required, bool stealth)` — a filter
// object constructed per route. Drogon cannot register that. `registerHandler` takes
// `internal::HttpConstraint`, which holds either an HTTP method or a filter
// NAME, and names are resolved through DrObject's class-name map. A filter with
// a non-default constructor sets `AutoCreation = false` precisely so it is NOT
// in that map, so there is no name to reference it by.
//
// The registry inverts it: one filter class, and the per-route policy is a
// compile-time table entry the filter looks up by matched path pattern. The
// masks stay `constexpr` and stay in `.rodata`, which was the point of §2.
//
// It also delivers something the per-route constructor could not: the claim that
// EVERY registered route declares a permission set or is explicitly public, and
// that a route with neither fails the build. With per-route constructors that is
// unenforceable — a route registered without a filter simply has none, and
// nothing anywhere knows it should have. With a registry it is a set comparison
// between the framework's route table and the application's array.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <span>

#include "anvil/core/perm_catalogue.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/types.h"

namespace anvil::accesscontrol {

enum class RouteAccess : std::uint8_t {
    // No access token required. Normal HTTP semantics on failure.
    Public,
    // A valid access token is required and a failure is a REAL 401. Used where
    // a 404 would break the product: a login form that 404s cannot function,
    // and a single-page client must be able to tell "re-authenticate" from
    // "route gone".
    Authenticated,
    // Token plus permissions, real 401/403. For protected routes that are not
    // secret — their existence is public even though their use is not.
    Guarded,
    // Token plus permissions, and EVERY failure is the byte-identical 404 in
    // stealth.h. Admin and staff surfaces only.
    Stealth,
};

// Which HTTP method an entry governs.
//
// Most patterns answer one method and declare `Any`. Media needs the
// distinction: `GET /media/{ns}/{id}` is public — the unguessable v4 id IS the
// read capability — while `DELETE` on the same pattern requires MediaDelete and
// stealths every failure. One policy per PATTERN would have to pick one of
// those, and either choice is wrong (docs/07-filesystem.md §6).
enum class RouteMethod : std::uint8_t { Any, Get, Post, Put, Patch, Delete };

[[nodiscard]] constexpr RouteMethod method_from_string(std::string_view method) noexcept {
    if (method == "GET" || method == "HEAD") { return RouteMethod::Get; }
    if (method == "POST") { return RouteMethod::Post; }
    if (method == "PUT") { return RouteMethod::Put; }
    if (method == "PATCH") { return RouteMethod::Patch; }
    if (method == "DELETE") { return RouteMethod::Delete; }
    // An unrecognised method matches only an `Any` entry, and a route that
    // declares none of them is denied. Failing closed, as everything here does.
    return RouteMethod::Any;
}

struct RoutePolicy final {
    // constexpr, computed at compile time, living in .rodata. The check is one
    // AND and one compare (docs/04-access-control.md §2).
    PermSet          required;
    // Exactly the string handed to drogon::app().registerHandler, including any
    // `{parameter}` placeholders. Drogon reports it back through
    // req->getMatchedPathPattern(), so the lookup is a view compare with no
    // parsing and no allocation.
    std::string_view pattern;
    RouteAccess      access;
    // Defaulted, so an entry that governs its whole pattern reads as one line.
    RouteMethod      method = RouteMethod::Any;
};

// --- Phase 3: authentication and session management ------------------------
//
// `/auth/refresh` is Public rather than Authenticated on purpose. Its
// credential is the refresh cookie, and the access token is usually EXPIRED at
// exactly the moment it is called — gating it on a valid access token would
// make refresh work only while it was unnecessary.
// --- Phase 4: media --------------------------------------------------------
//
// Reads are Public because the media id is a UUIDv4 and IS the read capability:
// it is unguessable and leaks nothing, the public site must be able to render
// images to anonymous visitors, and the namespace in the route is what stops
// one API's id resolving through another's handler. Uploads and
// deletions are Stealth: they are admin surfaces, and their existence is not
// something a failed request should confirm.
// --- Phase 5: content domains ----------------------------------------------
//
// Section READS are Public: they are the content the public site renders, and
// they are the hottest endpoint in the system. Section WRITES are Stealth and
// treated as near-superadmin, because that endpoint controls every string on
// the public site (docs/12-sections-cms.md §7). Reset carries a second gate
// beyond the permission bit — a capability token bound to the key, checked in
// the handler — because it is the most destructive non-delete operation here.
// --- dynamic forms ---------------------------------------------------------
//
// `GET /forms/{id}` and `POST /forms/{id}/submit` are PUBLIC, and that is the
// whole point of the feature: a membership application has to be fillable by
// someone who does not have an account yet. Everything about the shape of that
// exposure is therefore handled inside the handlers rather than by the filter —
// per-IP rate limiting, a 64 KB body cap, and a definition read that answers
// only for an ACTIVE form so a draft's existence is not public (docs/13-dynamic-forms.md §4).
//
// The three admin surfaces are Stealth and hold three DIFFERENT permissions on
// purpose. Reading submissions and reading the identity numbers inside them are
// separate authorities (`FormRead` and `FormPii`), and destroying a form is a
// third (`FormDrop`) that additionally needs a capability token bound to the
// form id — it is irreversible and it destroys personal data (docs/13-dynamic-forms.md §6, §7).
[[nodiscard]] constexpr const RoutePolicy* policy_for(
    std::span<const RoutePolicy> routes, std::string_view pattern, RouteMethod method = RouteMethod::Any) noexcept {
    const RoutePolicy* fallback = nullptr;
    for (const RoutePolicy& route : routes) {
        if (route.pattern != pattern) { continue; }
        if (route.method == method) { return &route; }
        if (route.method == RouteMethod::Any) { fallback = &route; }
    }
    return fallback;
}

// A pattern with no entry is a registration bug, not a permissive default.
// Nothing calls this to decide anything at runtime — the filter treats an
// unknown pattern as a denial — but the boot guard and the coverage test both
// read far better with it.
//
// It takes the METHOD, and it answers by calling policy_for, so it keys on
// EXACTLY what the filter keys on. The previous form asked only whether some
// entry mentioned the pattern, so a handler registered under a verb the registry
// does not declare booted clean and then denied every request to itself.
[[nodiscard]] constexpr bool is_declared(std::span<const RoutePolicy> routes, std::string_view pattern,
                                         RouteMethod method) noexcept {
    return policy_for(routes, pattern, method) != nullptr;
}

// Whether the pattern appears at all, under any method. For the coverage test,
// which asks a different question from the boot guard: "is this route in the
// table" rather than "will the filter answer for this registration".
[[nodiscard]] constexpr bool is_pattern_declared(std::span<const RoutePolicy> routes,
                                                    std::string_view pattern) noexcept {
    for (const RoutePolicy& route : routes) {
        if (route.pattern == pattern) { return true; }
    }
    return false;
}

}  // namespace anvil::accesscontrol
