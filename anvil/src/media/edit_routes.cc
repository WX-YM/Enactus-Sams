#include "anvil/media/edit_routes.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/route_registration.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/http/errors.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/response_writer.h"
#include "anvil/http/retry_after.h"
#include "anvil/images/recipe.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "anvil/media/pipeline.h"

namespace anvil::media {
namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using Responder = std::function<void(const HttpResponsePtr&)>;
namespace ac = accesscontrol;

// The largest recipe a request may carry, as base64url: the codec's own worst
// case. A longer string is refused before it is decoded, so a body stuffed with
// megabytes of base64 costs a length comparison.
constexpr std::size_t kMaxRecipeBytes =
    3 + 8 + 2 + images::kMaxEditStrokes * 9 + images::kMaxEditPoints * 4;
constexpr std::size_t kMaxRecipeText = (kMaxRecipeBytes * 4 + 2) / 3;

[[nodiscard]] HttpResponsePtr json(int status, std::string body) {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    response->setContentTypeString(http::kJsonContentType);
    // One account's media, and a shared cache holding it would hand it on.
    response->addHeader("Cache-Control", "private, no-store");
    response->setBody(std::move(body));
    return response;
}

// Every failure but the stealth one. A ValidationFailed carries the recipe's
// fault as a field and a reason; nothing else explains itself.
[[nodiscard]] HttpResponsePtr failure(const HttpRequestPtr& req, const Failure& failed) {
    // A source that does not exist, or exists in another namespace, is the
    // SHARED 404, byte-identical to an unmatched route: the same filter, so the
    // same bytes, or the pair of them is an existence oracle.
    if (failed.code == ErrorCode::NotFound) { return ac::not_found_response(); }
    std::string body;
    body.reserve(160);
    if (failed.code == ErrorCode::ValidationFailed) {
        const std::array<input::FieldError, 1> fields{images::field_error(failed.field)};
        http::append_error_body(body, failed.code, http::request_id_of(req), fields);
    } else {
        http::append_error_body(body, failed.code, http::request_id_of(req));
    }
    HttpResponsePtr response = json(http::http_status(failed.code), std::move(body));
    if (failed.code == ErrorCode::ServiceUnavailable) {
        http::apply_retry_after(*response, http::kShedRetryAfterSeconds);
    }
    return response;
}

[[nodiscard]] HttpResponsePtr edited(const EditedMedia& media) {
    std::string body;
    body.reserve(96);
    http::write_object<kEditResponse>(body)
        .uuid<"id">(media.id)
        .number<"width">(media.width)
        .number<"height">(media.height)
        .done();
    // 201 for a new object and 200 for one this exact edit already made: both
    // are success, and a client treats them alike.
    return json(media.created ? 201 : 200, std::move(body));
}

// The subject of a request, from its two path segments. Neither a namespace nor
// an id that fails to parse is a validation failure: a malformed address is an
// address that names nothing, and it answers exactly as one that names nothing.
struct Subject final {
    fs::Ns ns;
    Uuid   id;
};

[[nodiscard]] std::optional<Subject> subject_of(std::string_view ns, std::string_view id) {
    const std::optional<fs::Ns> space = fs::Ns::from_dir(ns);
    const std::optional<Uuid> parsed = uuid::parse(id);
    if (!space.has_value() || !parsed.has_value()) { return std::nullopt; }
    return Subject{*space, *parsed};
}

template <typename Work>
[[nodiscard]] bool on_db(Work work) {
    return Pools::db().try_post(anvil::guarded("db", [work = std::move(work)]() mutable {
        auto entry = db::MongoPool::instance().acquire();
        work(*entry);
    }));
}

template <typename Work>
[[nodiscard]] bool on_cpu(Work work) {
    return Pools::cpu().try_post(anvil::guarded("cpu", std::move(work)));
}

[[nodiscard]] Failure shed() { return fail(ErrorCode::ServiceUnavailable); }

// The one way an edit is answered, so every answer reaches the application's
// observer exactly once. Copied into each stage: a request pointer, a callback
// and a shared pointer, the same captures every stage already made.
class Reply final {
public:
    Reply(HttpRequestPtr req, Responder callback, std::shared_ptr<const EditObserver> observer,
          Uuid actor, Subject subject)
        : req_{std::move(req)},
          callback_{std::move(callback)},
          observer_{std::move(observer)},
          actor_{actor},
          subject_{subject} {}

    void operator()(const EditedMedia& media) const {
        callback_(edited(media));
        report(media.id, media.created, ErrorCode::Ok);
    }

    void operator()(const Failure& failed) const {
        callback_(failure(req_, failed));
        report(std::nullopt, false, failed.code);
    }

