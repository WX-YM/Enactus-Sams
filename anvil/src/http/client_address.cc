#include "anvil/http/client_address.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace anvil::http {
namespace {

// Prefixed onto every v4 address so the two families share one comparison path.
constexpr std::array<std::uint8_t, 12> kV4MappedPrefix{0, 0, 0, 0, 0,    0,
                                                       0, 0, 0, 0, 0xFF, 0xFF};

// A v4 CIDR's prefix length is quoted against 32 bits; stored v4-mapped, the
// same boundary sits 96 bits in.
constexpr std::uint8_t kV4MappedOffsetBits = 96;

// The header scan reads at most this many trailing bytes. A 64 KB
// X-Forwarded-For is not a deployment, it is an attempt to make this function
// expensive, and there is no answer past the last few hops that could change the
// result.
constexpr std::size_t kScanBytes = 512;

// And at most this many entries within them. Four hops is already more proxying
// than this deployment has.
constexpr std::size_t kScanEntries = 8;

[[nodiscard]] constexpr bool is_space(char ch) noexcept {
    return ch == ' ' || ch == '\t';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) { text.remove_prefix(1); }
    while (!text.empty() && is_space(text.back())) { text.remove_suffix(1); }
    return text;
}

[[nodiscard]] bool is_unspecified(const PackedAddress& address) noexcept {
    return std::all_of(address.begin(), address.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

// True when the first `bits` bits of the two addresses agree.
[[nodiscard]] bool prefix_matches(const PackedAddress& a, const PackedAddress& b,
                                  std::uint8_t bits) noexcept {
    const std::size_t whole_bytes = bits / 8U;
    if (std::memcmp(a.data(), b.data(), whole_bytes) != 0) { return false; }
    const unsigned remainder = bits % 8U;
    if (remainder == 0) { return true; }
    const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - remainder));
    return (a[whole_bytes] & mask) == (b[whole_bytes] & mask);
}

}  // namespace

PackedAddress pack_address(std::string_view text) noexcept {
    PackedAddress packed{};
    // inet_pton needs NUL termination, and the input is bounded by
    // INET6_ADDRSTRLEN, so this stays on the stack and rejects anything longer
    // without reading it.
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    if (text.empty() || text.size() >= buffer.size()) { return packed; }
    std::memcpy(buffer.data(), text.data(), text.size());

    std::array<std::uint8_t, 4> v4{};
    if (::inet_pton(AF_INET, buffer.data(), v4.data()) == 1) {
        std::copy(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), packed.begin());
        std::copy(v4.begin(), v4.end(), packed.begin() + 12);
        return packed;
    }
    if (::inet_pton(AF_INET6, buffer.data(), packed.data()) == 1) { return packed; }
    return PackedAddress{};
}

std::string format_address(const PackedAddress& address) {
    const bool unspecified =
        std::all_of(address.begin(), address.end(), [](std::uint8_t b) { return b == 0; });
    if (unspecified) { return {}; }

    const bool v4_mapped =
        std::equal(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), address.begin());

    std::array<char, INET6_ADDRSTRLEN> text{};
    if (v4_mapped) {
        if (::inet_ntop(AF_INET, address.data() + 12, text.data(), text.size()) == nullptr) {
            return {};
        }
        return std::string{text.data()};
    }
    if (::inet_ntop(AF_INET6, address.data(), text.data(), text.size()) == nullptr) {
        return {};
    }
    return std::string{text.data()};
}

bool TrustedProxies::parse(std::string_view list) noexcept {
    count_ = 0;
    entries_ = {};

    std::string_view rest = list;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view field =
            trim(comma == std::string_view::npos ? rest : rest.substr(0, comma));
        rest = (comma == std::string_view::npos) ? std::string_view{}
                                                 : rest.substr(comma + 1);
        // A trailing comma, or whitespace between two of them, is a typo and not
        // an entry. It is not worth refusing a boot over.
        if (field.empty()) { continue; }
        if (count_ == kMaxEntries) { return false; }

        const std::size_t slash = field.find('/');
        const std::string_view address_text =
            (slash == std::string_view::npos) ? field : field.substr(0, slash);
        const PackedAddress network = pack_address(address_text);
        // "::" parses and is the unspecified address; as a /0 it would trust
        // every peer on earth, which nobody writes on purpose.
        if (is_unspecified(network)) { return false; }

        const bool mapped_v4 =
            std::equal(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), network.begin());
        // A bare address is one host, which in the mapped form is /128 for both
        // families.
        std::uint8_t bits = 128;
        if (slash != std::string_view::npos) {
            const std::string_view suffix = trim(field.substr(slash + 1));
            unsigned quoted = 0;
            const auto [ptr, ec] =
                std::from_chars(suffix.data(), suffix.data() + suffix.size(), quoted);
            if (ec != std::errc{} || ptr != suffix.data() + suffix.size()) { return false; }
            const unsigned family_bits = mapped_v4 ? 32U : 128U;
            if (quoted > family_bits) { return false; }
            // A zero-length prefix trusts every peer in the family, which hands
            // every client the ability to choose its own address and therefore
            // its own rate-limit bucket. "0.0.0.0/0" is not caught by the
            // unspecified check above, because it packs to the v4-mapped form
            // and so is not all zeroes.
            if (quoted == 0) { return false; }
            bits = static_cast<std::uint8_t>(mapped_v4 ? kV4MappedOffsetBits + quoted : quoted);
        }

        entries_[count_] = Entry{.network = network, .bits = bits};
        ++count_;
    }
    return true;
}

bool TrustedProxies::contains(const PackedAddress& address) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (prefix_matches(address, entries_[i].network, entries_[i].bits)) { return true; }
    }
    return false;
}

PackedAddress resolve_client_address(const PackedAddress& peer,
                                     std::string_view forwarded_for,
                                     const TrustedProxies& trusted) noexcept {
    // The application is the edge. Identical to reading peerAddr() and nothing
    // else, which is the only safe reading when no proxy has been declared: the
    // header is then a value any client can set.
    if (trusted.empty()) { return peer; }
    // The header is only evidence when the machine that sent it is one we
    // configured. Otherwise it is attacker-supplied and is ignored ENTIRELY —
    // not merely distrusted for one entry, because a header a client writes is a
    // header a client writes all of.
    if (!trusted.contains(peer)) { return peer; }
    if (forwarded_for.empty()) { return peer; }

    std::string_view scan = forwarded_for;
    if (scan.size() > kScanBytes) { scan = scan.substr(scan.size() - kScanBytes); }

    // Right to left. Each proxy APPENDS, so the rightmost entry was written by
    // the hop nearest us and the leftmost by whoever spoke first — which may be
    // the client, inventing it.
    for (std::size_t examined = 0; examined < kScanEntries && !scan.empty(); ++examined) {
        const std::size_t comma = scan.rfind(',');
        const std::string_view field =
            trim(comma == std::string_view::npos ? scan : scan.substr(comma + 1));
        scan = (comma == std::string_view::npos) ? std::string_view{} : scan.substr(0, comma);

        const PackedAddress candidate = pack_address(field);
        // Unparseable where a trusted proxy was supposed to have written an
        // address. Something upstream is not what we think it is, so STOP rather
        // than keep walking left into values the client controls — continuing
        // would let a client hide the real hop behind one bad entry.
        if (is_unspecified(candidate)) { return peer; }
        if (!trusted.contains(candidate)) { return candidate; }
    }

    // Every hop was a configured proxy, or there were more than we will read.
    // The innermost peer is the last address we have evidence for.
    return peer;
}

}  // namespace anvil::http
