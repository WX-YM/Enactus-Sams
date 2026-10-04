#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <json/json.h>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>

// stb image — declaration only; definitions live in stb_impl.cc
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "third_party/stb_image_resize2.h"

#include "anvil/accesscontrol/route_registration.h"
#include "anvil/core/thread_pools.h"
#include "anvil/db/mongo_pool.h"
#include "routes.h"
#include "anvil/auth/password.h"
#include "security/input.h"
#include "security/jwt.h"
#include "security/rate_limiter.h"

using namespace drogon;
namespace sec = enactus::security;

namespace enactus {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

inline std::string trimString(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
static constexpr std::size_t kMinPasswordLen = 8;
static constexpr std::size_t kMaxPasswordLen = 256;
static constexpr std::size_t kMaxNameLen = 200;
static constexpr std::size_t kMaxTeamLen = 100;
static constexpr std::size_t kMaxPhoneLen = 40;
static constexpr std::size_t kMaxReasonLen = 5000;
static constexpr std::size_t kMaxDescLen = 2000;
static constexpr std::size_t kMaxRosterSize = 500;
static constexpr std::size_t kMaxContentBytes = 2 * 1024 * 1024;
static constexpr std::size_t kMaxFormSchemaBytes = 256 * 1024;
static constexpr std::size_t kMaxSubmissionBytes = 64 * 1024;
static constexpr std::size_t kMaxSubmissionFields = 100;
static constexpr std::size_t kMaxUploadBytes = 15 * 1024 * 1024;
static constexpr int kMaxImageSide = 16384;
static constexpr int64_t kMaxImagePixels = 50'000'000;
static constexpr int64_t kSessionLifetimeSec = 43200; // 12 hours

static const std::string kSuperAdminEmail = "admin@enactussams.org";

static constexpr std::array<std::string_view, 7> kAllPermissions{{
    "dashboard", "applications", "form_maker", "teams", "content", "gallery", "users",
}};
static constexpr std::array<std::string_view, 9> kAssignableRoles{{
    "superadmin", "high board", "director", "manager", "vice manager", "vice_manager", "HR", "hr", "member",
}};
static constexpr std::array<std::string_view, 5> kApplicationStatuses{{
    "pending", "accepted", "rejected", "referred", "interview_scheduled",
}};
// Content keys a gallery-only user may write; everything else in the CMS
// document needs the `content` permission.
static const std::unordered_set<std::string> kGalleryContentKeys{
    "mediaGallery", "aboutImages", "tafrahImages", "tafrahSiteImage",
};

template <std::size_t N>
static bool contains(const std::array<std::string_view, N>& arr, std::string_view v) {
    return std::find(arr.begin(), arr.end(), v) != arr.end();
}

static enactus::security::SlidingWindowRateLimiter gLoginLimiter(10, std::chrono::seconds(60));
static enactus::security::SlidingWindowRateLimiter gLoginEmailLimiter(5, std::chrono::seconds(300));
static enactus::security::SlidingWindowRateLimiter gSubmitLimiter(15, std::chrono::seconds(60));
static enactus::security::SlidingWindowRateLimiter gVisitLimiter(30, std::chrono::seconds(60));
static const anvil::auth::PasswordHasher gPasswordHasher(anvil::auth::kDefaultArgon2Params);

static int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Small accessors that never throw on a missing or mistyped field
// ---------------------------------------------------------------------------
static std::string getStr(const bsoncxx::document::view& v, const std::string& key) {
    auto e = v[key];
    if (!e || e.type() != bsoncxx::type::k_string) return "";
    return std::string(e.get_string().value);
}

static int64_t getInt(const bsoncxx::document::view& v, const std::string& key) {
    auto e = v[key];
    if (!e) return 0;
    switch (e.type()) {
        case bsoncxx::type::k_int64: return e.get_int64().value;
        case bsoncxx::type::k_int32: return e.get_int32().value;
        case bsoncxx::type::k_double: return static_cast<int64_t>(e.get_double().value);
        default: return 0;
    }
}

static std::string jstr(const Json::Value& obj, const char* key) {
    if (!obj.isObject()) return "";
    const Json::Value& v = obj[key];
    return v.isString() ? v.asString() : "";
}

static Json::Value docToJson(const bsoncxx::document::view& doc) {
    std::string jsonStr = bsoncxx::to_json(doc);
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value out;
    std::string errs;
    if (!reader->parse(jsonStr.data(), jsonStr.data() + jsonStr.size(), &out, &errs)) {
        return Json::Value(Json::objectValue);
    }
    return out;
}

static bsoncxx::types::b_regex exactMatch(const std::string& s) {
    return bsoncxx::types::b_regex{sec::exactMatchPattern(s), "i"};
}

static HttpResponsePtr jsonResponse(HttpStatusCode code, const Json::Value& body) {
    auto resp = HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(code);
    return resp;
}

static HttpResponsePtr jsonError(HttpStatusCode code, const std::string& message) {
    Json::Value err;
    err["status"] = "error";
    err["message"] = message;
    return jsonResponse(code, err);
}

static HttpResponsePtr jsonOk() {
    Json::Value ret;
    ret["status"] = "ok";
    return HttpResponse::newHttpJsonResponse(ret);
}

// Parsed JSON body, or nullptr (after answering 400) when the body is absent
// or not a JSON object.
static std::shared_ptr<Json::Value> requireJsonObject(const HttpRequestPtr& req,
                                                      std::function<void(const HttpResponsePtr&)>& callback) {
    auto json = req->getJsonObject();
    if (!json || !json->isObject()) {
        callback(jsonError(k400BadRequest, "Request body must be a JSON object"));
        return nullptr;
    }
    return json;
}

// ---------------------------------------------------------------------------
// Authentication and authorization
// ---------------------------------------------------------------------------
static bool isSuperAdmin(const enactus::security::JwtClaims& c) {
    return c.role == "superadmin" || sec::equalsIgnoreCase(trimString(c.email), kSuperAdminEmail);
}

static bool hasPerm(const enactus::security::JwtClaims& c, std::string_view perm) {
    if (isSuperAdmin(c)) return true;
    return std::find(c.permissions.begin(), c.permissions.end(), perm) != c.permissions.end();
}

static bool isManagerRole(std::string_view role) {
    return role == "manager" || role == "vice manager" || role == "vice_manager";
}

// Managers and vice managers only see and act on their own team.
static bool isTeamScoped(const enactus::security::JwtClaims& c) {
    return !isSuperAdmin(c) && isManagerRole(c.role) && !trimString(c.team).empty();
}

static bool sameTeam(const std::string& a, const std::string& b) {
    const std::string ta = trimString(a);
    return !ta.empty() && sec::equalsIgnoreCase(ta, trimString(b));
}

std::optional<enactus::security::JwtClaims> extractAdminClaims(const HttpRequestPtr& req) {
    std::string authHeader = req->getHeader("Authorization");
    std::string token;
    if (authHeader.starts_with("Bearer ") || authHeader.starts_with("bearer ")) {
        token = authHeader.substr(7);
    } else {
        token = req->getHeader("X-Admin-Token");
    }
    if (token.empty()) return std::nullopt;
    return enactus::security::verifyToken(token);
}

struct AuthResult {
    std::optional<enactus::security::JwtClaims> claims;
    HttpStatusCode code = k401Unauthorized;
    std::string message;
};

// Verify the bearer token, then re-load the account so role, team and
// permissions always come from the database rather than from the token, and
// revoked accounts or sessions are refused immediately.
static AuthResult authenticate(const HttpRequestPtr& req) {
    AuthResult result;
    auto claims = extractAdminClaims(req);
    if (!claims.has_value()) {
        result.message = "Unauthorized: invalid or expired session token";
        return result;
    }

    try {
        auto client = anvil::db::MongoPool::instance().acquire();
        auto usersColl = (*client)["application"]["users"];

        std::string userEmail = trimString(claims->email);
        auto userDoc = usersColl.find_one(make_document(kvp("email", exactMatch(userEmail))));
        if (!userDoc) {
            result.message = "Unauthorized: user account has been revoked or no longer exists";
            return result;
        }

        auto view = userDoc->view();
        const std::string st = getStr(view, "status");
        if (st == "revoked" || st == "suspended" || st == "disabled" || st == "inactive") {
            result.message = "Unauthorized: user account is inactive or revoked";
            return result;
        }

        // Set on logout and on password change; every token issued at or
        // before that instant is dead.
        const int64_t validAfter = getInt(view, "sessions_valid_after");
        if (validAfter > 0 && claims->iat <= validAfter) {
            result.message = "Unauthorized: session has been signed out";
            return result;
        }

        claims->email = getStr(view, "email");
        claims->role = getStr(view, "role");
        claims->team = getStr(view, "team");
        claims->permissions.clear();
        if (view["permissions"] && view["permissions"].type() == bsoncxx::type::k_array) {
            for (auto&& p : view["permissions"].get_array().value) {
                if (p.type() == bsoncxx::type::k_string) {
                    claims->permissions.push_back(std::string(p.get_string().value));
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[Security] user verification error: " << e.what();
        result.code = k503ServiceUnavailable;
        result.message = "Internal authorization service error";
        return result;
    }

    result.claims = std::move(claims);
    return result;
}

// Answers 401/403 and returns false unless the caller is signed in and, when
// `anyOf` is non-empty, holds at least one of the listed permissions.
bool requireAdminAuth(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>& callback,
                      std::optional<enactus::security::JwtClaims>& outClaims,
                      std::initializer_list<std::string_view> anyOf = {}) {
    AuthResult auth = authenticate(req);
    if (!auth.claims.has_value()) {
        callback(jsonError(auth.code, auth.message));
        return false;
    }
    if (anyOf.size() > 0) {
        const bool allowed = std::any_of(anyOf.begin(), anyOf.end(),
                                         [&](std::string_view p) { return hasPerm(*auth.claims, p); });
        if (!allowed) {
            callback(jsonError(k403Forbidden, "Forbidden: you do not have permission to perform this action"));
            return false;
        }
    }
    outClaims = std::move(auth.claims);
    return true;
}

void on_db(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)> callback,
           std::function<HttpResponsePtr(mongocxx::client&)> work) {
    const bool posted = anvil::Pools::db().try_post(anvil::guarded("db", [req, callback, work] {
        HttpResponsePtr resp;
        try {
            auto client = anvil::db::MongoPool::instance().acquire();
            resp = work(*client);
        } catch (const std::exception& e) {
            LOG_ERROR << "[API] " << req->path() << " failed: " << e.what();
            resp = jsonError(k500InternalServerError, "Internal server error");
        } catch (...) {
            LOG_ERROR << "[API] " << req->path() << " failed with an unknown exception";
            resp = jsonError(k500InternalServerError, "Internal server error");
        }
        callback(resp);
    }));
    if (!posted) {
        callback(jsonError(k503ServiceUnavailable, "Service Unavailable"));
    }
}


static void logSystemEvent(mongocxx::client& client, const std::string& action, const std::string& details, const std::string& type = "system") {
    try {
        auto logsColl = client["application"]["logs"];
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        logsColl.insert_one(make_document(
            kvp("timestamp", now_ms),
            kvp("action", action),
            kvp("details", details),
            kvp("type", type)
        ));
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// Auth endpoints
// ---------------------------------------------------------------------------
void login(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    if (!gLoginLimiter.isAllowed(sec::clientIp(req))) {
        callback(jsonError(k429TooManyRequests, "Too many login attempts. Please wait a minute and try again."));
        return;
    }

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    std::string email = sec::toLower(trimString(jstr(*json, "email")));
    std::string password = jstr(*json, "password");

    if (!sec::isValidEmail(email) || password.empty() || password.size() > kMaxPasswordLen) {
        callback(jsonError(k401Unauthorized, "Invalid email or password"));
        return;
    }
    if (!gLoginEmailLimiter.isAllowed(email)) {
        callback(jsonError(k429TooManyRequests, "Too many login attempts. Please wait a few minutes and try again."));
        return;
    }

    on_db(req, std::move(callback), [email, password](mongocxx::client& client) {
        auto collection = client["application"]["users"];
        auto doc = collection.find_one(make_document(kvp("email", exactMatch(email))));

        bool password_matches = false;
        if (doc) {
            const std::string stored_pass = getStr(doc->view(), "password");
            const std::string st = getStr(doc->view(), "status");
            const bool inactive = st == "revoked" || st == "suspended" || st == "disabled" || st == "inactive";
            // Only Argon2id hashes are accepted. Any legacy plaintext value is
            // converted at boot by migratePasswordsToArgon2().
            if (stored_pass.starts_with("$argon2id$")) {
                password_matches = gPasswordHasher.verify(stored_pass, password) == anvil::auth::VerifyOutcome::Match;
            } else {
                gPasswordHasher.consume_dummy_time();
            }
            if (inactive) password_matches = false;
        } else {
            gPasswordHasher.consume_dummy_time();
        }

        if (!password_matches) {
            logSystemEvent(client, "Login Failed", "Failed login attempt for " + email, "auth");
            return jsonError(k401Unauthorized, "Invalid email or password");
        }

        auto view = doc->view();
        const std::string canonicalEmail = getStr(view, "email");
        const std::string role = getStr(view, "role");
        const std::string team = getStr(view, "team");

        logSystemEvent(client, "User Login", "User " + canonicalEmail + " (" + role + ") logged into admin panel", "auth");

        const int64_t now_sec = nowSeconds();
        const int64_t exp_sec = now_sec + kSessionLifetimeSec;

        Json::Value ret;
        ret["status"] = "ok";
        ret["expires_in"] = static_cast<Json::Value::Int64>(kSessionLifetimeSec);
        ret["expires_at"] = static_cast<Json::Value::Int64>(exp_sec);
        ret["role"] = role;
        ret["team"] = team;

        enactus::security::JwtClaims claims;
        claims.email = canonicalEmail;
        claims.role = role;
        claims.team = team;
        claims.iat = now_sec;
        claims.exp = exp_sec;

        Json::Value permsArr(Json::arrayValue);
        if (view["permissions"] && view["permissions"].type() == bsoncxx::type::k_array) {
            for (auto&& p : view["permissions"].get_array().value) {
                if (p.type() != bsoncxx::type::k_string) continue;
                std::string permStr = std::string(p.get_string().value);
                permsArr.append(permStr);
                claims.permissions.push_back(permStr);
            }
        }
        ret["permissions"] = permsArr;
        ret["token"] = enactus::security::signToken(claims);
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void logout(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims)) return;

    const std::string email = claims->email;
    on_db(req, std::move(callback), [email](mongocxx::client& client) {
        auto usersColl = client["application"]["users"];
        usersColl.update_one(
            make_document(kvp("email", exactMatch(email))),
            make_document(kvp("$set", make_document(kvp("sessions_valid_after", nowSeconds()))))
        );
        logSystemEvent(client, "User Logout", "User " + email + " signed out", "auth");
        return jsonOk();
    });
}

void me(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims)) return;

    Json::Value ret;
    ret["status"] = "ok";
    ret["email"] = claims->email;
    ret["role"] = claims->role;
    ret["team"] = claims->team;
    Json::Value perms(Json::arrayValue);
    for (const auto& p : claims->permissions) perms.append(p);
    ret["permissions"] = perms;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------
void listUsers(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"users"})) return;

    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["users"];
        auto cursor = collection.find({});

        Json::Value users(Json::arrayValue);
        for (auto&& doc : cursor) {
            Json::Value jsonDoc = docToJson(doc);
            jsonDoc.removeMember("password");
            jsonDoc.removeMember("sessions_valid_after");
            users.append(jsonDoc);
        }

        Json::Value ret;
        ret["users"] = users;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void createUser(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"users"})) return;

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    const std::string email    = sec::toLower(trimString(jstr(*json, "email")));
    const std::string password = jstr(*json, "password");
    const std::string role     = trimString(jstr(*json, "role"));
    const std::string team     = trimString(jstr(*json, "team"));
    const std::string action   = jstr(*json, "action");
    const bool isUpdate = action == "update";

    if (!sec::isValidEmail(email)) {
        callback(jsonError(k400BadRequest, "A valid email address is required"));
        return;
    }
    if (!contains(kAssignableRoles, role)) {
        callback(jsonError(k400BadRequest, "Unknown role"));
        return;
    }
    if (!sec::isValidText(team, kMaxTeamLen, true)) {
        callback(jsonError(k400BadRequest, "Invalid team name"));
        return;
    }
    if (!isUpdate || !password.empty()) {
        if (password.size() < kMinPasswordLen || password.size() > kMaxPasswordLen) {
            callback(jsonError(k400BadRequest, "Password must be between 8 and 256 characters"));
            return;
        }
    }

    std::vector<std::string> perms;
    const Json::Value& permsJson = (*json)["permissions"];
    if (!permsJson.isNull() && !permsJson.isArray()) {
        callback(jsonError(k400BadRequest, "permissions must be an array"));
        return;
    }
    for (const auto& p : permsJson) {
        if (!p.isString() || !contains(kAllPermissions, p.asString())) {
            callback(jsonError(k400BadRequest, "Unknown permission"));
            return;
        }
        if (std::find(perms.begin(), perms.end(), p.asString()) == perms.end()) {
            perms.push_back(p.asString());
        }
    }

    // Privilege-escalation guards: only a super admin can mint super admins,
    // and nobody can hand out a permission they do not hold themselves.
    const bool actorSuper = isSuperAdmin(*claims);
    if (!actorSuper) {
        if (role == "superadmin") {
            callback(jsonError(k403Forbidden, "Only a super admin can assign the superadmin role"));
            return;
        }
        for (const auto& p : perms) {
            if (!hasPerm(*claims, p)) {
                callback(jsonError(k403Forbidden, "You cannot grant a permission you do not hold"));
                return;
            }
        }
    }
    if (sec::equalsIgnoreCase(email, kSuperAdminEmail) && (isUpdate || !actorSuper)) {
        callback(jsonError(k403Forbidden, "Super admin account cannot be modified"));
        return;
    }

    const std::string actorEmail = claims->email;
    on_db(req, std::move(callback), [email, password, role, team, isUpdate, perms, actorSuper, actorEmail](mongocxx::client& client) {
        auto collection = client["application"]["users"];
        auto teamsColl = client["application"]["teams"];

        bsoncxx::builder::basic::array permsArr{};
        for (const auto& p : perms) permsArr.append(p);

        auto existingDoc = collection.find_one(make_document(kvp("email", exactMatch(email))));

        if (isUpdate) {
            if (!existingDoc) {
                return jsonError(k404NotFound, "User not found");
            }
            auto view = existingDoc->view();
            const std::string targetEmail = getStr(view, "email");
            const std::string oldTeam = getStr(view, "team");
            const std::string oldRole = getStr(view, "role");

            if (!actorSuper && (oldRole == "superadmin" || sec::equalsIgnoreCase(trimString(targetEmail), kSuperAdminEmail))) {
                return jsonError(k403Forbidden, "Only a super admin can modify a super admin account");
            }

            if (!oldTeam.empty() && (oldTeam != team || oldRole != role)) {
                std::string oldField = (oldRole == "vice manager" || oldRole == "vice_manager") ? "viceManager" : "manager";
                teamsColl.update_one(
                    make_document(kvp("name", oldTeam)),
                    make_document(kvp("$unset", make_document(kvp(oldField, ""))))
                );
            }

            bsoncxx::builder::basic::document updateFields{};
            updateFields.append(kvp("role", role));
            updateFields.append(kvp("team", team));
            updateFields.append(kvp("permissions", permsArr.extract()));
            if (!password.empty()) {
                updateFields.append(kvp("password", gPasswordHasher.hash(password)));
                // A password reset ends every existing session for the account.
                updateFields.append(kvp("sessions_valid_after", nowSeconds()));
            }

            collection.update_one(
                make_document(kvp("_id", view["_id"].get_oid().value)),
                make_document(kvp("$set", updateFields.extract()))
            );

            if (!team.empty() && isManagerRole(role)) {
                std::string fieldName = (role == "vice manager" || role == "vice_manager") ? "viceManager" : "manager";
                teamsColl.update_one(
                    make_document(kvp("name", team)),
                    make_document(kvp("$set", make_document(kvp(fieldName, targetEmail))))
                );
            }

            logSystemEvent(client, "User Updated", "User " + targetEmail + " updated by " + actorEmail + " (role: " + role + (team.empty() ? "" : ", team: " + team) + ")", "auth");
            return jsonOk();
        }

        if (existingDoc) {
            return jsonError(k409Conflict, "A user with this email already exists");
        }

        bsoncxx::builder::basic::document doc{};
        doc.append(kvp("email", email));
        doc.append(kvp("password", gPasswordHasher.hash(password)));
        doc.append(kvp("role", role));
        if (!team.empty()) {
            doc.append(kvp("team", team));
        }
        doc.append(kvp("permissions", permsArr.extract()));
        collection.insert_one(doc.view());

        if (!team.empty() && isManagerRole(role)) {
            std::string fieldName = (role == "vice manager" || role == "vice_manager") ? "viceManager" : "manager";
            teamsColl.update_one(
                make_document(kvp("name", team)),
                make_document(kvp("$set", make_document(kvp(fieldName, email))))
            );
        }

        logSystemEvent(client, "User Created", "New user " + email + " created by " + actorEmail + " with role " + role, "auth");
        return jsonOk();
    });
}

void deleteUser(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"users"})) return;

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    const std::string email = sec::toLower(trimString(jstr(*json, "email")));
    if (!sec::isValidEmail(email)) {
        callback(jsonError(k400BadRequest, "A valid email address is required"));
        return;
    }
    if (sec::equalsIgnoreCase(email, kSuperAdminEmail)) {
        callback(jsonError(k403Forbidden, "Super admin account cannot be deleted"));
        return;
    }

    const bool actorSuper = isSuperAdmin(*claims);
    const std::string actorEmail = claims->email;
    on_db(req, std::move(callback), [email, actorSuper, actorEmail](mongocxx::client& client) {
        auto collection = client["application"]["users"];
        auto userDoc = collection.find_one(make_document(kvp("email", exactMatch(email))));
        if (!userDoc) {
            return jsonError(k404NotFound, "User not found");
        }

        auto view = userDoc->view();
        const std::string targetEmail = getStr(view, "email");
        const std::string role = getStr(view, "role");
        if (!actorSuper && role == "superadmin") {
            return jsonError(k403Forbidden, "Only a super admin can remove a super admin account");
        }

        const std::string teamName = getStr(view, "team");
        if (!teamName.empty()) {
            auto teamsColl = client["application"]["teams"];
            if (role == "manager") {
                teamsColl.update_one(
                    make_document(kvp("name", teamName)),
                    make_document(kvp("$unset", make_document(kvp("manager", ""))))
                );
            } else if (role == "vice manager" || role == "vice_manager") {
                teamsColl.update_one(
                    make_document(kvp("name", teamName)),
                    make_document(kvp("$unset", make_document(kvp("viceManager", ""))))
                );
            }
        }

        collection.delete_one(make_document(kvp("_id", view["_id"].get_oid().value)));
        logSystemEvent(client, "User Revoked", "User " + targetEmail + " access revoked by " + actorEmail, "auth");
        return jsonOk();
    });
}

