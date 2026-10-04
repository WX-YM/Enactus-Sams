#include "anvil/notifications/webhook.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"

namespace anvil::notifications {
namespace {

constexpr std::size_t kMaxUrlBytes = 2048;
constexpr std::string_view kHttpsScheme = "https://";

constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void append_hex(std::string& out, std::span<const std::uint8_t> bytes) {
    out.reserve(out.size() + (bytes.size() * 2));
    for (const std::uint8_t byte : bytes) {
        out.push_back(kHexDigits[byte >> 4U]);
        out.push_back(kHexDigits[byte & 0x0FU]);
    }
}

void append_decimal(std::string& out, std::int64_t value) {
    // A stack buffer rather than std::to_string: this runs once per delivery, and
    // the whole signing string is one allocation rather than two.
    std::array<char, 20> digits{};
    std::size_t length = 0;
    const bool negative = value < 0;
    // Negated into an UNSIGNED accumulator: -(-2^63) overflows in int64 and
    // signed overflow is undefined behaviour (CLAUDE.md §5).
    auto magnitude = negative ? (~static_cast<std::uint64_t>(value) + 1U)
                              : static_cast<std::uint64_t>(value);
    do {
        digits[length] = static_cast<char>('0' + (magnitude % 10U));
        ++length;
        magnitude /= 10U;
    } while (magnitude != 0U && length < digits.size());

    if (negative) { out.push_back('-'); }
    for (std::size_t i = length; i > 0; --i) { out.push_back(digits[i - 1]); }
}

[[nodiscard]] bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// The host and port of an `https://` URL, without a full URL parser.
//
// Bounded, linear and hand-written, for the reason CLAUDE.md §5 bans std::regex
// on a request path — and because a URL parser that disagrees with the HTTP
// client's own parser is precisely how an SSRF check gets bypassed. What this
// refuses rather than interprets is as important as what it accepts.
struct Authority final {
    std::string_view host;
    std::uint16_t    port;
    bool             well_formed;
};

[[nodiscard]] Authority split_authority(std::string_view url) noexcept {
    std::string_view rest = url.substr(kHttpsScheme.size());
    const std::size_t end = rest.find_first_of("/?");
    if (end != std::string_view::npos) { rest = rest.substr(0, end); }
    if (rest.empty()) { return {{}, 0, false}; }

    // `user:pass@host` is refused rather than parsed. Several HTTP clients read
    // the userinfo form differently from the host that ends up in a log, and a
    // check that disagrees with the client about which host it is checking is
    // not a check.
    if (rest.find('@') != std::string_view::npos) { return {{}, 0, false}; }

    std::string_view host = rest;
    std::uint16_t port = kHttpsPort;

    if (!host.empty() && host.front() == '[') {
        // A bracketed IPv6 literal. The port, if any, follows the bracket.
        const std::size_t close = host.find(']');
        if (close == std::string_view::npos) { return {{}, 0, false}; }
        std::string_view after = host.substr(close + 1);
        host = host.substr(1, close - 1);
        if (!after.empty()) {
            if (after.front() != ':') { return {{}, 0, false}; }
            after.remove_prefix(1);
            rest = after;
        } else {
            rest = {};
        }
        if (!rest.empty()) {
            std::uint32_t parsed = 0;
            for (const char c : rest) {
                if (!is_ascii_digit(c)) { return {{}, 0, false}; }
                parsed = (parsed * 10U) + static_cast<std::uint32_t>(c - '0');
                if (parsed > 65535U) { return {{}, 0, false}; }
            }
            port = static_cast<std::uint16_t>(parsed);
        }
        return {host, port, !host.empty()};
    }

    if (const std::size_t colon = host.rfind(':'); colon != std::string_view::npos) {
        const std::string_view digits = host.substr(colon + 1);
        if (digits.empty()) { return {{}, 0, false}; }
        std::uint32_t parsed = 0;
        for (const char c : digits) {
            if (!is_ascii_digit(c)) { return {{}, 0, false}; }
            parsed = (parsed * 10U) + static_cast<std::uint32_t>(c - '0');
            if (parsed > 65535U) { return {{}, 0, false}; }
        }
        port = static_cast<std::uint16_t>(parsed);
        host = host.substr(0, colon);
    }
    return {host, port, !host.empty()};
}

[[nodiscard]] bool private_v4(const in_addr& address) noexcept {
    const std::uint32_t value = ntohl(address.s_addr);
    const std::uint8_t first = static_cast<std::uint8_t>(value >> 24U);
    const std::uint8_t second = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);

