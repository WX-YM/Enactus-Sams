#include "app/staff.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/update.hpp>

#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/route_projection.h"
#include "anvil/accounts/identifier.h"
#include "anvil/auth/prehash.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"
#include "anvil/db/versioned.h"
#include "anvil/http/json_writer.h"
#include "anvil/identity/login_identity.h"
#include "anvil/input/schema.h"
#include "app/services.h"
#include "perms.h"
#include "rate_limits.h"
#include "route_descriptions.h"
#include "routes.h"

namespace enactus {

namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
namespace codec = anvil::db::codec;
namespace ac = anvil::accesscontrol;
namespace input = anvil::input;
using anvil::ErrorCode;
using anvil::PermSet;
using anvil::UserStatus;
using anvil::UserType;
using anvil::Uuid;

constexpr std::string_view kRole = "role";
constexpr std::string_view kTeam = "team";

constexpr input::TextRules kRoleRules{1, 24, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kTeamRules{0, 80, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kCredentialRules{43, 43, anvil::i18n::TextClass::Identifier, false};

[[nodiscard]] mongocxx::collection profiles(mongocxx::client& client) {
    return client[services().database][std::string{kStaffProfilesCollection}];
}

// Permission names from a request body, against the grantable set. Anything
// else — a typo, an implied bit, a bit that does not exist — is refused rather
// than ignored: a grant the admin believes they made must be a grant that
// happened.
[[nodiscard]] std::optional<PermSet> grant_from(const input::JsonValue& array) {
    PermSet granted{};
    for (const input::JsonValue& item : array.elements()) {
        const std::optional<std::string_view> name = item.as_string();
        if (!name.has_value()) { return std::nullopt; }
        const std::optional<std::uint8_t> bit = kPerms.bit_for_name(*name);
        if (!bit.has_value() || !kGrantable.test(*bit)) { return std::nullopt; }
        granted.set(*bit);
    }
    return granted;
}

// No account may hand out authority it does not hold itself, and only a
// superadmin may touch a superadmin. Anything a non-superadmin grants must be
// a subset of their own effective set.
[[nodiscard]] bool may_grant(const anvil::UserContext& actor, const PermSet& granted) {
    if (ac::is_superadmin(actor.user_type)) { return true; }
    return actor.permissions.contains_all(granted);
}

// Whether `actor` may edit or disable `target` at all. Below superadmin, only
// an account whose access is within the actor's own: otherwise anyone holding
// Access Control could strip or disable the board members above them. A
// superadmin target is a superadmin's alone.
[[nodiscard]] bool may_manage(const anvil::UserContext& actor, const anvil::identity::AccountRecord& target) {
    if (ac::is_superadmin(actor.user_type)) { return true; }
    if (target.user_type == UserType::SuperAdmin) { return false; }
    return actor.permissions.contains_all(target.direct_permissions & kGrantable);
}

void append_permissions(std::string& out, const PermSet& held) {
    out += '[';
    bool first = true;
    kPerms.for_each_name(held & kGrantable, [&](std::string_view name) {
        if (!first) { out += ','; }
        first = false;
        anvil::http::append_json_string(out, name);
    });
    out += ']';
}

[[nodiscard]] std::string_view type_name(UserType type) {
    switch (type) {
        case UserType::SuperAdmin: return "superadmin";
        case UserType::FullControl: return "full_control";
        case UserType::Staff: return "staff";
        case UserType::Client: return "client";
    }
    return "client";
}

[[nodiscard]] std::string_view status_name(UserStatus status) {
    switch (status) {
        case UserStatus::Active: return "active";
        case UserStatus::Disabled: return "disabled";
        case UserStatus::Locked: return "locked";
        default: return "pending";
    }
}

}  // namespace

bool is_staff_role(std::string_view role) noexcept {
    return std::find(kStaffRoles.begin(), kStaffRoles.end(), role) != kStaffRoles.end();
}

bool is_team_scoped_role(std::string_view role) noexcept {
    return role == "manager" || role == "vice manager";
}

anvil::Result<std::optional<StaffProfile>> StaffProfiles::find(mongocxx::client& client,
                                                               const Uuid& user) {
    return anvil::repo::guarded([&]() -> anvil::Result<std::optional<StaffProfile>> {
        const auto found = profiles(client).find_one(make_document(kvp("_id", codec::uuid_bin(user))));
        if (!found) { return std::optional<StaffProfile>{}; }
        const auto role = codec::read_text(found->view(), kRole);
        const auto team = codec::read_text(found->view(), kTeam);
        if (!role || !team) { return anvil::fail(ErrorCode::Internal, "profile"); }
        return std::optional<StaffProfile>{
            StaffProfile{std::string{role.value()}, std::string{team.value()}}};
    });
}

anvil::Status StaffProfiles::put(mongocxx::client& client, const Uuid& user,
                                 const StaffProfile& profile) {
    return anvil::repo::guarded([&]() -> anvil::Status {
        mongocxx::options::update upsert;
        upsert.upsert(true);
        profiles(client).update_one(
            make_document(kvp("_id", codec::uuid_bin(user))),
            make_document(kvp("$set", make_document(kvp(codec::key_of(kRole), profile.role),
                                                    kvp(codec::key_of(kTeam), profile.team)))),
            upsert);
        return anvil::ok();
    });
}

anvil::Status StaffProfiles::rename_team(mongocxx::client& client, std::string_view from,
                                         std::string_view to) {
    return anvil::repo::guarded([&]() -> anvil::Status {
        profiles(client).update_many(
            make_document(kvp(codec::key_of(kTeam), std::string{from})),
            make_document(kvp("$set", make_document(kvp(codec::key_of(kTeam), std::string{to})))));
        return anvil::ok();
    });
}

anvil::Result<std::optional<std::string>> team_scope(mongocxx::client& client,
                                                     const anvil::UserContext& ctx) {
    if (ac::is_superadmin(ctx.user_type)) { return std::optional<std::string>{}; }
    const anvil::Result<std::optional<StaffProfile>> profile = StaffProfiles::find(client, ctx.user_id);
    if (!profile) { return profile.error(); }
    if (!profile.value().has_value() || !is_team_scoped_role(profile.value()->role) ||
        profile.value()->team.empty()) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{profile.value()->team};
}

namespace routes {

// The holder-scoped route table: the paths this account may call, built with
// the same satisfies() the access filter enforces (anvil docs/01-seams.md §4.1).
// hammer fetches it first and compiles no privileged path into the bundle.
void session(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr) {
        respond(http::failure(req, ErrorCode::Unauthenticated));
        return;
    }
    std::string body{R"({"routes":)"};
    ac::append_reachable_routes(body, kRoutes, kRouteDescriptions, ctx->permissions,
                                ctx->user_type);
    body += R"(,"authority":)";
    ac::append_holder_authority(body, ctx->permissions, ctx->user_type, kPerms);
    body += '}';
    http::HttpResponsePtr response = http::json(200, std::move(body));
    response->addHeader("Vary", "Cookie");
    respond(response);
}

void me(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr) {
        respond(http::failure(req, ErrorCode::Unauthenticated));
        return;
    }
    http::db_or_shed(req, respond, [req, respond, ctx](mongocxx::client& client) {
        const auto account = services().accounts_repo.find_account(client, ctx->user_id);
        if (!account || !account.value().has_value()) {
            respond(http::failure(req, ErrorCode::Unauthenticated));
            return;
        }
        const auto profile = StaffProfiles::find(client, ctx->user_id);
        const StaffProfile empty{};
        const StaffProfile& p =
            profile && profile.value().has_value() ? *profile.value() : empty;

        std::string body{"{"};
        http::append_key(body, "id");
        anvil::http::append_json_uuid(body, ctx->user_id);
        http::append_string_field(body, "email", account.value()->email);
        http::append_string_field(body, "type", type_name(ctx->user_type));
        http::append_string_field(body, "role", p.role);
        http::append_string_field(body, "team", p.team);
        body += ',';
        http::append_key(body, "permissions");
        append_permissions(body, ctx->permissions);
        body += '}';
        respond(http::json(200, std::move(body)));
    });
}

void staff_list(const http::HttpRequestPtr& req, http::Responder&& respond) {
    http::db_or_shed(req, respond, [req, respond](mongocxx::client& client) {
        std::string body{R"({"staff":[)"};
        bool first = true;
        std::optional<anvil::identity::AccountCursor> after;
        // Paged through the repository's own cursor; a club's staff list is a
        // few dozen rows, and the loop is bounded regardless.
        for (int page = 0; page < 50; ++page) {
            anvil::identity::AccountQuery query{};
            query.after = after;
            query.limit = 100;
            const auto listed = services().accounts_repo.list_accounts(client, query);
            if (!listed) {
                respond(http::failure(req, listed.error()));
                return;
            }
            for (const anvil::identity::AccountRecord& account : listed.value()) {
                if (account.user_type == UserType::Client) { continue; }
                const auto profile = StaffProfiles::find(client, account.id);
                const StaffProfile empty{};
                const StaffProfile& p =
                    profile && profile.value().has_value() ? *profile.value() : empty;
                if (!first) { body += ','; }
                first = false;
                body += '{';
                http::append_key(body, "id");
                anvil::http::append_json_uuid(body, account.id);
                http::append_string_field(body, "email", account.email);
                http::append_string_field(body, "type", type_name(account.user_type));
                http::append_string_field(body, "status", status_name(account.status));
                http::append_string_field(body, "role", p.role);
                http::append_string_field(body, "team", p.team);
                body += ',';
                http::append_key(body, "version");
                anvil::http::append_json_int(body, account.version);
                body += ',';
                http::append_key(body, "permissions");
                append_permissions(body, account.direct_permissions);
                body += '}';
            }
            if (listed.value().size() < 100) { break; }
            const auto& last = listed.value().back();
            after = anvil::identity::AccountCursor{last.id, last.user_type};
        }
        body += "]}";
        respond(http::json(200, std::move(body)));
    });
}

// Who leads each team: the active accounts whose role is manager or vice
// manager, by the team their profile names. For the Manage Teams cards; the
// public team list carries no staff addresses.
void team_leads(const http::HttpRequestPtr& req, http::Responder&& respond) {
    http::db_or_shed(req, respond, [req, respond](mongocxx::client& client) {
        struct Lead final {
            Uuid        id;
            std::string role;
            std::string team;
        };
        const auto leads = anvil::repo::guarded([&]() -> anvil::Result<std::vector<Lead>> {
            mongocxx::options::find options{};
            options.limit(400);
            std::vector<Lead> out;
            auto cursor = profiles(client).find(
                make_document(kvp(codec::key_of(kRole),
                                  make_document(kvp("$in", bsoncxx::builder::basic::make_array(
                                                               "manager", "vice manager"))))),
                options);
            for (const bsoncxx::document::view row : cursor) {
                const auto id = codec::read_uuid(row, "_id");
                const auto role = codec::read_text(row, kRole);
                const auto team = codec::read_text(row, kTeam);
                if (!id || !role || !team || team.value().empty()) { continue; }
                out.push_back(Lead{id.value(), std::string{role.value()}, std::string{team.value()}});
            }
            return out;
        });
        if (!leads) {
            respond(http::failure(req, leads.error()));
            return;
        }
        std::string body{R"({"leads":[)"};
        bool first = true;
        for (const Lead& lead : leads.value()) {
            const auto account = services().accounts_repo.find_account(client, lead.id);
            if (!account) {
                respond(http::failure(req, account.error()));
                return;
            }
            if (!account.value().has_value() || account.value()->status != UserStatus::Active) {
                continue;
            }
            if (!first) { body += ','; }
            first = false;
            body += '{';
            http::append_key(body, "id");
            anvil::http::append_json_uuid(body, lead.id);
            http::append_string_field(body, "email", account.value()->email);
            http::append_string_field(body, "role", lead.role);
            http::append_string_field(body, "team", lead.team);
            body += '}';
        }
        body += "]}";
        respond(http::json(200, std::move(body)));
    });
}

// Creating an account. The body carries the new account's CREDENTIAL, never a
// password: the admin's browser asked the salt route for this email's
// enrolment salt and derived `k` with Argon2id itself (anvil docs/05 §12), so
// the temporary password exists only in the two browsers that type it.
void staff_create(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> actor = http::context(req);
    if (actor == nullptr || !http::origin_ok(req, respond)) { return; }

    const auto body = std::make_shared<http::Body>(req);
    if (!body->ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body->root()};
    std::string_view email_raw;
    std::string_view credential_text;
    std::string_view role;
    std::string_view team;
    std::optional<bool> superadmin;
    input::Reason reason = bind.text("email", input::TextRules{3, 254, anvil::i18n::TextClass::Identifier, false}, email_raw);
    if (!input::is_ok(reason)) {
        respond(http::invalid(req, "email", reason));
        return;
    }
    if (reason = bind.text("credential", kCredentialRules, credential_text); !input::is_ok(reason)) {
        respond(http::invalid(req, "credential", reason));
        return;
    }
    if (reason = bind.text("role", kRoleRules, role); !input::is_ok(reason) || !is_staff_role(role)) {
        respond(http::invalid(req, "role", input::is_ok(reason) ? input::Reason::NotAllowed : reason));
        return;
    }
    if (reason = bind.text("team", kTeamRules, team); !input::is_ok(reason)) {
        respond(http::invalid(req, "team", reason));
        return;
    }
    const input::JsonValue* permissions = bind.array("permissions", 16, reason);
    if (permissions == nullptr) {
        respond(http::invalid(req, "permissions", reason));
        return;
    }
    if (reason = bind.optional_boolean("superadmin", superadmin); !input::is_ok(reason)) {
        respond(http::invalid(req, "superadmin", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }

    std::string email;
    if (reason = anvil::accounts::canonicalise(anvil::identity::LoginIdentity::Email, email_raw, email);
        !input::is_ok(reason)) {
        respond(http::invalid(req, "email", reason));
        return;
    }
    const std::optional<PermSet> granted = grant_from(*permissions);
    if (!granted.has_value()) {
        respond(http::invalid(req, "permissions", input::Reason::NotAllowed));
        return;
    }
    const bool make_superadmin = superadmin.value_or(false);
    if (!may_grant(*actor, *granted) || (make_superadmin && !ac::is_superadmin(actor->user_type))) {
        respond(http::failure(req, ErrorCode::Forbidden));
        return;
    }
    auto credential = std::make_shared<std::optional<anvil::auth::PrehashKey>>(
        anvil::auth::decode_prehash_credential(credential_text));
    if (!credential->has_value()) {
        respond(http::invalid(req, "credential", input::Reason::BadFormat));
        return;
    }

    const StaffProfile profile{std::string{role}, std::string{team}};
    const PermSet direct = *granted;
    http::db_or_shed(req, respond, [req, respond, actor, email, credential, profile, direct,
                                    make_superadmin](mongocxx::client& client) {
        Services& s = services();
        const auto verdict = s.limiter.check_account(anvil::uuid::to_string(actor->user_id),
                                                     rate_rule("staff-write"));
        if (!verdict.allowed) {
            respond(http::rate_limited(req, verdict, rate_rule("staff-write")));
            return;
        }

        // The salt the browser derived against: the enrolment salt for this
        // email, which is a pure function of it (no lookup), so the account the
        // login route later finds answers with exactly this salt.
        const anvil::auth::PrehashSaltAnswer salt{
            s.prehash.derive_salt(static_cast<std::uint8_t>(anvil::identity::LoginIdentity::Email), email),
            s.prehash_policy().client};
        const std::string hash = s.prehash.enroll(**credential, salt);

        const Uuid id = anvil::uuid::generate_v7();
        const anvil::Status inserted = s.accounts_repo.insert(
            client, anvil::identity::NewUser{.id = id,
                                             .email_normalised = email,
                                             .email_display = email,
                                             .username_normalised = {},
                                             .username_display = {},
                                             .password_hash = hash,
                                             .phone_e164 = {},
                                             .locale = anvil::Locale{},
                                             .status = UserStatus::Active});
        if (!inserted) {
            respond(http::failure(req, inserted.error().code == ErrorCode::Conflict
                                           ? ErrorCode::Conflict
                                           : inserted.error().code));
            return;
        }

        const auto typed = s.staff.set_user_type(
            client, id, anvil::repo::kInitialVersion,
            make_superadmin ? UserType::SuperAdmin : UserType::Staff, with_implied(direct), {},
            anvil::identity::RoleTable{}, UserType::SuperAdmin);
        if (!typed) {
            respond(http::failure(req, typed.error()));
            return;
        }
        if (const anvil::Status put = StaffProfiles::put(client, id, profile); !put) {
            respond(http::failure(req, put.error()));
            return;
        }
        http::audit(req, Action::StaffCreated, id);

        std::string out{"{"};
        http::append_key(out, "id");
        anvil::http::append_json_uuid(out, id);
        out += '}';
        respond(http::json(201, std::move(out)));
    });
}

void staff_update(const http::HttpRequestPtr& req, http::Responder&& respond,
                  const std::string& id_text) {
    const std::shared_ptr<const anvil::UserContext> actor = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (actor == nullptr || !id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }

    const auto body = std::make_shared<http::Body>(req);
    if (!body->ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body->root()};
    std::int64_t version = 0;
    std::optional<std::string_view> role;
    std::optional<std::string_view> team;
    std::optional<bool> superadmin;
    std::optional<bool> active;
    input::Reason reason = bind.integer("version", 1, INT64_MAX, version);
    if (!input::is_ok(reason)) {
        respond(http::invalid(req, "version", reason));
        return;
    }
    if (reason = bind.optional_boolean("active", active); !input::is_ok(reason) || active == std::optional<bool>{false}) {
        // Disabling has its own route (DELETE), which refuses to disable the
        // caller; this flag only brings a disabled account back.
        respond(http::invalid(req, "active", input::is_ok(reason) ? input::Reason::NotAllowed : reason));
        return;
    }
    if (reason = bind.optional_text("role", kRoleRules, role);
        !input::is_ok(reason) || (role.has_value() && !is_staff_role(*role))) {
        respond(http::invalid(req, "role", input::is_ok(reason) ? input::Reason::NotAllowed : reason));
        return;
    }
    if (reason = bind.optional_text("team", kTeamRules, team); !input::is_ok(reason)) {
        respond(http::invalid(req, "team", reason));
        return;
    }
    const input::JsonValue* permissions = bind.optional_array("permissions", 16, reason);
    if (!input::is_ok(reason)) {
        respond(http::invalid(req, "permissions", reason));
        return;
    }
    if (reason = bind.optional_boolean("superadmin", superadmin); !input::is_ok(reason)) {
        respond(http::invalid(req, "superadmin", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    std::optional<PermSet> granted;
    if (permissions != nullptr) {
        granted = grant_from(*permissions);
        if (!granted.has_value()) {
            respond(http::invalid(req, "permissions", input::Reason::NotAllowed));
            return;
        }
        if (!may_grant(*actor, *granted)) {
            respond(http::failure(req, ErrorCode::Forbidden));
            return;
        }
    }
    if (superadmin.has_value() && !ac::is_superadmin(actor->user_type)) {
        respond(http::failure(req, ErrorCode::Forbidden));
        return;
    }

    const Uuid target = *id;
    const std::optional<std::string> new_role =
        role.has_value() ? std::optional<std::string>{std::string{*role}} : std::nullopt;
    const std::optional<std::string> new_team =
        team.has_value() ? std::optional<std::string>{std::string{*team}} : std::nullopt;
    http::db_or_shed(req, respond, [req, respond, actor, target, version, granted, superadmin, active,
                                    new_role, new_team](mongocxx::client& client) {
        Services& s = services();
        const auto verdict = s.limiter.check_account(anvil::uuid::to_string(actor->user_id),
                                                     rate_rule("staff-write"));
        if (!verdict.allowed) {
            respond(http::rate_limited(req, verdict, rate_rule("staff-write")));
            return;
        }
        const auto account = s.accounts_repo.find_account(client, target);
        if (!account) {
            respond(http::failure(req, account.error()));
            return;
        }
        if (!account.value().has_value() || account.value()->user_type == UserType::Client) {
            respond(http::failure(req, ErrorCode::NotFound));
            return;
        }
        const anvil::identity::AccountRecord& current = *account.value();
        if (!may_manage(*actor, current)) {
            respond(http::failure(req, ErrorCode::Forbidden));
            return;
        }
        if (current.version != version) {
            respond(http::failure(req, ErrorCode::VersionMismatch));
            return;
        }

        const PermSet direct = granted.has_value() ? *granted : (current.direct_permissions & kGrantable);
        const UserType type = superadmin.has_value()
                                  ? (*superadmin ? UserType::SuperAdmin : UserType::Staff)
                                  : current.user_type;
        if (granted.has_value() || type != current.user_type) {
            // set_user_type moves the type and the mask together, refuses to
            // demote the last active superadmin, and bumps the permission epoch
            // so the change reaches every open session within one cache TTL.
            const auto changed = s.staff.set_user_type(client, target, version, type,
                                                       with_implied(direct), {},
                                                       anvil::identity::RoleTable{},
                                                       UserType::SuperAdmin);
            if (!changed) {
                respond(http::failure(req, changed.error()));
                return;
            }
        }
        if (active.has_value() && current.status != UserStatus::Active) {
            const auto enabled = s.staff.set_status(client, target, UserStatus::Active, UserType::SuperAdmin,
                                                    anvil::db::now_ms());
            if (!enabled) {
                respond(http::failure(req, enabled.error()));
                return;
            }
        }
        if (new_role.has_value() || new_team.has_value()) {
            const auto existing = StaffProfiles::find(client, target);
            StaffProfile profile = existing && existing.value().has_value() ? *existing.value()
                                                                           : StaffProfile{};
            if (new_role.has_value()) { profile.role = *new_role; }
            if (new_team.has_value()) { profile.team = *new_team; }
            if (const anvil::Status put = StaffProfiles::put(client, target, profile); !put) {
                respond(http::failure(req, put.error()));
                return;
            }
            // A team or role change changes what application review scopes to,
            // so it must not wait out a token either.
            (void)s.authz.bump_epoch(client, target);
        }
        http::audit(req, Action::StaffUpdated, target);
        respond(http::json(200, R"({"updated":true})"));
    });
}

void staff_disable(const http::HttpRequestPtr& req, http::Responder&& respond,
                   const std::string& id_text) {
    const std::shared_ptr<const anvil::UserContext> actor = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (actor == nullptr || !id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    if (*id == actor->user_id) {
        // Locking yourself out is never what was meant.
        respond(http::failure(req, ErrorCode::Conflict));
        return;
    }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, actor, target](mongocxx::client& client) {
        Services& s = services();
        const auto account = s.accounts_repo.find_account(client, target);
        if (!account) {
            respond(http::failure(req, account.error()));
            return;
        }
        if (!account.value().has_value() || account.value()->user_type == UserType::Client) {
            respond(http::failure(req, ErrorCode::NotFound));
            return;
        }
        if (!may_manage(*actor, *account.value())) {
            respond(http::failure(req, ErrorCode::Forbidden));
            return;
        }
        // Disabled rather than deleted: the audit trail keeps naming the
        // account, and set_status revokes its sessions and bumps its epoch.
        const auto changed = s.staff.set_status(client, target, UserStatus::Disabled,
                                                UserType::SuperAdmin, anvil::db::now_ms());
        if (!changed) {
            respond(http::failure(req, changed.error()));
            return;
        }
        http::audit(req, Action::StaffDisabled, target);
        respond(http::json(200, R"({"disabled":true})"));
    });
}

}  // namespace routes
}  // namespace enactus