// ---------------------------------------------------------------------------
// Applications
// ---------------------------------------------------------------------------
void apply(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    if (!gSubmitLimiter.isAllowed(sec::clientIp(req))) {
        callback(jsonError(k429TooManyRequests, "Too many application submissions. Please try again later."));
        return;
    }

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    std::string name = trimString(jstr(*json, "name"));
    std::string team = trimString(jstr(*json, "team"));
    std::string reason = trimString(jstr(*json, "reason"));
    std::string email = sec::toLower(trimString(jstr(*json, "email")));
    std::string phone = trimString(jstr(*json, "phone"));

    if (!sec::isValidText(name, kMaxNameLen) || !sec::isValidText(team, kMaxTeamLen) ||
        !sec::isValidMultiline(reason, kMaxReasonLen) || !sec::isValidEmail(email) ||
        !sec::isValidText(phone, kMaxPhoneLen, true)) {
        callback(jsonError(k400BadRequest, "Please provide a valid name, email, team and reason"));
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    gmtime_r(&in_time_t, &tm_buf);
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    std::string timestamp = ss.str();

    on_db(req, std::move(callback), [name, team, reason, email, phone, timestamp](mongocxx::client& client) {
        auto collection = client["application"]["applications"];

        // One application per email. An existing record is left untouched:
        // this endpoint is unauthenticated, so letting a resubmission
        // overwrite it would let anyone who knows an applicant's email
        // rewrite their application. The response is identical either way
        // so it cannot be used to probe who has applied.
        auto existing = collection.find_one(make_document(kvp("email", exactMatch(email))));
        if (existing) {
            logSystemEvent(client, "Duplicate Application", "Duplicate application ignored for " + team, "application");
            return jsonOk();
        }

        bsoncxx::builder::basic::document doc{};
        doc.append(kvp("name", name));
        doc.append(kvp("email", email));
        doc.append(kvp("phone", phone));
        doc.append(kvp("team", team));
        doc.append(kvp("reason", reason));
        doc.append(kvp("status", "pending"));
        doc.append(kvp("submittedAt", timestamp));

        collection.insert_one(doc.view());
        logSystemEvent(client, "New Application", "New applicant " + name + " applied for " + team, "application");
        return jsonOk();
    });
}

void listApplications(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"applications", "dashboard"})) return;

    // Dashboard-only users get pipeline statistics, not applicant PII.
    const bool fullAccess = hasPerm(*claims, "applications");
    const bool scoped = isTeamScoped(*claims);
    const std::string userTeam = claims->team;

    on_db(req, std::move(callback), [fullAccess, scoped, userTeam](mongocxx::client& client) {
        auto collection = client["application"]["applications"];
        auto cursor = collection.find({});

        Json::Value arr = Json::arrayValue;
        for (auto&& doc : cursor) {
            Json::Value item = docToJson(doc);
            if (scoped) {
                const std::string t = item["team"].isString() ? item["team"].asString() : "";
                const std::string r = item["referredTo"].isString() ? item["referredTo"].asString() : "";
                if (!sameTeam(t, userTeam) && !sameTeam(r, userTeam)) continue;
            }
            if (!fullAccess) {
                Json::Value slim(Json::objectValue);
                for (const char* key : {"_id", "team", "referredTo", "status", "submittedAt"}) {
                    if (item.isMember(key)) slim[key] = item[key];
                }
                item = slim;
            }
            arr.append(item);
        }

        Json::Value ret;
        ret["applications"] = arr;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void updateApplicationStatus(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"applications"})) return;

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    const std::string id_str = jstr(*json, "id");
    const std::string action = jstr(*json, "action");
    const std::string status = trimString(jstr(*json, "status"));
    const std::string reason = trimString(jstr(*json, "reason"));
    const std::string referred_to = trimString(jstr(*json, "referredTo"));
    const std::string team_override = trimString(jstr(*json, "team"));

    if (!sec::isHexObjectId(id_str)) {
        callback(jsonError(k400BadRequest, "Invalid application id"));
        return;
    }
    if (action == "delete" && !isSuperAdmin(*claims)) {
        callback(jsonError(k403Forbidden, "Only a super admin can delete applications"));
        return;
    }
    if (action != "delete") {
        if (!status.empty() && !contains(kApplicationStatuses, status)) {
            callback(jsonError(k400BadRequest, "Unknown application status"));
            return;
        }
        if (!sec::isValidMultiline(reason, kMaxReasonLen) ||
            !sec::isValidText(referred_to, kMaxTeamLen, true) ||
            !sec::isValidText(team_override, kMaxTeamLen, true)) {
            callback(jsonError(k400BadRequest, "Invalid field value"));
            return;
        }
    }

    const bool scoped = isTeamScoped(*claims);
    const std::string userTeam = claims->team;
    const std::string actorEmail = claims->email;

    on_db(req, std::move(callback), [id_str, action, status, reason, referred_to, team_override, scoped, userTeam, actorEmail](mongocxx::client& client) {
        bsoncxx::oid oid(id_str);
        auto collection = client["application"]["applications"];

        auto current = collection.find_one(make_document(kvp("_id", oid)));
        if (!current) {
            return jsonError(k404NotFound, "Application not found");
        }

        if (action == "delete") {
            collection.delete_one(make_document(kvp("_id", oid)));
            logSystemEvent(client, "Application Deleted", "Application " + id_str + " deleted by " + actorEmail, "application");
            return jsonOk();
        }

        // Team managers may act only on applications for, or referred to,
        // their own team, and may only pull a candidate into that team.
        if (scoped) {
            const std::string appTeam = getStr(current->view(), "team");
            const std::string appReferred = getStr(current->view(), "referredTo");
            if (!sameTeam(appTeam, userTeam) && !sameTeam(appReferred, userTeam)) {
                return jsonError(k403Forbidden, "This application belongs to another team");
            }
            if (!team_override.empty() && !sameTeam(team_override, userTeam)) {
                return jsonError(k403Forbidden, "You can only move candidates into your own team");
            }
        }

        bsoncxx::builder::basic::document update_fields{};
        if (!status.empty()) update_fields.append(kvp("status", status));
        if (!reason.empty()) update_fields.append(kvp("reason", reason));
        if (!referred_to.empty()) update_fields.append(kvp("referredTo", referred_to));
        if (!team_override.empty()) update_fields.append(kvp("team", team_override));

        auto fields = update_fields.extract();
        if (!fields.view().empty()) {
            collection.update_one(
                make_document(kvp("_id", oid)),
                make_document(kvp("$set", fields.view()))
            );
        }

        // Auto-add candidate to team roster if accepted
        if (status == "accepted") {
            auto appDoc = collection.find_one(make_document(kvp("_id", oid)));
            if (appDoc) {
                auto appView = appDoc->view();
                std::string applicantName = getStr(appView, "name");
                std::string realTeamName = !team_override.empty() ? team_override : getStr(appView, "team");

                if (!realTeamName.empty() && !applicantName.empty()) {
                    auto teamsColl = client["application"]["teams"];
                    auto teamDoc = teamsColl.find_one(make_document(kvp("name", realTeamName)));
                    if (teamDoc) {
                        auto teamView = teamDoc->view();
                        bool alreadyMember = false;
                        if (teamView["memberList"] && teamView["memberList"].type() == bsoncxx::type::k_array) {
                            for (auto&& m : teamView["memberList"].get_array().value) {
                                if (m.type() != bsoncxx::type::k_document) continue;
                                if (getStr(m.get_document().value, "name") == applicantName) {
                                    alreadyMember = true;
                                    break;
                                }
                            }
                        }

                        if (!alreadyMember) {
                            auto newMember = make_document(
                                kvp("id", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                                kvp("name", applicantName),
                                kvp("role", "Member")
                            );
                            teamsColl.update_one(
                                make_document(kvp("name", realTeamName)),
                                make_document(
                                    kvp("$push", make_document(kvp("memberList", newMember))),
                                    kvp("$inc", make_document(kvp("members", 1)))
                                )
                            );
                        }
                    } else {
                        bsoncxx::builder::basic::array membersArr{};
                        auto newMember = make_document(
                            kvp("id", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                            kvp("name", applicantName),
                            kvp("role", "Member")
                        );
                        membersArr.append(newMember);

                        teamsColl.insert_one(make_document(
                            kvp("name", realTeamName),
                            kvp("desc", "Team created from accepted candidate"),
                            kvp("members", 1),
                            kvp("memberList", membersArr.extract())
                        ));
                    }
                }
            }
        }

        logSystemEvent(client, "Application Updated", "Application " + id_str + " status updated to " + status + " by " + actorEmail + (!reason.empty() ? " (Reason: " + reason + ")" : ""), "application");
        return jsonOk();
    });
}

// ---------------------------------------------------------------------------
// Teams
// ---------------------------------------------------------------------------
void listTeams(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    // Public visitors get the showcase fields only; manager emails and member
    // rosters are returned to signed-in staff.
    bool signedIn = false;
    if (!req->getHeader("Authorization").empty() || !req->getHeader("X-Admin-Token").empty()) {
        signedIn = authenticate(req).claims.has_value();
    }

    on_db(req, std::move(callback), [signedIn](mongocxx::client& client) {
        auto collection = client["application"]["teams"];
        auto cursor = collection.find({});

        std::unordered_set<std::string> seen;
        Json::Value arr = Json::arrayValue;
        for (auto&& doc : cursor) {
            Json::Value item = docToJson(doc);
            if (item.isMember("name") && item["name"].isString()) {
                std::string trimmed = trimString(item["name"].asString());
                std::string lower = sec::toLower(trimmed);
                if (seen.find(lower) != seen.end()) {
                    continue;
                }
                seen.insert(lower);
                item["name"] = trimmed;
            }
            if (!signedIn) {
                Json::Value pub(Json::objectValue);
                for (const char* key : {"_id", "name", "desc", "members"}) {
                    if (item.isMember(key)) pub[key] = item[key];
                }
                item = pub;
            }
            arr.append(item);
        }

        Json::Value ret;
        ret["teams"] = arr;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

// Find a team by ObjectId when one is supplied, otherwise by name
// (case-insensitive, literal).
static std::optional<bsoncxx::document::value> findTeam(mongocxx::collection& coll,
                                                        const std::string& teamId,
                                                        const std::string& teamName) {
    if (sec::isHexObjectId(teamId)) {
        auto doc = coll.find_one(make_document(kvp("_id", bsoncxx::oid(teamId))));
        if (doc) return bsoncxx::document::value(std::move(*doc));
    }
    if (!teamName.empty()) {
        auto doc = coll.find_one(make_document(kvp("name", exactMatch(teamName))));
        if (doc) return bsoncxx::document::value(std::move(*doc));
    }
    return std::nullopt;
}

void createTeam(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"teams", "content"})) return;

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    const std::string action = jstr(*json, "action");
    const bool scoped = isTeamScoped(*claims);
    const std::string userTeam = claims->team;
    const std::string actorEmail = claims->email;

    if (action == "delete") {
        if (scoped) {
            callback(jsonError(k403Forbidden, "Team managers cannot delete teams"));
            return;
        }
        std::string teamId = jstr(*json, "id");
        std::string teamName = trimString(jstr(*json, "name"));
        if (!sec::isValidText(teamName, kMaxTeamLen, true) || (teamName.empty() && !sec::isHexObjectId(teamId))) {
            callback(jsonError(k400BadRequest, "A team id or name is required"));
            return;
        }

        on_db(req, std::move(callback), [teamId, teamName, actorEmail](mongocxx::client& client) {
            auto collection = client["application"]["teams"];

            auto tDoc = findTeam(collection, teamId, teamName);
            std::string deletedName = teamName;
            if (tDoc) {
                const std::string stored = getStr(tDoc->view(), "name");
                if (!stored.empty()) deletedName = trimString(stored);
                collection.delete_one(make_document(kvp("_id", tDoc->view()["_id"].get_oid().value)));
            }

            if (!deletedName.empty()) {
                auto contentColl = client["application"]["content"];
                contentColl.update_one(
                    make_document(),
                    make_document(kvp("$pull", make_document(
                        kvp("recruitmentTeams", deletedName),
                        kvp("insideTeams", make_document(kvp("name", exactMatch(deletedName))))
                    )))
                );
            }

            logSystemEvent(client, "Team Deleted", "Team " + (deletedName.empty() ? teamId : deletedName) + " removed by " + actorEmail, "team");
            return jsonOk();
        });
        return;
    }

    if (action == "update_roster") {
        if (!hasPerm(*claims, "teams")) {
            callback(jsonError(k403Forbidden, "Forbidden: you do not have permission to manage rosters"));
            return;
        }
        std::string teamId = jstr(*json, "id");
        std::string teamName = trimString(jstr(*json, "name"));
        const Json::Value& memberList = (*json)["memberList"];
        if (!memberList.isNull() && (!memberList.isArray() || memberList.size() > kMaxRosterSize)) {
            callback(jsonError(k400BadRequest, "memberList must be an array of at most 500 members"));
            return;
        }

        struct Member { int64_t id; std::string name; std::string role; };
        std::vector<Member> roster;
        for (const auto& m : memberList) {
            if (!m.isObject()) {
                callback(jsonError(k400BadRequest, "Invalid roster entry"));
                return;
            }
            Member entry;
            entry.id = m["id"].isInt64() ? m["id"].asInt64() : 0;
            entry.name = trimString(jstr(m, "name"));
            entry.role = trimString(jstr(m, "role"));
            if (entry.role.empty()) entry.role = "Member";
            if (!sec::isValidText(entry.name, kMaxNameLen, true) || !sec::isValidText(entry.role, kMaxTeamLen)) {
                callback(jsonError(k400BadRequest, "Invalid roster entry"));
                return;
            }
            roster.push_back(std::move(entry));
        }
        int members = (*json)["members"].isInt() ? (*json)["members"].asInt() : static_cast<int>(roster.size());
        members = std::clamp(members, 0, 100000);

        on_db(req, std::move(callback), [teamId, teamName, members, roster, scoped, userTeam, actorEmail](mongocxx::client& client) {
            auto collection = client["application"]["teams"];

            auto tDoc = findTeam(collection, teamId, teamName);
            if (!tDoc) {
                return jsonError(k404NotFound, "Team not found");
            }
            const std::string storedName = getStr(tDoc->view(), "name");
            if (scoped && !sameTeam(storedName, userTeam)) {
                return jsonError(k403Forbidden, "You can only manage your own team's roster");
            }

            bsoncxx::builder::basic::array membersArr{};
            for (const auto& m : roster) {
                membersArr.append(make_document(kvp("id", m.id), kvp("name", m.name), kvp("role", m.role)));
            }

            collection.update_one(
                make_document(kvp("_id", tDoc->view()["_id"].get_oid().value)),
                make_document(kvp("$set", make_document(
                    kvp("members", members),
                    kvp("memberList", membersArr.extract())
                )))
            );

            logSystemEvent(client, "Roster Updated", "Team " + storedName + " roster updated by " + actorEmail + " (" + std::to_string(members) + " members)", "team");
            return jsonOk();
        });
        return;
    }

    if (action == "update") {
        std::string teamId = jstr(*json, "id");
        std::string oldName = trimString(jstr(*json, "oldName"));
        std::string name = trimString(jstr(*json, "name"));
        std::string desc = trimString(jstr(*json, "desc"));
        if (!sec::isValidText(oldName, kMaxTeamLen, true) || !sec::isValidText(name, kMaxTeamLen, true) ||
            !sec::isValidMultiline(desc, kMaxDescLen)) {
            callback(jsonError(k400BadRequest, "Invalid team name or description"));
            return;
        }

        on_db(req, std::move(callback), [teamId, oldName, name, desc, scoped, userTeam, actorEmail](mongocxx::client& client) {
            auto collection = client["application"]["teams"];

            std::string actualOldName = oldName;
            auto tDoc = findTeam(collection, teamId, oldName);
            if (tDoc) {
                const std::string stored = getStr(tDoc->view(), "name");
                if (!stored.empty()) actualOldName = stored;
            }
            if (scoped && !sameTeam(actualOldName, userTeam)) {
                return jsonError(k403Forbidden, "You can only edit your own team");
            }

            if (tDoc) {
                collection.update_one(
                    make_document(kvp("_id", tDoc->view()["_id"].get_oid().value)),
                    make_document(kvp("$set", make_document(
                        kvp("name", name.empty() ? actualOldName : name),
                        kvp("desc", desc)
                    )))
                );
            }

            if (!actualOldName.empty() && !name.empty() && actualOldName != name) {
                auto usersColl = client["application"]["users"];
                usersColl.update_many(
                    make_document(kvp("team", actualOldName)),
                    make_document(kvp("$set", make_document(kvp("team", name))))
                );
                auto appsColl = client["application"]["applications"];
                appsColl.update_many(
                    make_document(kvp("team", actualOldName)),
                    make_document(kvp("$set", make_document(kvp("team", name))))
                );
                appsColl.update_many(
                    make_document(kvp("referredTo", actualOldName)),
                    make_document(kvp("$set", make_document(kvp("referredTo", name))))
                );
                auto contentColl = client["application"]["content"];
                contentColl.update_one(
                    make_document(kvp("recruitmentTeams", actualOldName)),
                    make_document(kvp("$set", make_document(kvp("recruitmentTeams.$", name))))
                );
                contentColl.update_one(
                    make_document(kvp("insideTeams.name", exactMatch(actualOldName))),
                    make_document(kvp("$set", make_document(
                        kvp("insideTeams.$.name", name),
                        kvp("insideTeams.$.desc", desc)
                    )))
                );
            } else if (!name.empty() && !desc.empty()) {
                auto contentColl = client["application"]["content"];
                contentColl.update_one(
                    make_document(kvp("insideTeams.name", exactMatch(name))),
                    make_document(kvp("$set", make_document(
                        kvp("insideTeams.$.desc", desc)
                    )))
                );
            }

            logSystemEvent(client, "Team Updated", "Team " + (actualOldName.empty() ? name : actualOldName) + " details updated by " + actorEmail, "team");
            return jsonOk();
        });
        return;
    }

    if (scoped) {
        callback(jsonError(k403Forbidden, "Team managers cannot create teams"));
        return;
    }

    std::string name = trimString(jstr(*json, "name"));
    std::string desc = trimString(jstr(*json, "desc"));
    if (!sec::isValidText(name, kMaxTeamLen) || !sec::isValidMultiline(desc, kMaxDescLen)) {
        callback(jsonError(k400BadRequest, "Invalid team name or description"));
        return;
    }

    on_db(req, std::move(callback), [name, desc, actorEmail](mongocxx::client& client) {
        auto collection = client["application"]["teams"];

        // Prevent duplicate team insertion in teams collection
        auto existing = collection.find_one(make_document(kvp("name", exactMatch(name))));
        if (!existing) {
            bsoncxx::builder::basic::document doc{};
            doc.append(kvp("name", name));
            doc.append(kvp("desc", desc));
            doc.append(kvp("members", 0));
            collection.insert_one(doc.view());
        } else if (!desc.empty()) {
            collection.update_one(
                make_document(kvp("_id", existing->view()["_id"].get_oid().value)),
                make_document(kvp("$set", make_document(kvp("desc", desc))))
            );
        }

        // Automatically sync to both recruitmentTeams and insideTeams in content
        auto contentColl = client["application"]["content"];
        contentColl.update_one(
            make_document(),
            make_document(kvp("$addToSet", make_document(kvp("recruitmentTeams", name)))),
            mongocxx::options::update{}.upsert(true)
        );

        auto curContent = contentColl.find_one({});
        bool alreadyInInside = false;
        if (curContent && curContent->view()["insideTeams"] &&
            curContent->view()["insideTeams"].type() == bsoncxx::type::k_array) {
            for (auto&& it : curContent->view()["insideTeams"].get_array().value) {
                if (it.type() != bsoncxx::type::k_document) continue;
                if (trimString(getStr(it.get_document().value, "name")) == name) {
                    alreadyInInside = true;
                    break;
                }
            }
        }
        if (!alreadyInInside) {
            auto insideDoc = make_document(
                kvp("id", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count())),
                kvp("name", name),
                kvp("desc", desc)
            );
            contentColl.update_one(
                make_document(),
                make_document(kvp("$push", make_document(kvp("insideTeams", insideDoc))))
            );
        } else if (!desc.empty()) {
            contentColl.update_one(
                make_document(kvp("insideTeams.name", exactMatch(name))),
                make_document(kvp("$set", make_document(kvp("insideTeams.$.desc", desc))))
            );
        }

        logSystemEvent(client, "Team Created", "New team " + name + " added by " + actorEmail, "team");
        return jsonOk();
    });
}