    // A refusal that needed more than failure() writes: the rate limit's
    // Retry-After.
    void refuse(const HttpResponsePtr& response, ErrorCode code) const {
        callback_(response);
        report(std::nullopt, false, code);
    }

private:
    void report(std::optional<Uuid> edit, bool created, ErrorCode code) const noexcept {
        if (observer_ == nullptr) { return; }
        // After the answer, and never able to unmake it: an audit hook that
        // throws must not turn a made edit into a 500 on a pool thread.
        try {
            (*observer_)(EditOutcome{
                .request = req_,
                .actor = actor_,
                .ns = subject_.ns,
                .source = subject_.id,
                .edit = edit,
                .created = created,
                .code = code,
            });
        } catch (...) {
        }
    }

    HttpRequestPtr                      req_;
    Responder                           callback_;
    std::shared_ptr<const EditObserver> observer_;
    Uuid                                actor_;
    Subject                             subject_;
};

// --- POST: make an edit ----------------------------------------------------------

// The last stage, back on db_pool.
void record(const MediaService& service, std::shared_ptr<const PreparedEdit> edit,
            std::shared_ptr<const RenderedMedia> rendered, Subject subject, Uuid owner,
            Reply reply) {
    const bool posted = on_db([&service, edit, rendered, subject, owner,
                               reply](mongocxx::client& client) {
        const Result<EditedMedia> recorded = service.record_edit(
            client, subject.ns, owner, *edit, rendered->media, rendered->sha256);
        if (recorded) {
            reply(recorded.value());
        } else {
            reply(recorded.error());
        }
    });
    if (!posted) {
        // Shed between the render and the row: nothing points at the files, and
        // they are under an id nothing else will ever name.
        unlink_all_files(fs::Storage::instance(), subject.ns, rendered->media.id,
                         rendered->media.variants);
        reply(shed());
    }
}

void render_then_record(const MediaService& service, std::shared_ptr<const PreparedEdit> edit,
                        Subject subject, Uuid owner, Reply reply) {
    const bool posted = on_cpu([&service, edit, subject, owner, reply]() {
        Result<RenderedMedia> rendered = render(subject.ns, *edit);
        if (!rendered) {
            reply(rendered.error());
            return;
        }
        record(service, edit, std::make_shared<const RenderedMedia>(std::move(rendered).value()),
               subject, owner, reply);
    });
    if (!posted) { reply(shed()); }
}

void handle_edit(const MediaService& service, http::RateLimiter& limiter,
                 const http::RateLimitRule& budget,
                 const std::shared_ptr<const EditObserver>& observer, const HttpRequestPtr& req,
                 Responder&& callback, const std::string& ns, const std::string& id) {
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    const std::optional<Subject> subject = subject_of(ns, id);
    if (ctx == nullptr || !subject.has_value()) {
        callback(ac::not_found_response());
        return;
    }
    const Reply reply{req, std::move(callback), observer, ctx->user_id, *subject};
    // The CSRF control (http/origin_check.h). The access filter decides WHO may
    // make an edit and knows nothing of where the request came from; a page on
    // a same-site subdomain carries the SameSite cookie, and without this it
    // could spend the account's render budget and fill the library. Before the
    // body is read, so a forged request costs a header compare.
    if (http::is_rejection(http::check_request_origin(req))) {
        reply(fail(ErrorCode::Forbidden));
        return;
    }

    input::BodyArena arena;
    const input::JsonDocument document = input::parse_json(req->body(), arena);
    const input::JsonValue* recipe_field =
        document.ok() && document.root().is_object() ? document.root().find("recipe") : nullptr;
    const std::optional<std::string_view> text =
        recipe_field != nullptr ? recipe_field->as_string() : std::nullopt;
    bool detach = false;
    if (document.ok() && document.root().is_object()) {
        if (const input::JsonValue* flag = document.root().find("detach"); flag != nullptr) {
            const std::optional<bool> value = flag->as_bool();
            if (!value.has_value()) {
                reply(fail(ErrorCode::ValidationFailed, images::kFaultFormat));
                return;
            }
            detach = *value;
        }
    }
    if (!text.has_value() || text->empty() || text->size() > kMaxRecipeText) {
        reply(fail(ErrorCode::ValidationFailed,
                   text.has_value() && !text->empty() ? images::kFaultBounds
                                                      : images::kFaultFormat));
        return;
    }
    std::optional<std::vector<std::uint8_t>> bytes = crypto::base64url_decode(*text);
    if (!bytes.has_value()) {
        reply(fail(ErrorCode::ValidationFailed, images::kFaultFormat));
        return;
    }

    auto shared_bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes));
    const Uuid owner = ctx->user_id;
    const Subject target = *subject;
    // A pointer and a copy: the task runs after this frame is gone, and the
    // rule is 32 trivially copyable bytes whose bucket is a static literal.
    http::RateLimiter* const budgets = &limiter;
    const http::RateLimitRule rule = budget;
    const bool posted = on_db([&service, budgets, rule, req, shared_bytes, target, owner,
                               detach, reply](mongocxx::client& client) {
        // The budget is a Redis round trip, so it is spent here and not on the
        // loop thread that accepted the request (CLAUDE.md §4). After the body
        // is known to be well formed, so a malformed request costs no budget,
        // and before any lookup, so a refused one costs no query.
        const http::RateLimitVerdict verdict =
            budgets->check_account(uuid::to_string(owner), rule);
        if (!verdict.allowed) {
            HttpResponsePtr refused = failure(req, fail(ErrorCode::RateLimited));
            http::apply_retry_after(*refused, http::retry_after_seconds(verdict, rule));
            reply.refuse(refused, ErrorCode::RateLimited);
            return;
        }
        Result<std::variant<EditedMedia, PreparedEdit>> prepared =
            service.prepare_edit(client, target.ns, target.id, *shared_bytes, detach);
        if (!prepared) {
            reply(prepared.error());
            return;
        }
        if (const EditedMedia* existing = std::get_if<EditedMedia>(&prepared.value())) {
            reply(*existing);
            return;
        }
        render_then_record(service,
                           std::make_shared<const PreparedEdit>(
                               std::get<PreparedEdit>(std::move(prepared).value())),
                           target, owner, reply);
    });
    if (!posted) { reply(shed()); }
}

