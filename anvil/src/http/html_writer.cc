#include "anvil/http/html_writer.h"

#include <cstdint>

namespace anvil::http {
namespace {

// The attribute set: the text set plus both quote characters. One byte at a
// time, so `&` is handled before anything it could be part of and no
// double-escaping window exists.
void append_attr_escaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
            case '&':  out.append("&amp;"); break;
            case '<':  out.append("&lt;"); break;
            case '>':  out.append("&gt;"); break;
            case '"':  out.append("&quot;"); break;
            case '\'': out.append("&#x27;"); break;
            default:   out.push_back(c); break;
        }
    }
}

// An attribute name is a literal at every call site; this is what fails closed
// if one ever is not. Deliberately narrower than the HTML grammar allows —
// letters, digits and the hyphen cover every attribute anything here emits, and
// a rule nobody has to read the specification to verify is a rule that stays
// correct.
[[nodiscard]] bool is_attribute_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) { return false; }
    for (const char c : name) {
        const auto byte = static_cast<std::uint8_t>(c);
        const bool allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                             (byte >= '0' && byte <= '9') || byte == '-';
        if (!allowed) { return false; }
    }
    return true;
}

}  // namespace

void append_html_text(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
            case '&': out.append("&amp;"); break;
            case '<': out.append("&lt;"); break;
            case '>': out.append("&gt;"); break;
            default:  out.push_back(c); break;
        }
    }
}

void append_html_attr(std::string& out, std::string_view name, std::string_view value) {
    if (!is_attribute_name(name)) { return; }
    out.push_back(' ');
    out.append(name);
    out.append("=\"");
    append_attr_escaped(out, value);
    out.push_back('"');
}

bool append_url_attr(std::string& out, std::string_view name, std::string_view url) {
    if (!is_attribute_name(name) || !input::is_safe_link_target(url)) { return false; }
    append_html_attr(out, name, url);
    return true;
}

void append_sanitized(std::string& out, const input::SanitizedHtml& safe) {
    if (!input::is_ok(safe.verdict())) {
        out.append(kWithheldContent);
        return;
    }
    out.append(safe.html());
}

}  // namespace anvil::http