// ---------------------------------------------------------------------------
// Content CMS
// ---------------------------------------------------------------------------
void getContent(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["content"];
        auto doc = collection.find_one({});
        Json::Value ret;
        ret["content"] = doc ? docToJson(*doc) : Json::Value(Json::objectValue);
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

static bool isPlainIdentifier(std::string_view key) {
    if (key.empty() || key.size() > 64) return false;
    return std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_';
    });
}

void setContent(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"content", "gallery"})) return;

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    const bool contentEditor = hasPerm(*claims, "content");

    // Only plain top-level keys reach `$set`: no operators, no dotted paths
    // into other fields, no `_id`. Gallery-only editors are limited to the
    // image fields; anything else they send is ignored.
    Json::Value filtered(Json::objectValue);
    for (const auto& key : json->getMemberNames()) {
        if (key == "_id") continue;
        if (!isPlainIdentifier(key) || !sec::isSafeJsonTree((*json)[key])) {
            callback(jsonError(k400BadRequest, "Invalid content field: " + key.substr(0, 64)));
            return;
        }
        if (!contentEditor && kGalleryContentKeys.count(key) == 0) continue;
        filtered[key] = (*json)[key];
    }

    std::string jsonStr = sec::compactJson(filtered);
    if (jsonStr.size() > kMaxContentBytes) {
        callback(jsonError(k413RequestEntityTooLarge, "Content document is too large"));
        return;
    }

    // Extract insideTeams to sync with application.teams collection
    std::vector<std::pair<std::string, std::string>> insideList;
    if (contentEditor && filtered.isMember("insideTeams") && filtered["insideTeams"].isArray()) {
        for (const auto& it : filtered["insideTeams"]) {
            std::string tName = trimString(jstr(it, "name"));
            std::string tDesc = trimString(jstr(it, "desc"));
            if (sec::isValidText(tName, kMaxTeamLen) && sec::isValidMultiline(tDesc, kMaxDescLen)) {
                insideList.push_back({tName, tDesc});
            }
        }
    }

    const std::string actorEmail = claims->email;
    on_db(req, std::move(callback), [jsonStr, insideList, actorEmail](mongocxx::client& client) {
        auto collection = client["application"]["content"];

        auto update_doc = bsoncxx::from_json(jsonStr);
        if (!update_doc.view().empty()) {
            collection.update_one(
                make_document(),
                make_document(kvp("$set", update_doc.view())),
                mongocxx::options::update{}.upsert(true)
            );
        }

        // Sync insideTeams into application.teams collection
        auto teamsColl = client["application"]["teams"];
        for (const auto& p : insideList) {
            const std::string& tName = p.first;
            const std::string& tDesc = p.second;
            auto existing = teamsColl.find_one(make_document(kvp("name", exactMatch(tName))));
            if (!existing) {
                bsoncxx::builder::basic::document tDoc{};
                tDoc.append(kvp("name", tName));
                tDoc.append(kvp("desc", tDesc));
                tDoc.append(kvp("members", 0));
                teamsColl.insert_one(tDoc.view());
            } else if (!tDesc.empty()) {
                teamsColl.update_one(
                    make_document(kvp("_id", existing->view()["_id"].get_oid().value)),
                    make_document(kvp("$set", make_document(kvp("desc", tDesc))))
                );
            }
        }

        logSystemEvent(client, "Content Updated", "Website content updated in Content CMS by " + actorEmail, "content");
        return jsonOk();
    });
}

