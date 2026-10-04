// The reference application, served on a port.
//
// anvil is a library plus test executables: `anvil_listener_tests` starts Drogon
// in-process and exits, `testapp_emit_descriptor` writes a file, and until this
// program nothing served the reference application anywhere a browser could
// reach it. On a client generator's side that was four written suites — a live
// contract run and three browser runs, the two-tab credential run among them —
// that failed rather than skipped and had nowhere to run.
//
// The evidence that closing it was worth the binary is the browser run that DID
// execute, because it needed no server: it found a Trusted Types sink that all
// 1,064 of that side's own unit tests had passed over. A suite that cannot run
// finds nothing, and a suite that can finds the thing no unit test is shaped to
// see.
//
// --- what keeps this a test binary rather than a deployment -----------------
//
// A reference application that serves is a thing people deploy. Five rules, each
// of which exists because of the way it fails without them, and each marked in
// the code below where it is kept:
//
//   1. It binds LOOPBACK and prints its base URL as the first line of stdout, on
//      an EPHEMERAL port by default, so a harness reads the port rather than
//      guessing it and two runs on one machine do not collide.
//   2. Every credential is DRAWN AT BOOT and printed. A fixed password in a
//      repository is a fixed password in a deployment, and this binary exists to
//      be copied from.
//   3. It REFUSES TO START against a database it did not create, by a marker
//      document it writes on first use. "I pointed the reference server at the
//      wrong URI" must not be a thing only a backup recovers from.
//   4. It SHARES EVERY TABLE with tests/testapp/, so what a browser run sees and
//      what the suite asserts cannot disagree — which is the entire value of the
//      reference application being the proof (CLAUDE.md §1).
//   5. It is NEVER `install()`ed, and it serves exactly ONE static directory at
//      ONE path. That static exception to docs/00-architecture.md §1 — where
//      bytes on disk are the edge's job — is the reason this row exists at all:
//      `SameSite=Lax` cookies are not sent cross-site, so a harness fulfilling
//      its bundle from another origin would be testing a cookie policy no
//      deployment has.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <mongocxx/client.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/route_projection.h"
#include "anvil/accesscontrol/route_registration.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/accounts/routes.h"
#include "anvil/accounts/service.h"
#include "anvil/auth/password.h"
#include "anvil/auth/prehash.h"
#include "anvil/auth/token.h"
#include "anvil/audit/service.h"
#include "anvil/chat/repository.h"
#include "anvil/chat/live.h"
#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/prekeys.h"
#include "anvil/chat/push.h"
#include "anvil/chat/routes.h"
#include "anvil/chat/service.h"
#include "anvil/chat/socket.h"
#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/random.h"
#include "anvil/db/collection_options.h"
#include "anvil/db/codec.h"
#include "anvil/db/collections.h"
#include "anvil/db/migrations.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/http/client_address.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/response_writer.h"
#include "anvil/identity/authz.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "anvil/identity/session_service.h"
#include "anvil/identity/prehash_service.h"
#include "anvil/identity/users.h"
#include "anvil/media/grant.h"
#include "anvil/media/grant_route.h"
#include "anvil/media/service.h"
#include "anvil/notifications/outbound.h"
#include "anvil/notifications/repository.h"
#if ANVIL_HAS_VIPS
#include <vips/vips.h>

#include "anvil/fs/paths.h"
#include "anvil/fs/upload.h"
#include "anvil/images/probe.h"
#include "anvil/http/origin_check.h"
#include "anvil/media/edit_routes.h"
#include "anvil/media/pipeline.h"
#endif
#include "anvil/redis/redis_client.h"
#include "anvil/timer/queue.h"

#include "accounts.h"
#include "audit_actions.h"
#include "chat_cards.h"
#include "chat_collections.h"
#include "chat_kinds.h"
#include "chat_push.h"
#include "indexes.h"
#include "rate_limits.h"
#include "migrations.h"
#include "namespaces.h"
#include "perms.h"
#include "responses.h"
#include "route_descriptions.h"
#include "routes.h"

