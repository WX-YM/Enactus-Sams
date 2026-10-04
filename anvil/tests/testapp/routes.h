#pragma once

// The reference application's route table.
//
// anvil ships RoutePolicy, policy_for and is_declared; which routes exist and
// what each requires is the application's (docs/01-seams.md §3).
//
// The shapes here are chosen to exercise the lookup rather than to describe a
// real API: an Any entry, a method-specific entry, two entries sharing one
// pattern under different methods, a public route, an authenticated one, a
// permissioned one and a stealth one. A table of only ordinary routes would leave
// the interesting half of policy_for untested.
//
// `/auth/refresh` is the exception, and it is here for the opposite reason: it is
// named by anvil's own route_registry.h, which settles its access class and says
// why. A route the library reasons about in prose and no table declares is a
// claim nothing checks — and the reference consumer is where it is checked.

#include <array>

#include "anvil/accesscontrol/route_registry.h"

#include "perms.h"

namespace testapp {

namespace ac = anvil::accesscontrol;

inline constexpr std::array<ac::RoutePolicy, 69> kRoutes{{
    // Public: no token required, normal HTTP semantics on failure.
    {anvil::PermSet{}, "/login",
     ac::RouteAccess::Public, ac::RouteMethod::Post},

    // Public, and both must be: a client asks for its salt BEFORE it has any
    // credential, and registration is how it gets one (docs/05 §12). Neither
    // discloses whether an account exists — the salt route answers every
    // identifier with 200 and a stable salt, and registration answers a
    // duplicate byte-identically to a fresh one.
    {anvil::PermSet{}, "/auth/prehash",
     ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/signup",
     ac::RouteAccess::Public, ac::RouteMethod::Post},

    // The rest of the built-in account flows (accounts.h). Public, because each
    // is how somebody WITHOUT a session proves an address or recovers one; none
    // of them answers differently for an address that has no account.
    {anvil::PermSet{}, "/auth/verify",
     ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/auth/resend",
     ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/auth/reset",
     ac::RouteAccess::Public, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/auth/reset/confirm",
     ac::RouteAccess::Public, ac::RouteMethod::Post},

    // A password change is the one flow that needs a session: it is made from
    // inside one, and it ends every OTHER one.
    {anvil::PermSet{}, "/auth/password",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},

    // Public, and it must be: its credential is the refresh cookie, and the
    // access token is EXPIRED at exactly the moment this is called. Gating it on
    // a valid access token would make refresh work only while it was
    // unnecessary (anvil/accesscontrol/route_registry.h).
    //
    // It is one of two entries here that are Public and still protected, and
    // both are protected by a credential the FILTER does not read: this one by
    // the refresh cookie its handler looks up, `/preview/{id}` below by a
    // path-scoped capability cookie. Public describes what the filter does —
    // nothing — and never what the handler does.
    {anvil::PermSet{}, "/auth/refresh",
     ac::RouteAccess::Public, ac::RouteMethod::Post},

    // The holder-scoped route table (docs/01-seams.md §14). Authenticated and
    // unpermissioned, and it has to be both: a holder of no bits still has to be
    // able to learn that they hold none, and the routes it names are precisely
    // the ones a Public class would publish to anybody who asked.
    {anvil::PermSet{}, "/session",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},

    // Any: accepted under every method, which is what a route whose handler
    // dispatches internally needs.
    {anvil::PermSet{}, "/session/logout",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Any},

    // Authenticated but unpermissioned: a signed-in caller, no bit required.
    {anvil::PermSet{}, "/me",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},

    // One pattern, two methods, DIFFERENT authorities. Reading the shelf and
    // rewriting it are not the same act, and a table that gave them one entry
    // would grant the dangerous half to whoever holds the ordinary one.
    {anvil::perm_mask(Perm::ContentRead), "/content/{id}",
     ac::RouteAccess::Guarded, ac::RouteMethod::Get},
    {anvil::perm_mask(Perm::ContentDelete), "/content/{id}",
     ac::RouteAccess::Guarded, ac::RouteMethod::Delete},

    // Two methods that DO share an authority, declared separately so adding a
    // third does not silently inherit it.
    {anvil::perm_mask(Perm::MediaUpload), "/media/{ns}/{id}",
     ac::RouteAccess::Guarded, ac::RouteMethod::Get},
    {anvil::perm_mask(Perm::MediaDelete), "/media/{ns}/{id}",
     ac::RouteAccess::Guarded, ac::RouteMethod::Delete},

    // Image edits (docs/21-image-edits.md §6), under their own prefix rather
    // than under `/media/{ns}/{id}/…`: every GET there is a ROLE, and an edit
    // route beneath it would be one role name away from being answered by the
    // public object handler. Making an edit and reading one back share the
    // authority an upload needs, because an edit is an upload the server draws.
    {anvil::perm_mask(Perm::MediaUpload), "/media-edits/{ns}/{id}",
     ac::RouteAccess::Guarded, ac::RouteMethod::Post},
    {anvil::perm_mask(Perm::MediaUpload), "/media-edits/{ns}/{id}",
     ac::RouteAccess::Guarded, ac::RouteMethod::Get},

    // Serving a stored object, and the reason it is declared at all: the grammar
    // `GET /media/{ns}/{id}/{role}` is settled in docs/08-images.md §4 and no
    // table declared it, so every application wrote the pattern by hand. An
    // address in an application and an address in a route table are two things
    // to keep in agreement.
    //
    // PUBLIC for the same reason `/preview/{id}` below is, and by the same
    // mechanism: it is served from the media origin, where the host-only session
    // cookie never arrives, so the filter has no credential to read here and
    // saying otherwise would describe a check that does not happen. What
    // authorises an individual object is the handler. `Cache-Control: private`
    // on the response is about THAT authorisation and not about this class.
    //
    // NO rate bucket, deliberately. The `media` rule is 20 a minute and is sized
    // for UPLOADS — one libvips decode on cpu_pool each. A gallery page is
    // thirty images in one paint, so counting serves into that bucket would
    // answer 429 to an ordinary render, and the symptom would be images that
    // vanish above the fold.
    {anvil::PermSet{}, "/media/{ns}/{id}/{role}",
     ac::RouteAccess::Public, ac::RouteMethod::Get},

    // Stealth: a denial and a nonexistent route answer with identical bytes, so
    // the existence of the route is not itself disclosed.
    {anvil::perm_mask(Perm::AuditRead), "/audit",
     ac::RouteAccess::Stealth, ac::RouteMethod::Get},

    // Public for the reason `/auth/refresh` is, by a different credential. It is
    // served from CONTENT_ORIGIN, so the host-only session cookie never reaches
    // it and the filter has no credential to read; what authorises it is a
    // path-scoped capability cookie its HANDLER checks
    // (docs/19-server-side-rendering.md §6). Declaring it Public is the honest
    // description of what the filter does here — nothing — rather than a
    // relaxation.
    {anvil::PermSet{}, "/preview/{id}",
     ac::RouteAccess::Public, ac::RouteMethod::Get},

    // Two routes that UPGRADE, declared under Get because a handshake is a GET
    // and the filter resolves (pattern, method) like it does for anything else.
    //
    // Lowercase, and not as a style choice: Drogon keys its WebSocket map by the
    // lowercased path and reports that spelling back as the matched pattern, so
    // a pattern with a capital in it would be looked up here and not found —
    // which denies every upgrade, silently. `register_websocket_route` refuses
    // one rather than letting this table be the place it is discovered.
    {anvil::PermSet{}, "/ws/feed",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},

    // The stealth half of the pair, and the reason there are two: a refused
    // upgrade to a stealth route has to be byte-identical to an unmatched one,
    // and that property needs a route whose existence is secret to assert it
    // against.
    {anvil::perm_mask(Perm::AuditRead), "/ws/audit",
     ac::RouteAccess::Stealth, ac::RouteMethod::Get},

    // Conversations (docs/22-chat.md §9). Authenticated and no bit: taking part
    // needs nothing beyond being signed in, because MEMBERSHIP is what decides,
    // and the handler answers a non-member with the stealth 404 itself. Creating
    // a group or a channel needs a bit, but which one depends on the kind in the
    // body, so the kind table checks it rather than this one.
    {anvil::PermSet{}, "/chat/conversations",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/direct/{user}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Put},
    {anvil::PermSet{}, "/chat/conversations",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/conversations/{c}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/conversations/{c}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Patch},
    {anvil::PermSet{}, "/chat/conversations/{c}/timer",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Put},
    {anvil::PermSet{}, "/chat/conversations/{c}/members",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/conversations/{c}/members",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/conversations/{c}/members/{user}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Patch},
    {anvil::PermSet{}, "/chat/conversations/{c}/members/{user}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Delete},
    {anvil::PermSet{}, "/chat/conversations/{c}/messages",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/conversations/{c}/messages",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/conversations/{c}/messages/{seq}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Patch},
    {anvil::PermSet{}, "/chat/conversations/{c}/messages/{seq}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Delete},
    {anvil::PermSet{}, "/chat/conversations/{c}/messages/{seq}/reaction",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Put},
    {anvil::PermSet{}, "/chat/conversations/{c}/messages/{seq}/readers",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/conversations/{c}/receipts",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/conversations/{c}/preferences",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Patch},
    {anvil::PermSet{}, "/chat/conversations/{c}/invites",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/conversations/{c}/invites",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Delete},
    {anvil::PermSet{}, "/chat/join",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/conversations/{c}/follow",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/blocks/{user}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Put},
    {anvil::PermSet{}, "/chat/blocks/{user}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Delete},
    // The chat socket (docs/22-chat.md §8.2): an upgrade, so a GET, and
    // lowercase for the reason the two above say. Authenticated with no bit,
    // like every other chat route: what it pushes is decided per conversation
    // by membership, when the wake is published.
    {anvil::PermSet{}, "/chat/socket",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    // Presence (docs/22-chat.md §8.3), answered per viewer by the application's
    // hook; with presence off it answers as this route not existing.
    {anvil::PermSet{}, "/chat/presence/{user}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    // Devices, keys and the per-device queue (docs/22-chat.md §7.3–§7.6).
    // Authenticated with no bit: a device is the account's own, a claim is
    // decided by sharing an encrypted conversation, and the handlers refuse
    // everything else themselves. What a session alone cannot do (a first
    // device without a fresh sign-in, a later one without an existing device's
    // signature) is enforced by the service, not by a bit.
    {anvil::PermSet{}, "/chat/devices",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/devices",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Put},
    {anvil::PermSet{}, "/chat/devices/link",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/devices/{device}",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Delete},
    {anvil::PermSet{}, "/chat/keys",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/conversations/{c}/keys/claim",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/conversations/{c}/devices",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/device-queue",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    {anvil::PermSet{}, "/chat/device-queue",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Delete},

    // Presence for a page of accounts at once, each still asked of the hook.
    {anvil::PermSet{}, "/chat/presence",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Get},
    // A device replacing its own signed prekey and last-resort key.
    {anvil::PermSet{}, "/chat/devices/{device}/keys",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Put},
    // The link relay (docs/22-chat.md §7.3.1): a new device leaves its keys,
    // an approver of the same account reads them and signs, the new device
    // collects. Every token is in a BODY, never a path, which is in every
    // access log; reading and collecting are POSTs for that reason only.
    {anvil::PermSet{}, "/chat/link-requests",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/link-requests/read",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/link-requests/approve",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::PermSet{}, "/chat/link-requests/collect",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    // Staff review of a conversation they are not in (docs/22-chat.md §9.2).
    // STEALTH behind a bit of this application's, so the paths reach only a
    // holder's route table; the handler records every read in the audit log
    // before it shows anything.
    {anvil::perm_mask(Perm::ChatReview), "/chat/review/{c}",
     ac::RouteAccess::Stealth, ac::RouteMethod::Get},
    {anvil::perm_mask(Perm::ChatReview), "/chat/review/{c}/messages",
     ac::RouteAccess::Stealth, ac::RouteMethod::Get},
    // A member reports messages to staff. Authenticated: membership decides.
    {anvil::PermSet{}, "/chat/conversations/{c}/reports",
     ac::RouteAccess::Authenticated, ac::RouteMethod::Post},
    {anvil::perm_mask(Perm::ChatReview), "/chat/reports",
     ac::RouteAccess::Stealth, ac::RouteMethod::Get},
    // A chat attachment, served from the media origin by its grant
    // (docs/22-chat.md §6.1). Public, and protected by its handler, for the
    // preview's reason: the credential is the grant in the path, which the
    // access filter does not know how to read, and the session cookie never
    // reaches this origin anyway.
    {anvil::PermSet{}, "/m/{grant}/{role}",
     ac::RouteAccess::Public, ac::RouteMethod::Get},
}};

// Every stealth route requires a permission. A stealth route that required none
// would be a public route that answers 404 instead of 200, which is not stealth —
// it is a route nobody can reach, by accident.
static_assert([] {
    for (const ac::RoutePolicy& route : kRoutes) {
        if (route.access == ac::RouteAccess::Stealth && !route.required.any()) {
            return false;
        }
    }
    return true;
}(), "a stealth route must require a permission");

// A guarded route requires at least one bit, and a public one requires none.
// Either mismatch is a route that does not do what its access class says.
static_assert([] {
    for (const ac::RoutePolicy& route : kRoutes) {
        const bool needs = route.access == ac::RouteAccess::Guarded ||
                           route.access == ac::RouteAccess::Stealth;
        if (needs && !route.required.any()) { return false; }
        if (route.access == ac::RouteAccess::Public && route.required.any()) { return false; }
    }
    return true;
}(), "a guarded route with no bits, or a public route with some");

// (pattern, method) pairs are unique. A duplicate makes the second entry
// unreachable, and which of the two wins is whichever the linear scan meets
// first — so two entries disagreeing about an authority resolve silently.
static_assert([] {
    for (std::size_t i = 0; i < kRoutes.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (kRoutes[j].pattern == kRoutes[i].pattern &&
                kRoutes[j].method == kRoutes[i].method) {
                return false;
            }
        }
    }
    return true;
}(), "two entries share a (pattern, method) pair");

}  // namespace testapp
