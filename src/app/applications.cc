#include "app/applications.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "anvil/accesscontrol/decision.h"
#include "anvil/accounts/identifier.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"
#include "anvil/entries/registry.h"
#include "anvil/http/json_writer.h"
#include "anvil/identity/login_identity.h"
#include "anvil/input/schema.h"
#include "anvil/locale_egy/phone_egy.h"
#include "anvil/sections/registry.h"
#include "app/services.h"
#include "app/staff.h"
#include "app/teams.h"
#include "entries.h"
#include "rate_limits.h"
#include "sections.h"

namespace enactus {

namespace {

namespace ent = anvil::entries;
namespace sec = anvil::sections;
namespace input = anvil::input;
namespace ac = anvil::accesscontrol;
using anvil::ErrorCode;
using anvil::Uuid;

constexpr input::TextRules kNameRules{1, 80, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kEmailRules{3, 254, anvil::i18n::TextClass::Identifier, false};
constexpr input::TextRules kPhoneRules{1, 64, anvil::i18n::TextClass::Identifier, false};
constexpr input::TextRules kReasonRules{0, 3000, anvil::i18n::TextClass::Prose, true};
constexpr input::TextRules kTeamRules{1, 80, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kDecisionRules{0, 2000, anvil::i18n::TextClass::Prose, true};
constexpr input::TextRules kStatusRules{1, 24, anvil::i18n::TextClass::Identifier, false};

[[nodiscard]] const ent::KindSpec& application_kind() {
    static const ent::KindSpec* kind = ent::find_kind(kKinds, kApplicationKind);
    return *kind;
}

[[nodiscard]] std::string text_of(const ent::EntryDocument& entry, std::string_view field) {
    const ent::EntryContent* copy = entry.working();
    if (copy == nullptr) { return {}; }
    const sec::SectionField* found = copy->content.find(field);
    return found != nullptr ? found->value.primary() : std::string{};
}

[[nodiscard]] sec::SectionValue text_value(std::string_view text) {
    sec::SectionValue value{};
    value.primary() = std::string{text};
    return value;
}

[[nodiscard]] bool is_status(std::string_view status) noexcept {
    return sec::is_choice(kApplicationStatuses, status);
}

// A team manager sees an application filed for their team or referred to it.
[[nodiscard]] bool visible_to(const ent::EntryDocument& application,
                              const std::optional<std::string>& scope) {
    if (!scope.has_value()) { return true; }
    return same_team_name(text_of(application, "team"), *scope) ||
           same_team_name(text_of(application, "referred_to"), *scope);
}

void append_application(std::string& out, const ent::EntryDocument& application) {
    out += '{';
    http::append_key(out, "id");
    anvil::http::append_json_uuid(out, application.id);
    out += ',';
    http::append_key(out, "version");
    anvil::http::append_json_int(out, application.version);
    out += ',';
    http::append_key(out, "created_at");
    anvil::http::append_json_time(out, application.created_at.time_since_epoch().count());
    for (const sec::FieldSpec& field : application_kind().shape.fields) {
        http::append_string_field(out, field.key, text_of(application, field.key));
    }
    out += '}';
}

[[nodiscard]] bool spend_staff(const http::HttpRequestPtr& req, const http::Responder& respond,
                               const anvil::UserContext& ctx) {
    const auto verdict = services().limiter.check_account(anvil::uuid::to_string(ctx.user_id),
                                                          rate_rule("staff-write"));
    if (!verdict.allowed) {
        respond(http::rate_limited(req, verdict, rate_rule("staff-write")));
        return false;
    }
    return true;
}

struct Applicant final {
    std::string first_name;
    std::string last_name;
    std::string email;
    std::string phone;
    std::string team;
    std::string reason;
};

}  // namespace

std::string application_slug(std::string_view canonical_email) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest = anvil::crypto::sha256(canonical_email);
    std::string slug;
    slug.reserve(32);
    for (std::size_t i = 0; i < 16; ++i) {
        slug += kHex[digest[i] >> 4];
        slug += kHex[digest[i] & 0xF];
    }
    return slug;
}

anvil::Result<bool> recruitment_open(mongocxx::client& client) {
    const sec::SectionSpec* join = sec::find_section(kSections, "home.join");
    if (join == nullptr) { return false; }
    const auto document = services().sections.read_document(client, *join, sec::SectionState::Published);
    if (!document) { return document.error(); }
    if (!document.value().has_value()) { return false; }
    const sec::SectionField* open = document.value()->content.find("open");
    return open != nullptr && open->value.boolean;
}

namespace routes {

void applications_apply(const http::HttpRequestPtr& req, http::Responder&& respond) {
    if (!http::origin_ok(req, respond)) { return; }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body.root()};
    std::string_view first_name;
    std::string_view last_name;
    std::string_view email_raw;
    std::string_view phone_raw;
    std::string_view team;
    std::optional<std::string_view> reason_text;
    input::Reason reason = bind.text("first_name", kNameRules, first_name);
    if (!input::is_ok(reason)) { respond(http::invalid(req, "first_name", reason)); return; }
    if (reason = bind.text("last_name", kNameRules, last_name); !input::is_ok(reason)) {
        respond(http::invalid(req, "last_name", reason));
        return;
    }
    if (reason = bind.text("email", kEmailRules, email_raw); !input::is_ok(reason)) {
        respond(http::invalid(req, "email", reason));
        return;
    }
    if (reason = bind.text("phone", kPhoneRules, phone_raw); !input::is_ok(reason)) {
        respond(http::invalid(req, "phone", reason));
        return;
    }
    if (reason = bind.text("team", kTeamRules, team); !input::is_ok(reason)) {
        respond(http::invalid(req, "team", reason));
        return;
    }
    if (reason = bind.optional_text("reason", kReasonRules, reason_text); !input::is_ok(reason)) {
        respond(http::invalid(req, "reason", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    auto applicant = std::make_shared<Applicant>();
    if (reason = anvil::accounts::canonicalise(anvil::identity::LoginIdentity::Email, email_raw,
                                               applicant->email);
        !input::is_ok(reason)) {
        respond(http::invalid(req, "email", reason));
        return;
    }
    input::PhoneEgy phone{};
    if (reason = input::validate_phone_egy(phone_raw, phone); !input::is_ok(reason)) {
        respond(http::invalid(req, "phone", reason));
        return;
    }
    applicant->phone.assign(phone.e164.data(), phone.e164.size());
    applicant->first_name = std::string{first_name};
    applicant->last_name = std::string{last_name};
    applicant->team = std::string{team};
    applicant->reason = std::string{reason_text.value_or("")};
    const auto ip = http::client_ip(req);

    http::db_or_shed(req, respond, [req, respond, applicant, ip](mongocxx::client& client) {
        const auto verdict = services().limiter.check_ip(ip, rate_rule("apply"));
        if (!verdict.allowed) {
            respond(http::rate_limited(req, verdict, rate_rule("apply")));
            return;
        }
        const auto open = recruitment_open(client);
        if (!open) {
            respond(http::failure(req, open.error()));
            return;
        }
        if (!open.value()) {
            respond(http::failure(req, ErrorCode::Conflict));
            return;
        }
        const auto chosen = find_team_by_name(client, applicant->team);
        if (!chosen) {
            respond(http::failure(req, chosen.error()));
            return;
        }
        if (!chosen.value().has_value() || !chosen.value()->recruiting) {
            respond(http::invalid(req, "team", input::Reason::NotAllowed));
            return;
        }
        ent::NewEntry entry{};
        entry.slug = application_slug(applicant->email);
        entry.content.set("email", text_value(applicant->email));
        entry.content.set("first_name", text_value(applicant->first_name));
        entry.content.set("last_name", text_value(applicant->last_name));
        entry.content.set("phone", text_value(applicant->phone));
        entry.content.set("reason", text_value(applicant->reason));
        entry.content.set("status", text_value("pending"));
        // The registry's spelling of the team, not the applicant's.
        entry.content.set("team", text_value(chosen.value()->name));
        // The applicant has no account: the row is authored by the nil id.
        const auto created = services().entries.create(client, application_kind(), entry, Uuid{});
        if (created) {
            http::audit(req, Action::ApplicationReceived, created.value().id);
        } else if (created.error().code != ErrorCode::Conflict) {
            respond(http::failure(req, created.error()));
            return;
        }
        // A second application from the same address answers exactly as the
        // first did: whether an email has applied is not the public's to learn.
        respond(http::json(201, R"({"received":true})"));
    });
}

void applications_list(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr) {
        respond(http::failure(req, ErrorCode::Unauthenticated));
        return;
    }
    std::optional<Uuid> after;
    if (const std::string& cursor = req->getParameter("after"); !cursor.empty()) {
        after = http::uuid_param(cursor);
        if (!after.has_value()) { respond(http::invalid(req, "after", input::Reason::BadFormat)); return; }
    }
    std::optional<std::string> status;
    if (const std::string& wanted = req->getParameter("status"); !wanted.empty()) {
        if (!is_status(wanted)) { respond(http::invalid(req, "status", input::Reason::NotAllowed)); return; }
        status = wanted;
    }
    http::db_or_shed(req, respond, [req, respond, ctx, after, status](mongocxx::client& client) {
        const auto scope = team_scope(client, *ctx);
        if (!scope) {
            respond(http::failure(req, scope.error()));
            return;
        }
        constexpr std::size_t kPage = 100;
        std::string body{R"({"applications":[)"};
        std::size_t emitted = 0;
        std::optional<Uuid> next;
        ent::EntryQuery query{};
        query.stage = ent::Stage::Working;
        query.limit = ent::kMaxPage;
        if (after.has_value()) { query.after = ent::EntryCursor{*after}; }
        // Filters are applied here rather than in the query (anvil listings
        // filter by scope and flags only). The scan is bounded: the club takes a
        // few hundred applications a season.
        for (int page = 0; page < 50 && emitted < kPage; ++page) {
            const auto listed = services().entries.list(client, application_kind(), query);
            if (!listed) {
                respond(http::failure(req, listed.error()));
                return;
            }
            for (const ent::EntryDocument& application : listed.value().entries) {
                next = application.id;
                if (!visible_to(application, scope.value())) { continue; }
                if (status.has_value() && text_of(application, "status") != *status) { continue; }
                if (emitted != 0) { body += ','; }
                append_application(body, application);
                if (++emitted == kPage) { break; }
            }
            if (!listed.value().next.has_value()) {
                if (emitted < kPage) { next.reset(); }
                break;
            }
            query.after = listed.value().next;
        }
        body += "],";
        http::append_key(body, "next");
        if (next.has_value()) {
            anvil::http::append_json_uuid(body, *next);
        } else {
            body += "null";
        }
        body += '}';
        respond(http::json(200, std::move(body)));
    });
}

void applications_update(const http::HttpRequestPtr& req, http::Responder&& respond,
                         const std::string& id_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (ctx == nullptr || !id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body.root()};
    std::int64_t version = 0;
    std::optional<std::string_view> status;
    std::optional<std::string_view> decision;
    std::optional<std::optional<std::string_view>> referred_to;
    std::optional<std::string_view> team;
    input::Reason reason = bind.integer("version", 1, INT64_MAX, version);
    if (!input::is_ok(reason)) { respond(http::invalid(req, "version", reason)); return; }
    if (reason = bind.optional_text("status", kStatusRules, status); !input::is_ok(reason)) {
        respond(http::invalid(req, "status", reason));
        return;
    }
    if (status.has_value() && !is_status(*status)) {
        respond(http::invalid(req, "status", input::Reason::NotAllowed));
        return;
    }
    if (reason = bind.optional_text("decision", kDecisionRules, decision); !input::is_ok(reason)) {
        respond(http::invalid(req, "decision", reason));
        return;
    }
    if (reason = bind.optional_nullable_text("referred_to", kTeamRules, referred_to); !input::is_ok(reason)) {
        respond(http::invalid(req, "referred_to", reason));
        return;
    }
    if (reason = bind.optional_text("team", kTeamRules, team); !input::is_ok(reason)) {
        respond(http::invalid(req, "team", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    struct Change final {
        std::optional<std::string>                status;
        std::optional<std::string>                decision;
        std::optional<std::optional<std::string>> referred_to;
        std::optional<std::string>                team;
    };
    auto change = std::make_shared<Change>();
    if (status.has_value()) { change->status = std::string{*status}; }
    if (decision.has_value()) { change->decision = std::string{*decision}; }
    if (referred_to.has_value()) {
        change->referred_to = referred_to->has_value() ? std::optional<std::string>{std::string{**referred_to}}
                                                       : std::optional<std::string>{};
    }
    if (team.has_value()) { change->team = std::string{*team}; }
    const Uuid target = *id;

    http::db_or_shed(req, respond, [req, respond, ctx, target, version, change](mongocxx::client& client) {
        const auto scope = team_scope(client, *ctx);
        if (!scope) {
            respond(http::failure(req, scope.error()));
            return;
        }
        const auto found = services().entries.find(client, application_kind(), target, ent::Stage::Working);
        if (!found) {
            respond(http::failure(req, found.error()));
            return;
        }
        // Out of a manager's scope reads exactly as absent.
        if (!found.value().has_value() || !visible_to(*found.value(), scope.value())) {
            respond(http::failure(req, ErrorCode::NotFound));
            return;
        }
        // Moving an application between teams is a board decision; a manager
        // refers it instead.
        if (scope.value().has_value() && change->team.has_value()) {
            respond(http::failure(req, ErrorCode::Forbidden));
            return;
        }
        if (!spend_staff(req, respond, *ctx)) { return; }
        const ent::EntryDocument& before = *found.value();

        ent::EntryEdit edit{};
        if (change->team.has_value()) {
            const auto named = find_team_by_name(client, *change->team);
            if (!named || !named.value().has_value()) {
                respond(named ? http::invalid(req, "team", input::Reason::NotAllowed)
                              : http::failure(req, named.error()));
                return;
            }
            edit.patch.set("team", text_value(named.value()->name));
        }
        std::string referral = text_of(before, "referred_to");
        if (change->referred_to.has_value()) {
            if (change->referred_to->has_value()) {
                const auto named = find_team_by_name(client, **change->referred_to);
                if (!named || !named.value().has_value()) {
                    respond(named ? http::invalid(req, "referred_to", input::Reason::NotAllowed)
                                  : http::failure(req, named.error()));
                    return;
                }
                referral = named.value()->name;
            } else {
                referral.clear();
            }
            edit.patch.set("referred_to", text_value(referral));
        }
        const std::string next_status = change->status.value_or(text_of(before, "status"));
        if (next_status == "referred" && referral.empty()) {
            respond(http::invalid(req, "referred_to", input::Reason::Required));
            return;
        }
        if (change->status.has_value()) { edit.patch.set("status", text_value(*change->status)); }
        if (change->decision.has_value()) { edit.patch.set("decision", text_value(*change->decision)); }
        if (edit.patch.fields.empty()) {
            respond(http::failure(req, ErrorCode::ValidationFailed));
            return;
        }
        const auto written =
            services().entries.write(client, application_kind(), target, version, edit, ctx->user_id);
        if (!written) {
            respond(http::failure(req, written.error()));
            return;
        }
        // Acceptance puts the person on the roster of the team that accepted
        // them: the referral when there is one, otherwise the team applied to.
        if (next_status == "accepted" && text_of(before, "status") != "accepted") {
            const std::string joining =
                !referral.empty() ? referral
                                  : (change->team.has_value() ? *change->team : text_of(before, "team"));
            const auto roster = find_team_by_name(client, joining);
            if (roster && roster.value().has_value()) {
                const std::string person = text_of(before, "first_name") + " " + text_of(before, "last_name");
                (void)add_member_if_absent(client, roster.value()->id, person, ctx->user_id);
            }
        }
        http::audit(req, Action::ApplicationReviewed, target);
        std::string out{"{"};
        http::append_key(out, "version");
        anvil::http::append_json_int(out, written.value().version);
        out += '}';
        respond(http::json(200, std::move(out)));
    });
}

void applications_delete(const http::HttpRequestPtr& req, http::Responder&& respond,
                         const std::string& id_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (ctx == nullptr || !id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    // Deleting applicant data is irreversible and is kept to the superadmin.
    if (!ac::is_superadmin(ctx->user_type)) {
        respond(http::failure(req, ErrorCode::Forbidden));
        return;
    }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, ctx, target](mongocxx::client& client) {
        if (!spend_staff(req, respond, *ctx)) { return; }
        const auto found = services().entries.find(client, application_kind(), target, ent::Stage::Working);
        if (!found || !found.value().has_value()) {
            respond(http::failure(req, found ? ErrorCode::NotFound : found.error().code));
            return;
        }
        const anvil::Status removed =
            services().entries.remove(client, application_kind(), target, found.value()->version);
        if (!removed) {
            respond(http::failure(req, removed.error()));
            return;
        }
        http::audit(req, Action::ApplicationDeleted, target);
        respond(http::json(200, R"({"removed":true})"));
    });
}

}  // namespace routes
}  // namespace enactus