namespace {

namespace ac = anvil::accesscontrol;
namespace id = anvil::identity;
namespace input = anvil::input;

using anvil::ErrorCode;
using anvil::Locale;
using anvil::PermSet;
using anvil::UserContext;
using anvil::UserStatus;
using anvil::UserType;
using anvil::Uuid;
using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

using Responder = std::function<void(const HttpResponsePtr&)>;

constexpr std::string_view kUsersCollection = "users";
constexpr std::string_view kSessionsCollection = "user_sessions";

// RULE 3. The document that says "this database is mine".
//
// Its collection is deliberately NOT in `config::kCollections`: that table is the
// application's data, every entry of which gets indexes, options and migrations,
// and this is not data — it is a guard on the database as a whole, written before
// any of that runs. Declaring it there would mean the schema step creating it,
// which is precisely the step this check has to happen before.
constexpr std::string_view kMarkerCollection = "anvil_reference_marker";
constexpr std::string_view kMarkerId = "anvil_reference_server";

// --- configuration ----------------------------------------------------------

[[nodiscard]] std::string env_or(const char* key, std::string_view fallback) {
    const char* value = std::getenv(key);
    return (value != nullptr && *value != '\0') ? std::string{value} : std::string{fallback};
}

// RULE 1, half of it. The port may be pinned for a harness that wants a stable
// URL, and it defaults to 0 — the kernel picks a free one — because a fixed port
// is a process nobody can run twice.
//
// The HOST is not configurable and never will be. This binds loopback, and a
// reference application that could be told to bind an interface is one somebody
// binds to an interface.
[[nodiscard]] std::uint16_t configured_port() {
    const std::string text = env_or("ANVIL_REFERENCE_PORT", "0");
    const unsigned long parsed = std::strtoul(text.c_str(), nullptr, 10);
    return parsed <= 65535UL ? static_cast<std::uint16_t>(parsed) : std::uint16_t{0};
}

[[nodiscard]] const anvil::db::DatabaseNames& database_names() {
    static const std::string primary = env_or("ANVIL_REFERENCE_DB", "anvil_reference");
    static const std::string secondary = primary + "_analytics";
    static const anvil::db::DatabaseNames names{
        {std::string_view{primary}, std::string_view{secondary}}};
    return names;
}

[[nodiscard]] std::string primary_database() {
    return std::string{database_names().name_of(0)};
}

// --- the credentials, drawn at boot -----------------------------------------

// RULE 2. Everything secret this process uses comes out of the CSPRNG at boot and
// is printed once. Nothing here is a constant, and that is the whole point: a
// signing key written into a repository is a signing key in somebody's
// deployment, and this binary exists to be copied from.
struct Secrets final {
    std::array<std::uint8_t, anvil::auth::TokenKeys::kKeyBytes> signing_key;
    std::array<std::uint8_t, 32>                                pepper;
    // The two client-prehash keys (docs/05 §12): the stage pepper every stored
    // record is keyed under, and the key that derives a missing account's salt.
    std::array<std::uint8_t, 32>                                prehash_pepper;
    std::array<std::uint8_t, 32>                                prehash_salt_key;
    // Contact verification's two keys: one hashes the CODE, one the address
    // index (identity/verification.h says why they are two).
    std::array<std::uint8_t, 32>                                code_pepper;
    std::array<std::uint8_t, 32>                                address_index_key;
    // Chat: the key every media grant and upload handle is sealed under, and
    // the pepper mixed into every invite token's digest.
    std::array<std::uint8_t, anvil::media::GrantKeys::kKeyBytes> grant_key;
    std::array<std::uint8_t, 32>                                invite_pepper;
    std::string                                                 superadmin_password;
    std::string                                                 editor_password;
};

// 24 base64url characters is 144 bits, which is far past anything a password
// needs to resist — the point is that it is drawn rather than chosen, so it
// cannot be recognised from having been read here.
[[nodiscard]] std::string drawn_password() {
    return anvil::crypto::base64url_encode(anvil::crypto::random_array<18>());
}

[[nodiscard]] const Secrets& secrets() {
    static const Secrets drawn{
        .signing_key = anvil::crypto::random_array<anvil::auth::TokenKeys::kKeyBytes>(),
        .pepper = anvil::crypto::random_array<32>(),
        .prehash_pepper = anvil::crypto::random_array<32>(),
        .prehash_salt_key = anvil::crypto::random_array<32>(),
        .code_pepper = anvil::crypto::random_array<32>(),
        .address_index_key = anvil::crypto::random_array<32>(),
        .grant_key = anvil::crypto::random_array<anvil::media::GrantKeys::kKeyBytes>(),
        .invite_pepper = anvil::crypto::random_array<32>(),
        .superadmin_password = drawn_password(),
        .editor_password = drawn_password(),
    };
    return drawn;
}

[[nodiscard]] const std::shared_ptr<const anvil::auth::TokenKeys>& token_keys() {
    static const std::shared_ptr<const anvil::auth::TokenKeys> keys =
        std::make_shared<const anvil::auth::TokenKeys>(std::uint8_t{1}, secrets().signing_key);
    return keys;
}

// --- the services -----------------------------------------------------------

[[nodiscard]] id::AuthzService& authz() {
    static id::AuthzService service{primary_database(), kUsersCollection};
    return service;
}

[[nodiscard]] const anvil::chat::ChatService& chat_service();

// Every session this application revokes, by any path, ends the chat device it
// registered: otherwise "sign out everywhere" leaves a stolen phone in every
// conversation's device set, and senders keep encrypting to it (docs/22 §7.3).
void end_chat_devices(mongocxx::client& client, const Uuid& user,
                      std::span<const Uuid> sessions) {
    for (const Uuid& session : sessions) {
        if (const anvil::Status ended = chat_service().session_ended(client, user, session);
            !ended) {
            LOG_ERROR << "a revoked session's chat device was not ended: "
                      << static_cast<int>(ended.code());
        }
    }
}

[[nodiscard]] id::SessionService& sessions() {
    static id::SessionService service{primary_database(), kSessionsCollection,
                                      kUsersCollection,   secrets().pepper,
                                      token_keys(),       authz(),
                                      id::SessionPolicy{}, &end_chat_devices};
    return service;
}

[[nodiscard]] anvil::crypto::Key256 key_from(const std::array<std::uint8_t, 32>& bytes) {
    anvil::crypto::Key256 key;
    std::copy(bytes.begin(), bytes.end(), key.mutable_span().begin());
    return key;
}

// Client hashing with the keyed-digest stage — the account layer's default
// (docs/05 §12). This server receives no password on any route: the browser
// derives the credential, and a sign-in costs one HMAC rather than a 64 MiB
// Argon2 on hash_pool.
//
// The client parameters are plain mode's own defaults, so what a guess against
// a dump of this database costs is exactly what it cost before. A function
// that builds a fresh policy each time, because the policy owns its keys and
// both the account service and the seeding below need one.
[[nodiscard]] anvil::auth::PrehashPolicy prehash_policy() {
    return anvil::auth::PrehashPolicy{
        .client = anvil::auth::kDefaultArgon2Params,
        .server = anvil::auth::PrehashKeyedDigestStage{.key_id = "boot",
                                                       .key = key_from(secrets().prehash_pepper)},
        .retired_peppers = {},
        .salt_key = key_from(secrets().prehash_salt_key),
    };
}

[[nodiscard]] id::VerificationService& verification() {
    static id::VerificationService service{primary_database(), "email_verifications",
                                           secrets().code_pepper, secrets().address_index_key};
    return service;
}

[[nodiscard]] anvil::http::RateLimiter& limiter() {
    static anvil::http::RateLimiter shared;
    return shared;
}

// The budgets, by bucket, from this application's own table — with the bucket
// name prefixed by this run's database.
//
// A deployment shares its budgets across every process it runs, which is the
// point of keeping them in Redis. This binary is different: RULE 3 gives every
// run a database of its own, and Redis is the one piece of state two runs
// would otherwise share, so a harness that registered a few accounts an hour
// ago would find this run's signup budget already spent. The prefix makes each
// run's budgets as much its own as its database already is.
[[nodiscard]] anvil::http::RateLimitRule rule(std::string_view bucket) {
    static std::mutex guard;
    static std::deque<std::string> names;  // stable addresses for the string_views
    for (const anvil::http::RateLimitRule& candidate : testapp::kRateLimits) {
        if (candidate.bucket != bucket) { continue; }
        const std::lock_guard lock{guard};
        names.push_back(primary_database() + ":" + std::string{bucket});
        return anvil::http::RateLimitRule{names.back(), candidate.window, candidate.max_events};
    }
    throw std::logic_error{"no rate-limit rule named " + std::string{bucket}};
}

// Where a code goes. A deployment hands it to a queue that sends mail
// (anvil/notifications/outbound.h); this binary PRINTS it, one line per code,
// for the same reason it prints its drawn passwords (rule 2): a harness driving
// it has to be able to complete a verification and a reset, and the line is on
// stdout, which belongs to the harness.
void print_code(anvil::accounts::CodeDelivery delivery) {
    const char* purpose = delivery.purpose == anvil::accounts::CodePurpose::Verify  ? "verify"
                          : delivery.purpose == anvil::accounts::CodePurpose::Reset ? "reset"
                                                                                    : "exists";
    std::printf("code %s %s %s\n", purpose, delivery.address.c_str(),
                delivery.code.empty() ? "-" : delivery.code.c_str());
    std::fflush(stdout);
}

#if ANVIL_HAS_VIPS
// Where an edit's audit row goes. A deployment writes one through its
// AuditService; this binary PRINTS it, for print_code's reason: the harness on
// stdout is the only reader that can check the row was written, and written
// once per answer.
void print_edit(const anvil::media::EditOutcome& outcome) {
    std::printf("edited %s %s %s %s\n", std::string{anvil::http::wire_name(outcome.code)}.c_str(),
                anvil::uuid::to_string(outcome.source).c_str(),
                outcome.edit.has_value() ? anvil::uuid::to_string(*outcome.edit).c_str() : "-",
                outcome.created ? "created" : "-");
    std::fflush(stdout);
}
#endif

[[nodiscard]] const anvil::accounts::AccountService& accounts() {
    static const anvil::accounts::AccountService service{
        anvil::accounts::AccountServiceDeps{primary_database(), kUsersCollection, sessions(),
                                            authz(), verification(), limiter()},
        anvil::accounts::AccountConfig{
            .description = testapp::kAccounts,
            .credentials = anvil::accounts::ClientHashing{prehash_policy()},
            .budgets = {.sign_in_ip = rule("login"),
                        .sign_in_account = rule("login-acct"),
                        .register_ip = rule("signup"),
                        .code_ip = rule("verify"),
                        .code_account = rule("verify-addr"),
                        .issue_ip = rule("verify"),
                        .issue_account = rule("resend-addr")},
            // Five consecutive failures lock the account for fifteen minutes,
            // then each further one doubles it, to a day. This application's
            // decision (docs/05 §4 "Lockout is a counter, not a policy").
            .lock_after = [](std::int32_t failures,
                             anvil::db::TimeMs now) -> std::optional<anvil::db::TimeMs> {
                if (failures < 5) { return std::nullopt; }
                const std::int32_t doublings = std::min(failures - 5, 7);
                return now + std::chrono::minutes{15} * (1 << doublings);
            },
            .deliver = &print_code,
            .code_lifetime = std::chrono::minutes{15},
        }};
    return service;
}

[[nodiscard]] const anvil::media::MediaService& media_service() {
    static const anvil::media::MediaService service{primary_database(), "media"};
    return service;
}

[[nodiscard]] const anvil::media::GrantKeys& grant_keys() {
    static const anvil::media::GrantKeys keys{1, secrets().grant_key};
    return keys;
}

[[nodiscard]] const anvil::chat::ChatRepository& chat_repository() {
    static const anvil::chat::ChatRepository repository{database_names(),
                                                        testapp::kChatCollections,
                                                        testapp::kChatKinds};
    return repository;
}

// The device machinery an encrypted kind needs (docs/22-chat.md §7): the
// directory, the one-time keys and the per-device queue, all three or none.
[[nodiscard]] const anvil::chat::DeviceDirectory& device_directory() {
    static const anvil::chat::DeviceDirectory devices{
        database_names(), testapp::kDeviceCollections, testapp::kDeviceConfig};
    return devices;
}

[[nodiscard]] const anvil::chat::PrekeyDirectory& prekey_directory() {
    static const anvil::chat::PrekeyDirectory prekeys{
        database_names(), testapp::kDeviceCollections, testapp::kDeviceConfig};
    return prekeys;
}

[[nodiscard]] const anvil::chat::DeviceQueue& device_queue() {
    static const anvil::chat::DeviceQueue queue{database_names(),
                                                testapp::kDeviceCollections.queue};
    return queue;
}

// Live delivery: the wake channel, the hub and the member cache (docs/22-chat.md
// §8). The subscriber holds a Redis connection of its own, at the same URL the
// rest of the process uses; built on first use, which boot() makes after Redis
// has answered.
[[nodiscard]] anvil::chat::ChatLive& chat_live() {
    static anvil::chat::ChatLive live{
        anvil::chat::ChatLiveConfig{
            .subscriber = anvil::chat::WakeSubscriberConfig{
                .url = env_or("ANVIL_REFERENCE_REDIS_URL", "tcp://127.0.0.1:6379"),
                .connect_timeout = std::chrono::milliseconds{1000},
                .poll_interval = std::chrono::milliseconds{100},
                .reconnect_initial = std::chrono::milliseconds{100},
                .reconnect_max = std::chrono::milliseconds{5000},
                .client_name = "anvil-reference-chat"},
            .hub = anvil::chat::HubLimits{},
            .member_cache_bytes = 1U << 20U},
        anvil::redis::RedisClient::instance(), chat_repository()};
    return live;
}

// Push nudges (docs/22-chat.md §8.4). The queue is a real one, keyed under this
// run's database so two servers never share it, and the web push transport
// prints what it would send, one line per endpoint:
//
//     push <endpoint address> <count> <title>|<body>
//
// which is what check-reference-server.sh reads. A deployment's transport
// encrypts with notifications/webpush.h and POSTs.
[[nodiscard]] const anvil::notifications::NotificationRepository& notification_repository() {
    static const anvil::notifications::NotificationRepository repository{
        std::string{database_names().for_collection("notification_clients")},
        anvil::notifications::NotificationCollections{"notifications", "notification_inbox",
                                                      "notification_clients"},
        testapp::kTopics};
    return repository;
}

[[nodiscard]] anvil::timer::JobQueue& job_queue() {
    static anvil::timer::JobQueue queue{anvil::timer::JobQueueConfig{
        .prefix = "anvil-reference:" + primary_database() + ":jobs",
        .consumer = "reference",
        .block = std::chrono::milliseconds{500},
        .promoter_interval = std::chrono::milliseconds{250}}};
    return queue;
}

// How a push names an account. The two the server seeds, by the name they are
// printed under; anybody else is nameless, which a template renders as nothing.
[[nodiscard]] std::map<Uuid, std::string>& account_names() {
    static std::map<Uuid, std::string> names;
    return names;
}

[[nodiscard]] anvil::chat::ChatPush& chat_push() {
    static anvil::chat::ChatPush push{
        // A second's window and a second's grace, so a harness waits seconds.
        testapp::chat_push_config(std::chrono::seconds{1}, std::chrono::seconds{1}),
        anvil::chat::PushHooks{
            .enqueue = [](std::span<const std::uint8_t> args, anvil::db::TimeMs due,
                          std::string_view key) -> anvil::Status {
                const anvil::Result<anvil::timer::JobId> job =
                    job_queue().schedule_at(testapp::kChatPushJob, args, due, key);
                if (!job) { return job.error(); }
                return anvil::ok();
            },
            // Every reader in the default locale with previews on: the reference
            // accounts carry no language. An application reads its account row.
            .reader = [](mongocxx::client&, const Uuid&) {
                return anvil::chat::PushReader{Locale{}, true};
            },
            .name_of = [](mongocxx::client&, const Uuid& user) {
                const auto found = account_names().find(user);
                return found == account_names().end() ? std::string{} : found->second;
            }},
        chat_repository(), notification_repository(), testapp::kTemplates,
        [](const anvil::notifications::Delivery& delivery)
            -> anvil::Result<anvil::notifications::DeliveryVerdict> {
            std::printf("push %.*s %d %s|%s\n", static_cast<int>(delivery.address.size()),
                        delivery.address.data(), static_cast<int>(delivery.count),
                        delivery.content.title.c_str(), delivery.content.body.c_str());
            std::fflush(stdout);
            return anvil::notifications::DeliveryVerdict::Delivered;
        }};
    return push;
}

// Where a staff read of a conversation is recorded (docs/22-chat.md §9.2). The
// reference server keeps no other audit, so this one is the chat review's
// alone; it writes synchronously, and needs no timer.
[[nodiscard]] anvil::audit::AuditService& review_audit() {
    static anvil::audit::AuditService audit{
        std::string{database_names().for_collection("audit_log")}, "audit_log",
        testapp::kAuditActions, anvil::audit::AuditAction::of(testapp::Action::AccessDenied)};
    return audit;
}

[[nodiscard]] const anvil::chat::ChatService& chat_service() {
    static const anvil::chat::ChatService service{anvil::chat::ChatServiceDeps{
        .repository = chat_repository(),
        .media = media_service(),
        .grants = grant_keys(),
        .kinds = testapp::kChatKinds,
        .cards = testapp::kChatCards,
        .invite_pepper = secrets().invite_pepper,
        .hooks = anvil::chat::ChatHooks{
            // Everybody may reach everybody, which is the one policy a
            // reference server with two accounts can have. An application's
            // notion of a contact goes here (docs/22-chat.md §3.2), and leaving
            // it unset refuses every reach.
            .may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; },
            .on_membership = {},
            .on_message = {},
            .claim_activity_bump = {},
            // A session here is made only by signing in with a password, and
            // its id is a v7 minted at that instant, so its timestamp is when
            // the account last proved who it is on this session. An
            // application with step-up authentication answers from its own
            // record of that instead.
            .authenticated_at =
                [](mongocxx::client&, const Uuid&, const Uuid& session)
                    -> std::optional<anvil::db::TimeMs> {
                    return anvil::db::TimeMs{
                        std::chrono::milliseconds{anvil::uuid::v7_timestamp_ms(session)}};
                },
            .on_device = {}},
        .live = &chat_live(),
        .push = &chat_push(),
        .devices = &device_directory(),
        .queue = &device_queue(),
        .prekeys = &prekey_directory(),
        .review = anvil::chat::ChatReview{
            .audit = &review_audit(),
            .read_action =
                anvil::audit::AuditAction::of(testapp::Action::ChatConversationReviewed)}}};
    return service;
}