// ---------------------------------------------------------------------------
// Analytics and logs
// ---------------------------------------------------------------------------
void trackVisit(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    // Over the limit the request still "succeeds" — the client has nothing
    // to retry — it just stops inflating the counter.
    if (!gVisitLimiter.isAllowed(sec::clientIp(req))) {
        callback(jsonOk());
        return;
    }

    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["analytics"];

        auto now = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        gmtime_r(&tt, &tm_buf);
        char date_str[32];
        std::strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_buf);

        // Upsert: increment total visit count and daily visit count
        collection.update_one(
            make_document(kvp("_id", "visits")),
            make_document(kvp("$inc", make_document(
                kvp("count", (int64_t)1),
                kvp(std::string("daily.") + date_str, (int64_t)1)
            ))),
            mongocxx::options::update{}.upsert(true)
        );
        return jsonOk();
    });
}

void getAnalytics(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"dashboard"})) return;

    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["analytics"];
        auto doc = collection.find_one(make_document(kvp("_id", "visits")));
        Json::Value ret;
        int64_t visits = 0;
        int64_t today_visits = 0;
        Json::Value dailyObj(Json::objectValue);

        auto now = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        gmtime_r(&tt, &tm_buf);
        char today_str[32];
        std::strftime(today_str, sizeof(today_str), "%Y-%m-%d", &tm_buf);

        if (doc) {
            auto view = doc->view();
            visits = getInt(view, "count");
            if (view["daily"] && view["daily"].type() == bsoncxx::type::k_document) {
                auto dailyDoc = view["daily"].get_document().value;
                for (auto elem : dailyDoc) {
                    std::string k(elem.key());
                    int64_t v = 0;
                    if (elem.type() == bsoncxx::type::k_int64) {
                        v = elem.get_int64().value;
                    } else if (elem.type() == bsoncxx::type::k_int32) {
                        v = elem.get_int32().value;
                    } else if (elem.type() == bsoncxx::type::k_double) {
                        v = static_cast<int64_t>(elem.get_double().value);
                    }
                    dailyObj[k] = static_cast<Json::Value::Int64>(v);
                    if (k == today_str) {
                        today_visits = v;
                    }
                }
            }
        }
        ret["visits"] = static_cast<Json::Value::Int64>(visits);
        ret["today_visits"] = static_cast<Json::Value::Int64>(today_visits);
        ret["daily"] = dailyObj;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void listLogs(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"dashboard"})) return;

    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto logsColl = client["application"]["logs"];

        mongocxx::options::find opts{};
        opts.sort(make_document(kvp("timestamp", -1)));
        opts.limit(50);

        auto cursor = logsColl.find({}, opts);
        Json::Value logsArr = Json::arrayValue;
        for (auto&& doc : cursor) {
            logsArr.append(docToJson(doc));
        }

        Json::Value ret;
        ret["logs"] = logsArr;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

