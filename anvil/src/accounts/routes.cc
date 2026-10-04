#include "anvil/accounts/routes.h"

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/route_registration.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/http/client_address.h"
#include "anvil/http/errors.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/response_writer.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"

namespace anvil::accounts {
namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using Responder = std::function<void(const HttpResponsePtr&)>;
namespace ac = accesscontrol;

// --- responses ------------------------------------------------------------------------

[[nodiscard]] HttpResponsePtr json(int status, std::string body) {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    response->setContentTypeString(http::kJsonContentType);
    // Every answer here is about one person's account, and a shared cache
    // holding one would hand it to the next person who asked.
    response->addHeader("Cache-Control", "private, no-store");
    response->setBody(std::move(body));
    return response;
}

[[nodiscard]] HttpResponsePtr failure(const HttpRequestPtr& req, const AccountAnswer& answer) {
    std::string body;
    body.reserve(128);
    http::append_error_body(body, answer.code, http::request_id_of(req), answer.fields);
    return json(http::http_status(answer.code), std::move(body));
}

[[nodiscard]] drogon::Cookie session_cookie(std::string_view name, std::string value,
                                            std::string_view same_site,
                                            std::int64_t max_age_seconds) {
    drogon::Cookie cookie{std::string{name}, std::move(value)};
    cookie.setPath("/");
    // Secure although a development server is reached over http://127.0.0.1:
    // browsers treat localhost as a secure context, and a cookie written without
    // it here is one somebody copies into a deployment.
    cookie.setSecure(true);
    cookie.setHttpOnly(true);
    cookie.setSameSite(drogon::Cookie::convertString2SameSite(std::string{same_site}));
    cookie.setMaxAge(static_cast<int>(max_age_seconds));
    return cookie;
}

void set_session_cookies(const HttpResponsePtr& response, const identity::IssuedSession& issued) {
    response->addCookie(session_cookie(ac::kAccessCookieName, issued.access_token,
                                       ac::kAccessCookieSameSite,
                                       issued.access_expires_in_seconds));
    // Empty when the refresh token was not rotated, the common case between
    // rotations — and NOT an error. Overwriting the stored cookie with an empty
    // value would sign the caller out on its next refresh.
    if (!issued.refresh_token.empty()) {
        response->addCookie(session_cookie(ac::kRefreshCookieName, issued.refresh_token,
                                           ac::kRefreshCookieSameSite,
                                           issued.refresh_expires_in_seconds));
    }
}

void clear_session_cookies(const HttpResponsePtr& response) {
    response->addCookie(session_cookie(ac::kAccessCookieName, "", ac::kAccessCookieSameSite, 0));
    response->addCookie(session_cookie(ac::kRefreshCookieName, "", ac::kRefreshCookieSameSite, 0));
}

// What a success looks like for each role, and whether it is 200 or 202.
[[nodiscard]] HttpResponsePtr success(AccountRole role, AccountAnswer& answer) {
    switch (role) {
        case AccountRole::Salt: {
            std::string body;
            body.reserve(160);
            if (answer.salt.has_value()) { auth::append_prehash_salt_answer(body, *answer.salt); }
            return json(200, std::move(body));
        }
        case AccountRole::Register:
        case AccountRole::Resend:
        case AccountRole::ResetRequest:
            // Accepted, never Created: a new address and a taken one answer with
            // these same bytes.
            return json(202, R"({"accepted":true})");
        case AccountRole::Verify:       return json(200, R"({"verified":true})");
        case AccountRole::ResetConfirm: return json(200, R"({"reset":true})");
        case AccountRole::Change:       return json(200, R"({"changed":true})");
        case AccountRole::SignIn:
        case AccountRole::Refresh: {
            HttpResponsePtr response = json(
                200, role == AccountRole::SignIn ? R"({"signed_in":true})" : R"({"refreshed":true})");
            if (answer.session.has_value()) { set_session_cookies(response, *answer.session); }
            return response;
        }
        case AccountRole::SignOut: {
            HttpResponsePtr response = json(200, R"({"signed_out":true})");
            clear_session_cookies(response);
            return response;
        }
    }
    return json(200, "{}");
}

// --- the body -----------------------------------------------------------------------------

// The string fields a body carries, read through anvil's own parser. A key that
// is absent, not a string, or empty reads as empty, and each flow answers those
// alike: a form that learns WHICH is a form that can be probed.
class Body final {
public:
    explicit Body(const HttpRequestPtr& req) : document_{input::parse_json(req->body(), arena_)} {}

    [[nodiscard]] bool ok() const noexcept { return document_.ok() && document_.root().is_object(); }

    [[nodiscard]] std::string text(std::string_view key) const {
        if (!ok()) { return {}; }
        const input::JsonValue* value = document_.root().find(key);
        if (value == nullptr) { return {}; }
        const std::optional<std::string_view> text = value->as_string();
        return text.has_value() ? std::string{*text} : std::string{};
    }

