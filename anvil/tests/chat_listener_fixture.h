#pragma once

// The chat listener binary's one stack: the service, its routes and its live
// delivery, installed once behind the real access filter, and the helpers
// every chat listener file drives them with.
//
// One stack because the binary has one listener (tests/listener_fixture.h):
// a second file that installed its own routes would register every chat
// pattern twice. So the files share this header and the registrar in it, which
// is `inline` for the reason listener_fixture.h gives — a per-file copy would
// install the routes once per file.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/audit/service.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/epoch_resolver.h"
#include "anvil/auth/token.h"
#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/live.h"
#include "anvil/chat/prekeys.h"
#include "anvil/chat/presence.h"
#include "anvil/chat/routes.h"
#include "anvil/chat/service.h"
#include "anvil/chat/socket.h"
#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/random.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/rate_limit.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "anvil/media/grant.h"
#include "anvil/media/grant_route.h"
#include "anvil/media/service.h"
#include "anvil/notifications/sse.h"
#include "anvil/redis/redis_client.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "listener_fixture.h"
#include "testapp/audit_actions.h"
#include "testapp/chat_cards.h"
#include "testapp/chat_collections.h"
#include "testapp/chat_kinds.h"
#include "testapp/namespaces.h"
#include "testapp/perms.h"
#include "testapp/route_descriptions.h"
#include "testapp/routes.h"

