#pragma once

// The reference application's route descriptions.
//
// anvil ships RouteDescription and descriptions_match; which routes exist, what
// each is called by a client, and which of them are safe to repeat is the
// application's (docs/01-seams.md §14).
//
// One entry per entry in routes.h, and descriptions_match() asserts it below —
// including the pair that shares a pattern under two methods, which is the case
// a table keyed by path alone gets wrong.

#include <array>

#include "anvil/descriptor/route_description.h"

#include "responses.h"
#include "routes.h"

namespace testapp {

namespace d = anvil::descriptor;

inline constexpr std::array<d::RouteDescription, 14> kRouteDescriptions{{
    // Public, so the path is compiled into a client bundle. Not idempotent: a
    // repeated login is a second session and a second row in the security log.
    {"auth.login", "/login", "", "login", "", 0, ac::RouteMethod::Post, false},

    // Public, so a client compiles the path in — and it has to, because the call
    // that recovers a session cannot itself be discovered from one.
    //
    // NOT idempotent, and this is the entry that shows why the flag is not the
    // verb. Repeating a refresh is safe only inside `rotation_grace` — sixty
    // seconds, and it exists so two tabs racing both succeed, not so a client may
    // retry. Past it the same token is a REPLAY: the session is revoked and the
    // epoch bumped, which is the correct answer to a leaked credential and a
    // catastrophic answer to a lost response. A client told `true` here would
    // sign its user out by retrying after a backoff.
    {"auth.refresh", "/auth/refresh", "", "refresh", "", 0, ac::RouteMethod::Post, false},

    // The route table this holder reaches, which is where every path below that
    // is not public comes from. Named for the SESSION rather than for the table,
    // because the document is an envelope: `{"routes":{…}}` can grow a sibling
    // without a client having to decide whether it is reading a table or a
    // wrapper.
    //
    // Idempotent, and it is the one route here where that is load-bearing in the
    // other direction: a client that cannot safely repeat this call cannot
    // recover its own table after a dropped response, and without the table it
    // cannot spell any of the calls that would have recovered anything else.
    //
    // BOOTSTRAP, and the only entry here that is. Its policy is Authenticated,
    // so a client is not told this path by the holder-scoped table — and the
    // holder-scoped table is what this route RETURNS, so waiting to be told
    // where it is means never being told. The address of the session arrives
    // with the session, which on a cold load does not exist.
    //
    // The alternative is the application writing `/session` by hand, which is
    // the second copy of an address the route table already holds. Declaring the
    // route Public instead would publish the path and move the credential check
    // out of the filter, which is how a route stops failing closed; this keeps
    // `access` Authenticated, so the route still answers 401 and a client can
    // still tell "re-authenticate" from "route gone".
    {"session.current", "/session", "", "", "", 0, ac::RouteMethod::Get, true, true},

    // Repeating a logout is a no-op on a session that is already gone, which is
    // what makes it safe to retry after a response nobody saw.
    //
    // POST, although the POLICY is `Any`. The policy says the handler dispatches
    // internally and accepts every method; the description says what a client
    // sends, and there is no method called ANY. It said `Any` until
    // descriptions_match refused one, and for as long as it did this repository
    // shipped a reference route no generated client could call.
    {"auth.logout", "/session/logout", "", "", "", 0, ac::RouteMethod::Post, true},

    // The one route here whose success body is DESCRIBED, and the trailing two
    // values are the whole of what adopting the response binder costs: not an
    // array, and this shape. Every other row leaves them off and gets
    // `"response":null`, which is what all twelve got before the field existed.
    //
    // The shape is not a second copy of the handler. `write_object<kMeResponse>`
    // walks exactly this declaration and will not compile against a handler that
    // writes a key out of order, a key that is not here, or fewer keys than are —
    // so the schema a client is generated from is a description of the bytes
    // rather than a claim about them (anvil/http/response_writer.h).
    {"identity.me", "/me", "", "", "", 0, ac::RouteMethod::Get, true, false, false,
     kMeResponse},

    {"content.get", "/content/{id}", "", "", "", 0, ac::RouteMethod::Get, true},

    // A capability, because a delete is the most destructive thing here, and the
    // scope is single_use so a client may never retry the call carrying it.
    {"content.delete", "/content/{id}", "ContentDelete", "", "",
     0, ac::RouteMethod::Delete, false},

    // A list route: it names the key it pages by and the ceiling it accepts, and
    // that pair is what makes an offset unspellable in a generated client.
    {"media.list", "/media/{ns}/{id}", "", "media", "_id",
     100, ac::RouteMethod::Get, true},

    {"media.delete", "/media/{ns}/{id}", "MediaUpload", "media", "",
     0, ac::RouteMethod::Delete, false},

    // The media grammar, declared here rather than derived anywhere.
    //
    // It does NOT become a field in the `media` object. That table says what a
    // role IS — its name and the width it resolves to, per namespace — and the
    // route table says where a route LIVES; one address in two tables is two
    // things to keep in agreement, which is the defect this row closed.
    //
    // A generated client joins the two by segment name: `{ns}` and `{role}` are
    // the keys the `media` object already publishes values for, so both
    // parameters come out as enumerations rather than as free strings. The
    // encoding stays the route builder's, so the hazard docs/08 §4 is about — a
    // client assembling `w640.avif` — stays closed, because nothing here tells
    // it either half of that.
    {"media.object", "/media/{ns}/{id}/{role}", "", "", "",
     0, ac::RouteMethod::Get, true},

    // Stealth: the path never reaches a bundle, and a client may not report a
    // denial here as a denial.
    {"audit.list", "/audit", "", "", "_id", 100, ac::RouteMethod::Get, true},

    // Public in the route table and protected by its handler: it is served from
    // CONTENT_ORIGIN, where the host-only session cookie never arrives, so what
    // authorises it is a path-scoped capability cookie. Its path is a link
    // somebody is handed, so compiling it into a bundle discloses nothing.
    {"content.preview", "/preview/{id}", "DraftPreview", "", "",
     0, ac::RouteMethod::Get, true},

    // An upgrade. Idempotent in the only sense that matters for one: opening a
    // second connection is not a second side effect, so a client that lost its
    // response may retry the handshake — which is precisely what it does after
    // the connection is closed for any of the reasons `still_authorized` closes
    // it.
    {"live.feed", "/ws/feed", "", "", "", 0, ac::RouteMethod::Get, true},

    // Stealth, so the path never reaches a bundle — the same rule as
    // `audit.list`, and it applies to an upgrade for exactly the same reason.
    {"live.audit", "/ws/audit", "", "", "", 0, ac::RouteMethod::Get, true},
}};

static_assert(d::descriptions_match(kRoutes, kRouteDescriptions),
              "every route is described exactly once, every description names a route "
              "that exists and names a real method rather than Any, every id is unique, "
              "and a list route carries both a cursor and a ceiling or neither");

}  // namespace testapp
