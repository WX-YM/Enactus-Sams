#include "app/forms.h"

#include <array>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "anvil/accesscontrol/decision.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"
#include "anvil/forms/export.h"
#include "anvil/forms/repository.h"
#include "anvil/http/json_writer.h"
#include "anvil/input/schema.h"
#include "app/services.h"
#include "field_types.h"
#include "perms.h"
#include "rate_limits.h"

namespace enactus {

namespace {

namespace fm = anvil::forms;
namespace input = anvil::input;
namespace ac = anvil::accesscontrol;
namespace codec = anvil::db::codec;
using anvil::ErrorCode;
using anvil::Uuid;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

constexpr input::TextRules kTitleRules{1, fm::kMaxLabelCodePoints, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kLabelRules{1, fm::kMaxLabelCodePoints, anvil::i18n::TextClass::Prose, false};
constexpr input::TextRules kWireRules{1, 32, anvil::i18n::TextClass::Identifier, false};
constexpr input::TextRules kOptionValueRules{1, fm::kMaxOptionValueLength, anvil::i18n::TextClass::Identifier, false};

// What a CSV export may grow to in memory. The club's forms collect hundreds of
// rows, so this is far above any real export, and it keeps one request from
// holding an unbounded buffer.
constexpr std::size_t kMaxExportBytes = 16U * 1024U * 1024U;

constexpr std::array<std::pair<std::string_view, fm::FormStatus>, 3> kStatusNames{{
    {"active", fm::FormStatus::Active},
    {"closed", fm::FormStatus::Closed},
    {"draft", fm::FormStatus::Draft},
}};

[[nodiscard]] std::string_view status_name(fm::FormStatus status) noexcept {
    for (const auto& [name, value] : kStatusNames) {
        if (value == status) { return name; }
    }
    return "draft";
}

[[nodiscard]] std::optional<fm::FormStatus> status_of(std::string_view name) noexcept {
    for (const auto& [text, value] : kStatusNames) {
        if (text == name) { return value; }
    }
    return std::nullopt;
}

[[nodiscard]] fm::LocalizedText localized(std::string_view text) {
    fm::LocalizedText out{};
    out[anvil::config::kDefaultLocale] = std::string{text};
    return out;
}

[[nodiscard]] const std::string& primary(const fm::LocalizedText& text) noexcept {
    return text[anvil::config::kDefaultLocale];
}

[[nodiscard]] bool may_build(const anvil::UserContext* ctx) noexcept {
    return ctx != nullptr &&
           ac::satisfies(ctx->permissions, ctx->user_type, anvil::perm_mask(Perm::FormMaker));
}

// Binds a create or edit body. The error names a fixed field, never a key or a
// value from the request.
[[nodiscard]] std::optional<input::FieldError> bind_schema(input::ObjectBinder& bind, fm::FormSchema& out) {
    const auto failed = [](std::string_view field, input::Reason reason) {
        return std::optional<input::FieldError>{input::FieldError{field, reason}};
    };
    std::string_view title;
    if (const auto r = bind.text("title", kTitleRules, title); !input::is_ok(r)) { return failed("title", r); }
    out.title = localized(title);

    std::string_view status_text;
    if (const auto r = bind.text("status", kWireRules, status_text); !input::is_ok(r)) { return failed("status", r); }
    const auto status = status_of(status_text);
    if (!status.has_value()) { return failed("status", input::Reason::NotAllowed); }
    out.status = *status;

    std::optional<std::optional<std::int64_t>> closes_at;
    if (const auto r = bind.optional_nullable_timestamp("closes_at", closes_at); !input::is_ok(r)) {
        return failed("closes_at", r);
    }
    if (closes_at.has_value() && closes_at->has_value()) {
        out.closes_at = anvil::db::TimeMs{std::chrono::milliseconds{**closes_at}};
    }
    std::optional<std::int64_t> max_submissions;
    if (const auto r = bind.optional_integer("max_submissions", 0, 1'000'000, max_submissions); !input::is_ok(r)) {
        return failed("max_submissions", r);
    }
    out.max_submissions = max_submissions.value_or(0);
    // Public forms are anonymous, so there is no person to hold to one response.
    out.one_per_user = false;

    input::Reason reason = input::Reason::Ok;
    const input::JsonValue* fields = bind.array("fields", fm::kMaxFormFields, reason);
    if (!input::is_ok(reason) || fields == nullptr) { return failed("fields", reason); }
    out.fields.clear();
    out.fields.reserve(fields->elements().size());
    for (const input::JsonValue& element : fields->elements()) {
        input::ObjectBinder field_bind{element};
        if (!field_bind.is_object()) { return failed("fields", input::Reason::BadFormat); }
        fm::FieldSpec field{};
        std::string_view fid_text;
        if (const auto r = field_bind.text("fid", kWireRules, fid_text); !input::is_ok(r)) { return failed("fid", r); }
        const auto fid = fm::Fid::parse(fid_text);
        if (!fid.has_value()) { return failed("fid", input::Reason::BadFormat); }
        field.fid = *fid;
        std::string_view label;
        if (const auto r = field_bind.text("label", kLabelRules, label); !input::is_ok(r)) { return failed("label", r); }
        field.label = localized(label);
        std::string_view type_name;
        if (const auto r = field_bind.text("type", kWireRules, type_name); !input::is_ok(r)) { return failed("type", r); }
        const fm::FieldTypeSpec* type = fm::field_type_by_name(kFieldTypes, type_name);
        if (type == nullptr) { return failed("type", input::Reason::NotAllowed); }
        field.type = type->code;
        std::optional<bool> optional;
        if (const auto r = field_bind.optional_boolean("optional", optional); !input::is_ok(r)) {
            return failed("optional", r);
        }
        field.optional = optional.value_or(false);
        std::optional<std::int64_t> max_cp;
        if (const auto r = field_bind.optional_integer("max_cp", 0, fm::kMaxFieldCodePoints, max_cp); !input::is_ok(r)) {
            return failed("max_cp", r);
        }
        field.max_code_points = static_cast<std::uint32_t>(max_cp.value_or(0));
        const input::JsonValue* options = field_bind.optional_array("options", fm::kMaxFieldOptions, reason);
        if (!input::is_ok(reason)) { return failed("options", reason); }
        if (options != nullptr) {
            for (const input::JsonValue& option_value : options->elements()) {
                input::ObjectBinder option_bind{option_value};
                if (!option_bind.is_object()) { return failed("options", input::Reason::BadFormat); }
                std::string_view value;
                std::string_view option_label;
                if (const auto r = option_bind.text("value", kOptionValueRules, value); !input::is_ok(r)) {
                    return failed("options", r);
                }
                if (const auto r = option_bind.text("label", kLabelRules, option_label); !input::is_ok(r)) {
                    return failed("options", r);
                }
                if (option_bind.finish().has_value()) { return failed("options", input::Reason::NotAllowed); }
                field.options.push_back(fm::FormOption{std::string{value}, localized(option_label)});
            }
        }
        if (field_bind.finish().has_value()) { return failed("fields", input::Reason::NotAllowed); }
        out.fields.push_back(std::move(field));
    }
    return std::nullopt;
}

void append_definition(std::string& out, const fm::FormDefinition& form, bool staff) {
    out += '{';
    http::append_key(out, "id");
    anvil::http::append_json_uuid(out, form.id);
    out += ',';
    http::append_key(out, "version");
    anvil::http::append_json_int(out, form.version);
    http::append_string_field(out, "title", primary(form.title));
    http::append_string_field(out, "status", status_name(form.status));
    out += ',';
    http::append_key(out, "closes_at");
    if (form.closes_at.has_value()) {
        anvil::http::append_json_time(out, form.closes_at->time_since_epoch().count());
    } else {
        out += "null";
    }
    if (staff) {
        out += ',';
        http::append_key(out, "max_submissions");
        anvil::http::append_json_int(out, form.max_submissions);
        out += ',';
        http::append_key(out, "submission_count");
        anvil::http::append_json_int(out, form.submission_count);
    }
    out += R"(,"fields":[)";
    for (std::size_t i = 0; i < form.fields.size(); ++i) {
        const fm::FieldSpec& field = form.fields[i];
        if (i != 0) { out += ','; }
        out += '{';
        http::append_string_field(out, "fid", field.fid.view(), false);
        http::append_string_field(out, "label", primary(field.label));
        http::append_string_field(out, "type", fm::field_type_name(kFieldTypes, field.type));
        out += R"(,"optional":)";
        out += field.optional ? "true" : "false";
        out += ',';
        http::append_key(out, "max_cp");
        anvil::http::append_json_int(out, field.max_code_points);
        out += R"(,"options":[)";
        for (std::size_t j = 0; j < field.options.size(); ++j) {
            if (j != 0) { out += ','; }
            out += '{';
            http::append_string_field(out, "value", field.options[j].value, false);
            http::append_string_field(out, "label", primary(field.options[j].label));
            out += '}';
        }
        out += "]}";
    }
    out += "]}";
}

void append_answer(std::string& out, const fm::Answer& answer) {
    switch (answer.kind) {
        case fm::AnswerKind::Text:
        case fm::AnswerKind::Choice:
            anvil::http::append_json_string(out, answer.text);
            return;
        case fm::AnswerKind::Choices:
            out += '[';
            for (std::size_t i = 0; i < answer.choices.size(); ++i) {
                if (i != 0) { out += ','; }
                anvil::http::append_json_string(out, answer.choices[i]);
            }
            out += ']';
            return;
        case fm::AnswerKind::Number:
            anvil::http::append_json_int(out, answer.number);
            return;
        case fm::AnswerKind::Media:
            anvil::http::append_json_uuid(out, answer.media);
            return;
    }
    out += "null";
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

// A fresh read for staff paths: the submission cache may be up to a minute old,
// and an editor must start from the stored version.
[[nodiscard]] anvil::Result<std::optional<fm::FormDefinition>> stored(mongocxx::client& client, const Uuid& id) {
    return services().forms.repository().find_definition(client, id);
}

void write_id(std::string& out, const Uuid& id) {
    out += '{';
    http::append_key(out, "id");
    anvil::http::append_json_uuid(out, id);
    out += '}';
}

}  // namespace

namespace routes {

void forms_list(const http::HttpRequestPtr& req, http::Responder&& respond) {
    std::optional<Uuid> after;
    if (const std::string& cursor = req->getParameter("after"); !cursor.empty()) {
        after = http::uuid_param(cursor);
        if (!after.has_value()) { respond(http::invalid(req, "after", input::Reason::BadFormat)); return; }
    }
    http::db_or_shed(req, respond, [req, respond, after](mongocxx::client& client) {
        const auto page = services().forms.list(client, after, fm::kMaxFormListLimit, anvil::Locale{});
        if (!page) {
            respond(http::failure(req, page.error()));
            return;
        }
        std::string body{R"({"forms":[)"};
        for (std::size_t i = 0; i < page.value().entries.size(); ++i) {
            const fm::FormListEntry& form = page.value().entries[i];
            if (i != 0) { body += ','; }
            body += '{';
            http::append_key(body, "id");
            anvil::http::append_json_uuid(body, form.id);
            http::append_string_field(body, "title", form.title);
            http::append_string_field(body, "status", status_name(form.status));
            body += ',';
            http::append_key(body, "version");
            anvil::http::append_json_int(body, form.version);
            body += ',';
            http::append_key(body, "submission_count");
            anvil::http::append_json_int(body, form.submission_count);
            body += ',';
            http::append_key(body, "created_at");
            anvil::http::append_json_time(body, form.created_at.time_since_epoch().count());
            body += '}';
        }
        body += "],";
        http::append_key(body, "next");
        if (page.value().next.has_value()) {
            anvil::http::append_json_uuid(body, *page.value().next);
        } else {
            body += "null";
        }
        body += '}';
        respond(http::json(200, std::move(body)));
    });
}

void forms_create(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr || !http::origin_ok(req, respond)) { return; }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body.root()};
    auto schema = std::make_shared<fm::FormSchema>();
    if (auto bad = bind_schema(bind, *schema); bad.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*bad, 1}));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    if (const anvil::Status valid = fm::validate_schema(kFieldTypes, *schema); !valid) {
        respond(http::failure(req, valid.error()));
        return;
    }
    http::db_or_shed(req, respond, [req, respond, ctx, schema](mongocxx::client& client) {
        if (!spend_staff(req, respond, *ctx)) { return; }
        const auto created = services().forms.create(client, *schema, ctx->user_id);
        if (!created) {
            respond(http::failure(req, created.error()));
            return;
        }
        http::audit(req, Action::FormCreated, created.value());
        std::string out;
        write_id(out, created.value());
        respond(http::json(201, std::move(out)));
    });
}

