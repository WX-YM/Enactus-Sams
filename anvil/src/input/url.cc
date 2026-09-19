#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "anvil/input/fields.h"

// URL validation as an allow-list, because a deny-list of dangerous schemes is
// always incomplete: `javascript:`, `data:` and `vbscript:` are the three
// everyone remembers, and `blob:`, `filesystem:`, `intent:` and whatever the
// next browser ships are the ones that get through.
//
// Two profiles. A Link is something a browser follows, so the question is only
// "can this navigate somewhere dangerous". A ServerFetch is something THIS
// process will request, so it is an SSRF surface: the cloud metadata endpoint
// at 169.254.169.254 is one HTTP GET away from credentials.
//
// What this file cannot do is the half of SSRF defence that matters most: a
// hostname resolving to a public address at validation time and a private one
// at connect time. That check belongs to the HTTP client, which is the only
// component that knows the address it actually connected to, and it is
// specified with the webhook transport in docs/11-notifications.md §7.6.

namespace anvil::input {
namespace {

[[nodiscard]] constexpr bool is_ascii_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool starts_with_scheme(std::string_view url, std::string_view scheme) noexcept {
    if (url.size() < scheme.size()) { return false; }
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        if (lower(url[i]) != scheme[i]) { return false; }
    }
    return true;
}

// A host that is all digits and dots is an IPv4 literal however it is spelled,
// including the decimal (2130706433) and octal (0177.0.0.1) forms that every
// naive "starts with 127." check misses.
[[nodiscard]] bool looks_like_ip_literal(std::string_view host) noexcept {
    if (host.empty()) { return false; }
    if (host.front() == '[') { return true; }   // IPv6 literal
    for (const char c : host) {
        if (!is_ascii_digit(c) && c != '.' && c != 'x' && c != 'X') { return false; }
    }
    return true;
}

// Names that resolve to the loopback or metadata address on essentially every
// deployment. The address-level check happens at connect time; this is the
// cheap first pass that catches the obvious attempt.
[[nodiscard]] bool is_reserved_name(std::string_view host) noexcept {
    constexpr std::array<std::string_view, 4> kReserved{"localhost", "metadata",
                                                        "metadata.google.internal",
                                                        "instance-data"};
    for (const std::string_view reserved : kReserved) {
        if (host.size() != reserved.size()) { continue; }
        bool same = true;
        for (std::size_t i = 0; i < host.size(); ++i) {
            if (lower(host[i]) != reserved[i]) {
                same = false;
                break;
            }
        }
        if (same) { return true; }
    }
    // Anything under .local or .internal is by definition not a public target.
    return host.ends_with(".local") || host.ends_with(".internal");
}

[[nodiscard]] Reason check_host(std::string_view host, UrlUse use) noexcept {
    if (host.empty()) { return Reason::BadFormat; }
    // Credentials in the authority are how a link is made to read as one origin
    // and resolve to another: https://www.bank.com@evil.test/.
    if (host.find('@') != std::string_view::npos) { return Reason::NotAllowed; }

    // An IPv6 literal is bracketed and contains colons, so it must be judged
    // before the port is stripped — otherwise "[::1]" splits into "[".
    if (host.front() == '[') { return Reason::NotAllowed; }

    std::string_view name = host;
    if (const std::size_t colon = name.find(':'); colon != std::string_view::npos) {
        name = name.substr(0, colon);   // strip the port; the host is what is judged
    }
    if (name.empty()) { return Reason::BadFormat; }

    for (const char c : name) {
        // Non-ASCII means the host has not been IDNA-encoded, and an
        // un-encoded host is exactly where a homoglyph domain hides.
        if (static_cast<unsigned char>(c) > 0x7F) { return Reason::BadCharset; }
        if (!is_ascii_letter(c) && !is_ascii_digit(c) && c != '-' && c != '.') {
            return Reason::BadFormat;
        }
    }
    if (looks_like_ip_literal(name)) { return Reason::NotAllowed; }
    // Before the TLD rule, because "localhost" has no dot and the reason it is
    // refused is what it resolves to, not how it is spelled.
    if (use == UrlUse::ServerFetch && is_reserved_name(name)) { return Reason::NotAllowed; }
    if (name.find('.') == std::string_view::npos) { return Reason::BadFormat; }   // no TLD

    return Reason::Ok;
}

}  // namespace

Reason check_url(std::string_view url, UrlUse use) noexcept {
    if (url.empty()) { return Reason::Required; }
    if (url.size() > 2048) { return Reason::TooLong; }

    for (const char c : url) {
        // A control character in a URL is a header-splitting attempt the moment
        // the value is echoed into a Location or a Link header.
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) {
            return Reason::BadCharset;
        }
    }

    // A fragment reference resolves against the document it is in. It carries
    // no scheme, names no host, and cannot navigate off-origin under any
    // encoding — it is the narrowest link shape there is, narrower than the
    // site-relative form allowed below.
    //
    // It is also the ONLY shape that names a page of this site: the public site
    // is a hash-routed SPA, so "the café" is `#/cafe` and `/cafe` is a path
    // Nginx answers with index.html, landing the reader on the home page. Until
    // this existed, `home.hero.cta_href` could hold a link to another website
    // or to a page that does not exist, and nothing else.
    if (url.front() == '#') {
        // A server must never be asked to fetch a fragment: there is no
        // resource there to fetch.
        return use == UrlUse::ServerFetch ? Reason::NotAllowed : Reason::Ok;
    }

    // A site-relative link is the common case in section content and cannot
    // navigate off-origin. "//host/path" is protocol-relative and is NOT
    // relative in any useful sense, so it is excluded.
    if (url.front() == '/') {
        if (use == UrlUse::ServerFetch) { return Reason::NotAllowed; }
        return url.starts_with("//") ? Reason::NotAllowed : Reason::Ok;
    }

    if (starts_with_scheme(url, "mailto:")) {
        if (use == UrlUse::ServerFetch) { return Reason::NotAllowed; }
        return check_email(url.substr(7));
    }

    if (!starts_with_scheme(url, "https://")) {
        // Everything else, named or not: javascript:, data:, vbscript:, file:,
        // and plain http:, which would carry the request in clear text.
        return Reason::NotAllowed;
    }

    const std::string_view rest = url.substr(8);
    const std::size_t authority_end = rest.find_first_of("/?#");
    const std::string_view authority =
        authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);

    return check_host(authority, use);
}

}  // namespace anvil::input