// ---------------------------------------------------------------------------
// Uploads
// ---------------------------------------------------------------------------
static std::string base64_decode_str(const std::string &in) {
    std::string out;
    out.reserve(in.size() * 3 / 4);
    std::array<int, 256> T;
    T.fill(-1);
    for (int i = 0; i < 64; i++)
        T[static_cast<unsigned char>("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i])] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = ((val << 6) + T[c]) & 0xFFFFFF;
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Image resize helper
// Decodes src_data (JPEG or PNG — the only decoders compiled in, see
// stb_impl.cc), downsizes so neither side exceeds max_side, and re-encodes as
// JPEG. Re-encoding is what makes uploads safe to serve: whatever the client
// sent, only freshly encoded pixels reach disk. Returns an empty vector on any
// failure, including images whose header declares oversized dimensions
// (decompression bombs) — the caller then rejects the upload.
// ---------------------------------------------------------------------------
static std::vector<unsigned char> resize_image_to_jpeg(
        const unsigned char *src_data, int src_len,
        int max_side = 1600, int jpeg_quality = 82) {

    int w = 0, h = 0, ch = 0;
    if (!stbi_info_from_memory(src_data, src_len, &w, &h, &ch)) return {};
    if (w <= 0 || h <= 0 || w > kMaxImageSide || h > kMaxImageSide ||
        static_cast<int64_t>(w) * h > kMaxImagePixels) {
        return {};
    }

    // Force 3 channels (RGB) — JPEG doesn't support alpha anyway
    stbi_uc *pixels = stbi_load_from_memory(src_data, src_len, &w, &h, &ch, 3);
    if (!pixels) return {};

    int new_w = w, new_h = h;
    if (w > max_side || h > max_side) {
        if (w >= h) { new_w = max_side; new_h = static_cast<int>((static_cast<int64_t>(h) * max_side) / w); }
        else         { new_h = max_side; new_w = static_cast<int>((static_cast<int64_t>(w) * max_side) / h); }
        if (new_w < 1) new_w = 1;
        if (new_h < 1) new_h = 1;
    }

    std::vector<unsigned char> out_pixels;
    if (new_w != w || new_h != h) {
        out_pixels.resize(static_cast<std::size_t>(new_w) * new_h * 3);
        unsigned char* resized = stbir_resize_uint8_linear(pixels, w, h, 0,
                                  out_pixels.data(), new_w, new_h, 0,
                                  STBIR_RGB);
        stbi_image_free(pixels);
        pixels = nullptr;
        if (!resized) return {};
    }

    // Encode as JPEG in-memory
    std::vector<unsigned char> jpeg_bytes;
    auto write_cb = [](void *ctx, void *data, int size) {
        auto *buf = static_cast<std::vector<unsigned char> *>(ctx);
        const auto *p = static_cast<unsigned char *>(data);
        buf->insert(buf->end(), p, p + size);
    };

    const unsigned char *src = (new_w != w || new_h != h)
                                ? out_pixels.data()
                                : pixels;
    int ok = stbi_write_jpg_to_func(write_cb, &jpeg_bytes,
                                    new_w, new_h, 3, src, jpeg_quality);
    if (pixels) stbi_image_free(pixels);

    return ok ? jpeg_bytes : std::vector<unsigned char>{};
}

// ---------------------------------------------------------------------------
// Upload route — POST /api/upload
// Accepts multipart/form-data or application/json {data:"data:...base64...", filename:"x"}
// Only JPEG and PNG are accepted. Every upload is decoded and re-encoded as a
// JPEG under a server-chosen name; the client's filename is used only to read
// its extension, never to build a path.
// ---------------------------------------------------------------------------
void uploadFile(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"content", "gallery"})) return;

    auto extension_of = [](const std::string& filename) {
        auto pos = filename.find_last_of('.');
        if (pos == std::string::npos) return std::string{};
        return sec::toLower(filename.substr(pos));
    };
    auto is_allowed_image = [](const std::string &ext) {
        return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
    };

    std::string savedUrl;
    std::string error = "No file received";

    auto process_and_save = [&](const unsigned char *data, std::size_t len, const std::string &ext) {
        if (!is_allowed_image(ext)) {
            error = "Only JPEG and PNG images can be uploaded";
            return;
        }
        if (len == 0 || len > kMaxUploadBytes) {
            error = "Image must be smaller than 15 MB";
            return;
        }
        auto jpeg = resize_image_to_jpeg(data, static_cast<int>(len));
        if (jpeg.empty()) {
            error = "The file is not a valid JPEG or PNG image, or its dimensions are too large";
            return;
        }
        std::string ts = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::string fname = "img_" + ts + ".jpg";
        std::ofstream f("./public/assets/uploads/" + fname, std::ios::binary);
        if (!f) {
            error = "Failed to store image";
            return;
        }
        f.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        f.close();
        savedUrl = "/assets/uploads/" + fname;
    };

    if (req->getContentType() == CT_MULTIPART_FORM_DATA) {
        drogon::MultiPartParser parser;
        if (parser.parse(req) == 0 && !parser.getFiles().empty()) {
            auto &file = parser.getFiles()[0];
            process_and_save(reinterpret_cast<const unsigned char *>(file.fileData()),
                             file.fileLength(), extension_of(file.getFileName()));
        }
    } else if (auto json = req->getJsonObject(); json && json->isObject()) {
        std::string data_str = jstr(*json, "data");
        std::string name     = jstr(*json, "filename");
        auto comma = data_str.find(',');
        if (comma != std::string::npos) data_str = data_str.substr(comma + 1);
        if (data_str.size() > (kMaxUploadBytes / 3 + 1) * 4) {
            error = "Image must be smaller than 15 MB";
        } else {
            std::string decoded = base64_decode_str(data_str);
            std::string ext = name.empty() ? ".png" : extension_of(name);
            process_and_save(reinterpret_cast<const unsigned char *>(decoded.data()), decoded.size(), ext);
        }
    }

    if (savedUrl.empty()) {
        callback(jsonError(k400BadRequest, error));
        return;
    }
    Json::Value ret;
    ret["status"] = "ok";
    ret["url"]    = savedUrl;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// ---------------------------------------------------------------------------
// Forms
// ---------------------------------------------------------------------------
void getFormSchema(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["form_schema"];
        auto doc = collection.find_one({});
        Json::Value ret;
        ret["schema"] = doc ? docToJson(*doc) : Json::Value(Json::objectValue);
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void setFormSchema(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"form_maker"})) return;

    auto json = requireJsonObject(req, callback);
    if (!json) return;

    Json::Value schema = *json;
    schema.removeMember("_id");
    if (!sec::isSafeJsonTree(schema)) {
        callback(jsonError(k400BadRequest, "Invalid form schema"));
        return;
    }
    std::string jsonStr = sec::compactJson(schema);
    if (jsonStr.size() > kMaxFormSchemaBytes) {
        callback(jsonError(k413RequestEntityTooLarge, "Form schema is too large"));
        return;
    }

    on_db(req, std::move(callback), [jsonStr](mongocxx::client& client) {
        auto collection = client["application"]["form_schema"];
        auto doc = bsoncxx::from_json(jsonStr);
        collection.delete_many({});
        collection.insert_one(doc.view());
        return jsonOk();
    });
}

