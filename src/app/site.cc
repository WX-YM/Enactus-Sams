#include "app/site.h"

#include <optional>
#include <string>
#include <utility>

#include "anvil/analytics/ingest.h"
#include "anvil/forms/service.h"
#include "anvil/forms/submission_service.h"
#include "anvil/core/locale.h"
#include "anvil/entries/registry.h"
#include "anvil/http/json_writer.h"
#include "anvil/sections/registry.h"
#include "app/applications.h"
#include "app/content.h"
#include "app/services.h"
#include "app/teams.h"
#include "entries.h"
#include "events.h"
#include "rate_limits.h"
#include "sections.h"

namespace enactus::routes {

namespace {

namespace ent = anvil::entries;
namespace sec = anvil::sections;
using anvil::ErrorCode;

[[nodiscard]] std::string text_of(const ent::EntryDocument& entry, std::string_view field) {
    const ent::EntryContent* copy = entry.working();
    if (copy == nullptr) { return {}; }
    const sec::SectionField* found = copy->content.find(field);
    return found != nullptr ? found->value.primary() : std::string{};
}

}  // namespace

// {"open":bool, "sections":{"home.hero":{…},…}, "teams":[…],
//  "gallery":{"about":[{"caption","image":{"id","src"}}],…}, "forms":[{"id","title"}]}
//
// Sections come from anvil's process cache as pre-serialised bytes. Every
// string in the payload is data: the page renders it with textContent.
void site_get(const http::HttpRequestPtr& req, http::Responder&& respond) {
    http::db_or_shed(req, respond, [req, respond](mongocxx::client& client) {
        const auto open = recruitment_open(client);
        if (!open) {
            respond(http::failure(req, open.error()));
            return;
        }
        std::string body{R"({"open":)"};
        body += open.value() ? "true" : "false";

        body += R"(,"sections":{)";
        bool first = true;
        for (const sec::SectionSpec& spec : kSections) {
            std::shared_ptr<const sec::SerializedSection> section =
                services().sections.peek(spec, anvil::Locale{});
            if (section == nullptr) {
                auto loaded = services().sections.load(client, spec, anvil::Locale{});
                if (!loaded) {
                    respond(http::failure(req, loaded.error()));
                    return;
                }
                section = std::move(loaded).value();
            }
            if (section == nullptr) { continue; }
            if (!first) { body += ','; }
            first = false;
            http::append_key(body, spec.key);
            body += section->json;
        }
        body += '}';

        const auto teams = list_teams(client);
        if (!teams) {
            respond(http::failure(req, teams.error()));
            return;
        }
        body += R"(,"teams":[)";
        first = true;
        for (const TeamSummary& team : teams.value()) {
            if (!team.recruiting && !team.showcase) { continue; }
            if (!first) { body += ','; }
            first = false;
            body += '{';
            http::append_string_field(body, "name", team.name, false);
            http::append_string_field(body, "desc", team.desc);
            body += R"(,"recruiting":)";
            body += team.recruiting ? "true" : "false";
            body += R"(,"showcase":)";
            body += team.showcase ? "true" : "false";
            body += '}';
        }
        body += ']';

        body += R"(,"gallery":{)";
        first = true;
        for (const std::string_view key : kGalleryKinds) {
            const ent::KindSpec* kind = ent::find_kind(kKinds, key);
            ent::EntryQuery query{};
            query.stage = ent::Stage::Published;
            query.limit = ent::kMaxPage;
            const auto listed = services().entries.list(client, *kind, query);
            if (!listed) {
                respond(http::failure(req, listed.error()));
                return;
            }
            if (!first) { body += ','; }
            first = false;
            http::append_key(body, key.substr(key.find('.') + 1));
            body += '[';
            bool first_photo = true;
            for (const ent::EntryDocument& photo : listed.value().entries) {
                const ent::EntryContent* copy = photo.working();
                if (copy == nullptr || copy->content.find_image("photo") == nullptr) { continue; }
                if (!first_photo) { body += ','; }
                first_photo = false;
                body += '{';
                http::append_string_field(body, "caption", text_of(photo, "caption"), false);
                body += ',';
                http::append_key(body, "image");
                append_image(body, copy->content, "photo");
                body += '}';
            }
            body += ']';
        }
        body += '}';

        // The forms the public may fill in right now, newest first, so the
        // /apply page can find one without knowing its id.
        const auto forms = services().forms.list(client, std::nullopt, anvil::forms::kMaxFormListLimit,
                                                 anvil::Locale{});
        if (!forms) {
            respond(http::failure(req, forms.error()));
            return;
        }
        body += R"(,"forms":[)";
        first = true;
        const anvil::db::TimeMs now = anvil::db::now_ms();
        for (const anvil::forms::FormListEntry& entry : forms.value().entries) {
            if (entry.status != anvil::forms::FormStatus::Active) { continue; }
            const auto form = services().forms.definition(client, entry.id, now);
            if (!form || !anvil::forms::check_form_open(*form.value(), now)) { continue; }
            if (!first) { body += ','; }
            first = false;
            body += '{';
            http::append_key(body, "id");
            anvil::http::append_json_uuid(body, entry.id);
            http::append_string_field(body, "title", entry.title);
            body += '}';
        }
        body += "]}";

        http::HttpResponsePtr response = http::json(200, std::move(body));
        // Public, identical for every visitor, and edited a few times a term:
        // a short shared cache absorbs a burst without hiding an edit for long.
        response->removeHeader("Cache-Control");
        response->addHeader("Cache-Control", "public, max-age=30");
        respond(response);
    });
}

// The page-view beacon. No body: everything recorded is derived server-side,
// and the visitor id is a keyed daily digest of the coarsened address (anvil
// analytics sessions), never the address.
void visits_record(const http::HttpRequestPtr& req, http::Responder&& respond) {
    if (!http::origin_ok(req, respond)) { return; }
    const auto ip = http::client_ip(req);
    http::db_or_shed(req, respond, [req, respond, ip](mongocxx::client&) {
        const auto verdict = services().limiter.check_ip(ip, rate_rule("visit"));
        if (!verdict.allowed) {
            respond(http::rate_limited(req, verdict, rate_rule("visit")));
            return;
        }
        anvil::analytics::Offer offer{};
        offer.code = kPageView;
        offer.dimensions = anvil::analytics::no_dimensions();
        offer.address = ip;
        offer.consented = false;
        (void)services().events.offer(offer);
        http::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k204NoContent);
        response->addHeader("Cache-Control", "no-store");
        respond(response);
    });
}

}  // namespace enactus::routes
