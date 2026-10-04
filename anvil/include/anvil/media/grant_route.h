#pragma once

// The grant route: `GET MEDIA_ORIGIN/{…}/{grant}/{role}`, the one way an object
// in a PRIVATE namespace leaves the process (docs/22-chat.md §6.1).
//
// The media origin never sees the session cookie, so this handler cannot ask
// who is calling. It does not need to: the decision was made on the site
// origin, by a handler that knew the caller and had just checked their right to
// see the object, and the grant is that decision sealed. What this handler
// checks is that the grant is authentic and unexpired, and that the object is
// still held by something. A delete-for-everyone that released the last
// reference stops every grant for the object at the next request, not at the
// grant's expiry.
//
// Every refusal is the stealth 404 — a forged grant, an expired one, a role
// that is not a role, a row that is gone or no longer held — so a holder of a
// URL learns nothing about why it stopped working.
//
// Public in the route table, and protected by its handler, which is the
// arrangement the draft preview has and for the same reason: the credential is
// one the access filter does not know how to read.
//
// --- caching ------------------------------------------------------------------
//
// `private, max-age` up to the grant's own expiry, never longer. The URL is the
// grant, so a browser cache keyed by it holds the bytes exactly as long as the
// grant would have opened; a year-long `immutable` would keep an object a member
// was removed from on their device long after the revocation latency the design
// states (10–20 minutes).

#include <span>
#include <string_view>

#include "anvil/accesscontrol/route_registry.h"
#include "anvil/descriptor/route_description.h"
#include "anvil/media/grant.h"
#include "anvil/media/service.h"

namespace anvil::media {

// Throws std::invalid_argument at boot when `route_id` is not described, or its
// pattern does not carry exactly two placeholders: the grant, then the role.
//
// `service` and `keys` must outlive HTTP serving; the handler holds both by
// reference, as every handler in an application holds its services.
void install_media_grant_route(const MediaService& service, const GrantKeys& keys,
                               std::span<const accesscontrol::RoutePolicy> routes,
                               std::span<const descriptor::RouteDescription> descriptions,
                               std::string_view route_id);

}  // namespace anvil::media
