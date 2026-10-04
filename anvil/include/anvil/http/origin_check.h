#pragma once

// CSRF defence at pipeline stage 2 (docs/00-architecture.md §4,
// docs/05-auth-sessions.md §2).
//
// SameSite cookies are necessary and not sufficient. They do not cover
// same-site subdomains — a compromised `blog.example.com` is same-site with
// `api.example.com` and its requests carry a SameSite=Lax cookie — and older
// clients ignore the attribute entirely. So every state-changing request also
// compares `Origin` against the exact configured origins.
//
// Two rules that are easy to get subtly wrong:
//
//   * An ABSENT Origin on a state-changing method is a REJECTION, not a pass.
//     "Absent means not a browser, so it cannot be CSRF" is false: some
//     navigations and some proxies strip the header, and treating absence as
//     safe converts the whole check into an opt-out an attacker can take.
//   * The comparison is byte-for-byte against a fixed list. Suffix matching
//     ("ends with example.com") is an open door: `evil-example.com` and
//     `example.com.attacker.test` both pass a naive suffix test.
//
// Pure: a header compare with no I/O and no allocation, so it costs nothing and
// runs before anything expensive.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// Declared, not included: this header is compiled into the foundation library,
// which does not link Drogon. The one function that needs a request is defined
// in origin_check_drogon.cc, which is compiled into anvil::platform. Same split
// as client_address.h, for the same reason.
namespace drogon {
class HttpRequest;
}  // namespace drogon

namespace anvil::http {

enum class OriginVerdict : std::uint8_t {
    Allowed,
    // The method cannot change state, so no Origin is required. GET/HEAD/
    // OPTIONS. Note this makes it a rule about the METHOD, and any handler that
    // mutates on GET has broken the rule, not this check.
    NotRequired,
    Missing,
    Mismatched,
};

// True for methods that may change state and therefore require an Origin.
//
// It stays a statement about METHODS and deliberately learns nothing about
// transports. The one case where the method is the wrong question is answered by
// `OriginRequirement` below, at the call site that knows.
[[nodiscard]] constexpr bool is_state_changing(std::string_view method) noexcept {
    return method != "GET" && method != "HEAD" && method != "OPTIONS";
}

// Whether the METHOD gets to decide that no Origin is needed.
//
// A WebSocket handshake is an HTTP GET with `Upgrade: websocket`, so the rule
// above answers `NotRequired` for every upgrade and `is_rejection` answers
// false. For an ordinary GET that is right, because the rule behind it is that
// GET does not mutate. For an upgrade it is wrong three times over:
//
//   * Same-origin policy does not constrain WebSockets. A browser opens a socket
//     from any origin to any origin — no preflight, no CORS negotiation, and no
//     `Access-Control-Allow-Origin` to withhold.
//   * The cookie is attached anyway. `__Host-` keeps the jar per-origin; it does
//     not stop another origin from opening a socket and having the browser send
//     the session cookie with the handshake.
//   * `SameSite` is the partial defence the top of this file already calls
//     necessary and not sufficient — it does not cover same-site subdomains.
//
// So on an upgrade the origin check is not defence in depth, it is the ONLY CSRF
// defence there is, and by method it would be skipped. The failure shape is the
// one worth naming: the verdict is a pass rather than an error, so there is no
// log line, no metric and no failing test — a control that is present, called,
// and answering the wrong thing.
//
// A defaulted parameter rather than a change to `is_state_changing`, so no
// existing call site changes and no consumer's build does either.
enum class OriginRequirement : std::uint8_t {
    // GET, HEAD and OPTIONS need none. The behaviour every caller had before
    // this enum existed, and still the default.
    ByMethod,
    // Required whatever the method. The method tells you nothing about the risk
    // of a request that opens a bidirectional channel.
    Always,
};

// `origin_header` is the raw `Origin:` value, empty when absent. `allowed` is
// the exact set of acceptable origins — scheme, host and port, no path, no
// trailing slash — which is what config validates on the way in.
//
// Under `Always` the verdict `NotRequired` is never returned: an absent Origin
// on a GET is `Missing`, which is a rejection, for the same reason an absent one
// on a POST is.
[[nodiscard]] OriginVerdict check_origin(
    std::string_view method, std::string_view origin_header,
    std::span<const std::string_view> allowed,
    OriginRequirement requirement = OriginRequirement::ByMethod) noexcept;

// --- the allow-list, and the one place a process holds it -------------------
//
// `check_origin` above takes its list as a span because it is pure, and until
// now that was the whole story: anvil enforced the check on no route, so every
// caller had its own list in its own hand. A route anvil registers has to be
// able to answer the question without one, which means the process has to hold
// the list somewhere.
//
// Fixed capacity in automatic storage (CLAUDE.md §2.1) and the same shape as
// `TrustedProxies`, for the same reason: it is a handful of entries, it is read
// on a request path, and a heap indirection per entry would be a cache miss to
// save nothing. A deployment that needs more than this many origins has an
// architecture question, not a configuration one.
class AllowedOrigins final {
public:
    static constexpr std::size_t kMaxEntries = 8;

    AllowedOrigins() = default;

    // Comma-separated — "https://example.test, https://admin.example.test" —
    // with surrounding whitespace ignored. Each entry is scheme, host and
    // optional port: no path, no trailing slash, no query, which is exactly the
    // form a browser puts in the header and exactly what the byte-for-byte
    // compare needs.
    //
    // Returns false on a malformed entry or on more than `kMaxEntries` of them,
    // so config can refuse to BOOT rather than run with a list that silently
    // refuses everything. The distinction matters: a list that failed to parse
    // and a list that is genuinely empty both reject every origin, and only one
    // of them is what the operator meant.
    [[nodiscard]] bool parse(std::string_view list) noexcept;

    [[nodiscard]] bool contains(std::string_view origin) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    std::array<std::string, kMaxEntries> entries_{};
    std::size_t                          count_{0};
};

// The same decision, against a parsed list. One rule, two ways to look an origin
// up — the comparison itself is written once, in origin_check.cc.
[[nodiscard]] OriginVerdict check_origin(
    std::string_view method, std::string_view origin_header, const AllowedOrigins& allowed,
    OriginRequirement requirement = OriginRequirement::ByMethod) noexcept;

// Installs the list this process will accept, once at boot.
//
// `shared_ptr<const>`, swapped atomically, for the reason
// `install_trusted_proxies` is: a request that started under the old list
// finishes under it rather than seeing a half-updated one. In practice it is
// written once at boot, and "in practice written once" is how a data race gets
// shipped.
//
// Nothing installed means NO origin is accepted, so every check that consults
// the list rejects. That is the deny-by-default answer and it is deliberately
// not the `install_trusted_proxies` one — an absent proxy list means "this
// process is the edge", which is a real deployment, while an absent origin list
// means nobody said which origins are ours, and there is no safe way to guess.
void install_allowed_origins(std::shared_ptr<const AllowedOrigins> origins) noexcept;

// The decision for a live request, against the installed list.
//
// `Mismatched` when nothing is installed: there is no allow-list, so nothing is
// on it. The refusal is visible — the request is answered — rather than a pass
// that nobody notices, which is the failure shape `OriginRequirement` above
// exists to close.
[[nodiscard]] OriginVerdict check_request_origin(
    const std::shared_ptr<drogon::HttpRequest>& req,
    OriginRequirement requirement = OriginRequirement::ByMethod) noexcept;

[[nodiscard]] constexpr bool is_rejection(OriginVerdict verdict) noexcept {
    return verdict == OriginVerdict::Missing || verdict == OriginVerdict::Mismatched;
}

}  // namespace anvil::http