void forms_update(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
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
    if (const auto r = bind.integer("version", 1, INT64_MAX, version); !input::is_ok(r)) {
        respond(http::invalid(req, "version", r));
        return;
    }
    auto schema = std::make_shared<fm::FormSchema>();
    if (auto bad = bind_schema(bind, *schema); bad.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*bad, 1}));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    if (const anvil::Status valid = fm::validate_schema(kFieldTypes, *schema); !valid) {
        respond(http::failure(req, valid.error()));
        return;
    }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, ctx, target, version, schema](mongocxx::client& client) {
        if (!spend_staff(req, respond, *ctx)) { return; }
        const auto edited = services().forms.edit(client, target, version, *schema);
        if (!edited) {
            respond(http::failure(req, edited.error()));
            return;
        }
        services().forms.invalidate(target);
        http::audit(req, Action::FormUpdated, target);
        std::string out{"{"};
        http::append_key(out, "version");
        anvil::http::append_json_int(out, edited.value());
        out += '}';
        respond(http::json(200, std::move(out)));
    });
}

void forms_delete(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (ctx == nullptr || !id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, ctx, target](mongocxx::client& client) {
        if (!spend_staff(req, respond, *ctx)) { return; }
        const auto dropped = services().forms.drop(client, target);
        if (!dropped) {
            respond(http::failure(req, dropped.error()));
            return;
        }
        services().forms.invalidate(target);
        http::audit(req, Action::FormDeleted, target);
        std::string out{"{"};
        http::append_key(out, "responses_removed");
        anvil::http::append_json_int(out, dropped.value().submissions_destroyed);
        out += '}';
        respond(http::json(200, std::move(out)));
    });
}