    if (first == 0) { return true; }                      // 0.0.0.0/8, "this host"
    if (first == 10) { return true; }                     // RFC 1918
    if (first == 127) { return true; }                    // loopback
    // 169.254.0.0/16. This is where 169.254.169.254 lives — the cloud metadata
    // address, which is the single highest-value SSRF target there is: it hands
    // out instance credentials to anything that can make a plain GET.
    if (first == 169 && second == 254) { return true; }
    if (first == 172 && second >= 16 && second <= 31) { return true; }   // RFC 1918
    if (first == 192 && second == 168) { return true; }                  // RFC 1918
    if (first == 100 && second >= 64 && second <= 127) { return true; }  // CGNAT, RFC 6598
    if (first == 192 && second == 0) { return true; }                    // 192.0.0.0/24, test nets
    if (first >= 224) { return true; }                    // multicast and reserved
    return false;
}

[[nodiscard]] bool private_v6(const in6_addr& address) noexcept {
    const std::uint8_t* bytes = address.s6_addr;

    bool all_zero_but_last = true;
    for (std::size_t i = 0; i < 15; ++i) {
        if (bytes[i] != 0) {
            all_zero_but_last = false;
            break;
        }
    }
    // ::1 loopback and :: unspecified.
    if (all_zero_but_last && (bytes[15] == 1 || bytes[15] == 0)) { return true; }

    if ((bytes[0] & 0xFEU) == 0xFCU) { return true; }                     // fc00::/7 unique-local
    if (bytes[0] == 0xFE && (bytes[1] & 0xC0U) == 0x80U) { return true; } // fe80::/10 link-local
    if (bytes[0] == 0xFF) { return true; }                                // multicast

    // ::ffff:0:0/96 — an IPv4 address wearing an IPv6 spelling. Without this, a
    // private v4 address reaches the network by being written the other way.
    bool v4_mapped = true;
    for (std::size_t i = 0; i < 10; ++i) {
        if (bytes[i] != 0) {
            v4_mapped = false;
            break;
        }
    }
    if (v4_mapped && bytes[10] == 0xFF && bytes[11] == 0xFF) {
        in_addr embedded{};
        std::memcpy(&embedded.s_addr, bytes + 12, sizeof(embedded.s_addr));
        return private_v4(embedded);
    }
    return false;
}

}  // namespace

std::string signing_string(std::int64_t timestamp_seconds, std::string_view body) {
    std::string out;
    out.reserve(24 + body.size());
    append_decimal(out, timestamp_seconds);
    // The separator is what makes the encoding unambiguous. Without it, a
    // timestamp of 1 and a body of "23..." signs the same bytes as a timestamp of
    // 12 and a body of "3..." — a length-extension of the cheapest kind, against
    // a field an attacker supplies.
    out.push_back('.');
    out.append(body);
    return out;
}

std::string sign(std::span<const std::uint8_t> secret, std::int64_t timestamp_seconds,
                 std::string_view body) {
    const std::string material = signing_string(timestamp_seconds, body);
    const crypto::Digest256 mac = crypto::hmac_sha256(secret, material);

    std::string out;
    out.reserve(kSignatureVersion.size() + 1 + kSignatureHexChars);
    out.append(kSignatureVersion);
    // A scheme prefix, so a future construction can be introduced without a
    // receiver having to guess which one it is looking at.
    out.push_back('=');
    append_hex(out, mac);
    return out;
}