// The group the two seeded accounts share, printed as `chat <id>`, so a harness
// has a conversation to send in without first learning how to make one.
std::optional<Uuid>& seeded_group() {
    static std::optional<Uuid> id;
    return id;
}

// The one stored image this process starts with, printed as `media <ns> <id>`.
// A harness needs a SOURCE to edit, and this server has no upload route: an
// upload is a streamed body on a loop thread, which is a subsystem with suites
// of its own, and what a live run proves here is the edit path around it.
std::optional<Uuid>& seeded_media() {
    static std::optional<Uuid> id;
    return id;
}

[[nodiscard]] id::UserRepository users() {
    return id::UserRepository{primary_database(), kUsersCollection};
}

// --- rule 3: a database this process created, or none at all ----------------

// True when the database is ours to use: either empty, in which case the marker
// is written now, or already carrying our marker.
//
// The "empty" arm is what makes first use work without a setup step, and it is
// also the narrowest arm that can: a database holding ANY collection we did not
// create is one we refuse, because the alternative is a reference server that
// applies a schema and seeds accounts over somebody's data.
[[nodiscard]] bool claim_database(mongocxx::client& client, std::string& why_not) {
    const std::string database = primary_database();
    auto db = client[database];

    const auto marker = db[std::string{kMarkerCollection}].find_one(
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("_id", std::string{kMarkerId})));
    if (marker) { return true; }

    std::vector<std::string> existing = db.list_collection_names();
    if (!existing.empty()) {
        std::sort(existing.begin(), existing.end());
        why_not = "database '" + database + "' holds " + std::to_string(existing.size()) +
                  " collection(s) and none of them is this server's marker (first: '" +
                  existing.front() + "'). Refusing: this process seeds accounts and applies a "
                  "schema, and doing that over somebody else's data is not something a backup "
                  "makes cheap. Point ANVIL_REFERENCE_DB at a database of its own.";
        return false;
    }

    db[std::string{kMarkerCollection}].insert_one(bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("_id", std::string{kMarkerId}),
        bsoncxx::builder::basic::kvp(
            "created_at_utc",
            bsoncxx::types::b_date{std::chrono::system_clock::now()})));
    return true;
}

