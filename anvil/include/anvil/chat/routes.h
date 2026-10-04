#pragma once

// Chat on the wire (docs/22-chat.md §9): the handlers over ChatService,
// installed at the patterns the APPLICATION declared for the route ids it
// names, the arrangement install_account_routes and install_media_edit_routes
// make. anvil ships the handlers because each would otherwise be written by
// every application and each would get the order wrong once: the caller, then
// the origin, then the budget, then the work on db_pool, then one answer.
//
// The application declares the routes, their permissions and their budgets,
// because those are facts about the application. The reference application's
// patterns are in tests/testapp/routes.h; a pattern's placeholders are read in
// order, and the installer refuses at boot a pattern with the wrong number.
//
// --- what every handler does the same way --------------------------------------
//
//   * Every conversation-scoped refusal is the STEALTH 404: not a member, never a
//     member, no such conversation, a malformed id in the path. One filter, one
//     answer, byte-identical to an unmatched route.
//   * Every WRITE checks Origin before reading its body (http/origin_check.h):
//     the access filter decides who may act and knows nothing of where the
//     request came from.
//   * The body is parsed on db_pool, not on the loop: the parsed document
//     borrows from an arena on the stack of whatever parses it, so it cannot
//     cross the pool hop, and the work after it is on db_pool anyway.
//   * A validation failure names its field and reason in the `fields` map every
//     anvil failure uses (http/errors.h). Nothing echoes a submitted value.
//   * Responses are `private, no-store`: one account's conversations, and a
//     shared cache holding one would hand it on.
//
// --- what is NOT here ------------------------------------------------------------
//
// Uploads. Streaming bytes into storage is the application's upload handler,
// as it always has been (docs/07-filesystem.md §4); a chat upload is an upload
// into the kind's media namespace whose answer is media::mint_upload_handle
// rather than an id. The per-account byte budget for a Sealed namespace is that
// handler's to enforce, before it opens the sink.
//
// Responses are hand-written and therefore UNDESCRIBED in the descriptor: a
// message nests attachments, mentions, a system event and a card, which the
// flat response grammar (http/response_writer.h) does not express, and a body
// approximated into a schema is one a generated client would then trust
// (descriptor/route_description.h).

#include <span>
#include <string_view>

#include "anvil/accesscontrol/route_registry.h"
#include "anvil/chat/service.h"
#include "anvil/descriptor/route_description.h"
#include "anvil/http/rate_limit.h"

namespace anvil::chat {

// The route ids, each declared in the application's route descriptions. The
// placeholders each pattern must carry, in order, are in brackets.
struct ChatRouteIds final {
    std::string_view create;          // POST
    std::string_view open_direct;     // PUT    [user]
    std::string_view list;            // GET
    std::string_view get;             // GET    [conversation]
    std::string_view update;          // PATCH  [conversation]
    std::string_view set_timer;       // PUT    [conversation]
    std::string_view members;         // GET    [conversation]
    std::string_view add_members;     // POST   [conversation]
    std::string_view update_member;   // PATCH  [conversation, user]
    std::string_view remove_member;   // DELETE [conversation, user]
    std::string_view send;            // POST   [conversation]
    std::string_view history;         // GET    [conversation]
    std::string_view edit;            // PATCH  [conversation, seq]
    std::string_view revoke;          // DELETE [conversation, seq]
    std::string_view react;           // PUT    [conversation, seq]
    std::string_view read_by;         // GET    [conversation, seq]
    std::string_view receipts;        // POST   [conversation]
    std::string_view preferences;     // PATCH  [conversation]
    std::string_view create_invite;   // POST   [conversation]
    std::string_view revoke_invite;   // DELETE [conversation]
    std::string_view join;            // POST
    std::string_view follow;          // POST   [conversation]
    std::string_view block;           // PUT    [user]
    std::string_view unblock;         // DELETE [user]
    std::string_view presence;        // GET    [user]
    // Devices, keys and the per-device queue (docs/22 §7.3, §7.5, §7.6).
    std::string_view my_devices;            // GET
    std::string_view register_device;       // PUT
    std::string_view link_device;           // POST
    std::string_view unlink_device;         // DELETE [device]
    std::string_view upload_prekeys;        // POST
    std::string_view claim_prekeys;         // POST   [conversation]
    std::string_view conversation_devices;  // GET    [conversation]
    std::string_view device_queue;          // GET
    std::string_view acknowledge_queue;     // DELETE
    std::string_view presence_many;           // GET    ?users=a,b,…
    std::string_view rotate_prekeys;          // PUT    [device]
    std::string_view request_link;            // POST
    std::string_view read_link_request;       // POST   {token}
    std::string_view approve_link_request;    // POST   {token, approver, timestamp, link_signature}
    std::string_view collect_link_approval;   // POST   {token}
    std::string_view review_conversation;     // GET    [conversation]  (staff; optional)
    std::string_view review_history;          // GET    [conversation]  (staff; optional)
    std::string_view report;                  // POST   [conversation]  (optional)
    std::string_view reports;                 // GET    (staff; optional)
};

struct ChatRoutes final {
    ChatRouteIds        ids;
    // Per ACCOUNT. Sending is the volume, so it is its own budget, apart from
    // the writes that change a conversation, which a client makes rarely.
    http::RateLimitRule send_budget;
    http::RateLimitRule write_budget;
    // Prekey claims, budgeted TWICE (docs/22 §7.5): per claiming account, and
    // per account whose keys are claimed. Exhausting a victim's one-time keys
    // pushes every new session with them onto the reusable last-resort key,
    // and the per-target rule is what an attacker with many accounts runs
    // into. The two rules need distinct buckets, or one account's claims and
    // the claims against it would share a counter.
    http::RateLimitRule claim_budget;
    http::RateLimitRule claim_target_budget;
};

// Throws std::invalid_argument at boot when an id is not described, or its
// pattern carries the wrong number of placeholders.
//
// `service` and `limiter` must outlive HTTP serving; the handlers hold both by
// reference, as every handler in an application holds its services.
void install_chat_routes(const ChatService& service, http::RateLimiter& limiter,
                         std::span<const accesscontrol::RoutePolicy> routes,
                         std::span<const descriptor::RouteDescription> descriptions,
                         const ChatRoutes& config);

}  // namespace anvil::chat
