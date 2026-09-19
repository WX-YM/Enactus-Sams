#include "anvil/http/content_headers.h"

#include <cstdint>

#include "anvil/http/html_writer.h"

namespace anvil::http {
namespace {

// Not "is this a well-formed origin" — that is config::require_origin_shape's
// job, at boot, where a bad one is a startup failure rather than a page served
// under a policy nobody meant. This is narrower and answers one question: can
// these bytes end the directive they are inside?
[[nodiscard]] bool is_policy_safe_origin(std::string_view origin) noexcept {
    if (origin.empty() || origin.size() > 255) { return false; }
    for (const char c : origin) {
        const auto byte = static_cast<std::uint8_t>(c);
        // Everything at or below 0x20 is whitespace or a control byte, both of
        // which separate tokens inside a directive; 0x7F up is not an origin.
        if (byte <= 0x20U || byte >= 0x7FU) { return false; }
        if (c == ';' || c == ',' || c == '\'' || c == '"' || c == '\\') { return false; }
    }
    return true;
}

}  // namespace

std::string content_security_policy(std::string_view content_origin) {
    const bool usable = is_policy_safe_origin(content_origin);

    std::string policy;
    policy.reserve(160 + content_origin.size());
    policy.append("default-src 'none'; img-src ");
    if (usable) {
        policy.append(content_origin);
    } else {
        policy.append("'none'");
    }
    policy.append("; style-src 'self'; font-src 'self'; frame-ancestors 'none'; ");
    policy.append("base-uri 'none'; form-action 'none'");
    return policy;
}

void apply_content_headers(drogon::HttpResponse& response, std::string_view policy) {
    // setContentTypeString rather than setContentTypeCode: the code form
    // appends its own charset parameter, and the charset on a page assembled
    // from UTF-8 is not a detail to leave to a library default.
    response.setContentTypeString(kHtmlContentType);

    response.addHeader("Content-Security-Policy", std::string{policy});
    response.addHeader("Cache-Control", "private, no-store");
    response.addHeader("X-Robots-Tag", "noindex, nofollow");
    response.addHeader("Referrer-Policy", "no-referrer");
    response.addHeader("X-Content-Type-Options", "nosniff");
}

}  // namespace anvil::http