// The public read. A draft, closed or expired form is NotFound to the public;
// a form builder (signed in, FormMaker) sees every state so the editor and the
// preview read the same route.
void forms_public(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (!id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const bool staff = may_build(ctx.get());
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, target, staff](mongocxx::client& client) {
        const anvil::db::TimeMs now = anvil::db::now_ms();
        if (staff) {
            const auto found = stored(client, target);
            if (!found || !found.value().has_value()) {
                respond(http::failure(req, found ? ErrorCode::NotFound : found.error().code));
                return;
            }
            std::string body;
            append_definition(body, *found.value(), true);
            respond(http::json(200, std::move(body)));
            return;
        }
        const auto form = services().forms.definition(client, target, now);
        if (!form) {
            respond(http::failure(req, form.error()));
            return;
        }
        if (const anvil::Status open = fm::check_form_open(*form.value(), now); !open) {
            // Closed or full reads as absent too: the page has nothing to show.
            respond(http::failure(req, ErrorCode::NotFound));
            return;
        }
        std::string body;
        append_definition(body, *form.value(), false);
        respond(http::json(200, std::move(body)));
    });
}

// {"answers":{"f1":"text","f2":"option_value",…}}
void forms_submit(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (!id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    if (req->body().size() > fm::kMaxSubmissionBytes) {
        respond(http::failure(req, ErrorCode::PayloadTooLarge));
        return;
    }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body.root()};
    const input::JsonValue* answers = bind.object("answers");
    if (answers == nullptr || !answers->is_object()) {
        respond(http::invalid(req, "answers", input::Reason::Required));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    if (answers->members().size() > fm::kMaxFormFields) {
        respond(http::invalid(req, "answers", input::Reason::TooLong));
        return;
    }
    auto submission = std::make_shared<fm::SubmissionInput>();
    submission->ip = http::client_ip(req);
    submission->answers.reserve(answers->members().size());
    for (const input::JsonMember& member : answers->members()) {
        const auto fid = fm::Fid::parse(member.key);
        if (!fid.has_value()) {
            respond(http::invalid(req, "answers", input::Reason::NotAllowed));
            return;
        }
        fm::RawAnswer raw{};
        raw.fid = *fid;
        raw.shape = member.value.type();
        if (const auto text = member.value.as_string(); text.has_value()) {
            raw.text = std::string{*text};
        } else if (member.value.is_array()) {
            if (member.value.elements().size() > fm::kMaxFieldOptions) {
                respond(http::invalid(req, "answers", input::Reason::TooLong));
                return;
            }
            for (const input::JsonValue& choice : member.value.elements()) {
                const auto text_choice = choice.as_string();
                if (!text_choice.has_value()) {
                    respond(http::invalid(req, "answers", input::Reason::BadFormat));
                    return;
                }
                raw.choices.emplace_back(*text_choice);
            }
        } else if (const auto number = member.value.as_int64(); number.has_value()) {
            raw.number = *number;
        }
        submission->answers.push_back(std::move(raw));
    }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, target, submission](mongocxx::client& client) {
        const auto verdict = services().limiter.check_ip(submission->ip, rate_rule("submit"));
        if (!verdict.allowed) {
            respond(http::rate_limited(req, verdict, rate_rule("submit")));
            return;
        }
        const anvil::db::TimeMs now = anvil::db::now_ms();
        const auto form = services().forms.definition(client, target, now);
        if (!form) {
            respond(http::failure(req, form.error()));
            return;
        }
        const auto accepted = services().submissions.submit(client, *form.value(), *submission, now);
        if (!accepted) {
            respond(http::failure(req, accepted.error()));
            return;
        }
        respond(http::json(201, R"({"received":true})"));
    });
}