    // The profile object's string members, in document order. A member that is
    // not a string is dropped here and reported by the service as missing.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> profile() const {
        std::vector<std::pair<std::string, std::string>> out;
        if (!ok()) { return out; }
        const input::JsonValue* object = document_.root().find("profile");
        if (object == nullptr || !object->is_object()) { return out; }
        for (const input::JsonMember& member : object->members()) {
            const std::optional<std::string_view> text = member.value.as_string();
            if (text.has_value()) {
                out.emplace_back(std::string{member.key}, std::string{*text});
            }
        }
        return out;
    }

private:
    input::BodyArena          arena_;
    input::JsonDocument       document_;
};

// The secret under the name the service's hashing gives it, as the variant the
// service takes. nullopt when it is missing or, for a credential, not exactly
// one — the service answers that as it answers a wrong secret.
[[nodiscard]] std::optional<Secret> secret_of(const Body& body, Hashing hashing,
                                              std::string_view prefix) {
    const std::string name =
        std::string{prefix} + (hashing == Hashing::Client ? "credential" : "password");
    std::string text = body.text(name);
    if (text.empty()) { return std::nullopt; }
    if (hashing == Hashing::Server) { return Secret{std::move(text)}; }
    std::optional<auth::PrehashKey> key = auth::decode_prehash_credential(text);
    if (!key.has_value()) { return std::nullopt; }
    return Secret{std::move(*key)};
}

[[nodiscard]] Origin origin_of(const HttpRequestPtr& req) {
    return Origin{http::client_address(req), req->getHeader("user-agent")};
}

[[nodiscard]] drogon::HttpMethod drogon_method(accesscontrol::RouteMethod method) {
    switch (method) {
        case accesscontrol::RouteMethod::Get:    return drogon::Get;
        case accesscontrol::RouteMethod::Post:   return drogon::Post;
        case accesscontrol::RouteMethod::Put:    return drogon::Put;
        case accesscontrol::RouteMethod::Patch:  return drogon::Patch;
        case accesscontrol::RouteMethod::Delete: return drogon::Delete;
        default:                                 return drogon::Post;
    }
}

// --- one role, one handler -----------------------------------------------------------------

// Calls the service and answers, whichever way the service answers: at once
// (a malformed request, a shed pool) or later from a pool thread.
void run(const HttpRequestPtr& req, const Responder& callback, AccountRole role,
         const std::function<std::optional<AccountAnswer>(Done)>& call) {
    auto respond = [req, callback, role](AccountAnswer answer) {
        callback(answer.code == ErrorCode::Ok ? success(role, answer) : failure(req, answer));
    };
    std::optional<AccountAnswer> immediate = call(respond);
    if (immediate.has_value()) { respond(std::move(*immediate)); }
}

[[nodiscard]] AccountAnswer malformed() {
    AccountAnswer out;
    out.code = ErrorCode::ValidationFailed;
    return out;
}