namespace anvil::chatfixture {

namespace ac = accesscontrol;
using testfixture::Exchange;

inline constexpr std::string_view kAllowedOrigin = "https://example.test";
inline constexpr std::string_view kForeignOrigin = "https://attacker.test";

[[nodiscard]] inline std::int64_t now_unix() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// --- the credential ---------------------------------------------------------

// The environment variable a process passes its key to its peers in, as hex.
inline constexpr const char* kTokenKeyEnv = "ANVIL_CHAT_TOKEN_KEY";

// Drawn rather than fixed, for the reason session_listener_test.cc gives: a key
// constant in a test file is one somebody copies into a deployment. A peer
// process takes its parent's from kTokenKeyEnv, because a cookie one process
// signs must open on the other, as it does across a deployment's instances.
[[nodiscard]] inline const std::shared_ptr<const auth::TokenKeys>& keys() {
    static const std::shared_ptr<const auth::TokenKeys> shared = [] {
        std::array<std::uint8_t, auth::TokenKeys::kKeyBytes> key =
            crypto::random_array<auth::TokenKeys::kKeyBytes>();
        const char* hex = std::getenv(kTokenKeyEnv);
        if (hex != nullptr && std::string_view{hex}.size() == key.size() * 2) {
            for (std::size_t i = 0; i < key.size(); ++i) {
                key[i] = static_cast<std::uint8_t>(
                    std::stoul(std::string{hex + (2 * i), 2}, nullptr, 16));
            }
        }
        return std::make_shared<const auth::TokenKeys>(1, key);
    }();
    return shared;
}

class CachedEpochs final : public ac::EpochResolver {
public:
    [[nodiscard]] ac::EpochVerdict check_cached(const Uuid&,
                                                std::uint64_t) const noexcept override {
        return ac::EpochVerdict::Match;
    }
    void resolve_async(const Uuid&, std::function<void(Result<std::uint64_t>)> done) override {
        done(fail(ErrorCode::ServiceUnavailable));
    }
};

[[nodiscard]] inline CachedEpochs& epochs() {
    static CachedEpochs resolver{};
    return resolver;
}

// The `__Host-at` cookie a browser signed in as `user` would send. Everybody
// may create a group, so a case can make one as whoever it likes.
[[nodiscard]] inline std::string cookie_for(const Uuid& user, const Uuid& session) {
    const auth::AccessClaims claims{
        .user_id = user,
        .session_id = session,
        .permissions = perm_mask(testapp::Perm::ChatCreateGroup),
        .perm_epoch = 1,
        .expires_at = static_cast<std::uint32_t>(now_unix() + 900),
        .user_type = UserType::Client,
        .locale = *Locale::from_tag("en"),
        .reserved = {},
    };
    return std::string{ac::kAccessCookieName} + "=" + auth::encode(claims, *keys());
}

// A staff member's cookie: the review permission and nothing else.
[[nodiscard]] inline std::string staff_cookie_for(const Uuid& user) {
    const auth::AccessClaims claims{
        .user_id = user,
        .session_id = uuid::generate_v7(),
        .permissions = perm_mask(testapp::Perm::ChatReview),
        .perm_epoch = 1,
        .expires_at = static_cast<std::uint32_t>(now_unix() + 900),
        .user_type = UserType::Client,
        .locale = *Locale::from_tag("en"),
        .reserved = {},
    };
    return std::string{ac::kAccessCookieName} + "=" + auth::encode(claims, *keys());
}

// A fresh session each call, so every cookie is a device of its own.
[[nodiscard]] inline std::string cookie_for(const Uuid& user) {
    return cookie_for(user, uuid::generate_v7());
}

// --- the service the routes are installed over ------------------------------

inline constexpr std::string_view kMedia = "media";

[[nodiscard]] inline media::GrantKeys& grant_keys() {
    static media::GrantKeys keys{1, crypto::random_array<media::GrantKeys::kKeyBytes>()};
    return keys;
}

inline constexpr std::array<std::uint8_t, 32> kPepper{{3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7,
                                                      9, 3, 2, 3, 8, 4, 6, 2, 6, 4, 3, 3, 8, 3,
                                                      2, 7, 9, 5}};

// The one account whose presence nobody else may see, so the route's case can
// ask about it: the hook below is what an application's contacts would decide.
[[nodiscard]] inline const Uuid& shy_account() {
    static const Uuid shy = uuid::generate_v4();
    return shy;
}

// Live delivery, when there is a Redis to carry it. The routes' own cases need
// none, and without one the socket is simply not installed and its cases skip.
[[nodiscard]] inline std::unique_ptr<chat::ChatLive> live_for(
    const chat::ChatRepository& repository, notifications::SseHub& sse) {
    if (!testfixture::redis_ready()) { return nullptr; }
    return std::make_unique<chat::ChatLive>(
        chat::ChatLiveConfig{
            .subscriber =
                chat::WakeSubscriberConfig{.url = testfixture::redis_url(),
                                           .connect_timeout = std::chrono::milliseconds{500},
                                           .poll_interval = std::chrono::milliseconds{20},
                                           .reconnect_initial = std::chrono::milliseconds{50},
                                           .reconnect_max = std::chrono::milliseconds{500},
                                           .client_name = "anvil-chat-listener"},
            .hub = chat::HubLimits{.max_sockets = 0, .max_per_account = 5},
            .member_cache_bytes = 1U << 20U,
            .sse = &sse,
            .presence =
                chat::PresenceConfig{
                    .enabled = true,
                    .may_see = [](mongocxx::client&, const Uuid&,
                                  const Uuid& subject) { return subject != shy_account(); },
                    .collection = "chat_presence",
                    .databases = testfixture::scratch_names()}},
        redis::RedisClient::instance(), repository);
}

// Two hooks a deployment wires and the listener cases leave unset, so they see
// every send bump the chat list and report nothing. The load case sets both
// before it sends — the Redis activity gate, and a commit clock — and the stack
// reads them on every send.
struct StackHooks final {
    std::function<void(const chat::MessageEvent&)> on_message;
    std::function<bool(const Uuid&)>               claim_activity_bump;
};

[[nodiscard]] inline StackHooks& stack_hooks() {
    static StackHooks hooks{};
    return hooks;
}

// Built once per process and never destroyed: the handlers hold it by reference
// for as long as the listener serves, which is until the process exits.
struct Stack final {
    chat::ChatRepository repository{testfixture::scratch_names(), testapp::kChatCollections,
                                    testapp::kChatKinds};
    media::MediaService  media{std::string{testfixture::scratch_names().for_collection(kMedia)},
                              kMedia};
    // The notification streams, for the fallback's cases (docs/22 §8.1).
    notifications::SseHub sse{notifications::SseLimits{.max_streams = 4096, .max_per_reader = 4}};
    std::unique_ptr<chat::ChatLive> live{live_for(repository, sse)};
    chat::DeviceDirectory devices{testfixture::scratch_names(), testapp::kDeviceCollections,
                                  testapp::kDeviceConfig};
    chat::DeviceQueue     queue{testfixture::scratch_names(), testapp::kDeviceCollections.queue};
    chat::PrekeyDirectory prekeys{testfixture::scratch_names(), testapp::kDeviceCollections,
                                  testapp::kDeviceConfig};
    // Where staff reads are recorded (docs/22 §9.2), in the scratch database.
    audit::AuditService  audit{std::string{testfixture::scratch_names().for_collection("audit_log")},
                               "audit_log", testapp::kAuditActions,
                               audit::AuditAction::of(testapp::Action::AccessDenied)};
    chat::ChatService    service{chat::ChatServiceDeps{
           .repository = repository,
           .media = media,
           .grants = grant_keys(),
           .kinds = testapp::kChatKinds,
           .cards = testapp::kChatCards,
           .invite_pepper = kPepper,
           .hooks = chat::ChatHooks{
               .may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; },
               .on_membership = {},
               .on_message =
                   [](const chat::MessageEvent& event) {
                       if (stack_hooks().on_message) { stack_hooks().on_message(event); }
                   },
               .claim_activity_bump =
                   [](const Uuid& conversation) {
                       return !stack_hooks().claim_activity_bump ||
                              stack_hooks().claim_activity_bump(conversation);
                   },
               // Every cookie here is a fresh v7 session, minted as a sign-in
               // mints one, so its instant is when it signed in.
               .authenticated_at =
                   [](mongocxx::client&, const Uuid&, const Uuid& session)
                       -> std::optional<db::TimeMs> {
                       return db::TimeMs{std::chrono::milliseconds{uuid::v7_timestamp_ms(session)}};
                   },
               .on_device = {}},
           .live = live.get(),
           .devices = &devices,
           .queue = &queue,
           .prekeys = &prekeys,
           .review = chat::ChatReview{
               .audit = &audit,
               .read_action = audit::AuditAction::of(testapp::Action::ChatConversationReviewed)}}};
    http::RateLimiter    limiter;
};

