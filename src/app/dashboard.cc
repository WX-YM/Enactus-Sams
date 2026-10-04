#include "app/dashboard.h"

#include <array>
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anvil/analytics/query.h"
#include "anvil/audit/repository.h"
#include "anvil/entries/registry.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"
#include "anvil/identity/session_service.h"
#include "app/services.h"
#include "app/staff.h"
#include "app/teams.h"
#include "audit_actions.h"
#include "entries.h"
#include "events.h"

namespace enactus::routes {

namespace {

namespace ent = anvil::entries;
namespace sec = anvil::sections;
namespace input = anvil::input;
using anvil::ErrorCode;
using anvil::Uuid;

constexpr std::int32_t kAuditPage = 100;
constexpr int kVisitDays = 30;

[[nodiscard]] std::string text_of(const ent::EntryDocument& entry, std::string_view field) {
    const ent::EntryContent* copy = entry.working();
    if (copy == nullptr) { return {}; }
    const sec::SectionField* found = copy->content.find(field);
    return found != nullptr ? found->value.primary() : std::string{};
}

[[nodiscard]] std::optional<std::int64_t> parse_ms(const std::string& text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0) { return std::nullopt; }
    return value;
}

}  // namespace

// Visits over the last month (anvil analytics rollups), application counts by
// status and team, and the team roster sizes. A team manager's application
// figures cover their own team only.
void dashboard_get(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr) {
        respond(http::failure(req, ErrorCode::Unauthenticated));
        return;
    }
    http::db_or_shed(req, respond, [req, respond, ctx](mongocxx::client& client) {
        const auto scope = team_scope(client, *ctx);
        if (!scope) {
            respond(http::failure(req, scope.error()));
            return;
        }
        const anvil::db::TimeMs now = anvil::db::now_ms();
        const anvil::analytics::TimeRange range{now - std::chrono::days{kVisitDays}, now};
        const auto visits = services().analytics.counts_over_time(client, kPageView, range,
                                                                  anvil::analytics::Granularity::Day);
        if (!visits) {
            respond(http::failure(req, visits.error()));
            return;
        }
        std::string body{R"({"visits":[)"};
        std::int64_t visit_total = 0;
        for (std::size_t i = 0; i < visits.value().size(); ++i) {
            const anvil::analytics::Bucket& bucket = visits.value()[i];
            visit_total += bucket.count;
            if (i != 0) { body += ','; }
            body += '{';
            http::append_key(body, "at");
            anvil::http::append_json_time(body, bucket.at.time_since_epoch().count());
            body += ',';
            http::append_key(body, "count");
            anvil::http::append_json_int(body, bucket.count);
            body += ',';
            http::append_key(body, "sessions");
            anvil::http::append_json_int(body, bucket.sessions);
            body += '}';
        }
        body += "],";
        http::append_key(body, "visits_total");
        anvil::http::append_json_int(body, visit_total);

        // Applications: one bounded scan, folded here.
        const ent::KindSpec* kind = ent::find_kind(kKinds, kApplicationKind);
        std::map<std::string, std::int64_t> by_status;
        std::map<std::string, std::int64_t> by_team;
        std::int64_t applications = 0;
        ent::EntryQuery query{};
        query.stage = ent::Stage::Working;
        query.limit = ent::kMaxPage;
        for (int page = 0; page < 100; ++page) {
            const auto listed = services().entries.list(client, *kind, query);
            if (!listed) {
                respond(http::failure(req, listed.error()));
                return;
            }
            for (const ent::EntryDocument& application : listed.value().entries) {
                const std::string team = text_of(application, "team");
                const std::string referred = text_of(application, "referred_to");
                if (scope.value().has_value() && !same_team_name(team, *scope.value()) &&
                    !same_team_name(referred, *scope.value())) {
                    continue;
                }
                ++applications;
                ++by_status[text_of(application, "status")];
                ++by_team[team];
            }
            if (!listed.value().next.has_value()) { break; }
            query.after = listed.value().next;
        }
        body += ',';
        http::append_key(body, "applications_total");
        anvil::http::append_json_int(body, applications);
        const auto append_counts = [&body](std::string_view key, const std::map<std::string, std::int64_t>& counts) {
            body += ',';
            http::append_key(body, key);
            body += '{';
            bool first = true;
            for (const auto& [name, count] : counts) {
                if (!first) { body += ','; }
                first = false;
                http::append_key(body, name);
                anvil::http::append_json_int(body, count);
            }
            body += '}';
        };
        append_counts("applications_by_status", by_status);
        append_counts("applications_by_team", by_team);

        const auto teams = list_teams(client);
        if (!teams) {
            respond(http::failure(req, teams.error()));
            return;
        }
        body += R"(,"teams":[)";
        for (std::size_t i = 0; i < teams.value().size(); ++i) {
            if (i != 0) { body += ','; }
            body += '{';
            http::append_string_field(body, "name", teams.value()[i].name, false);
            body += ',';
            http::append_key(body, "members");
            anvil::http::append_json_int(body, teams.value()[i].members);
            body += '}';
        }
        body += "]}";
        respond(http::json(200, std::move(body)));
    });
}