// Cursor: ?after=<response id>&at=<epoch ms>, both from the previous page.
void responses_list(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (!id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    std::optional<fm::SubmissionCursor> after;
    const std::string& after_text = req->getParameter("after");
    const std::string& at_text = req->getParameter("at");
    if (!after_text.empty() || !at_text.empty()) {
        const std::optional<Uuid> after_id = http::uuid_param(after_text);
        std::int64_t at_ms = 0;
        const auto parsed = std::from_chars(at_text.data(), at_text.data() + at_text.size(), at_ms);
        if (!after_id.has_value() || parsed.ec != std::errc{} || parsed.ptr != at_text.data() + at_text.size() ||
            at_ms < 0) {
            respond(http::invalid(req, "after", input::Reason::BadFormat));
            return;
        }
        after = fm::SubmissionCursor{*after_id, anvil::db::TimeMs{std::chrono::milliseconds{at_ms}}};
    }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, target, after](mongocxx::client& client) {
        const auto form = stored(client, target);
        if (!form || !form.value().has_value()) {
            respond(http::failure(req, form ? ErrorCode::NotFound : form.error().code));
            return;
        }
        const auto page = services().forms.repository().page_of(client, *form.value(), after,
                                                                fm::kMaxSubmissionPageSize);
        if (!page) {
            respond(http::failure(req, page.error()));
            return;
        }
        std::string body{R"({"form":)"};
        append_definition(body, *form.value(), true);
        body += R"(,"responses":[)";
        for (std::size_t i = 0; i < page.value().entries.size(); ++i) {
            const fm::SubmissionRecord& record = page.value().entries[i];
            if (i != 0) { body += ','; }
            body += '{';
            http::append_key(body, "id");
            anvil::http::append_json_uuid(body, record.id);
            body += ',';
            http::append_key(body, "submitted_at");
            anvil::http::append_json_time(body, record.submitted_at.time_since_epoch().count());
            body += ',';
            http::append_key(body, "form_version");
            anvil::http::append_json_int(body, record.form_version);
            body += R"(,"answers":{)";
            for (std::size_t j = 0; j < record.answers.size(); ++j) {
                if (j != 0) { body += ','; }
                http::append_key(body, record.answers[j].fid.view());
                append_answer(body, record.answers[j]);
            }
            body += "}}";
        }
        body += "],";
        http::append_key(body, "next");
        if (page.value().next.has_value()) {
            body += '{';
            http::append_key(body, "after");
            anvil::http::append_json_uuid(body, page.value().next->id);
            body += ',';
            http::append_key(body, "at");
            anvil::http::append_json_int(body, page.value().next->submitted_at.time_since_epoch().count());
            body += '}';
        } else {
            body += "null";
        }
        body += '}';
        respond(http::json(200, std::move(body)));
    });
}