[[nodiscard]] inline Stack& stack() {
    static Stack* const built = new Stack{};
    return *built;
}

inline constexpr chat::ChatRouteIds kIds{
    .create = "chat.create",
    .open_direct = "chat.open_direct",
    .list = "chat.list",
    .get = "chat.get",
    .update = "chat.update",
    .set_timer = "chat.set_timer",
    .members = "chat.members",
    .add_members = "chat.add_members",
    .update_member = "chat.update_member",
    .remove_member = "chat.remove_member",
    .send = "chat.send",
    .history = "chat.history",
    .edit = "chat.edit",
    .revoke = "chat.revoke",
    .react = "chat.react",
    .read_by = "chat.read_by",
    .receipts = "chat.receipts",
    .preferences = "chat.preferences",
    .create_invite = "chat.create_invite",
    .revoke_invite = "chat.revoke_invite",
    .join = "chat.join",
    .follow = "chat.follow",
    .block = "chat.block",
    .unblock = "chat.unblock",
    .presence = "chat.presence",
    .my_devices = "chat.my_devices",
    .register_device = "chat.register_device",
    .link_device = "chat.link_device",
    .unlink_device = "chat.unlink_device",
    .upload_prekeys = "chat.upload_prekeys",
    .claim_prekeys = "chat.claim_prekeys",
    .conversation_devices = "chat.conversation_devices",
    .device_queue = "chat.device_queue",
    .acknowledge_queue = "chat.acknowledge_queue",
    .presence_many = "chat.presence_many",
    .rotate_prekeys = "chat.rotate_prekeys",
    .request_link = "chat.request_link",
    .read_link_request = "chat.read_link_request",
    .approve_link_request = "chat.approve_link_request",
    .collect_link_approval = "chat.collect_link_approval",
    .review_conversation = "chat.review_conversation",
    .review_history = "chat.review_history",
    .report = "chat.report",
    .reports = "chat.reports",
};

inline void install_chat() {
    ac::AccessControl::init(ac::AccessControlDeps{
        .keys = keys(), .epochs = &epochs(), .denials = nullptr, .routes = testapp::kRoutes});
    auto origins = std::make_shared<http::AllowedOrigins>();
    if (!origins->parse(kAllowedOrigin)) {
        throw std::logic_error{"the origin list this suite installs is malformed"};
    }
    http::install_allowed_origins(std::move(origins));

    chat::install_chat_routes(
        stack().service, stack().limiter, testapp::kRoutes, testapp::kRouteDescriptions,
        chat::ChatRoutes{.ids = kIds,
                         .send_budget = {"chat-send", std::chrono::minutes{1}, 1000},
                         .write_budget = {"chat-write", std::chrono::minutes{1}, 1000},
                         .claim_budget = {"chat-claim", std::chrono::minutes{1}, 1000},
                         // Tight enough that a case can run into it.
                         .claim_target_budget = {"chat-claim-target", std::chrono::minutes{1},
                                                 3}});
    media::install_media_grant_route(stack().media, grant_keys(), testapp::kRoutes,
                                     testapp::kRouteDescriptions, "media.grant");
    if (stack().live != nullptr) {
        chat::install_chat_socket(*stack().live, stack().service, testapp::kRoutes,
                                  testapp::kRouteDescriptions, "chat.socket");
    }
}

inline const testfixture::RouteRegistrar kChatRegistrar{&install_chat};

// --- driving it -------------------------------------------------------------