// --- seeding ----------------------------------------------------------------

struct SeededAccount final {
    std::string email;
    std::string password;
    PermSet     permissions;
    UserType    type;
};

// Enrolled on THIS thread, at boot, which is the one place in the system where
// Argon2 on the calling thread is correct: there is no event loop yet, nothing is
// waiting, and `hash_pool` exists to bound concurrent hashes rather than to move
// them off a thread that has nothing else to do (CLAUDE.md §4).
//
// From the plaintext, because this process drew it: the server runs the client
// stage itself under the salt the salt route will serve for this email, so a
// browser that asks for the salt and hashes the printed password arrives at the
// same credential.
// The account's id when this call enrolled it, nullopt when it was already
// there from an earlier start of this same database.
std::optional<Uuid> seed(mongocxx::client& client, const SeededAccount& account) {
    // Under the salt the account layer derives for this email at registration,
    // so the salt route answers a browser that hashes the printed password with
    // exactly what this record was enrolled under.
    const anvil::auth::PrehashHasher hasher{prehash_policy()};
    const std::string hash = hasher.enroll_plaintext(
        account.password,
        hasher.derive_salt(static_cast<std::uint8_t>(id::LoginIdentity::Email), account.email));

    const Uuid id = anvil::uuid::generate_v7();
    const std::string username = account.email.substr(0, account.email.find('@'));
    const anvil::Status inserted =
        users().insert(client, id::NewUser{.id = id,
                                           .email_normalised = account.email,
                                           .email_display = account.email,
                                           .username_normalised = username,
                                           .username_display = username,
                                           .password_hash = hash,
                                           .phone_e164 = {},
                                           .locale = Locale{},
                                           .status = UserStatus::Active});
    if (!inserted.ok()) {
        // Conflict means the account is already there from an earlier run of this
        // same database, which is the ordinary case on a second start. Anything
        // else is a real failure and the caller sees it in the log.
        if (inserted.error().code != ErrorCode::Conflict) {
            LOG_ERROR << "seeding " << account.email << " failed";
        }
        return std::nullopt;
    }

    auto session = client.start_session();
    session.start_transaction();
    // DIRECT and EFFECTIVE are the same mask here, and that is a fact about this
    // seed rather than a shortcut: `effective` is the union of direct grants and
    // role masks, computed at write time so a read is one AND
    // (anvil/identity/authz.h), and these accounts hold no roles. Passing an
    // empty `effective` would store an account whose grid shows permissions and
    // whose TOKEN carries none — which is what the first version of this did,
    // and the symptom was a signed-in editor handed a route table with the
    // content routes missing.
    const auto typed = users().set_user_type(client, session, id, 1, account.type,
                                             account.permissions, account.permissions);
    if (!typed.ok()) {
        session.abort_transaction();
        LOG_ERROR << "granting " << account.email << " its authority failed";
        return std::nullopt;
    }
    session.commit_transaction();
    return id;
}

