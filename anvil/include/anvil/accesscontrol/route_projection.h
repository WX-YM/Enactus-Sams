#pragma once

// What a session response tells a client about the holder it belongs to: the
// routes they may reach, and the authority they hold.
//
// Both are projections of one holder and both agree with `satisfies()` by
// calling it, which is why they sit in one place. Neither is an authority the
// server later trusts — the filter re-derives everything on every request — and
// the first half is the one with a disclosure property attached to it.
//
// --- why a client is given a table at all ----------------------------------
//
// A client has to know a route's path to call it, and the obvious place to put
// that knowledge is the client's own bundle. For a route anyone may call that is
// correct and costs nothing. For an administrative route it publishes the map
// the stealth 404 exists to withhold: a bundle is a public file, a lazily-loaded
// chunk is a public URL, and neither becomes private by being split.
//
// So the path of a non-public route is not compiled into anything. It is sent
// here, with the session, filtered to what the holder's permissions actually
// reach — which is strictly better than a bundle split three ways, because the
// filtering is done by the only participant an attacker does not own, and
// because it is per HOLDER rather than per audience: a content editor is not
// handed the paths of the routes above them, so a compromised account yields
// that account's map rather than the whole one.
//
// --- one authority function, not two ---------------------------------------
//
// What is reachable is decided by `satisfies()` — the same function the filter
// calls, superadmin short-circuit included. A second implementation of "may this
// holder reach this route" is a second one to keep in agreement, and the one
// that drifts is the one no attacker is reading. Drift in either direction is a
// defect: a route listed but denied is an affordance that fails, and a route
// denied here but allowed by the filter is a feature that has silently
// disappeared for somebody who is entitled to it.

#include <span>
#include <string>

#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/core/perm_catalogue.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/types.h"
#include "anvil/descriptor/route_description.h"

namespace anvil::accesscontrol {

// Whether a holder of `held`/`type` may reach this route at all.
//
// Authenticated routes require no bit, so every authenticated holder reaches
// them. This is the AUTHORITY question only; whether a path is worth SENDING is
// `descriptor::path_in_bundle`, and the two are deliberately separate — a
// bootstrap route is reachable and still not worth sending, because the client
// already compiled its path in.
[[nodiscard]] constexpr bool reachable(const RoutePolicy& policy, const PermSet& held,
                                       UserType type) noexcept {
    switch (policy.access) {
        case RouteAccess::Public:
        case RouteAccess::Authenticated:
            return true;
        case RouteAccess::Guarded:
        case RouteAccess::Stealth:
            return satisfies(held, type, policy.required);
    }
    return false;
}

// Appends `{"<id>":"<METHOD> <path>", …}` for every route this holder reaches
// whose path the client does not already have.
//
// In description order, so two calls with the same input produce the same bytes
// and the response can carry an ETag that means something. The ETag is keyed to
// `perm_epoch` by the caller: a grant then reaches the table at the next
// revalidation rather than at the next login.
//
// `include_bundled` was `include_public`, and the rename is the point rather
// than tidying: what is skipped by default is every path the BUNDLE already
// holds, which is public paths and bootstrap ones alike. Resending either is
// bytes on a response every signed-in tab asks for.
void append_reachable_routes(std::string& out, std::span<const RoutePolicy> routes,
                             std::span<const descriptor::RouteDescription> descriptions,
                             const PermSet& held, UserType type,
                             bool include_bundled = false);

// Appends `{"superadmin":<bool>,"perms":["…"]}` — what this holder can be
// OFFERED, for the affordances that are not routes.
//
// --- the defect this closes -------------------------------------------------
//
// `satisfies()` short-circuits on `UserType::SuperAdmin`, and a superadmin's
// permission set is deliberately not all-ones — so the one account that reaches
// everything counts ZERO bits. A client rendering a non-route affordance from
// the bits it was given therefore hid every one of them from exactly that
// account, which is the opposite of what a superadmin is for and looks like a
// missing feature rather than a bug.
//
// The route table was never affected: the server builds it with `satisfies()`,
// so it already contains what a superadmin reaches. What was missing is the
// answer for a button, a menu entry or a column that is not a route.
//
// --- it is a HINT, and never an authority -----------------------------------
//
// Nothing downstream is allowed to trust it. The server re-derives both halves
// on every request, and a client that hides a control it should not have is a
// cosmetic bug while a server that skips a check is not. The reason this is
// worth sending at all is the failure ABOVE — a client with no answer must
// choose a fallback, and the only safe one, treating an absent flag as false,
// hides more rather than less.
//
// --- and it is not an all-ones mask -----------------------------------------
//
// `perms` carries the bits this holder actually holds, which for a superadmin
// may be none, and `superadmin` is a separate boolean read from the same enum
// `is_superadmin()` tests. Sending `~PermSet{}` instead would put the very
// conflation `core/types.h` keeps apart onto the wire for every client to
// un-conflate — and it would hand out the reserved gaps between an
// application's blocks as though they meant something.
//
// Names are emitted in BIT order, which is the order the descriptor emits them
// in, so two responses for one holder are byte-identical and the ETag the
// session response carries keeps meaning something.
void append_holder_authority(std::string& out, const PermSet& held, UserType type,
                             const PermCatalogue& catalogue);

}  // namespace anvil::accesscontrol
