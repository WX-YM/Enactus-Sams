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
//      reference application being the proof (ENGINEERING_RULES.md §1).
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
#include <functional>
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
#include "anvil/auth/password.h"
#include "anvil/auth/token.h"
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
#include "anvil/identity/users.h"
#include "anvil/redis/redis_client.h"

#include "indexes.h"
#include "migrations.h"
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

[[nodiscard]] id::SessionService& sessions() {
    static id::SessionService service{primary_database(),  kSessionsCollection,
                                      kUsersCollection,    secrets().pepper,
                                      token_keys(),        authz()};
    return service;
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

// Hashed on THIS thread, at boot, which is the one place in the system where
// Argon2 on the calling thread is correct: there is no event loop yet, nothing is
// waiting, and `hash_pool` exists to bound concurrent hashes rather than to move
// them off a thread that has nothing else to do (ENGINEERING_RULES.md §4).
void seed(mongocxx::client& client, const SeededAccount& account) {
    const anvil::auth::PasswordHasher hasher{anvil::auth::kDefaultArgon2Params};
    const std::string hash = hasher.hash(account.password);

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
        return;
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
        return;
    }
    session.commit_transaction();
}

// --- responses --------------------------------------------------------------

[[nodiscard]] HttpResponsePtr json(int status, std::string body) {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    response->setContentTypeString(anvil::http::kJsonContentType);
    response->addHeader("Cache-Control", "private, no-store");
    response->setBody(std::move(body));
    return response;
}

// Through the one writer, so this server's failures are byte-identical to the
// filter's own and to what `anvil_emit_envelopes` records.
[[nodiscard]] HttpResponsePtr failure(const HttpRequestPtr& req, ErrorCode code) {
    std::string body;
    body.reserve(96);
    anvil::http::append_error_body(body, code, anvil::http::request_id_of(req));
    return json(anvil::http::http_status(code), std::move(body));
}

void set_session_cookies(const HttpResponsePtr& response, const id::IssuedSession& issued) {
    // Secure is set although this server is reached over http://127.0.0.1.
    // Browsers treat localhost as a secure context, so the cookie is still
    // stored — and dropping the attribute here would make this binary a worked
    // example of a cookie nobody should copy.
    drogon::Cookie access{std::string{ac::kAccessCookieName}, issued.access_token};
    access.setPath("/");
    access.setSecure(true);
    access.setHttpOnly(true);
    access.setSameSite(drogon::Cookie::convertString2SameSite(
        std::string{ac::kAccessCookieSameSite}));
    access.setMaxAge(static_cast<int>(issued.access_expires_in_seconds));
    response->addCookie(std::move(access));

    // Empty when the refresh token was not rotated, which is the common case
    // between rotations — and NOT an error. Overwriting the stored cookie with
    // an empty value would sign the caller out on the next refresh.
    if (!issued.refresh_token.empty()) {
        drogon::Cookie refresh{std::string{ac::kRefreshCookieName}, issued.refresh_token};
        refresh.setPath("/");
        refresh.setSecure(true);
        refresh.setHttpOnly(true);
        refresh.setSameSite(drogon::Cookie::convertString2SameSite(
            std::string{ac::kRefreshCookieSameSite}));
        refresh.setMaxAge(static_cast<int>(issued.refresh_expires_in_seconds));
        response->addCookie(std::move(refresh));
    }
}

// Every database touch goes through here. Nothing blocking runs on a Trantor
// event-loop thread (ENGINEERING_RULES.md §4), and a full queue sheds 503 rather than
// queueing — which is what the bound is for.
void on_db(const HttpRequestPtr& req, Responder callback,
           std::function<HttpResponsePtr(mongocxx::client&)> work) {
    const bool posted = anvil::Pools::db().try_post(anvil::guarded("db", [req, callback, work] {
        auto client = anvil::db::MongoPool::instance().acquire();
        callback(work(*client));
    }));
    if (!posted) { callback(failure(req, ErrorCode::ServiceUnavailable)); }
}

// --- handlers ---------------------------------------------------------------

