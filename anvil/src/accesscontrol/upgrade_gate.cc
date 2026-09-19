#include "anvil/accesscontrol/upgrade_gate.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/types.h"
#include "anvil/http/origin_check.h"
#include "denial.h"

namespace anvil::accesscontrol {
namespace {

// Written only by `register_upgrade_path`, which an application calls from boot
// through `register_websocket_route`, and read on every handshake afterwards.
// Plain values rather than atomics for the same reason `g_deps` in
// access_filter.cc is one: the writes happen before the threads that read them
// exist.
std::array<std::string_view, kMaxUpgradePaths> g_paths{};
std::size_t                                    g_path_count = 0;

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII only, and deliberately: `routeWs` folds with `tolower` under the C
// locale, so folding any further here would make the gate admit a path the
// router would then fail to find — which is the disagreement that puts a
// handshake back on the path this file exists to keep it off.
[[nodiscard]] constexpr bool ascii_ci_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) { return false; }
    }
    return true;
}

// std::string, because Drogon's cookie map is keyed by one. Constructed once at
// load rather than per handshake, and short enough for the small-string
// optimisation so it never touches the heap.
const std::string kAccessCookie{kAccessCookieName};

[[nodiscard]] std::int64_t now_unix() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The audit row and the counter, AFTER the bytes have gone.
//
// A sync advice returns its response to the framework, so it cannot answer first
// the way `AccessFilter::deny` does — and `DenialSink::record` is contracted to
// be called once the client has been answered, because the stealth claim is that
// a denied route and a nonexistent one are timed the same. Queueing it puts it
// in `funcs_`, which `EventLoop::loop` drains after every active channel has
// been handled — that is, after `sendResponses` has written the refusal.
//
// The cost is one `std::function` allocation on a path that is already a denial,
// bought to keep the recording off the timed window entirely rather than trying
// to make it small enough not to matter.
void record_after_the_answer(const drogon::HttpRequestPtr& req, ErrorCode code,
                             const std::optional<Uuid>& actor) noexcept {
    trantor::EventLoop* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if (loop == nullptr) {
        detail::record_denial(req, true, code, actor);
        return;
    }
    loop->queueInLoop(
        [req, code, actor] { detail::record_denial(req, true, code, actor); });
}

// Every refusal this file makes, so the three that must be one answer are one
// object built by one call.
[[nodiscard]] drogon::HttpResponsePtr refuse() { return not_found_upgrade_response(); }

[[nodiscard]] drogon::HttpResponsePtr decide(const drogon::HttpRequestPtr& req) {
    // A handshake is a GET, always. The same first question `isWebSocket` asks,
    // and it is asked here for the same reason: this advice runs on EVERY
    // request in the process, and a method compare is what keeps the ones that
    // could not be upgrades from paying for a header lookup.
    if (req->method() != drogon::Get) { return nullptr; }
    // The same question Drogon's router asks, asked the same way (stealth.h): a
    // request with no `Sec-WebSocket-Key` is not an upgrade whatever its
    // `Upgrade` header claims, and answering a different question here would
    // hold back requests the router would have routed.
    if (!is_upgrade_request(req)) { return nullptr; }

    const std::string_view pattern = upgrade_path_for(req->path());
    if (pattern.empty()) { return refuse(); }

    // Fail closed rather than throw. `deps()` raises on an uninitialised process
    // and this frame is inside the event loop, where an exception is not a 500 —
    // it is an unhandled throw out of `HttpServer::onRequests`.
    if (!AccessControl::initialised()) {
        LOG_ERROR << "an upgrade reached '" << pattern
                  << "' before AccessControl::init; refusing. Call it before the "
                     "first listener starts";
        return refuse();
    }

    // An upgrade is a GET, always, so the policy is looked up under `Get` and
    // there is no method to pass inconsistently — the same lookup
    // `register_websocket_route` made at boot, against the same table.
    const RoutePolicy* policy =
        policy_for(AccessControl::deps().routes, pattern, RouteMethod::Get);
    if (policy == nullptr || policy->access != RouteAccess::Stealth) {
        // Not a class whose existence is a secret. Its refusals keep their real
        // codes and their request ids, through the filters, where a trailing
        // close frame discloses nothing (upgrade_gate.h). A registered pattern
        // with no policy cannot happen — registration throws on it — and if it
        // somehow did, the filter's own `policy == nullptr` branch is closed.
        return nullptr;
    }

    // The origin check first, exactly as `register_websocket_route` orders the
    // filters, and for the same reason: it is a header compare with no I/O, so a
    // cross-origin handshake is refused without the process ever deciding
    // whether the caller was signed in.
    if (http::is_rejection(http::check_request_origin(req, http::OriginRequirement::Always))) {
        return refuse();
    }

    const Evaluation evaluation =
        evaluate_token(req->getCookie(kAccessCookie), *policy, *AccessControl::deps().keys,
                       *AccessControl::deps().epochs, now_unix());
    if (evaluation.step != Step::Deny) {
        // Allow, or an epoch this thread cannot resolve without blocking. Both
        // go to the filter chain, which decides for real — this one has decided
        // nothing and attached nothing.
        return nullptr;
    }

    drogon::HttpResponsePtr response = refuse();
    record_after_the_answer(req, evaluation.code, std::nullopt);
    return response;
}

}  // namespace

void register_upgrade_path(std::string_view pattern) {
    for (const char c : pattern) {
        if (c >= 'A' && c <= 'Z') {
            throw std::logic_error{"websocket route '" + std::string{pattern} +
                                   "' is not lowercase — Drogon lowercases both the key it "
                                   "registers and the path it looks up, so this entry could "
                                   "never match a handshake"};
        }
    }
    if (g_path_count == kMaxUpgradePaths) {
        throw std::logic_error{"more than " + std::to_string(kMaxUpgradePaths) +
                               " websocket routes registered; raise kMaxUpgradePaths in "
                               "anvil/accesscontrol/upgrade_gate.h"};
    }
    g_paths[g_path_count] = pattern;
    ++g_path_count;
}

std::string_view upgrade_path_for(std::string_view request_path) noexcept {
    for (std::size_t i = 0; i < g_path_count; ++i) {
        if (ascii_ci_equal(g_paths[i], request_path)) { return g_paths[i]; }
    }
    return {};
}

void install_upgrade_gate() {
    // Function-local static: initialised exactly once and thread-safely, the
    // same shape `install_request_scope` and `not_found_response` use. A second
    // call is a no-op rather than a second advice, because two copies of this
    // one would each build a refusal and the second would be discarded.
    static const bool installed = [] {
        drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req)
                                             -> drogon::HttpResponsePtr {
            // Nothing below throws by contract, but everything below allocates,
            // and an exception leaving a sync advice leaves it inside
            // `HttpServer::onRequests` where no handler is waiting for one.
            //
            // The fallback is a PASS, and that is safe for one specific reason:
            // this gate is in front of the filters rather than instead of them.
            // A handshake that reaches the router unrefused is refused there by
            // the access filter exactly as it was before this file existed. What
            // is lost is the frame suppression, not the refusal.
            try {
                return decide(req);
            } catch (const std::exception& error) {
                LOG_ERROR << "upgrade gate failed, falling through to the filters: "
                          << error.what();
                return nullptr;
            } catch (...) {
                LOG_ERROR << "upgrade gate failed, falling through to the filters";
                return nullptr;
            }
        });
        return true;
    }();
    (void)installed;
}

}  // namespace anvil::accesscontrol