#if ANVIL_HAS_VIPS
// Storage in a directory of this run's own, like the database (RULE 3): a
// harness's edits are files, and two runs sharing a tree would see each other's.
std::string& storage_dir() {
    static std::string dir;
    return dir;
}

[[nodiscard]] bool open_storage(std::string& why_not) {
    std::string pattern = env_or("TMPDIR", "/tmp") + "/anvil-reference-XXXXXX";
    if (::mkdtemp(pattern.data()) == nullptr) {
        why_not = "could not create a storage directory";
        return false;
    }
    anvil::images::init("anvil_reference_server");
    anvil::fs::Storage::init(pattern);
    storage_dir() = pattern;
    std::fprintf(stderr, "anvil_reference_server: media storage at %s\n", pattern.c_str());
    return true;
}

// A generated picture, stored through the same stages an upload takes: a
// gradient with a bright band, so an edit of it is visibly an edit.
void seed_media(mongocxx::client& client) {
    VipsImage* xy = nullptr;
    if (vips_xyz(&xy, 1600, 1200, nullptr) != 0) { return; }
    VipsImage* scaled = nullptr;
    const int scaled_ok = vips_linear1(xy, &scaled, 0.12, 0.0, "uchar", TRUE, nullptr);
    g_object_unref(xy);
    if (scaled_ok != 0) { return; }
    VipsImage* rgb = nullptr;
    const int joined = vips_bandjoin_const1(scaled, &rgb, 180.0, nullptr);
    g_object_unref(scaled);
    if (joined != 0) { return; }
    void* buffer = nullptr;
    std::size_t size = 0;
    const int encoded = vips_image_write_to_buffer(rgb, ".png", &buffer, &size, nullptr);
    g_object_unref(rgb);
    if (encoded != 0) { return; }

    auto opened = anvil::fs::UploadSink::open(
        anvil::fs::Storage::instance(),
        anvil::fs::UploadLimits{anvil::images::kMaxBytes, 0}, testapp::kMedia);
    if (!opened) {
        g_free(buffer);
        return;
    }
    anvil::fs::UploadSink sink = std::move(opened).value();
    const anvil::Status written =
        sink.write(std::span<const std::uint8_t>{static_cast<std::uint8_t*>(buffer), size});
    g_free(buffer);
    if (!written) { return; }
    const anvil::Result<anvil::fs::UploadResult> finished = sink.finish("image/png");
    if (!finished) { return; }
    const anvil::Result<anvil::media::ProcessedMedia> processed =
        anvil::media::process(testapp::kMedia, finished.value());
    if (!processed) {
        LOG_ERROR << "seeding the reference image failed";
        return;
    }
    if (media_service()
            .record(client, testapp::kMedia, Uuid{}, processed.value(), finished.value().sha256,
                    std::nullopt)
            .ok()) {
        seeded_media() = processed.value().id;
    }
}
#endif

