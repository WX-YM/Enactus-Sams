#include "app/http.h"

#include <utility>

#include <drogon/HttpTypes.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/http/client_address.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/retry_after.h"
#include "app/services.h"

namespace enactus::http {

HttpResponsePtr json(int status, std::string body) {
    HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    response->setContentTypeString(anvil::http::kJsonContentType);
    response->addHeader("Cache-Control", "private, no-store");
    response->setBody(std::move(body));
    return response;
}

HttpResponsePtr failure(const HttpRequestPtr& req, anvil::ErrorCode code,
                        std::span<const anvil::input::FieldError> fields) {
    if (code == anvil::ErrorCode::NotFound) { return anvil::accesscontrol::not_found_response(); }
    std::string body;
    body.reserve(160);
    anvil::http::append_error_body(body, code, anvil::http::request_id_of(req), fields);
    HttpResponsePtr response = json(anvil::http::http_status(code), std::move(body));
    if (code == anvil::ErrorCode::ServiceUnavailable) {
        anvil::http::apply_retry_after(*response, anvil::http::kShedRetryAfterSeconds);
    }
    return response;
}

HttpResponsePtr failure(const HttpRequestPtr& req, const anvil::Failure& failed) {
    if (failed.code == anvil::ErrorCode::ValidationFailed && !failed.field.empty()) {
        const std::array<anvil::input::FieldError, 1> fields{
            anvil::input::FieldError{failed.field, anvil::input::Reason::BadFormat}};
        return failure(req, failed.code, fields);
    }
    return failure(req, failed.code);
}

HttpResponsePtr invalid(const HttpRequestPtr& req, std::string_view field,
                        anvil::input::Reason reason) {
    const std::array<anvil::input::FieldError, 1> fields{anvil::input::FieldError{field, reason}};
    return failure(req, anvil::ErrorCode::ValidationFailed, fields);
}

HttpResponsePtr shed(const HttpRequestPtr& req) {
    return failure(req, anvil::ErrorCode::ServiceUnavailable);
}

HttpResponsePtr rate_limited(const HttpRequestPtr& req,
                             const anvil::http::RateLimitVerdict& verdict,
                             const anvil::http::RateLimitRule& rule) {
    HttpResponsePtr response = failure(req, anvil::ErrorCode::RateLimited);
    anvil::http::apply_retry_after(*response, anvil::http::retry_after_seconds(verdict, rule));
    return response;
}

std::shared_ptr<const anvil::UserContext> context(const HttpRequestPtr& req) {
    return anvil::accesscontrol::user_context(req);
}

bool origin_ok(const HttpRequestPtr& req, const Responder& respond) {
    if (anvil::http::is_rejection(anvil::http::check_request_origin(req))) {
        respond(failure(req, anvil::ErrorCode::Forbidden));
        return false;
    }
    return true;
}

std::array<std::uint8_t, 16> client_ip(const HttpRequestPtr& req) {
    return anvil::http::client_address(req);
}

Body::Body(const HttpRequestPtr& req)
    : req_{req}, arena_{}, document_{anvil::input::parse_json(req_->body(), arena_)} {}

bool on_db(std::function<void(mongocxx::client&)> work) {
    return anvil::Pools::db().try_post(anvil::guarded("db", [work = std::move(work)]() {
        auto entry = anvil::db::MongoPool::instance().acquire();
        work(*entry);
    }));
}

bool on_cpu(std::function<void()> work) {
    return anvil::Pools::cpu().try_post(anvil::guarded("cpu", std::move(work)));
}

void db_or_shed(const HttpRequestPtr& req, const Responder& respond,
                std::function<void(mongocxx::client&)> work) {
    // A task that throws past `guarded` would leave the client with no answer
    // at all; the catch here turns it into a 500 with a request id.
    const bool posted = on_db([req, respond, work = std::move(work)](mongocxx::client& client) {
        try {
            work(client);
        } catch (...) {
            respond(failure(req, anvil::ErrorCode::Internal));
            throw;
        }
    });
    if (!posted) { respond(shed(req)); }
}

void audit(const HttpRequestPtr& req, Action action, const std::optional<anvil::Uuid>& subject,
           anvil::ErrorCode code) {
    const std::shared_ptr<const anvil::UserContext> ctx = context(req);
    anvil::audit::AuditEntry entry{
        .actor = ctx != nullptr ? std::optional<anvil::Uuid>{ctx->user_id} : std::nullopt,
        .subject = subject,
        .from_state = std::nullopt,
        .to_state = std::nullopt,
        .ip = client_ip(req),
        .action = enactus::audit(action),
        .code = code,
        .succeeded = code == anvil::ErrorCode::Ok,
    };
    services().audit.write_async(entry);
}

void append_key(std::string& out, std::string_view key) {
    anvil::http::append_json_string(out, key);
    out += ':';
}

void append_string_field(std::string& out, std::string_view key, std::string_view value,
                         bool comma) {
    if (comma) { out += ','; }
    append_key(out, key);
    anvil::http::append_json_string(out, value);
}

std::optional<anvil::Uuid> uuid_param(std::string_view text) { return anvil::uuid::parse(text); }

}  // namespace enactus::http
