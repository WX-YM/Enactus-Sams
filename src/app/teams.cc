#include "app/teams.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"
#include "anvil/entries/payload.h"
#include "anvil/entries/registry.h"
#include "anvil/http/json_writer.h"
#include "anvil/input/schema.h"
#include "app/services.h"
#include "app/staff.h"
#include "entries.h"
#include "rate_limits.h"

namespace enactus {

namespace {

namespace ent = anvil::entries;
namespace sec = anvil::sections;
namespace input = anvil::input;
using anvil::ErrorCode;
using anvil::Uuid;

constexpr input::TextRules kNameRules{1, 80, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kDescRules{0, 600, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kPersonRules{1, 120, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kMemberRoleRules{1, 60, anvil::i18n::TextClass::Prose, false};

[[nodiscard]] const ent::KindSpec& team_kind() {
    static const ent::KindSpec* kind = ent::find_kind(kKinds, kTeamKind);
    return *kind;
}
[[nodiscard]] const ent::KindSpec& member_kind() {
    static const ent::KindSpec* kind = ent::find_kind(kKinds, kMemberKind);
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

[[nodiscard]] TeamSummary summary_of(const ent::EntryDocument& entry) {
    return TeamSummary{
        .id = entry.id,
        .name = text_of(entry, "name"),
        .desc = text_of(entry, "desc"),
        .version = entry.version,
        .members = entry.children,
        .recruiting = entry.has(ent::flag_bit(team_kind(), "recruiting")),
        .showcase = entry.has(ent::flag_bit(team_kind(), "showcase")),
    };
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) { text.remove_prefix(1); }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) { text.remove_suffix(1); }
    return text;
}

void append_team(std::string& out, const TeamSummary& team) {
    out += '{';
    http::append_key(out, "id");
    anvil::http::append_json_uuid(out, team.id);
    http::append_string_field(out, "name", team.name);
    http::append_string_field(out, "desc", team.desc);
    out += ',';
    http::append_key(out, "version");
    anvil::http::append_json_int(out, team.version);
    out += ',';
    http::append_key(out, "members");
    anvil::http::append_json_int(out, team.members);
    out += R"(,"recruiting":)";
    out += team.recruiting ? "true" : "false";
    out += R"(,"showcase":)";
    out += team.showcase ? "true" : "false";
    out += '}';
}

// A team manager may manage their own team and nothing else, and may not create,
// delete or reorder teams.
[[nodiscard]] anvil::Result<std::optional<std::string>> scope_of(mongocxx::client& client,
                                                                 const anvil::UserContext& ctx) {
    return team_scope(client, ctx);
}

[[nodiscard]] bool spend(const http::HttpRequestPtr& req, const http::Responder& respond,
                         const anvil::UserContext& ctx) {
    const auto verdict = services().limiter.check_account(anvil::uuid::to_string(ctx.user_id),
                                                          rate_rule("staff-write"));
    if (!verdict.allowed) {
        respond(http::rate_limited(req, verdict, rate_rule("staff-write")));
        return false;
    }
    return true;
}

// Loads the team and enforces a manager's scope. Answers and returns nullopt on
// any refusal.
[[nodiscard]] std::optional<ent::EntryDocument> load_team_in_scope(
    mongocxx::client& client, const http::HttpRequestPtr& req, const http::Responder& respond,
    const anvil::UserContext& ctx, const Uuid& id) {
    const auto found = services().entries.find(client, team_kind(), id, ent::Stage::Working);
    if (!found) {
        respond(http::failure(req, found.error()));
        return std::nullopt;
    }
    if (!found.value().has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return std::nullopt;
    }
    const auto scope = scope_of(client, ctx);
    if (!scope) {
        respond(http::failure(req, scope.error()));
        return std::nullopt;
    }
    if (scope.value().has_value() && !same_team_name(*scope.value(), text_of(*found.value(), "name"))) {
        respond(http::failure(req, ErrorCode::Forbidden));
        return std::nullopt;
    }
    return std::move(found).value();
}

// After a rename: staff profiles and applications name teams by their display
// name, so they follow it.
void propagate_rename(mongocxx::client& client, std::string_view from, std::string_view to,
                      const Uuid& actor) {
    (void)StaffProfiles::rename_team(client, from, to);
    const ent::KindSpec* applications = ent::find_kind(kKinds, kApplicationKind);
    std::optional<ent::EntryCursor> after;
    for (int page = 0; page < 2000; ++page) {
        ent::EntryQuery query{};
        query.after = after;
        query.limit = ent::kMaxPage;
        const auto listed = services().entries.list(client, *applications, query);
        if (!listed) { return; }
        for (const ent::EntryDocument& application : listed.value().entries) {
            ent::EntryEdit edit{};
            if (same_team_name(text_of(application, "team"), from)) {
                edit.patch.set("team", text_value(to));
            }
            if (same_team_name(text_of(application, "referred_to"), from)) {
                edit.patch.set("referred_to", text_value(to));
            }
            if (!edit.patch.fields.empty()) {
                (void)services().entries.write(client, *applications, application.id,
                                               application.version, edit, actor);
            }
        }
        if (!listed.value().next.has_value()) { return; }
        after = listed.value().next;
    }
}

}  // namespace

bool same_team_name(std::string_view a, std::string_view b) noexcept {
    a = trim(a);
    b = trim(b);
    if (a.empty() || a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string slugify(std::string_view name) {
    std::string out;
    bool hyphen = false;
    for (const char raw : name) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 && c < 0x80) {
            if (hyphen && !out.empty()) { out += '-'; }
            hyphen = false;
            out += static_cast<char>(std::tolower(c));
            if (out.size() >= 56) { break; }
        } else {
            hyphen = true;
        }
    }
    return out;
}

anvil::Result<std::vector<TeamSummary>> list_teams(mongocxx::client& client) {
    ent::EntryQuery query{};
    query.limit = ent::kMaxPage;
    query.stage = ent::Stage::Working;
    const auto page = services().entries.list(client, team_kind(), query);
    if (!page) { return page.error(); }
    std::vector<TeamSummary> teams;
    teams.reserve(page.value().entries.size());
    for (const ent::EntryDocument& entry : page.value().entries) { teams.push_back(summary_of(entry)); }
    return teams;
}

anvil::Result<std::optional<TeamSummary>> find_team_by_name(mongocxx::client& client,
                                                            std::string_view name) {
    const auto teams = list_teams(client);
    if (!teams) { return teams.error(); }
    for (const TeamSummary& team : teams.value()) {
        if (same_team_name(team.name, name)) { return std::optional<TeamSummary>{team}; }
    }
    return std::optional<TeamSummary>{};
}

anvil::Status add_member_if_absent(mongocxx::client& client, const Uuid& team,
                                   std::string_view person, const Uuid& actor) {
    ent::EntryQuery query{};
    query.parent = team;
    query.limit = ent::kMaxPage;
    query.stage = ent::Stage::Working;
    std::optional<ent::EntryCursor> after;
    for (int page = 0; page < 10; ++page) {
        query.after = after;
        const auto listed = services().entries.list(client, member_kind(), query);
        if (!listed) { return listed.error(); }
        for (const ent::EntryDocument& member : listed.value().entries) {
            if (same_team_name(text_of(member, "name"), person)) { return anvil::ok(); }
        }
        if (!listed.value().next.has_value()) { break; }
        after = listed.value().next;
    }
    ent::NewEntry entry{};
    entry.parent = team;
    entry.content.set("name", text_value(person));
    entry.content.set("role", text_value("Member"));
    const auto created = services().entries.create(client, member_kind(), entry, actor);
    if (!created) { return created.error(); }
    return anvil::ok();
}

namespace routes {

void teams_list(const http::HttpRequestPtr& req, http::Responder&& respond) {
    http::db_or_shed(req, respond, [req, respond](mongocxx::client& client) {
        const auto teams = list_teams(client);
        if (!teams) {
            respond(http::failure(req, teams.error()));
            return;
        }
        std::string body{R"({"teams":[)"};
        for (std::size_t i = 0; i < teams.value().size(); ++i) {
            if (i != 0) { body += ','; }
            append_team(body, teams.value()[i]);
        }
        body += "]}";
        http::HttpResponsePtr response = http::json(200, std::move(body));
        respond(response);
    });
}

void teams_create(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr || !http::origin_ok(req, respond)) { return; }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body.root()};
    std::string_view name;
    std::optional<std::string_view> desc;
    std::optional<bool> recruiting;
    std::optional<bool> showcase;
    input::Reason reason = bind.text("name", kNameRules, name);
    if (!input::is_ok(reason)) { respond(http::invalid(req, "name", reason)); return; }
    if (reason = bind.optional_text("desc", kDescRules, desc); !input::is_ok(reason)) {
        respond(http::invalid(req, "desc", reason));
        return;
    }
    if (reason = bind.optional_boolean("recruiting", recruiting); !input::is_ok(reason)) {
        respond(http::invalid(req, "recruiting", reason));
        return;
    }
    if (reason = bind.optional_boolean("showcase", showcase); !input::is_ok(reason)) {
        respond(http::invalid(req, "showcase", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    auto entry = std::make_shared<ent::NewEntry>();
    const std::string trimmed{trim(name)};
    entry->content.set("name", text_value(trimmed));
    entry->content.set("desc", text_value(desc.value_or("")));
    entry->slug = slugify(trimmed);
    if (entry->slug.empty()) {
        // A name in a script with no ASCII letters still gets a valid slug.
        const auto bytes = anvil::crypto::random_array<6>();
        static constexpr char kHex[] = "0123456789abcdef";
        entry->slug = "team-";
        for (const std::uint8_t b : bytes) {
            entry->slug += kHex[b >> 4];
            entry->slug += kHex[b & 0xF];
        }
    }
    if (recruiting.value_or(true)) { entry->flags |= ent::flag_bit(team_kind(), "recruiting"); }
    if (showcase.value_or(true)) { entry->flags |= ent::flag_bit(team_kind(), "showcase"); }

    http::db_or_shed(req, respond, [req, respond, ctx, entry, trimmed](mongocxx::client& client) {
        const auto scope = scope_of(client, *ctx);
        if (!scope || scope.value().has_value()) {
            respond(http::failure(req, scope ? ErrorCode::Forbidden : scope.error().code));
            return;
        }
        if (!spend(req, respond, *ctx)) { return; }
        const auto existing = find_team_by_name(client, trimmed);
        if (existing && existing.value().has_value()) {
            respond(http::failure(req, ErrorCode::Conflict));
            return;
        }
        const auto created = services().entries.create(client, team_kind(), *entry, ctx->user_id);
        if (!created) {
            respond(http::failure(req, created.error()));
            return;
        }
        http::audit(req, Action::TeamCreated, created.value().id);
        std::string out{"{"};
        http::append_key(out, "id");
        anvil::http::append_json_uuid(out, created.value().id);
        out += '}';
        respond(http::json(201, std::move(out)));
    });
}

void teams_update(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
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
    std::optional<std::string_view> name;
    std::optional<std::string_view> desc;
    std::optional<bool> recruiting;
    std::optional<bool> showcase;
    input::Reason reason = bind.integer("version", 1, INT64_MAX, version);
    if (!input::is_ok(reason)) { respond(http::invalid(req, "version", reason)); return; }
    if (reason = bind.optional_text("name", kNameRules, name); !input::is_ok(reason)) {
        respond(http::invalid(req, "name", reason));
        return;
    }
    if (reason = bind.optional_text("desc", kDescRules, desc); !input::is_ok(reason)) {
        respond(http::invalid(req, "desc", reason));
        return;
    }
    if (reason = bind.optional_boolean("recruiting", recruiting); !input::is_ok(reason)) {
        respond(http::invalid(req, "recruiting", reason));
        return;
    }
    if (reason = bind.optional_boolean("showcase", showcase); !input::is_ok(reason)) {
        respond(http::invalid(req, "showcase", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    auto edit = std::make_shared<ent::EntryEdit>();
    std::optional<std::string> new_name;
    if (name.has_value()) {
        new_name = std::string{trim(*name)};
        edit->patch.set("name", text_value(*new_name));
    }
    if (desc.has_value()) { edit->patch.set("desc", text_value(*desc)); }
    ent::FlagSet set = 0;
    ent::FlagSet clear = 0;
    const ent::FlagSet recruiting_bit = ent::flag_bit(team_kind(), "recruiting");
    const ent::FlagSet showcase_bit = ent::flag_bit(team_kind(), "showcase");
    if (recruiting.has_value()) { (*recruiting ? set : clear) |= recruiting_bit; }
    if (showcase.has_value()) { (*showcase ? set : clear) |= showcase_bit; }

    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, ctx, target, version, edit, new_name, set,
                                    clear](mongocxx::client& client) {
        const auto team = load_team_in_scope(client, req, respond, *ctx, target);
        if (!team.has_value()) { return; }
        if (!spend(req, respond, *ctx)) { return; }
        const std::string old_name = text_of(*team, "name");
        if (new_name.has_value() && !same_team_name(*new_name, old_name)) {
            const auto clash = find_team_by_name(client, *new_name);
            if (clash && clash.value().has_value()) {
                respond(http::failure(req, ErrorCode::Conflict));
                return;
            }
        }
        if (!edit->patch.fields.empty()) {
            const auto written = services().entries.write(client, team_kind(), target, version, *edit,
                                                          ctx->user_id);
            if (!written) {
                respond(http::failure(req, written.error()));
                return;
            }
        } else if (team->version != version) {
            respond(http::failure(req, ErrorCode::VersionMismatch));
            return;
        }
        if (set != 0 || clear != 0) {
            const anvil::Status flagged = services().entries.set_flags(client, team_kind(), target, set, clear);
            if (!flagged) {
                respond(http::failure(req, flagged.error()));
                return;
            }
        }
        if (new_name.has_value() && *new_name != old_name) {
            propagate_rename(client, old_name, *new_name, ctx->user_id);
        }
        http::audit(req, Action::TeamUpdated, target);
        respond(http::json(200, R"({"updated":true})"));
    });
}

void teams_delete(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (ctx == nullptr || !id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, ctx, target](mongocxx::client& client) {
        const auto scope = scope_of(client, *ctx);
        if (!scope || scope.value().has_value()) {
            respond(http::failure(req, scope ? ErrorCode::Forbidden : scope.error().code));
            return;
        }
        if (!spend(req, respond, *ctx)) { return; }
        // A team with members is not removable (anvil refuses it); its roster
        // goes first, one member at a time (anvil docs/20 §9: no cascade).
        ent::EntryQuery query{};
        query.parent = target;
        query.limit = ent::kMaxPage;
        query.stage = ent::Stage::Working;
        for (int pass = 0; pass < 20; ++pass) {
            const auto members = services().entries.list(client, member_kind(), query);
            if (!members) {
                respond(http::failure(req, members.error()));
                return;
            }
            if (members.value().entries.empty()) { break; }
            for (const ent::EntryDocument& member : members.value().entries) {
                (void)services().entries.remove(client, member_kind(), member.id, member.version);
            }
        }
        const auto team = services().entries.find(client, team_kind(), target, ent::Stage::Working);
        if (!team || !team.value().has_value()) {
            respond(http::failure(req, team ? ErrorCode::NotFound : team.error().code));
            return;
        }
        const anvil::Status removed =
            services().entries.remove(client, team_kind(), target, team.value()->version);
        if (!removed) {
            respond(http::failure(req, removed.error()));
            return;
        }
        http::audit(req, Action::TeamDeleted, target);
        respond(http::json(200, R"({"removed":true})"));
    });
}

void teams_reorder(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr || !http::origin_ok(req, respond)) { return; }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    auto order = std::make_shared<std::vector<Uuid>>();
    if (auto bad = ent::bind_order(body.root().find("order"), team_kind().capacity, *order); bad.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*bad, 1}));
        return;
    }
    http::db_or_shed(req, respond, [req, respond, ctx, order](mongocxx::client& client) {
        const auto scope = scope_of(client, *ctx);
        if (!scope || scope.value().has_value()) {
            respond(http::failure(req, scope ? ErrorCode::Forbidden : scope.error().code));
            return;
        }
        if (!spend(req, respond, *ctx)) { return; }
        const anvil::Status reordered = services().entries.reorder(client, team_kind(), std::nullopt, *order);
        if (!reordered) {
            respond(http::failure(req, reordered.error()));
            return;
        }
        http::audit(req, Action::TeamUpdated, std::nullopt);
        respond(http::json(200, R"({"reordered":true})"));
    });
}

void members_list(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (!id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    const Uuid team = *id;
    http::db_or_shed(req, respond, [req, respond, team](mongocxx::client& client) {
        ent::EntryQuery query{};
        query.parent = team;
        query.limit = ent::kMaxPage;
        query.stage = ent::Stage::Working;
        std::string body{R"({"members":[)"};
        bool first = true;
        std::optional<ent::EntryCursor> after;
        for (int page = 0; page < 10; ++page) {
            query.after = after;
            const auto listed = services().entries.list(client, member_kind(), query);
            if (!listed) {
                respond(http::failure(req, listed.error()));
                return;
            }
            for (const ent::EntryDocument& member : listed.value().entries) {
                if (!first) { body += ','; }
                first = false;
                body += '{';
                http::append_key(body, "id");
                anvil::http::append_json_uuid(body, member.id);
                http::append_string_field(body, "name", text_of(member, "name"));
                http::append_string_field(body, "role", text_of(member, "role"));
                body += ',';
                http::append_key(body, "version");
                anvil::http::append_json_int(body, member.version);
                body += '}';
            }
            if (!listed.value().next.has_value()) { break; }
            after = listed.value().next;
        }
        body += "]}";
        respond(http::json(200, std::move(body)));
    });
}

void members_add(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
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
    std::string_view name;
    std::string_view role;
    input::Reason reason = bind.text("name", kPersonRules, name);
    if (!input::is_ok(reason)) { respond(http::invalid(req, "name", reason)); return; }
    if (reason = bind.text("role", kMemberRoleRules, role); !input::is_ok(reason)) {
        respond(http::invalid(req, "role", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    auto entry = std::make_shared<ent::NewEntry>();
    entry->parent = *id;
    entry->content.set("name", text_value(trim(name)));
    entry->content.set("role", text_value(trim(role)));
    const Uuid team = *id;
    http::db_or_shed(req, respond, [req, respond, ctx, team, entry](mongocxx::client& client) {
        if (!load_team_in_scope(client, req, respond, *ctx, team).has_value()) { return; }
        if (!spend(req, respond, *ctx)) { return; }
        const auto created = services().entries.create(client, member_kind(), *entry, ctx->user_id);
        if (!created) {
            respond(http::failure(req, created.error()));
            return;
        }
        http::audit(req, Action::RosterUpdated, team);
        std::string out{"{"};
        http::append_key(out, "id");
        anvil::http::append_json_uuid(out, created.value().id);
        out += '}';
        respond(http::json(201, std::move(out)));
    });
}

void members_update(const http::HttpRequestPtr& req, http::Responder&& respond,
                    const std::string& id_text, const std::string& member_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    const std::optional<Uuid> member = http::uuid_param(member_text);
    if (ctx == nullptr || !id.has_value() || !member.has_value()) {
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
    std::optional<std::string_view> name;
    std::optional<std::string_view> role;
    input::Reason reason = bind.integer("version", 1, INT64_MAX, version);
    if (!input::is_ok(reason)) { respond(http::invalid(req, "version", reason)); return; }
    if (reason = bind.optional_text("name", kPersonRules, name); !input::is_ok(reason)) {
        respond(http::invalid(req, "name", reason));
        return;
    }
    if (reason = bind.optional_text("role", kMemberRoleRules, role); !input::is_ok(reason)) {
        respond(http::invalid(req, "role", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    auto edit = std::make_shared<ent::EntryEdit>();
    if (name.has_value()) { edit->patch.set("name", text_value(trim(*name))); }
    if (role.has_value()) { edit->patch.set("role", text_value(trim(*role))); }
    const Uuid team = *id;
    const Uuid target = *member;
    http::db_or_shed(req, respond, [req, respond, ctx, team, target, version, edit](mongocxx::client& client) {
        if (!load_team_in_scope(client, req, respond, *ctx, team).has_value()) { return; }
        if (!spend(req, respond, *ctx)) { return; }
        const auto found = services().entries.find(client, member_kind(), target, ent::Stage::Working);
        if (!found || !found.value().has_value() || found.value()->parent != std::optional<Uuid>{team}) {
            respond(http::failure(req, found ? ErrorCode::NotFound : found.error().code));
            return;
        }
        const auto written = services().entries.write(client, member_kind(), target, version, *edit, ctx->user_id);
        if (!written) {
            respond(http::failure(req, written.error()));
            return;
        }
        http::audit(req, Action::RosterUpdated, team);
        respond(http::json(200, R"({"updated":true})"));
    });
}

void members_remove(const http::HttpRequestPtr& req, http::Responder&& respond,
                    const std::string& id_text, const std::string& member_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    const std::optional<Uuid> member = http::uuid_param(member_text);
    if (ctx == nullptr || !id.has_value() || !member.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    const Uuid team = *id;
    const Uuid target = *member;
    http::db_or_shed(req, respond, [req, respond, ctx, team, target](mongocxx::client& client) {
        if (!load_team_in_scope(client, req, respond, *ctx, team).has_value()) { return; }
        if (!spend(req, respond, *ctx)) { return; }
        const auto found = services().entries.find(client, member_kind(), target, ent::Stage::Working);
        if (!found || !found.value().has_value() || found.value()->parent != std::optional<Uuid>{team}) {
            respond(http::failure(req, found ? ErrorCode::NotFound : found.error().code));
            return;
        }
        const anvil::Status removed =
            services().entries.remove(client, member_kind(), target, found.value()->version);
        if (!removed) {
            respond(http::failure(req, removed.error()));
            return;
        }
        http::audit(req, Action::RosterUpdated, team);
        respond(http::json(200, R"({"removed":true})"));
    });
}

}  // namespace routes
}  // namespace enactus