// --- responses --------------------------------------------------------------

[[nodiscard]] HttpResponsePtr json(int status, std::string body) {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    response->setContentTypeString(anvil::http::kJsonContentType);
    response->addHeader("Cache-Control", "private, no-store");
    response->setBody(std::move(body));
    return response;
}

// --- handlers ---------------------------------------------------------------

// The holder-scoped route table, which is the first call a cold client makes and
// the only one whose address it is allowed to compile in.
void session(const HttpRequestPtr& req, Responder&& callback) {
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    if (ctx == nullptr) {
        callback(ac::not_found_response());
        return;
    }

    std::string body{R"({"routes":)"};
    ac::append_reachable_routes(body, testapp::kRoutes, testapp::kRouteDescriptions,
                                ctx->permissions, ctx->user_type);
    body += R"(,"authority":)";
    ac::append_holder_authority(body, ctx->permissions, ctx->user_type, testapp::kPerms);
    body += '}';

    const HttpResponsePtr response = json(200, std::move(body));
    // Per HOLDER. A shared cache holding one copy would serve one holder's map to
    // another, which is exactly the disclosure the projection exists to prevent
    // reintroduced one layer downstream (docs/01-seams.md §14).
    response->addHeader("Vary", "Cookie");
    callback(response);
}

// The described response, written through the binder — the same handler shape
// tests/session_listener_test.cc asserts byte for byte, so a live contract run
// and the suite are looking at one implementation.
void me(const HttpRequestPtr& req, Responder&& callback) {
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    if (ctx == nullptr) {
        callback(ac::not_found_response());
        return;
    }

    std::array<std::string_view, testapp::kPerms.size()> held{};
    std::size_t count = 0;
    testapp::kPerms.for_each_name(ctx->permissions,
                                  [&](std::string_view name) { held[count++] = name; });

    std::string body;
    body.reserve(256);
    const auto at_permissions = anvil::http::write_object<testapp::kMeResponse>(body)
                                    .uuid<"id">(ctx->user_id)
                                    .uuid<"session_id">(ctx->session_id)
                                    .text<"locale">(ctx->locale.tag());
    if (ac::is_superadmin(ctx->user_type)) {
        at_permissions.null_field<"permissions">().done();
    } else {
        at_permissions.strings<"permissions">(std::span{held}.first(count)).done();
    }
    callback(json(200, std::move(body)));
}

// --- the routes that exist so the table is complete -------------------------
//
// Every route in `testapp::kRoutes` is registered, because a client generated
// from the descriptor can spell every one of them and a call that 404s for want
// of a handler is a contract run failing on this server's gaps rather than on the
// library's. What is behind them is deliberately thin: this binary proves the
// TRANSPORT — the filter, the credential, the projection, the stealth 404 — and
// the subsystems behind these paths have suites of their own against a live
// cluster.

void empty_list(const HttpRequestPtr&, Responder&& callback) {
    callback(json(200, R"({"items":[],"next":null})"));
}

void not_found(const HttpRequestPtr&, Responder&& callback) {
    // The SHARED object, so a missing thing on a stealth route and a denial on it
    // are byte-identical without this handler having to know which it is.
    callback(ac::not_found_response());
}

void no_content(const HttpRequestPtr&, Responder&& callback) {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    response->setContentTypeCode(drogon::CT_NONE);
    callback(response);
}

// --- boot -------------------------------------------------------------------

void install_routes() {
    // Every account flow — salt, registration, verification, sign-in, reset,
    // change, refresh, sign-out — at the paths this application declared for
    // the route ids accounts.h gives each role.
    anvil::accounts::install_account_routes(accounts(), testapp::kRoutes,
                                            testapp::kRouteDescriptions);
    ac::register_route(testapp::kRoutes, "/session", drogon::Get, &session);
    ac::register_route(testapp::kRoutes, "/me", drogon::Get, &me);

    ac::register_route(testapp::kRoutes, "/content/{id}", drogon::Get, &not_found);
    ac::register_route(testapp::kRoutes, "/content/{id}", drogon::Delete, &no_content);
    ac::register_route(testapp::kRoutes, "/media/{ns}/{id}", drogon::Get, &empty_list);
    ac::register_route(testapp::kRoutes, "/media/{ns}/{id}", drogon::Delete, &no_content);
    ac::register_route(testapp::kRoutes, "/media/{ns}/{id}/{role}", drogon::Get, &not_found);
#if ANVIL_HAS_VIPS
    anvil::media::install_media_edit_routes(
        media_service(), limiter(), testapp::kRoutes, testapp::kRouteDescriptions,
        anvil::media::EditRoutes{.edit_route_id = "media.edit",
                                 .state_route_id = "media.edit_state",
                                 .budget = rule("media"),
                                 .on_edit = &print_edit});
#else
    ac::register_route(testapp::kRoutes, "/media-edits/{ns}/{id}", drogon::Post, &not_found);
    ac::register_route(testapp::kRoutes, "/media-edits/{ns}/{id}", drogon::Get, &not_found);
#endif
    ac::register_route(testapp::kRoutes, "/audit", drogon::Get, &empty_list);
    ac::register_route(testapp::kRoutes, "/preview/{id}", drogon::Get, &not_found);

    anvil::chat::install_chat_routes(
        chat_service(), limiter(), testapp::kRoutes, testapp::kRouteDescriptions,
        anvil::chat::ChatRoutes{
            .ids = {.create = "chat.create",
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
                    .reports = "chat.reports"},
            .send_budget = rule("chat-send"),
            .write_budget = rule("chat-write"),
            .claim_budget = rule("chat-claim"),
            .claim_target_budget = rule("chat-claim-target")});
    // In the same process here; a deployment serves it from MEDIA_ORIGIN, where
    // the session cookie never arrives and the grant is the whole authority.
    anvil::media::install_media_grant_route(media_service(), grant_keys(), testapp::kRoutes,
                                            testapp::kRouteDescriptions, "media.grant");
    anvil::chat::install_chat_socket(chat_live(), chat_service(), testapp::kRoutes,
                                     testapp::kRouteDescriptions, "chat.socket");
}