// --- GET: what an editor reopens -------------------------------------------------

void handle_state(const MediaService& service, const HttpRequestPtr& req, Responder&& callback,
                  const std::string& ns, const std::string& id) {
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    const std::optional<Subject> subject = subject_of(ns, id);
    if (ctx == nullptr || !subject.has_value()) {
        callback(ac::not_found_response());
        return;
    }
    const Subject target = *subject;
    const bool posted = on_db([&service, target, req, callback](mongocxx::client& client) {
        const Result<std::optional<MediaRecord>> found = service.find(client, target.ns, target.id);
        if (!found) {
            callback(failure(req, found.error()));
            return;
        }
        if (!found.value().has_value()) {
            callback(ac::not_found_response());
            return;
        }
        const MediaRecord& row = *found.value();

        std::string body;
        body.reserve(160);
        if (!row.source.has_value()) {
            http::write_object<kEditStateResponse>(body)
                .uuid<"source">(row.id)
                .number<"width">(row.width)
                .number<"height">(row.height)
                .null_field<"recipe">()
                .done();
            callback(json(200, std::move(body)));
            return;
        }

        // An edit: the editor opens on its SOURCE, at the source's size.
        const Result<std::optional<MediaRecord>> source =
            service.find(client, target.ns, *row.source);
        if (!source) {
            callback(failure(req, source.error()));
            return;
        }
        if (!source.value().has_value()) {
            // The edit holds a reference on its source, so this is a broken
            // invariant rather than a state a client can cause. Answered as the
            // object being gone, which is the honest thing to tell a client.
            callback(ac::not_found_response());
            return;
        }
        http::write_object<kEditStateResponse>(body)
            .uuid<"source">(*row.source)
            .number<"width">(source.value()->width)
            .number<"height">(source.value()->height)
            .text<"recipe">(crypto::base64url_encode(row.edit))
            .done();
        callback(json(200, std::move(body)));
    });
    if (!posted) { callback(failure(req, shed())); }
}

// --- installation ----------------------------------------------------------------

[[nodiscard]] const descriptor::RouteDescription& described(
    std::span<const descriptor::RouteDescription> descriptions, std::string_view id) {
    for (const descriptor::RouteDescription& candidate : descriptions) {
        if (candidate.id != id) { continue; }
        constexpr std::string_view kTail = "{ns}/{id}";
        if (candidate.pattern.size() < kTail.size() ||
            candidate.pattern.substr(candidate.pattern.size() - kTail.size()) != kTail) {
            throw std::invalid_argument{"media edit route '" + std::string{id} +
                                        "' must end in {ns}/{id}"};
        }
        return candidate;
    }
    throw std::invalid_argument{"media edit route id '" + std::string{id} +
                                "' is not in the route descriptions"};
}

}  // namespace

void install_media_edit_routes(const MediaService& service, http::RateLimiter& limiter,
                               std::span<const accesscontrol::RoutePolicy> routes,
                               std::span<const descriptor::RouteDescription> descriptions,
                               const EditRoutes& config) {
    const descriptor::RouteDescription& edit = described(descriptions, config.edit_route_id);
    const descriptor::RouteDescription& state = described(descriptions, config.state_route_id);
    const http::RateLimitRule budget = config.budget;
    // Null when the application records nothing, so an answer costs one pointer
    // test rather than a call through an empty std::function.
    std::shared_ptr<const EditObserver> observer =
        config.on_edit ? std::make_shared<const EditObserver>(config.on_edit) : nullptr;

    ac::register_route(routes, std::string{edit.pattern}, drogon::Post,
                       [&service, &limiter, budget, observer](
                           const HttpRequestPtr& req, Responder&& callback,
                           const std::string& ns, const std::string& id) {
                           handle_edit(service, limiter, budget, observer, req,
                                       std::move(callback), ns, id);
                       });
    ac::register_route(routes, std::string{state.pattern}, drogon::Get,
                       [&service](const HttpRequestPtr& req, Responder&& callback,
                                  const std::string& ns, const std::string& id) {
                           handle_state(service, req, std::move(callback), ns, id);
                       });
}

}  // namespace anvil::media