// Removes one response. The form's submission_count is anvil's monotonic
// counter and is left alone: the edit-compatibility rules stay as strict as
// they were, which is the safe direction.
void responses_delete(const http::HttpRequestPtr& req, http::Responder&& respond,
                      const std::string& id_text, const std::string& response_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    const std::optional<Uuid> response = http::uuid_param(response_text);
    if (ctx == nullptr || !id.has_value() || !response.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    const Uuid form = *id;
    const Uuid target = *response;
    http::db_or_shed(req, respond, [req, respond, ctx, form, target](mongocxx::client& client) {
        if (!spend_staff(req, respond, *ctx)) { return; }
        const anvil::Result<bool> removed = anvil::repo::guarded([&]() -> anvil::Result<bool> {
            auto collection = client[services().database][std::string{kFormResponsesCollection}];
            const auto result = collection.delete_one(
                make_document(kvp(codec::key_of(fm::form_fields::kId), codec::uuid_bin(target)),
                              kvp(codec::key_of(fm::form_fields::kForm), codec::uuid_bin(form))));
            return result.has_value() && result->deleted_count() == 1;
        });
        if (!removed) {
            respond(http::failure(req, removed.error()));
            return;
        }
        if (!removed.value()) {
            respond(http::failure(req, ErrorCode::NotFound));
            return;
        }
        http::audit(req, Action::FormResponseDeleted, target);
        respond(http::json(200, R"({"removed":true})"));
    });
}

// CSV through anvil's exporter: formula-injection guard, BOM, definition column
// order. Bounded in memory by kMaxExportBytes.
void responses_export(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id_text) {
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (!id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, target](mongocxx::client& client) {
        const auto form = stored(client, target);
        if (!form || !form.value().has_value()) {
            respond(http::failure(req, form ? ErrorCode::NotFound : form.error().code));
            return;
        }
        std::string csv;
        bool too_large = false;
        const anvil::Status exported = fm::export_submissions(
            client, services().forms.repository(), *form.value(), fm::ExportOptions{},
            [&csv, &too_large](std::string_view chunk) {
                if (csv.size() + chunk.size() > kMaxExportBytes) {
                    too_large = true;
                    return false;
                }
                csv.append(chunk);
                return true;
            });
        if (too_large) {
            respond(http::failure(req, ErrorCode::PayloadTooLarge));
            return;
        }
        if (!exported) {
            respond(http::failure(req, exported.error()));
            return;
        }
        http::audit(req, Action::FormResponsesExported, target);
        http::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeString("text/csv; charset=utf-8");
        response->addHeader("Content-Disposition", "attachment; filename=\"responses.csv\"");
        response->addHeader("Cache-Control", "private, no-store");
        response->addHeader("X-Content-Type-Options", "nosniff");
        response->setBody(std::move(csv));
        respond(response);
    });
}

}  // namespace routes
}  // namespace enactus