// Why this process is not going to serve, if it is not.
//
// Three outcomes and not two, because a harness has to tell them apart: "there
// is no database on this machine" is a SKIP in CI and "you pointed me at
// somebody's data" is a failure, and a single non-zero exit makes a green run
// out of the second one on a machine that happens to be missing the first.
enum class Boot : int {
    Ok = 0,
    // A configuration or safety refusal. The operator has to do something.
    Refused = 1,
    // A dependency this server needs is not reachable. Nobody has done anything
    // wrong; there is simply no MongoDB or no Redis here.
    Unavailable = 3,
};

// Everything that has to be true before a socket is accepted. Returns a verdict
// rather than throwing so the failure is a message and an exit code rather than
// a stack trace a harness has to parse.
[[nodiscard]] Boot boot(std::string& why_not) {
    anvil::Pools::init(anvil::PoolSizes{.db_threads = 4,
                                        .db_queue = 64,
                                        .cpu_threads = 2,
                                        .cpu_queue = 16,
                                        // The memory cap, not a tuning knob:
                                        // 64 MiB per Argon2 hash (CLAUDE.md §4).
                                        // No request path hashes here in
                                        // prehash mode: a login is one HMAC
                                        // (docs/05 §12).
                                        .hash_threads = 2,
                                        .hash_queue = 8,
                                        .audit_threads = 1,
                                        .audit_queue = 16,
                                        .analytics_threads = 1,
                                        .analytics_queue = 16});

    const std::string mongo_uri =
        env_or("ANVIL_REFERENCE_MONGODB_URI", "mongodb://127.0.0.1:27017");
    try {
        anvil::db::MongoPool::init(mongo_uri, 16);
        auto probe = anvil::db::MongoPool::instance().acquire();
        (*probe)["admin"].run_command(
            bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("ping", 1)));
    } catch (const std::exception& unreachable) {
        why_not = "no MongoDB at " + mongo_uri + ": " + unreachable.what();
        return Boot::Unavailable;
    }

    try {
        anvil::redis::RedisClient::init(anvil::redis::RedisConfig{
            .url = env_or("ANVIL_REFERENCE_REDIS_URL", "tcp://127.0.0.1:6379"),
            .pool_size = 4,
            .connect_timeout_ms = 1000,
            .socket_timeout_ms = 1000});
    } catch (const std::exception&) {
        // Swallowed on purpose: the health probe below is the authority, and it
        // gives one message for "the URL is wrong" and "the server is down".
    }
    if (!anvil::redis::RedisClient::healthy()) {
        why_not = "no Redis. The revocation channel is what makes a sign-out end an "
                  "outstanding access token rather than waiting it out, and a reference "
                  "server without one would demonstrate a session model this library does "
                  "not have.";
        return Boot::Unavailable;
    }

    auto client = anvil::db::MongoPool::instance().acquire();

    // RULE 3, before anything writes. A REFUSAL and never an "unavailable": the
    // database is right there and answering, and what is wrong is that it is
    // somebody else's.
    if (!claim_database(*client, why_not)) { return Boot::Refused; }

    // RULE 4: the reference application's own tables, in the deployment order
    // tests/app_fixture.h explains — options first, because a collection created
    // implicitly by createIndexes is not clustered and that door is one-way.
    (void)anvil::db::apply_collection_options(*client, database_names(),
                                              testapp::kCollectionOptions,
                                              anvil::db::OptionsPhase::Create);
    (void)anvil::db::apply_migrations(*client, database_names(), testapp::kIndexes,
                                      testapp::kSchemaVersion, testapp::kRetiredIndexes);
    (void)anvil::db::apply_collection_options(*client, database_names(),
                                              testapp::kCollectionOptions,
                                              anvil::db::OptionsPhase::Validate);

    const std::optional<Uuid> root =
        seed(*client, SeededAccount{.email = "root@reference.test",
                                    .password = secrets().superadmin_password,
                                    .permissions = PermSet{},
                                    .type = UserType::SuperAdmin});
    const std::optional<Uuid> editor =
        seed(*client, SeededAccount{.email = "editor@reference.test",
                                    .password = secrets().editor_password,
                                    .permissions = anvil::perm_mask(testapp::Perm::ContentRead),
                                    .type = UserType::Staff});
    // The editor's group with root in it. Made through the service with the
    // creating bit handed to the seed rather than to the account: the editor
    // then holds exactly the authority it is printed with, and a harness can
    // still assert what a member who could never have made a group may do in
    // one. Only on a first start, when both ids are this run's.
    if (root.has_value() && editor.has_value()) {
        const std::array<Uuid, 1> members{*root};
        const anvil::Result<anvil::chat::CreatedConversation> made = chat_service().create(
            *client,
            anvil::chat::Actor{*editor, anvil::perm_mask(testapp::Perm::ChatCreateGroup)},
            anvil::chat::CreateConversation{"group", "Reference", "", members, false});
        if (!made) {
            why_not = "seeding the chat group failed";
            return Boot::Refused;
        }
        seeded_group() = made.value().conversation.id;
        account_names()[*root] = "root";
        account_names()[*editor] = "editor";
        // Root's browser, as notifications registers one. Its keys are empty
        // because this server's transport prints rather than encrypts.
        anvil::notifications::ClientRow browser{};
        browser.id = anvil::uuid::generate_v4();
        browser.addr = "https://push.reference.test/root";
        browser.owner = *root;
        browser.created_at = anvil::db::now_ms();
        browser.prefs = anvil::notifications::Preferences::all_enabled();
        browser.type = anvil::notifications::ClientType::WebPush;
        if (!notification_repository().insert_client(*client, browser)) {
            why_not = "seeding root's push endpoint failed";
            return Boot::Refused;
        }
    }

    // The queue the push nudges run on. Its handler reaches the push through
    // the one pointer testapp/chat_push.h keeps.
    testapp::installed_chat_push().store(&chat_push(), std::memory_order_release);
    if (!job_queue().ensure_group()) {
        why_not = "the job queue could not be created";
        return Boot::Unavailable;
    }
    job_queue().start();

