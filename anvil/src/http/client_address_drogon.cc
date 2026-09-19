// The one part of client_address that needs a request, and therefore the one part
// that cannot live in the foundation library.
//
// The trusted-proxy list is INSTALLED at boot rather than read from a global. In
// the system anvil was extracted from, this file was the single place in the whole
// library that reached for a configuration singleton — and a library that reaches
// for one cannot be embedded twice, cannot be tested without booting a
// process-wide configuration, and cannot be reasoned about locally.
//
// Installing it keeps what the singleton was actually buying: the list is parsed
// and validated once, at boot, by the same parser the request path uses, rather
// than by a second one that can disagree with it.

#include "anvil/http/client_address.h"

#include <drogon/HttpRequest.h>
#include <trantor/net/InetAddress.h>

#include <atomic>
#include <memory>

namespace anvil::http {
namespace {

// std::string because getHeader is keyed by one; short enough for the
// small-string optimisation, and constructed once at load rather than per
// request.
const std::string kForwardedForHeader{"x-forwarded-for"};

// trantor already holds the peer as a sockaddr, so nothing is parsed back out of
// a string. This is the packing anvil/accesscontrol/access_filter.cc used to carry
// privately, and the seven controllers reached by round-tripping through
// peerAddr().toIp() — a string format and a re-parse per request, for an address
// that was already 16 bytes.
[[nodiscard]] PackedAddress pack_peer(const trantor::InetAddress& peer) noexcept {
    PackedAddress packed{};
    if (peer.isIpV6()) {
        const auto* words = peer.ip6NetEndian();
        if (words == nullptr) { return packed; }
        for (std::size_t i = 0; i < 4; ++i) {
            const std::uint32_t word = words[i];
            // Already network byte order: copy, never byteswap.
            packed[(i * 4) + 0] = static_cast<std::uint8_t>(word & 0xFFU);
            packed[(i * 4) + 1] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
            packed[(i * 4) + 2] = static_cast<std::uint8_t>((word >> 16U) & 0xFFU);
            packed[(i * 4) + 3] = static_cast<std::uint8_t>((word >> 24U) & 0xFFU);
        }
        return packed;
    }
    const std::uint32_t v4 = peer.ipNetEndian();
    packed[10] = 0xFF;
    packed[11] = 0xFF;   // v4-mapped, so one 16-byte field holds both families
    packed[12] = static_cast<std::uint8_t>(v4 & 0xFFU);
    packed[13] = static_cast<std::uint8_t>((v4 >> 8U) & 0xFFU);
    packed[14] = static_cast<std::uint8_t>((v4 >> 16U) & 0xFFU);
    packed[15] = static_cast<std::uint8_t>((v4 >> 24U) & 0xFFU);
    return packed;
}

// shared_ptr<const>, swapped atomically: a request that started under the old
// list finishes under it rather than seeing a half-updated one. In practice this
// is written once at boot, but "in practice written once" is how a data race gets
// shipped.
std::shared_ptr<const TrustedProxies> g_trusted_proxies;

}  // namespace

void install_trusted_proxies(std::shared_ptr<const TrustedProxies> proxies) noexcept {
    std::atomic_store_explicit(&g_trusted_proxies, std::move(proxies),
                               std::memory_order_release);
}

PackedAddress client_address(const std::shared_ptr<drogon::HttpRequest>& req) noexcept {
    if (!req) { return PackedAddress{}; }
    const PackedAddress peer = pack_peer(req->peerAddr());

    // Nothing installed is precisely the "this process is the edge" case, which
    // is the correct answer rather than a papered-over error: a test driving one
    // handler does not boot a deployment's proxy list, and neither does a process
    // that genuinely is the edge.
    const std::shared_ptr<const TrustedProxies> proxies =
        std::atomic_load_explicit(&g_trusted_proxies, std::memory_order_acquire);
    if (!proxies) { return peer; }

    return resolve_client_address(peer, req->getHeader(kForwardedForHeader), *proxies);
}

bool peer_is_trusted_proxy(const std::shared_ptr<drogon::HttpRequest>& req) noexcept {
    if (!req) { return false; }
    const std::shared_ptr<const TrustedProxies> proxies =
        std::atomic_load_explicit(&g_trusted_proxies, std::memory_order_acquire);
    if (!proxies) { return false; }
    return proxies->contains(pack_peer(req->peerAddr()));
}

}  // namespace anvil::http