void getFormSubmissions(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"applications", "form_maker"})) return;

    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["form_submissions"];
        mongocxx::options::find opts;
        opts.sort(make_document(kvp("_id", -1)));
        auto cursor = collection.find({}, opts);
        Json::Value ret;
        ret["submissions"] = Json::arrayValue;
        for (auto&& doc : cursor) {
            ret["submissions"].append(docToJson(doc));
        }
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

static void removeFormSubmission(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback,
                                 const std::string& subId, const std::string& actorEmail) {
    if (subId.empty() || !sec::isValidText(subId, 64)) {
        callback(jsonError(k400BadRequest, "Invalid submission id"));
        return;
    }
    on_db(req, std::move(callback), [subId, actorEmail](mongocxx::client& client) {
        auto collection = client["application"]["form_submissions"];
        if (sec::isHexObjectId(subId)) {
            collection.delete_one(make_document(kvp("_id", bsoncxx::oid(subId))));
        } else {
            collection.delete_one(make_document(kvp("id", subId)));
        }
        logSystemEvent(client, "Form Submission Deleted", "Submission " + subId + " deleted by " + actorEmail, "form");
        return jsonOk();
    });
}

void deleteFormSubmission(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::optional<enactus::security::JwtClaims> claims;
    if (!requireAdminAuth(req, callback, claims, {"applications", "form_maker"})) return;

    auto json = req->getJsonObject();
    std::string subId = (json && json->isObject() && (*json)["id"].isString())
                            ? (*json)["id"].asString()
                            : req->getParameter("id");
    removeFormSubmission(req, std::move(callback), subId, claims->email);
}