#if ANVIL_HAS_VIPS
    if (!open_storage(why_not)) { return Boot::Refused; }
    seed_media(*client);
#endif

    ac::AccessControl::init(ac::AccessControlDeps{
        .keys = token_keys(),
        .epochs = &authz(),
        .denials = nullptr,
        .routes = testapp::kRoutes,
    });
    ac::install_as_framework_404();
    anvil::http::install_request_scope();
    install_routes();
    return Boot::Ok;
}

// RULE 2, the printing half. One block, on stdout, after the URL.
void announce_credentials() {
    std::printf("\n");
    std::printf("accounts (drawn at boot, printed once, never a constant):\n");
    std::printf("  root@reference.test    %s   superadmin\n",
                secrets().superadmin_password.c_str());
    std::printf("  editor@reference.test  %s   staff, ContentRead\n",
                secrets().editor_password.c_str());
    std::printf("\n");
    // A source for the image edit routes, if this build can render one. The
    // same one-line shape as the codes below, so a harness reads it the same way.
    if (seeded_media().has_value()) {
        std::printf("media %s %s\n", std::string{testapp::kMedia.dir()}.c_str(),
                    anvil::uuid::to_string(*seeded_media()).c_str());
        std::printf("\n");
    }
    if (seeded_group().has_value()) {
        std::printf("chat %s\n", anvil::uuid::to_string(*seeded_group()).c_str());
        std::printf("\n");
    }
    std::printf("the signing key and the session pepper are also drawn at boot and are\n");
    std::printf("deliberately NOT printed: nothing outside this process needs them, and a\n");
    std::printf("key on a terminal is a key in a scrollback buffer.\n");
    std::printf("\n");
    std::printf("every verification and reset code this process sends is printed as one\n");
    std::printf("line, `code <verify|reset|exists> <address> <code>`, when it is sent.\n");
    std::fflush(stdout);
}

}  // namespace

int main() {
    // stdout belongs to the harness — RULE 1 says the first line of it is the
    // base URL — so every log line in this process goes to stderr instead.
    // Trantor writes to stdout by default, and one framework line before the
    // advice below would make "the first line" a lie.
    trantor::Logger::setOutputFunction(
        [](const char* message, const std::uint64_t length) {
            std::fwrite(message, 1, length, stderr);
        },
        [] { std::fflush(stderr); });

    std::string why_not;
    Boot verdict = Boot::Refused;
    try {
        verdict = boot(why_not);
    } catch (const std::exception& failure) {
        why_not = failure.what();
    }
    if (verdict != Boot::Ok) {
        std::fprintf(stderr, "anvil_reference_server: %s\n", why_not.c_str());
        return static_cast<int>(verdict);
    }

    // RULE 5. ONE directory at ONE path, and the reason it is here at all rather
    // than at the edge — which is where docs/00-architecture.md §1 puts bytes on
    // disk — is the access cookie's `SameSite=Lax`: a browser does not send it
    // cross-site, so a harness serving its bundle from another origin would be
    // exercising a cookie policy no deployment has.
    //
    // `allowAll` is false, so only files with a recognised extension are served:
    // a directory a harness drops a bundle into is a directory somebody
    // eventually drops something else into.
    const std::string static_dir = env_or("ANVIL_REFERENCE_STATIC", "");
    if (!static_dir.empty()) {
        drogon::app().addALocation("/app", "", static_dir, false, false, true);
    }

    drogon::app().registerBeginningAdvice([] {
        const std::uint16_t port = drogon::app().getListeners().at(0).toPort();
        // Its own address is the one origin a write may come from. Installed
        // before the address is announced, so no harness can send a write the
        // list does not yet cover.
        auto origins = std::make_shared<anvil::http::AllowedOrigins>();
        if (!origins->parse("http://127.0.0.1:" + std::to_string(port))) {
            std::fprintf(stderr, "anvil_reference_server: cannot parse its own origin\n");
            std::exit(1);
        }
        anvil::http::install_allowed_origins(std::move(origins));
        // THE FIRST LINE OF STDOUT, flushed, so a harness reads the port rather
        // than guessing it.
        std::printf("http://127.0.0.1:%u\n", static_cast<unsigned>(port));
        std::fflush(stdout);
        announce_credentials();
    });

    drogon::app()
        .setLogLevel(trantor::Logger::kWarn)
        .setThreadNum(2)
        .addListener("127.0.0.1", configured_port())
        .run();

    // Everything with a thread of its own that reaches the pools, before them.
    job_queue().stop();
    (void)job_queue().purge();
    chat_live().stop();
    anvil::Pools::shutdown();
#if ANVIL_HAS_VIPS
    // After the pools, so no render is still writing into the tree it removes.
    if (anvil::fs::Storage::initialised()) {
        anvil::fs::Storage::shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(storage_dir(), ignored);
    }
    anvil::images::shutdown();
#endif
    return 0;
}