[[nodiscard]] inline Exchange call(drogon::HttpMethod method, std::string_view path,
                                   const Uuid& user, std::string_view body = {},
                                   std::string_view origin = kAllowedOrigin) {
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(std::string{path});
    req->addHeader("Cookie", cookie_for(user));
    if (!origin.empty()) { req->addHeader("Origin", std::string{origin}); }
    if (!body.empty()) {
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(std::string{body});
    }
    return testfixture::send(req);
}

// A request from a staff member holding the review permission.
[[nodiscard]] inline Exchange call_as_staff(drogon::HttpMethod method, std::string_view path,
                                            const Uuid& user) {
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(std::string{path});
    req->addHeader("Cookie", staff_cookie_for(user));
    req->addHeader("Origin", std::string{kAllowedOrigin});
    return testfixture::send(req);
}

[[nodiscard]] inline Exchange unmatched(drogon::HttpMethod method, const Uuid& user) {
    return call(method, "/chat/no-such-route-at-all", user);
}

// Status, body, type and every header but Date, which names the second the
// response was written and so separates two identical answers a second apart.
[[nodiscard]] inline bool same_response(const drogon::HttpResponsePtr& a,
                                        const drogon::HttpResponsePtr& b) {
    const auto headers = [](const drogon::HttpResponsePtr& r) {
        std::map<std::string, std::string> out{r->headers().begin(), r->headers().end()};
        out.erase("date");
        return out;
    };
    return a->statusCode() == b->statusCode() && a->body() == b->body() &&
           a->contentTypeString() == b->contentTypeString() && headers(a) == headers(b);
}

[[nodiscard]] inline std::string conversation_path(const Uuid& c, std::string_view tail = {}) {
    return "/chat/conversations/" + uuid::to_string(c) + std::string{tail};
}

// A fresh client id, as the composer mints one before its first attempt.
[[nodiscard]] inline std::string cid() {
    return crypto::base64url_encode(crypto::random_array<16>());
}

// One integer member of a JSON object response, or nullopt.
[[nodiscard]] inline std::optional<std::int64_t> json_int(std::string_view body,
                                                          std::string_view key) {
    input::BodyArena arena;
    const input::JsonDocument document = input::parse_json(body, arena);
    if (!document.ok() || !document.root().is_object()) { return std::nullopt; }
    const input::JsonValue* value = document.root().find(key);
    return value == nullptr ? std::nullopt : value->as_int64();
}

class ChatRoutesListener : public ::testing::Test {
protected:
    void SetUp() override {
        if (!testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << testfixture::test_uri();
        }
        if (!testfixture::transactions_available()) {
            GTEST_SKIP() << "transactions need a replica set";
        }
        ASSERT_TRUE(testfixture::pools_ready());
        testfixture::ensure_indexes();
        (void)testfixture::redis_ready();
    }

    // A group `owner` made through the route, with `members` in it.
    [[nodiscard]] Uuid group(const Uuid& owner, const std::vector<Uuid>& members = {}) {
        std::string body = R"({"cid":")" + cid() + R"(","kind":"group","title":"Team","members":[)";
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i != 0) { body += ','; }
            body += '"' + uuid::to_string(members[i]) + '"';
        }
        body += "]}";
        const Exchange made = call(drogon::Post, "/chat/conversations", owner, body);
        EXPECT_EQ(made.result, drogon::ReqResult::Ok);
        EXPECT_EQ(made.response->statusCode(), drogon::k201Created)
            << made.response->body();
        input::BodyArena arena;
        const input::JsonDocument document = input::parse_json(made.response->body(), arena);
        const std::optional<std::string_view> id =
            document.root().find("conversation")->find("id")->as_string();
        return *uuid::parse(*id);
    }

    // A stored object in the chat namespace, as an upload leaves it, and the
    // handle that upload would have answered with.
    [[nodiscard]] std::pair<Uuid, std::string> uploaded(const Uuid& uploader) {
        auto client = db::MongoPool::instance().acquire();
        const Uuid object = uuid::generate_v4();
        EXPECT_TRUE(stack()
                        .media.repository()
                        .insert(*client, media::NewMedia{.variants = {}, .sha256 = {},
                                                         .bytes = 10, .id = object,
                                                         .owner = uploader,
                                                         .uploader_ip = std::nullopt,
                                                         .width = 0, .height = 0,
                                                         .ns = testapp::kChat,
                                                         .mime = fs::Mime::Pdf})
                        .ok());
        return {object, media::mint_upload_handle(grant_keys(), testapp::kChat, object,
                                                  uploader, now_unix())};
    }
};

}  // namespace anvil::chatfixture