void handle(const AccountService& service, AccountRole role, const HttpRequestPtr& req,
            Responder&& callback) {
    // The CSRF control (http/origin_check.h), for every role and first of all.
    // No filter in front of these routes asks where a request came from, and
    // each one is a write: a forged sign-in plants the forger's session in the
    // victim's browser (login CSRF), a forged refresh spends a rotation, a
    // forged sign-out or reset request is a nuisance on demand. Before the body
    // is read, so a forged request costs a header compare and learns nothing
    // about the account it named.
    if (http::is_rejection(http::check_request_origin(req))) {
        AccountAnswer refused;
        refused.code = ErrorCode::Forbidden;
        callback(failure(req, refused));
        return;
    }

    const Hashing hashing = service.hashing();

    // Refresh reads a cookie and nothing else.
    if (role == AccountRole::Refresh) {
        std::string token = req->getCookie(std::string{ac::kRefreshCookieName});
        run(req, callback, role, [&service, token = std::move(token)](Done done) mutable {
            return service.refresh(std::move(token), std::move(done));
        });
        return;
    }

    // The two roles behind the access filter read the context it attached. A
    // missing one means the route was declared Public, which install refuses;
    // answered as the stealth 404 all the same rather than trusted.
    if (role == AccountRole::SignOut || role == AccountRole::Change) {
        const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
        if (ctx == nullptr) {
            callback(ac::not_found_response());
            return;
        }
        if (role == AccountRole::SignOut) {
            run(req, callback, role, [&service, ctx](Done done) {
                return service.sign_out(ctx->user_id, ctx->session_id, std::move(done));
            });
            return;
        }
        const Body body{req};
        std::optional<Secret> current = secret_of(body, hashing, "current_");
        std::optional<Secret> replacement = secret_of(body, hashing, "new_");
        if (!body.ok() || !current.has_value() || !replacement.has_value()) {
            callback(failure(req, malformed()));
            return;
        }
        auto shared_current = std::make_shared<Secret>(std::move(*current));
        auto shared_new = std::make_shared<Secret>(std::move(*replacement));
        run(req, callback, role,
            [&service, ctx, identifier = body.text("identifier"), shared_current,
             shared_new](Done done) mutable {
                return service.change(ctx->user_id, ctx->session_id, std::move(identifier),
                                      std::move(*shared_current), std::move(*shared_new),
                                      std::move(done));
            });
        return;
    }

    const Body body{req};
    if (!body.ok()) {
        callback(failure(req, malformed()));
        return;
    }
    const Origin origin = origin_of(req);

    switch (role) {
        case AccountRole::Salt: {
            const std::string purpose = body.text("purpose");
            if (!purpose.empty() && purpose != "sign_in" && purpose != "enroll") {
                callback(failure(req, malformed()));
                return;
            }
            run(req, callback, role, [&service, &body, &purpose, origin](Done done) {
                return service.salt(body.text("identifier"),
                                    purpose == "enroll" ? SaltPurpose::Enroll : SaltPurpose::SignIn,
                                    origin, std::move(done));
            });
            return;
        }
        case AccountRole::Register: {
            std::optional<Secret> secret = secret_of(body, hashing, "");
            RegisterRequest request{
                .email = body.text("email"),
                .username = body.text("username"),
                .phone = body.text("phone"),
                .profile = body.profile(),
                .locale = Locale::from_tag(body.text("locale")),
                // A missing secret travels as the wrong alternative, so the
                // service reports it as a field reason beside any others rather
                // than this handler answering before the rest were checked.
                .secret = secret.has_value()
                              ? std::move(*secret)
                              : (hashing == Hashing::Client ? Secret{std::string{}}
                                                            : Secret{auth::PrehashKey{}}),
                .origin = origin,
            };
            auto shared = std::make_shared<RegisterRequest>(std::move(request));
            run(req, callback, role, [&service, shared](Done done) {
                return service.register_account(std::move(*shared), std::move(done));
            });
            return;
        }
        case AccountRole::Verify:
            run(req, callback, role, [&service, &body, origin](Done done) {
                return service.verify(body.text("identifier"), body.text("code"), origin,
                                      std::move(done));
            });
            return;
        case AccountRole::Resend:
            run(req, callback, role, [&service, &body, origin](Done done) {
                return service.resend(body.text("identifier"), origin, std::move(done));
            });
            return;
        case AccountRole::ResetRequest:
            run(req, callback, role, [&service, &body, origin](Done done) {
                return service.reset_request(body.text("identifier"), origin, std::move(done));
            });
            return;
        case AccountRole::SignIn:
        case AccountRole::ResetConfirm: {
            std::optional<Secret> secret = secret_of(body, hashing, "");
            if (!secret.has_value()) {
                // Unauthenticated rather than malformed: a sign-in form that
                // could tell "no secret" from "wrong secret" answers nothing
                // useful, and one fewer shape is one fewer thing to probe.
                AccountAnswer refused;
                refused.code = ErrorCode::Unauthenticated;
                callback(failure(req, refused));
                return;
            }
            auto shared = std::make_shared<Secret>(std::move(*secret));
            if (role == AccountRole::SignIn) {
                run(req, callback, role, [&service, &body, origin, shared](Done done) {
                    return service.sign_in(body.text("identifier"), std::move(*shared), origin,
                                           std::move(done));
                });
            } else {
                run(req, callback, role, [&service, &body, origin, shared](Done done) {
                    return service.reset_confirm(body.text("identifier"), body.text("code"),
                                                 std::move(*shared), origin, std::move(done));
                });
            }
            return;
        }
        default:
            callback(failure(req, malformed()));
            return;
    }
}

}  // namespace

void install_account_routes(const AccountService& service,
                            std::span<const accesscontrol::RoutePolicy> routes,
                            std::span<const descriptor::RouteDescription> descriptions) {
    for (const AccountRoute& route : service.description().routes) {
        const descriptor::RouteDescription* described = nullptr;
        for (const descriptor::RouteDescription& candidate : descriptions) {
            if (candidate.id == route.route_id) { described = &candidate; }
        }
        if (described == nullptr) {
            throw std::invalid_argument{"account role '" + std::string{role_name(route.role)} +
                                        "' names route id '" + std::string{route.route_id} +
                                        "', which the route descriptions do not declare"};
        }
        const AccountRole role = route.role;
        ac::register_route(routes, std::string{described->pattern},
                           drogon_method(described->method),
                           [&service, role](const HttpRequestPtr& req, Responder&& callback) {
                               handle(service, role, req, std::move(callback));
                           });
    }
}

}  // namespace anvil::accounts
