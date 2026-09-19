#pragma once

// The reference application's capability scopes.
//
// Compiled by every build of the test suite, so the worked example in
// docs/01-seams.md is a file that must keep compiling rather than a snippet that
// can rot.
//
// There is no "general" scope here and there must never be one anywhere: an
// unscoped capability is a bearer token with authority over everything and a
// replay window as long as its lifetime (anvil/identity/capability_spec.h).

#include <array>
#include <cstdint>

#include "anvil/identity/capability_spec.h"

namespace testapp {

// STORED as int32 in the capability row. APPEND ONLY — never renumber, never
// reuse a retired value.
enum class Scope : std::int32_t {
    // A destructive confirmation. The object being deleted is the subject, so a
    // token minted for one row cannot be presented against another.
    ContentDelete = 1,
    // An upload slot. No subject: the object does not exist yet, which is the
    // whole point of asking for the slot.
    MediaUpload = 2,
    // Changing somebody's permission grid. Its own scope rather than a reuse of
    // ContentDelete, for the reason every scope here is its own: a token names
    // the operation it authorises, and one that meant "delete or grant,
    // whichever you present it to" is the unbound grant the design refuses.
    StaffPermissionChange = 3,
    // The one NON-single-use scope, and the one presented without a session.
    //
    // A preview lives on an origin the host-only session cookie never reaches,
    // so this is the credential that does arrive there: authority over ONE
    // draft, expiring with it, carrying no permission bits at all. A page is
    // reloaded, so it is verified rather than consumed — burning it on first use
    // would break the feature it exists for.
    DraftPreview = 4,
};

inline constexpr std::array<anvil::identity::CapabilityScopeSpec, 4> kScopes{{
    {"ContentDelete", static_cast<std::int32_t>(Scope::ContentDelete), true},
    {"MediaUpload", static_cast<std::int32_t>(Scope::MediaUpload), true},
    {"StaffPermissionChange", static_cast<std::int32_t>(Scope::StaffPermissionChange), true},
    {"DraftPreview", static_cast<std::int32_t>(Scope::DraftPreview), false},
}};

static_assert(anvil::identity::capability_table_is_well_formed(kScopes),
              "duplicate scope value or name, empty name, or a non-positive value");

// The one scope in this table that is not burned on redemption. Asserted rather
// than assumed, because the single-use flag is what decides whether redeem()
// consumes or verifies — and a scope silently flipped to non-single-use is a
// double-spend nobody would see in review.
static_assert(
    anvil::identity::capability_spec_of(
        kScopes, anvil::identity::CapabilityScope::of(Scope::DraftPreview))->single_use == false,
    "a preview credential is read-only and is presented on every page load");
static_assert(
    anvil::identity::capability_spec_of(
        kScopes, anvil::identity::CapabilityScope::of(Scope::ContentDelete))->single_use,
    "a destructive confirmation presented twice is a double-spend");

}  // namespace testapp