void submitForm(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = requireJsonObject(req, callback);
    if (!json) return;

    if (jstr(*json, "action") == "delete") {
        std::optional<enactus::security::JwtClaims> claims;
        if (!requireAdminAuth(req, callback, claims, {"applications", "form_maker"})) return;
        removeFormSubmission(req, std::move(callback), jstr(*json, "id"), claims->email);
        return;
    }

    if (!gSubmitLimiter.isAllowed(sec::clientIp(req))) {
        callback(jsonError(k429TooManyRequests, "Too many submissions. Please try again later."));
        return;
    }

    Json::Value payload;
    if (json->size() > kMaxSubmissionFields || !sec::sanitizeKeysForStorage(*json, payload)) {
        callback(jsonError(k400BadRequest, "Invalid submission"));
        return;
    }
    payload.removeMember("_id");
    payload.removeMember("action");

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    gmtime_r(&in_time_t, &tm_buf);
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    payload["submittedAt"] = ss.str();

    std::string jsonStr = sec::compactJson(payload);
    if (jsonStr.size() > kMaxSubmissionBytes) {
        callback(jsonError(k413RequestEntityTooLarge, "Submission is too large"));
        return;
    }

    on_db(req, std::move(callback), [jsonStr](mongocxx::client& client) {
        auto collection = client["application"]["form_submissions"];
        auto doc = bsoncxx::from_json(jsonStr);
        collection.insert_one(doc.view());
        return jsonOk();
    });
}