bool verify(std::span<const std::uint8_t> secret, std::string_view signature,
            std::int64_t timestamp_seconds, std::string_view body, db::TimeMs now) noexcept {
    // The window first, because it is the cheap half and because a replay that
    // carries a genuinely valid signature is exactly what it exists to stop.
    const std::int64_t now_seconds = now.time_since_epoch().count() / 1000;
    const std::int64_t skew = now_seconds - timestamp_seconds;
    const std::int64_t tolerance = kReplayTolerance.count();
    // Both directions. A timestamp in the future is a clock that disagrees, and
    // accepting an unbounded one hands an attacker a signature that stays valid
    // for as long as they chose.
    if (skew > tolerance || skew < -tolerance) { return false; }

    const std::string expected = sign(secret, timestamp_seconds, body);
    // Constant time over the WHOLE value, prefix included. Comparing the prefix
    // separately with == would be harmless on its own and is not worth the second
    // code path that could later be got wrong.
    return crypto::secure_equal(
        std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(expected.data()),
                                      expected.size()},
        std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(signature.data()),
                                      signature.size()});
}

WebhookSecret generate_secret() { return crypto::random_array<kWebhookSecretBytes>(); }

bool is_private_address(std::string_view host) noexcept {
    // inet_pton needs a terminated string and the caller's view may not be one.
    // A fixed buffer rather than a std::string: the longest textual IPv6 address
    // is 45 characters, so anything longer is not an address literal at all and
    // the copy has nowhere to grow.
    std::array<char, 64> buffer{};
    if (host.empty() || host.size() >= buffer.size()) { return false; }
    std::copy(host.begin(), host.end(), buffer.begin());

    in6_addr v6{};
    if (inet_pton(AF_INET6, buffer.data(), &v6) == 1) { return private_v6(v6); }
    in_addr v4{};
    if (inet_pton(AF_INET, buffer.data(), &v4) == 1) { return private_v4(v4); }
    // Not a literal. A HOSTNAME is not decidable here — it has to be re-checked
    // after resolution, which is the HTTP client's job and is why this function is
    // exposed rather than kept private to check_webhook_url.
    return false;
}

UrlVerdict check_webhook_url(std::string_view url) noexcept {
    if (url.empty() || url.size() > kMaxUrlBytes) { return UrlVerdict::Malformed; }
    // A fragment never reaches a server, so one in a webhook URL means the
    // operator pasted something they did not mean to — including, sometimes, a
    // credential that a naive client would send and a careful one would strip.
    if (url.find('#') != std::string_view::npos) { return UrlVerdict::Malformed; }
    for (const char c : url) {
        // Control characters and non-ASCII. A newline is request splitting; a
        // non-ASCII host needs IDNA, and two implementations that disagree about
        // how to encode one disagree about which host they are talking to.
        if (static_cast<unsigned char>(c) <= 0x20U || static_cast<unsigned char>(c) >= 0x7FU) {
            return UrlVerdict::Malformed;
        }
    }
    // `https://` only. The signature proves who sent a delivery, not that nobody
    // else read it — over plaintext the body is public and so is anything the
    // notification contains.
    if (url.size() <= kHttpsScheme.size() || url.compare(0, kHttpsScheme.size(), kHttpsScheme) != 0) {
        return UrlVerdict::NotHttps;
    }

    const Authority authority = split_authority(url);
    if (!authority.well_formed) { return UrlVerdict::Malformed; }
    if (authority.port != kHttpsPort && authority.port != kAlternateHttpsPort) {
        return UrlVerdict::PortNotAllowed;
    }
    if (is_private_address(authority.host)) { return UrlVerdict::NotPublic; }
    return UrlVerdict::Ok;
}

}  // namespace anvil::notifications