// The audit log, newest first. Cursor ?after=<row id>&at=<epoch ms>; optional
// ?action=<name> from the audit action table. Addresses are coarsened to their
// network before they leave the server.
void audit_list(const http::HttpRequestPtr& req, http::Responder&& respond) {
    anvil::audit::AuditQuery query{};
    query.limit = kAuditPage;
    const std::string& after_text = req->getParameter("after");
    const std::string& at_text = req->getParameter("at");
    if (!after_text.empty() || !at_text.empty()) {
        const std::optional<Uuid> after = http::uuid_param(after_text);
        const std::optional<std::int64_t> at = parse_ms(at_text);
        if (!after.has_value() || !at.has_value()) {
            respond(http::invalid(req, "after", input::Reason::BadFormat));
            return;
        }
        query.after = anvil::audit::AuditCursor{*after, anvil::db::TimeMs{std::chrono::milliseconds{*at}}};
    }
    if (const std::string& action = req->getParameter("action"); !action.empty()) {
        const auto parsed = anvil::audit::audit_action_from_name(kAuditActions, action);
        if (!parsed.has_value()) {
            respond(http::invalid(req, "action", input::Reason::NotAllowed));
            return;
        }
        query.action = parsed;
    }
    http::db_or_shed(req, respond, [req, respond, query](mongocxx::client& client) {
        const anvil::audit::AuditRepository repository{services().database, kAuditCollection};
        const auto page = repository.listing(client, query);
        if (!page) {
            respond(http::failure(req, page.error()));
            return;
        }
        std::vector<Uuid> actors;
        for (const anvil::audit::AuditView& row : page.value().rows) {
            if (row.actor.has_value()) { actors.push_back(*row.actor); }
        }
        const auto names = services().accounts_repo.names_of(client, actors);
        const auto name_of = [&names](const Uuid& id) -> std::string_view {
            if (!names) { return {}; }
            for (const anvil::identity::AccountName& name : names.value()) {
                if (name.id == id) { return name.name; }
            }
            return {};
        };
        std::string body{R"({"rows":[)"};
        for (std::size_t i = 0; i < page.value().rows.size(); ++i) {
            const anvil::audit::AuditView& row = page.value().rows[i];
            if (i != 0) { body += ','; }
            body += '{';
            http::append_key(body, "id");
            anvil::http::append_json_uuid(body, row.id);
            body += ',';
            http::append_key(body, "at");
            anvil::http::append_json_time(body, row.at.time_since_epoch().count());
            http::append_string_field(body, "action", anvil::audit::audit_action_name(kAuditActions, row.action));
            http::append_string_field(body, "code", anvil::http::wire_name(row.code));
            body += R"(,"succeeded":)";
            body += row.succeeded ? "true" : "false";
            body += ',';
            http::append_key(body, "repeats");
            anvil::http::append_json_int(body, row.repeats);
            body += ',';
            http::append_key(body, "actor");
            if (row.actor.has_value()) {
                body += '{';
                http::append_key(body, "id");
                anvil::http::append_json_uuid(body, *row.actor);
                http::append_string_field(body, "name", name_of(*row.actor));
                body += '}';
            } else {
                body += "null";
            }
            body += ',';
            http::append_key(body, "subject");
            if (row.subject.has_value()) {
                anvil::http::append_json_uuid(body, *row.subject);
            } else {
                body += "null";
            }
            http::append_string_field(body, "network", anvil::identity::coarse_network_of(row.ip));
            body += '}';
        }
        body += "],";
        http::append_key(body, "next");
        if (page.value().next.has_value()) {
            body += '{';
            http::append_key(body, "after");
            anvil::http::append_json_uuid(body, page.value().next->id);
            body += ',';
            http::append_key(body, "at");
            anvil::http::append_json_int(body, page.value().next->at.time_since_epoch().count());
            body += '}';
        } else {
            body += "null";
        }
        body += '}';
        respond(http::json(200, std::move(body)));
    });
}

}  // namespace enactus::routes
