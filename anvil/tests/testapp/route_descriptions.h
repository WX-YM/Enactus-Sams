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
#include "anvil/media/edit_shapes.h"

#include "responses.h"
#include "routes.h"

namespace testapp {

namespace d = anvil::descriptor;

inline constexpr std::array<d::RouteDescription, 69> kRouteDescriptions{{
    // Public, so the path is compiled into a client bundle. Not idempotent: a
    // repeated login is a second session and a second row in the security log.
    {"auth.login", "/login", "", "login", "", 0, ac::RouteMethod::Post, false},

    // The salt route. Counted into the login bucket, because it is the first
    // half of a login and an unmetered one is a free way to walk the users
    // index. Idempotent: it is a read, and a client that lost the answer asks
    // again.
    {"auth.prehash", "/auth/prehash", "", "login", "", 0, ac::RouteMethod::Post, true},

    // Not idempotent: a registration that is retried after a lost response is
    // answered as a duplicate — correctly, and indistinguishably — but it is
    // still a second attempt at a write, and a client must not be told it may
    // repeat one freely.
    {"auth.signup", "/signup", "", "signup", "", 0, ac::RouteMethod::Post, false},

    // A code guess counts into `verify`; asking for a new code into
    // `resend-addr`. Neither is idempotent: a verify consumes a code, and a
    // resend or a reset request mints a new one that replaces the last.
    {"auth.verify", "/auth/verify", "", "verify", "", 0, ac::RouteMethod::Post, false},
    {"auth.resend", "/auth/resend", "", "resend-addr", "", 0, ac::RouteMethod::Post, false},
    {"auth.reset", "/auth/reset", "", "resend-addr", "", 0, ac::RouteMethod::Post, false},
    {"auth.reset_confirm", "/auth/reset/confirm", "", "verify", "", 0,
     ac::RouteMethod::Post, false},
    {"auth.password", "/auth/password", "", "", "", 0, ac::RouteMethod::Post, false},

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

    // An edit is one libvips render on cpu_pool, the same cost as an upload, so
    // it counts into the same `media` bucket. NOT idempotent in the transport
    // sense — it creates an object — although the server does resolve the same
    // recipe on the same source to one object (docs/21-image-edits.md §3). The
    // declared shapes are the library's own, so they are the bytes its handlers
    // write rather than a description of them kept here.
    {"media.edit", "/media-edits/{ns}/{id}", "", "media", "", 0, ac::RouteMethod::Post, false,
     false, false, anvil::media::kEditResponse},
    {"media.edit_state", "/media-edits/{ns}/{id}", "", "", "", 0, ac::RouteMethod::Get, true,
     false, false, anvil::media::kEditStateResponse},

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

    // Conversations (docs/22-chat.md §9). The handlers are anvil's; the ids,
    // patterns and budgets are this table's, and install_chat_routes refuses at
    // boot a pattern with the wrong number of placeholders.
    //
    // A send is IDEMPOTENT although it is a POST, and it is the route the flag
    // exists for: the client id in the body is the key, and a retry after a lost
    // response is answered with the first message (docs/22 §4.2). Creating a
    // conversation is too, for the same reason: its required `cid` finds the
    // first conversation on a retry (§3.1). Receipts are watermarks that only
    // move forward, and a join while already a member is that membership.
    // Adding people, removing one, revoking and minting an invite are not: each
    // repeated is a second effect, or a refusal a client would misread.
    //
    // Responses are hand-written and undescribed: a message nests attachments,
    // mentions, a card and a system event, which the flat response grammar does
    // not express (anvil/chat/routes.h).
    {"chat.create", "/chat/conversations", "", "chat-write", "",
     0, ac::RouteMethod::Post, true},
    {"chat.open_direct", "/chat/direct/{user}", "", "chat-write", "",
     0, ac::RouteMethod::Put, true},
    {"chat.list", "/chat/conversations", "", "", "act", 100, ac::RouteMethod::Get, true},
    {"chat.get", "/chat/conversations/{c}", "", "", "", 0, ac::RouteMethod::Get, true},
    {"chat.update", "/chat/conversations/{c}", "", "chat-write", "",
     0, ac::RouteMethod::Patch, true},
    {"chat.set_timer", "/chat/conversations/{c}/timer", "", "chat-write", "",
     0, ac::RouteMethod::Put, true},
    {"chat.members", "/chat/conversations/{c}/members", "", "", "u",
     100, ac::RouteMethod::Get, true},
    {"chat.add_members", "/chat/conversations/{c}/members", "", "chat-write", "",
     0, ac::RouteMethod::Post, false},
    {"chat.update_member", "/chat/conversations/{c}/members/{user}", "", "chat-write", "",
     0, ac::RouteMethod::Patch, true},
    {"chat.remove_member", "/chat/conversations/{c}/members/{user}", "", "chat-write", "",
     0, ac::RouteMethod::Delete, false},
    {"chat.send", "/chat/conversations/{c}/messages", "", "chat-send", "",
     0, ac::RouteMethod::Post, true},
    {"chat.history", "/chat/conversations/{c}/messages", "", "", "seq",
     100, ac::RouteMethod::Get, true},
    {"chat.edit", "/chat/conversations/{c}/messages/{seq}", "", "chat-send", "",
     0, ac::RouteMethod::Patch, true},
    {"chat.revoke", "/chat/conversations/{c}/messages/{seq}", "", "chat-write", "",
     0, ac::RouteMethod::Delete, false},
    {"chat.react", "/chat/conversations/{c}/messages/{seq}/reaction", "", "chat-send", "",
     0, ac::RouteMethod::Put, true},
    {"chat.read_by", "/chat/conversations/{c}/messages/{seq}/readers", "", "", "",
     0, ac::RouteMethod::Get, true},
    {"chat.receipts", "/chat/conversations/{c}/receipts", "", "chat-send", "",
     0, ac::RouteMethod::Post, true},
    {"chat.preferences", "/chat/conversations/{c}/preferences", "", "chat-write", "",
     0, ac::RouteMethod::Patch, true},
    {"chat.create_invite", "/chat/conversations/{c}/invites", "", "chat-write", "",
     0, ac::RouteMethod::Post, false},
    {"chat.revoke_invite", "/chat/conversations/{c}/invites", "", "chat-write", "",
     0, ac::RouteMethod::Delete, true},
    {"chat.join", "/chat/join", "", "chat-write", "", 0, ac::RouteMethod::Post, true},
    {"chat.follow", "/chat/conversations/{c}/follow", "", "chat-write", "",
     0, ac::RouteMethod::Post, true},
    {"chat.block", "/chat/blocks/{user}", "", "chat-write", "", 0, ac::RouteMethod::Put, true},
    {"chat.unblock", "/chat/blocks/{user}", "", "chat-write", "",
     0, ac::RouteMethod::Delete, true},
    // An upgrade, idempotent for live.feed's reason: a second handshake from
    // the same device replaces the first socket rather than adding to it.
    {"chat.socket", "/chat/socket", "", "", "", 0, ac::RouteMethod::Get, true},
    {"chat.presence", "/chat/presence/{user}", "", "", "", 0, ac::RouteMethod::Get, true},
    // Devices, keys and the queue. Registering and linking are not idempotent:
    // a second attempt with the same device id is a Conflict, never the first
    // answer again. Unlinking and acknowledging are.
    {"chat.my_devices", "/chat/devices", "", "", "", 0, ac::RouteMethod::Get, true},
    {"chat.register_device", "/chat/devices", "", "chat-write", "",
     0, ac::RouteMethod::Put, false},
    {"chat.link_device", "/chat/devices/link", "", "chat-write", "",
     0, ac::RouteMethod::Post, false},
    {"chat.unlink_device", "/chat/devices/{device}", "", "chat-write", "",
     0, ac::RouteMethod::Delete, true},
    {"chat.upload_prekeys", "/chat/keys", "", "chat-write", "", 0, ac::RouteMethod::Post, false},
    // The per-claimer bucket, once per request; the per-target one is checked
    // inside, once for each account named in the body that is a member.
    {"chat.claim_prekeys", "/chat/conversations/{c}/keys/claim", "", "chat-claim", "",
     0, ac::RouteMethod::Post, false},
    {"chat.conversation_devices", "/chat/conversations/{c}/devices", "", "", "u",
     64, ac::RouteMethod::Get, true},
    {"chat.device_queue", "/chat/device-queue", "", "", "_id", 100, ac::RouteMethod::Get, true},
    {"chat.acknowledge_queue", "/chat/device-queue", "", "chat-send", "",
     0, ac::RouteMethod::Delete, true},

    {"chat.presence_many", "/chat/presence", "", "", "", 0, ac::RouteMethod::Get, true},
    // Idempotent: the same keys again are the same state.
    {"chat.rotate_prekeys", "/chat/devices/{device}/keys", "", "chat-write", "",
     0, ac::RouteMethod::Put, true},
    // Not idempotent: each request is a new token replacing the last.
    {"chat.request_link", "/chat/link-requests", "", "chat-write", "",
     0, ac::RouteMethod::Post, false},
    {"chat.read_link_request", "/chat/link-requests/read", "", "chat-send", "",
     0, ac::RouteMethod::Post, true},
    // Not idempotent: a second approval is a Conflict, never the first again.
    {"chat.approve_link_request", "/chat/link-requests/approve", "", "chat-write", "",
     0, ac::RouteMethod::Post, false},
    // Not idempotent: the approval is handed over once and then gone.
    {"chat.collect_link_approval", "/chat/link-requests/collect", "", "chat-send", "",
     0, ac::RouteMethod::Post, false},
    {"chat.review_conversation", "/chat/review/{c}", "", "", "u", 100, ac::RouteMethod::Get,
     true},
    {"chat.review_history", "/chat/review/{c}/messages", "", "", "seq", 100,
     ac::RouteMethod::Get, true},
    // Idempotent: a report over the same range by the same member is the first.
    {"chat.report", "/chat/conversations/{c}/reports", "", "chat-write", "",
     0, ac::RouteMethod::Post, true},
    {"chat.reports", "/chat/reports", "", "", "_id", 100, ac::RouteMethod::Get, true},
    // Public, so the path is in a client bundle, and it has to be: a client
    // builds MEDIA_ORIGIN + this pattern from the grant a history page handed
    // it. The grant is the authority, so naming where it is spent discloses
    // nothing.
    {"media.grant", "/m/{grant}/{role}", "", "", "", 0, ac::RouteMethod::Get, true},
}};

static_assert(d::descriptions_match(kRoutes, kRouteDescriptions),
              "every route is described exactly once, every description names a route "
              "that exists and names a real method rather than Any, every id is unique, "
              "and a list route carries both a cursor and a ceiling or neither");

}  // namespace testapp
