#pragma once

// The address of the client, as opposed to the address of the machine that
// forwarded the request.
//
// Every per-IP bucket in rate_limit.h, every `ip` field in the session listing,
// the sign-in notification and every denial record used to read
// `req->peerAddr()`. Behind the Nginx front end this repository ships that is
// `127.0.0.1` on every request, so all of them collapsed into ONE bucket shared
// by the whole internet: an hourly sign-up budget meant for one visitor became
// that budget for all of them combined, and the security log recorded the
// loopback as the source of every denial. Nothing failed, nothing logged, and each mechanism reported success.
//
// `X-Forwarded-For` cannot simply be believed. It is attacker-supplied on any
// request that reaches the application directly, and reading it unconditionally
// replaces a collapsed per-IP layer with an absent one — strictly worse, because
// an attacker then picks a fresh bucket per request.
//
// So the header is only consulted when the PEER is a configured proxy, and the
// entry selected is the RIGHTMOST one that is not itself a configured proxy.
// Rightmost, because each proxy appends: everything to the left of the last
// trusted hop was written by whoever spoke to it, up to and including the
// client, and a client that prepends `X-Forwarded-For: 1.2.3.4` must not be able
// to select that value.
//
// An empty TRUSTED_PROXIES means the application IS the edge, and this reduces
// to peerAddr() exactly — which is what the test suite and release-check.sh run
// as, so their existing expectations are unchanged.
//
// Pure and allocation-free: parsing is inet_pton into a stack buffer, the header
// scan is bounded in both bytes and entries, and nothing here touches the heap.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// Declared, not included: this header is compiled into the foundation library,
// which does not link Drogon. The one function that needs a request is defined
// in client_address_drogon.cc, which is compiled into core.
namespace drogon {
class HttpRequest;
}

namespace anvil::http {

// The same 16 bytes services::pack_ip produces, and for the same reason: v4 is
// stored v4-mapped so one field holds both families and every comparison stays
// a memcmp.
using PackedAddress = std::array<std::uint8_t, 16>;

// v4 or v6 text into 16 bytes. All-zero on anything unparseable, which is also
// what an unspecified address packs to — callers treat both as "no address",
// because neither can be attributed to anybody.
[[nodiscard]] PackedAddress pack_address(std::string_view text) noexcept;

// The inverse of pack_address: sixteen bytes back to text, a v4-mapped address
// rendered as a dotted quad rather than as `::ffff:1.2.3.4`.
//
// Its caller is an operator screen that has to show the address it is about to
// act on and then accept that same spelling back on the route that acts.
// Round-tripping through pack_address is
// therefore a property this pair has to hold, which is why the two live beside
// each other rather than one being written at the call site.
//
// Empty for the unspecified address — which is also what pack_address answers
// for anything it could not parse, so "no address" has one spelling on both
// sides.
[[nodiscard]] std::string format_address(const PackedAddress& address);

// A parsed TRUSTED_PROXIES list.
//
// Fixed capacity in automatic storage (ENGINEERING_RULES.md §2.1): the list is a handful of
// load-balancer subnets, it is read on every request, and a heap indirection per
// entry would be a cache miss on the hot path to save nothing. A deployment that
// needs more than this many entries has a routing problem, not a config problem.
class TrustedProxies final {
public:
    static constexpr std::size_t kMaxEntries = 16;

    TrustedProxies() = default;

    // Comma-separated CIDRs — "127.0.0.1/32, 10.0.0.0/8, ::1" — where a bare
    // address means a single host. Returns false on a malformed entry, on an
    // out-of-range prefix length, or on more than kMaxEntries of them, so config
    // can refuse to boot rather than silently trusting nothing — a malformed
    // entry that parsed as "trust no proxy" would read every forwarded address
    // as the edge's own.
    [[nodiscard]] bool parse(std::string_view list) noexcept;

    [[nodiscard]] bool contains(const PackedAddress& address) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct Entry final {
        PackedAddress network;
        // Bits of `network` that must match, counted over the 128-bit mapped
        // form: a v4 /8 is stored as /104.
        std::uint8_t  bits;
    };

    // Largest first (ENGINEERING_RULES.md §2.3). Entry is 17 bytes and alignof 1, so the
    // array packs with no padding.
    std::array<Entry, kMaxEntries> entries_{};
    std::size_t                    count_{0};
};

// The decision function. `peer` is the address the socket reports.
//
// Returns `peer` unchanged whenever the header must not be believed: no trusted
// proxies configured, a peer that is not one of them, a header holding nothing
// but trusted hops, or a header this function refuses to keep reading.
[[nodiscard]] PackedAddress resolve_client_address(const PackedAddress& peer,
                                                   std::string_view forwarded_for,
                                                   const TrustedProxies& trusted) noexcept;

// The one call the eight request-path sites make. Reads TRUSTED_PROXIES from the
// process configuration and applies the rule above to this request.
//
// One function rather than the same three lines in eight places, which is how
// the ninth call site gets it wrong.
// Installs the trusted-proxy list this process will believe, once at boot.
//
// Nothing installed means this process is the EDGE and every address comes from
// the socket — which is what a test, and a genuinely edge-facing deployment, both
// want. Setting it wrong in either direction is a security failure and both
// failures are silent: unset behind a reverse proxy collapses every per-IP bucket
// into one shared by the whole internet, and set too widely lets a client choose
// its own address and therefore its own bucket. Only the count in the boot
// summary distinguishes them (docs/00-architecture.md §4.1).
void install_trusted_proxies(std::shared_ptr<const TrustedProxies> proxies) noexcept;

[[nodiscard]] PackedAddress client_address(
    const std::shared_ptr<drogon::HttpRequest>& req) noexcept;

// Whether this request arrived on a socket the trusted-proxy list covers.
//
// The predicate `client_address()` already computes privately, exposed because
// `X-Forwarded-For` is not the only header that must not be believed from an
// arbitrary client, and the second one must not grow a second copy of the list
// to ask. False when nothing is installed, which is the "this process is the
// edge" case and the correct answer for it: an edge has no hop to trust.
[[nodiscard]] bool peer_is_trusted_proxy(
    const std::shared_ptr<drogon::HttpRequest>& req) noexcept;

}  // namespace anvil::http