void migratePasswordsToArgon2() {
    try {
        auto client = anvil::db::MongoPool::instance().acquire();
        auto usersColl = (*client)["application"]["users"];
        auto cursor = usersColl.find({});
        for (auto&& doc : cursor) {
            const std::string pass = getStr(doc, "password");
            if (pass.empty() || pass.starts_with("$argon2id$") || !doc["_id"]) continue;
            usersColl.update_one(
                make_document(kvp("_id", doc["_id"].get_value())),
                make_document(kvp("$set", make_document(kvp("password", gPasswordHasher.hash(pass)))))
            );
            std::cout << "[Security] Migrated user " << getStr(doc, "email") << " password to Argon2id." << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Security] Password migration warning: " << e.what() << std::endl;
    }
}

} // namespace enactus

void registerApiHandlers() {
    enactus::migratePasswordsToArgon2();
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/auth/login", drogon::Post, &enactus::login);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/auth/logout", drogon::Post, &enactus::logout);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/auth/me", drogon::Get, &enactus::me);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/applications", drogon::Post, &enactus::apply);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/applications_list", drogon::Get, &enactus::listApplications);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/teams", drogon::Get, &enactus::listTeams);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/teams", drogon::Post, &enactus::createTeam);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/content", drogon::Get, &enactus::getContent);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/content", drogon::Post, &enactus::setContent);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/users", drogon::Get, &enactus::listUsers);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/users", drogon::Post, &enactus::createUser);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/users", drogon::Delete, &enactus::deleteUser);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/track_visit", drogon::Post, &enactus::trackVisit);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/analytics", drogon::Get, &enactus::getAnalytics);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/logs", drogon::Get, &enactus::listLogs);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/applications_update", drogon::Post, &enactus::updateApplicationStatus);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/upload", drogon::Post, &enactus::uploadFile);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_schema", drogon::Get, &enactus::getFormSchema);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_schema", drogon::Post, &enactus::setFormSchema);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_submissions", drogon::Get, &enactus::getFormSubmissions);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_submissions", drogon::Post, &enactus::submitForm);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_submissions", drogon::Delete, &enactus::deleteFormSubmission);
}
