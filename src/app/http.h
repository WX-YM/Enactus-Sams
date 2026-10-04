#pragma once

// The handful of things every handler does, done one way (anvil
// docs/00-architecture.md §8): answer with anvil's error envelope, parse a body
// into the request arena, move blocking work onto db_pool, refuse a cross-site
// write, and spend a rate-limit budget.

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <mongocxx/client.hpp>

#include "anvil/audit/record.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"
#include "anvil/http/rate_limit.h"
#include "anvil/input/arena.h"
#include "anvil/input/fields.h"
#include "anvil/input/json.h"
#include "audit_actions.h"

namespace enactus::http {

using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;
using Responder = std::function<void(const HttpResponsePtr&)>;

// A JSON body with the cache policy every API answer carries.
[[nodiscard]] HttpResponsePtr json(int status, std::string body);

// anvil's failure envelope for `code`, with the request id the scope minted.
// NotFound is the shared not-found response, byte-identical to an unmatched
// route.
[[nodiscard]] HttpResponsePtr failure(const HttpRequestPtr& req, anvil::ErrorCode code,
                                      std::span<const anvil::input::FieldError> fields = {});
[[nodiscard]] HttpResponsePtr failure(const HttpRequestPtr& req, const anvil::Failure& failed);

// One field, one fixed reason. The field name is always a compile-time constant
// from this codebase, never a key taken from the request.
[[nodiscard]] HttpResponsePtr invalid(const HttpRequestPtr& req, std::string_view field,
                                      anvil::input::Reason reason);

// A 503 with Retry-After, for a refused pool post.
[[nodiscard]] HttpResponsePtr shed(const HttpRequestPtr& req);

// 429 with the window's remaining time.
[[nodiscard]] HttpResponsePtr rate_limited(const HttpRequestPtr& req,
                                           const anvil::http::RateLimitVerdict& verdict,
                                           const anvil::http::RateLimitRule& rule);

// The request's authority, as the access filter attached it. Null only on a
// Public route with no valid token.
[[nodiscard]] std::shared_ptr<const anvil::UserContext> context(const HttpRequestPtr& req);

// The CSRF check for a state-changing request: Origin / Sec-Fetch-Site against
// SITE_ORIGIN (anvil http/origin_check.h). True when the request may proceed;
// otherwise the 403 has already been sent.
[[nodiscard]] bool origin_ok(const HttpRequestPtr& req, const Responder& respond);

// The client address, through TRUSTED_PROXIES (anvil http/client_address.h).
[[nodiscard]] std::array<std::uint8_t, 16> client_ip(const HttpRequestPtr& req);

// A parsed request body. Keeps the arena and the body alive with the document,
// because the document borrows from both (anvil docs/06-input-validation.md §2)
// and must never outlive them.
class Body final {
public:
    explicit Body(const HttpRequestPtr& req);
    Body(const Body&) = delete;
    Body& operator=(const Body&) = delete;

    [[nodiscard]] bool ok() const noexcept { return document_.ok() && document_.root().is_object(); }
    [[nodiscard]] const anvil::input::JsonValue& root() const noexcept { return document_.root(); }

private:
    HttpRequestPtr                req_;  // owns the bytes the document views
    anvil::input::BodyArena       arena_;
    anvil::input::JsonDocument    document_;
};

// Runs `work` on db_pool with a client of its own. False when the pool refused
// it, in which case the caller answers `shed`. Captures must be by value
// (anvil docs/00-architecture.md §3).
[[nodiscard]] bool on_db(std::function<void(mongocxx::client&)> work);

// Runs `work` on cpu_pool (image decoding).
[[nodiscard]] bool on_cpu(std::function<void()> work);

// Posts to db_pool and answers `shed` itself on refusal.
void db_or_shed(const HttpRequestPtr& req, const Responder& respond,
                std::function<void(mongocxx::client&)> work);

// An audit row, written off the request path through the batching sink.
void audit(const HttpRequestPtr& req, Action action, const std::optional<anvil::Uuid>& subject,
           anvil::ErrorCode code = anvil::ErrorCode::Ok);

// Small JSON writing helpers for responses this codebase builds by hand. Every
// string goes through anvil's escaping writer.
void append_key(std::string& out, std::string_view key);
void append_string_field(std::string& out, std::string_view key, std::string_view value,
                         bool comma = true);

// Reads a route parameter as a UUID. Nullopt for anything that is not one; the
// caller answers NotFound, which is what a well-formed id that names nothing
// gets too.
[[nodiscard]] std::optional<anvil::Uuid> uuid_param(std::string_view text);

}  // namespace enactus::http