void login(const HttpRequestPtr& req, Responder&& callback) {
    // Through anvil's own parser, not a scan of the body for two quoted keys.
    // A reference application that hand-rolled this would be demonstrating the
    // one thing `input/json.h` exists to make unnecessary: every limit enforced
    // DURING the parse, duplicate keys refused rather than resolved, and the
    // arena on the stack so a body costs no allocation.
    //
    // `as_string()` is the type assertion, and it is the half that matters:
    // `{"email":{"$gt":""}}` must not reach a query as an object, which is why
    // there is no accessor that coerces (ENGINEERING_RULES.md §5).
    input::BodyArena arena;
    const input::JsonDocument document = input::parse_json(req->body(), arena);
    if (!document.ok() || !document.root().is_object()) {
        callback(failure(req, ErrorCode::ValidationFailed));
        return;
    }

    const auto field = [&document](std::string_view key) -> std::string {
        const input::JsonValue* value = document.root().find(key);
        if (value == nullptr) { return {}; }
        const std::optional<std::string_view> text = value->as_string();
        return text.has_value() ? std::string{*text} : std::string{};
    };

    const std::string email = field("email");
    const std::string password = field("password");
    if (email.empty() || password.empty()) {
        // One code for "no such key", "not a string" and "empty": a login form
        // that learns WHICH is a login form that can be probed.
        callback(failure(req, ErrorCode::ValidationFailed));
        return;
    }

    const anvil::http::PackedAddress ip = anvil::http::client_address(req);
    const std::string agent = req->getHeader("user-agent");

    on_db(req, std::move(callback), [email, password, ip, agent](mongocxx::client& client) {
        const auto found =
            users().find_for_login(client, email, id::LoginIdentity::Email);
        // One answer for "no such account" and for "wrong password". Telling the
        // two apart is account enumeration in one response.
        const anvil::auth::PasswordHasher hasher{anvil::auth::kDefaultArgon2Params};
        const std::string stored =
            (found.ok() && found.value().has_value()) ? found.value()->password_hash : std::string{};
        if (hasher.verify(stored, password) != anvil::auth::VerifyOutcome::Match) {
            std::string body;
            anvil::http::append_error_body(body, ErrorCode::Unauthenticated,
                                           anvil::http::RequestId{});
            return json(401, std::move(body));
        }

        const auto issued = sessions().create(
            client, *found.value(), ip,
            agent, anvil::db::now_ms());
        if (!issued.ok()) {
            std::string body;
            anvil::http::append_error_body(body, issued.error().code, anvil::http::RequestId{});
            return json(anvil::http::http_status(issued.error().code), std::move(body));
        }

        const HttpResponsePtr response = json(200, R"({"signed_in":true})");
        set_session_cookies(response, issued.value());
        return response;
    });
}

void refresh(const HttpRequestPtr& req, Responder&& callback) {
    const std::string token = req->getCookie(std::string{ac::kRefreshCookieName});
    if (token.empty()) {
        callback(failure(req, ErrorCode::Unauthenticated));
        return;
    }

    on_db(req, std::move(callback), [token](mongocxx::client& client) {
        const auto issued =
            sessions().refresh(client, token, anvil::db::now_ms());
        if (!issued.ok()) {
            std::string body;
            anvil::http::append_error_body(body, issued.error().code, anvil::http::RequestId{});
            return json(anvil::http::http_status(issued.error().code), std::move(body));
        }
        const HttpResponsePtr response = json(200, R"({"refreshed":true})");
        set_session_cookies(response, issued.value());
        return response;
    });
}

void logout(const HttpRequestPtr& req, Responder&& callback) {
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    if (ctx == nullptr) {
        callback(ac::not_found_response());
        return;
    }
    const Uuid session_id = ctx->session_id;
    const Uuid user_id = ctx->user_id;

    on_db(req, std::move(callback), [session_id, user_id](mongocxx::client& client) {
        // The result is deliberately not reported: revoking a session that is
        // already gone is the ordinary shape of a retried logout, and
        // `auth.logout` is described as idempotent precisely so a client may
        // repeat it after a response nobody saw.
        (void)sessions().revoke(client, session_id, user_id);

        const HttpResponsePtr response = json(200, R"({"signed_out":true})");
        for (const std::string_view name : {ac::kAccessCookieName, ac::kRefreshCookieName}) {
            drogon::Cookie cleared{std::string{name}, ""};
            cleared.setPath("/");
            cleared.setSecure(true);
            cleared.setHttpOnly(true);
            cleared.setMaxAge(0);
            response->addCookie(std::move(cleared));
        }
        return response;
    });
}

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
    ac::register_route(testapp::kRoutes, "/login", drogon::Post, &login);
    ac::register_route(testapp::kRoutes, "/auth/refresh", drogon::Post, &refresh);
    ac::register_route(testapp::kRoutes, "/session", drogon::Get, &session);
    ac::register_route(testapp::kRoutes, "/session/logout", drogon::Post, &logout);
    ac::register_route(testapp::kRoutes, "/me", drogon::Get, &me);

    ac::register_route(testapp::kRoutes, "/content/{id}", drogon::Get, &not_found);
    ac::register_route(testapp::kRoutes, "/content/{id}", drogon::Delete, &no_content);
    ac::register_route(testapp::kRoutes, "/media/{ns}/{id}", drogon::Get, &empty_list);
    ac::register_route(testapp::kRoutes, "/media/{ns}/{id}", drogon::Delete, &no_content);
    ac::register_route(testapp::kRoutes, "/media/{ns}/{id}/{role}", drogon::Get, &not_found);
    ac::register_route(testapp::kRoutes, "/audit", drogon::Get, &empty_list);
    ac::register_route(testapp::kRoutes, "/preview/{id}", drogon::Get, &not_found);
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
                                        // 64 MiB per Argon2 hash (ENGINEERING_RULES.md §4).
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

    seed(*client, SeededAccount{.email = "root@reference.test",
                                .password = secrets().superadmin_password,
                                .permissions = PermSet{},
                                .type = UserType::SuperAdmin});
    seed(*client, SeededAccount{.email = "editor@reference.test",
                                .password = secrets().editor_password,
                                .permissions = anvil::perm_mask(testapp::Perm::ContentRead),
                                .type = UserType::Staff});

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
    std::printf("the signing key and the session pepper are also drawn at boot and are\n");
    std::printf("deliberately NOT printed: nothing outside this process needs them, and a\n");
    std::printf("key on a terminal is a key in a scrollback buffer.\n");
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

    anvil::Pools::shutdown();
    return 0;
}
