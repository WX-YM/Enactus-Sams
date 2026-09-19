#include "anvil/accesscontrol/cookies.h"

namespace anvil::accesscontrol {
namespace {

[[nodiscard]] constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

}  // namespace

std::string_view read_cookie(std::string_view cookie_header, std::string_view name) noexcept {
    if (cookie_header.size() > kMaxCookieHeaderBytes || name.empty()) { return {}; }

    std::size_t at = 0;
    while (at < cookie_header.size()) {
        while (at < cookie_header.size() && is_space(cookie_header[at])) { ++at; }

        const std::size_t name_start = at;
        while (at < cookie_header.size() && cookie_header[at] != '=' &&
               cookie_header[at] != ';') {
            ++at;
        }
        // A pair with no '=' is not a cookie. Skip it rather than treating the
        // remainder as a value.
        if (at >= cookie_header.size() || cookie_header[at] != '=') {
            while (at < cookie_header.size() && cookie_header[at] != ';') { ++at; }
            if (at < cookie_header.size()) { ++at; }
            continue;
        }

        const std::string_view key =
            cookie_header.substr(name_start, at - name_start);
        ++at;   // step over '='

        const std::size_t value_start = at;
        while (at < cookie_header.size() && cookie_header[at] != ';') { ++at; }
        const std::string_view value = cookie_header.substr(value_start, at - value_start);
        if (at < cookie_header.size()) { ++at; }   // step over ';'

        // Case-sensitive: cookie names are case-sensitive per RFC 6265, and the
        // `__Host-` prefix check browsers apply is case-sensitive too. Matching
        // loosely here would accept a `__host-at` a subdomain is allowed to set.
        if (key == name) { return value; }
    }
    return {};
}

}  // namespace anvil::accesscontrol
