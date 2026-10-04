#include "anvil/input/html.h"

#include <array>
#include <vector>

#include "anvil/i18n/utf8.h"

namespace anvil::input {
namespace {

// --- character classes ------------------------------------------------------
//
// ASCII only, and deliberately not <cctype>: those functions consult the C
// locale, which is global mutable state that another library can change, and
// they have implementation-defined behaviour on bytes above 0x7F — which every
// Arabic character consists of.

[[nodiscard]] constexpr bool is_ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

[[nodiscard]] constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive over ASCII. Tag and attribute names are ASCII by definition;
// a name containing a byte above 0x7F simply will not match any allow-list
// entry, which is the correct outcome.
[[nodiscard]] bool equals_ascii_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) { return false; }
    }
    return true;
}

[[nodiscard]] bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && equals_ascii_ci(text.substr(0, prefix.size()), prefix);
}

// --- allow-lists ------------------------------------------------------------
//
// All three tables are constexpr and live in .rodata: shared across every
// thread and every request, with no runtime construction (CLAUDE.md §2.1).

constexpr std::array<std::string_view, 14> kAllowedElements{
    {"p", "br", "strong", "em", "u", "ul", "ol", "li", "h2", "h3", "blockquote", "a", "figure",
     "figcaption"}};

// `img` is allowed but is handled separately: it is void, and its src carries
// the origin restriction that no other attribute has.
constexpr std::string_view kImageElement = "img";

// Elements whose mere presence is an attack, not a formatting choice. `svg` and
// `math` are here because both introduce foreign content, where HTML's parsing
// rules change and `<svg><script>` becomes live again.
constexpr std::array<std::string_view, 11> kHostileElements{
    {"script", "iframe", "object", "embed", "applet", "style", "link", "meta", "base", "svg",
     "math"}};

constexpr std::array<std::string_view, 2> kVoidElements{{"br", "img"}};

template <std::size_t N>
[[nodiscard]] bool table_has(const std::array<std::string_view, N>& table,
                            std::string_view name) noexcept {
    for (const std::string_view entry : table) {
        if (equals_ascii_ci(entry, name)) { return true; }
    }
    return false;
}

// The allow-list entry a matched name corresponds to. Emitting THAT rather than
// the input's own bytes is what normalises `<STRONG>` to `<strong>` without a
// second lowercasing pass, and it guarantees the emitted name is one of the
// fourteen this file knows about rather than whatever the input contained.
[[nodiscard]] std::string_view canonical_element(std::string_view name) noexcept {
    if (equals_ascii_ci(name, kImageElement)) { return kImageElement; }
    for (const std::string_view entry : kAllowedElements) {
        if (equals_ascii_ci(entry, name)) { return entry; }
    }
    return {};
}

// --- entity handling --------------------------------------------------------
//
// A bare `&` is escaped; a recognised reference is copied through unchanged.
// Copying an UNRECOGNISED named reference through would be a bypass: browsers
// resolve a long tail of legacy names, and the sanitiser must not depend on
// agreeing with that list.
constexpr std::array<std::string_view, 6> kNamedEntities{
    {"amp", "lt", "gt", "quot", "apos", "nbsp"}};

// Returns the length of the reference INCLUDING '&' and ';', or 0 when the text
// at `pos` is not one this sanitiser is willing to preserve.
[[nodiscard]] std::size_t entity_length(std::string_view text, std::size_t pos) noexcept {
    const std::size_t max_len = 12;
    const std::size_t end = text.size() < pos + max_len ? text.size() : pos + max_len;
    if (pos + 1 >= end) { return 0; }

    std::size_t i = pos + 1;
    if (text[i] == '#') {
        ++i;
        const bool hex = i < end && (text[i] == 'x' || text[i] == 'X');
        if (hex) { ++i; }
        const std::size_t digits_start = i;
        while (i < end && (is_ascii_digit(text[i]) ||
                           (hex && ((lower(text[i]) >= 'a') && (lower(text[i]) <= 'f'))))) {
            ++i;
        }
        if (i == digits_start || i >= end || text[i] != ';') { return 0; }
        return i + 1 - pos;
    }

    const std::size_t name_start = i;
    while (i < end && is_ascii_alpha(text[i])) { ++i; }
    if (i >= end || text[i] != ';') { return 0; }
    const std::string_view name = text.substr(name_start, i - name_start);
    for (const std::string_view entity : kNamedEntities) {
        if (equals_ascii_ci(entity, name)) { return i + 1 - pos; }
    }
    return 0;
}

void append_text(std::string& out, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '&') {
            const std::size_t length = entity_length(text, i);
            if (length != 0) {
                out.append(text.substr(i, length));
                i += length - 1;
            } else {
                out.append("&amp;");
            }
        } else if (c == '<') {
            out.append("&lt;");
        } else if (c == '>') {
            out.append("&gt;");
        } else {
            out.push_back(c);
        }
    }
}

void append_attribute_value(std::string& out, std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        switch (c) {
            case '&': {
                // An ALREADY-ESCAPED reference is copied through unchanged, for
                // the same reason text content does it: a renderer that
                // re-sanitises stored content as defence in depth would
                // otherwise turn `&amp;` into `&amp;amp;` on every render — a
                // value that grows without bound and displays wrongly after the
                // first pass.
                const std::size_t length = entity_length(value, i);
                if (length != 0) {
                    out.append(value.substr(i, length));
                    i += length - 1;
                } else {
                    out.append("&amp;");
                }
                break;
            }
            case '<':  out.append("&lt;"); break;
            case '>':  out.append("&gt;"); break;
            case '"':  out.append("&quot;"); break;
            case '\'': out.append("&#x27;"); break;
            default:
                // Control bytes are dropped rather than escaped: nothing
                // legitimate carries them inside an attribute, and a raw CR or
                // LF in an href is how a value survives one parser and changes
                // meaning in another.
                if (static_cast<std::uint8_t>(c) >= 0x20U) { out.push_back(c); }
                break;
        }
    }
}

// --- attributes -------------------------------------------------------------

struct Attribute final {
    std::string_view name;
    std::string_view value;
};

[[nodiscard]] bool is_event_handler(std::string_view name) noexcept {
    return name.size() > 2 && starts_with_ci(name, "on");
}

// Whitespace and control bytes are skipped during the compare: `java\tscript:`
// and ` javascript:` are both live in a browser, and a naive prefix test on the
// raw value misses both.
//
// Skipped in place rather than folded into a copy first. The copy was a heap
// allocation inside a noexcept predicate, so an allocation failure was
// std::terminate, and every validator that delegates its scheme decision here
// inherited an allocation per call on its reject path.
[[nodiscard]] bool folded_starts_with(std::string_view value, std::string_view scheme) noexcept {
    std::size_t matched = 0;
    for (const char c : value) {
        if (matched == scheme.size()) { break; }
        if (static_cast<std::uint8_t>(c) <= 0x20U) { continue; }
        if (lower(c) != scheme[matched]) { return false; }
        ++matched;
    }
    return matched == scheme.size();
}

[[nodiscard]] bool has_dangerous_scheme(std::string_view value) noexcept {
    return folded_starts_with(value, "javascript:") || folded_starts_with(value, "vbscript:") ||
           folded_starts_with(value, "data:");
}

[[nodiscard]] bool is_valid_lang(std::string_view value) noexcept {
    if (value.empty() || value.size() > 16) { return false; }
    for (const char c : value) {
        if (!is_ascii_alpha(c) && !is_ascii_digit(c) && c != '-') { return false; }
    }
    return true;
}

}  // namespace

bool is_safe_link_target(std::string_view href) noexcept {
    if (href.empty() || href.size() > 2048) { return false; }
    if (has_dangerous_scheme(href)) { return false; }
    // Protocol-relative: `//evil.test/x` inherits the page's scheme and is an
    // absolute off-site link wearing a relative-looking prefix.
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/') { return false; }
    if (href[0] == '/' || href[0] == '#') { return true; }
    return starts_with_ci(href, "https://") || starts_with_ci(href, "mailto:");
}

bool is_safe_image_source(std::string_view src, std::string_view content_origin) noexcept {
    if (src.empty() || src.size() > 2048) { return false; }
    if (has_dangerous_scheme(src)) { return false; }
    if (src.size() >= 2 && src[0] == '/' && src[1] == '/') { return false; }
    if (src[0] == '/') { return true; }
    // Origin match is a prefix compare followed by a '/' check, so
    // `https://www.example.com.evil.test/x` does not pass as
    // `https://www.example.com`.
    if (content_origin.empty() || src.size() <= content_origin.size()) { return false; }
    return src.compare(0, content_origin.size(), content_origin) == 0 &&
           src[content_origin.size()] == '/';
}

SanitizedHtml sanitize_rich_text(std::string_view html, const HtmlPolicy& policy) {
    if (!i18n::within_code_point_bounds(html, 0, policy.max_code_points)) {
        return SanitizedHtml{std::string{}, HtmlVerdict::TooLong};
    }

    std::string out;
    out.reserve(html.size() + (html.size() / 4));

    // Only elements actually EMITTED are pushed. An element that was dropped
    // (unknown, or an <a> whose href failed) leaves no entry, so its closing
    // tag finds nothing to match and is dropped too — which is what keeps the
    // output balanced without trusting the input's balance.
    std::vector<std::string_view> open;
    open.reserve(policy.max_depth);

    std::size_t i = 0;
    std::size_t text_start = 0;

    const auto flush_text = [&](std::size_t end) {
        if (end > text_start) { append_text(out, html.substr(text_start, end - text_start)); }
    };

    while (i < html.size()) {
        if (html[i] != '<') {
            ++i;
            continue;
        }
        flush_text(i);

        // Comments and declarations carry no renderable content and are the
        // classic place to hide a conditional-comment script. Dropped whole.
        if (html.compare(i, 4, "<!--") == 0) {
            const std::size_t end = html.find("-->", i + 4);
            i = (end == std::string_view::npos) ? html.size() : end + 3;
            text_start = i;
            continue;
        }
        if (i + 1 < html.size() && html[i + 1] == '!') {
            const std::size_t end = html.find('>', i);
            i = (end == std::string_view::npos) ? html.size() : end + 1;
            text_start = i;
            continue;
        }

        const bool closing = (i + 1 < html.size() && html[i + 1] == '/');
        std::size_t cursor = i + (closing ? 2 : 1);
        if (cursor >= html.size() || !is_ascii_alpha(html[cursor])) {
            // A bare `<` that starts nothing is text, and escaping it here is
            // what stops `a < b` becoming an element in the browser.
            out.append("&lt;");
            ++i;
            text_start = i;
            continue;
        }

        const std::size_t name_start = cursor;
        while (cursor < html.size() && (is_ascii_alpha(html[cursor]) || is_ascii_digit(html[cursor]))) {
            ++cursor;
        }
        const std::string_view name = html.substr(name_start, cursor - name_start);

        if (table_has(kHostileElements, name)) {
            return SanitizedHtml{std::string{}, HtmlVerdict::Hostile};
        }

        // --- attributes ------------------------------------------------------
        std::vector<Attribute> attributes;
        bool self_closing = false;
        while (cursor < html.size() && html[cursor] != '>') {
            if (is_space(html[cursor])) {
                ++cursor;
                continue;
            }
            if (html[cursor] == '/') {
                self_closing = true;
                ++cursor;
                continue;
            }
            const std::size_t attr_start = cursor;
            while (cursor < html.size() && !is_space(html[cursor]) && html[cursor] != '=' &&
                   html[cursor] != '>' && html[cursor] != '/') {
                ++cursor;
            }
            const std::string_view attr_name = html.substr(attr_start, cursor - attr_start);

            std::string_view attr_value;
            while (cursor < html.size() && is_space(html[cursor])) { ++cursor; }
            if (cursor < html.size() && html[cursor] == '=') {
                ++cursor;
                while (cursor < html.size() && is_space(html[cursor])) { ++cursor; }
                if (cursor < html.size() && (html[cursor] == '"' || html[cursor] == '\'')) {
                    const char quote = html[cursor];
                    ++cursor;
                    const std::size_t value_start = cursor;
                    while (cursor < html.size() && html[cursor] != quote) { ++cursor; }
                    attr_value = html.substr(value_start, cursor - value_start);
                    if (cursor < html.size()) { ++cursor; }
                } else {
                    const std::size_t value_start = cursor;
                    while (cursor < html.size() && !is_space(html[cursor]) &&
                           html[cursor] != '>') {
                        ++cursor;
                    }
                    attr_value = html.substr(value_start, cursor - value_start);
                }
            }

            // An event handler is the single clearest signal that this is not
            // an editor's output. Reject the whole body rather than cleaning it
            // — the caller writes an audit row.
            if (is_event_handler(attr_name)) {
                return SanitizedHtml{std::string{}, HtmlVerdict::Hostile};
            }
            if (!attr_name.empty()) { attributes.push_back(Attribute{attr_name, attr_value}); }
        }
        if (cursor < html.size()) { ++cursor; }   // consume '>'
        i = cursor;
        text_start = i;

        const std::string_view element = canonical_element(name);
        const bool is_image = element == kImageElement;
        const bool allowed = !element.empty();

        // --- closing tag -----------------------------------------------------
        if (closing) {
            if (!allowed) { continue; }
            // Close down to the matching element, closing anything left open
            // above it. Input like `<ul><li>x</ul>` therefore still produces
            // balanced output.
            std::size_t depth = open.size();
            while (depth > 0 && open[depth - 1] != element) { --depth; }
            if (depth == 0) { continue; }
            while (open.size() >= depth) {
                out.append("</");
                out.append(open.back());
                out.push_back('>');
                open.pop_back();
            }
            continue;
        }

        if (!allowed) {
            // Unknown but harmless: the element disappears and its children are
            // kept. `<div>text</div>` becomes `text`.
            continue;
        }

        // --- element-specific attribute policy --------------------------------
        std::string_view href;
        std::string_view src;
        std::string_view alt;
        std::string_view dir;
        std::string_view lang;
        for (const Attribute& attribute : attributes) {
            if (equals_ascii_ci(attribute.name, "href")) {
                href = attribute.value;
            } else if (equals_ascii_ci(attribute.name, "src")) {
                src = attribute.value;
            } else if (equals_ascii_ci(attribute.name, "alt")) {
                alt = attribute.value;
            } else if (equals_ascii_ci(attribute.name, "dir")) {
                dir = attribute.value;
            } else if (equals_ascii_ci(attribute.name, "lang")) {
                lang = attribute.value;
            }
            // Everything else — style, srcset, class, id, data-* — is dropped
            // silently. They are formatting noise from a paste, not an attack.
        }

        const bool is_anchor = element == "a";
        if (is_anchor && !href.empty() && has_dangerous_scheme(href)) {
            return SanitizedHtml{std::string{}, HtmlVerdict::Hostile};
        }
        if (is_image && !src.empty() && has_dangerous_scheme(src)) {
            return SanitizedHtml{std::string{}, HtmlVerdict::Hostile};
        }
        if (is_anchor && !is_safe_link_target(href)) {
            continue;   // the anchor goes, its text stays
        }
        if (is_image && !is_safe_image_source(src, policy.content_origin)) {
            continue;
        }

        const bool is_void = table_has(kVoidElements, element);
        if (!is_void && open.size() >= policy.max_depth) {
            return SanitizedHtml{std::string{}, HtmlVerdict::TooDeep};
        }

        out.push_back('<');
        out.append(element);
        if (is_anchor) {
            out.append(" href=\"");
            append_attribute_value(out, href);
            out.push_back('"');
            // Off-site links get the full set unconditionally. `noopener` is the
            // one that matters: without it the opened page holds window.opener
            // and can navigate the admin tab to a phishing page.
            if (starts_with_ci(href, "https://")) {
                out.append(R"( rel="noopener noreferrer nofollow" target="_blank")");
            }
        }
        if (is_image) {
            out.append(" src=\"");
            append_attribute_value(out, src);
            out.append("\" alt=\"");
            append_attribute_value(out, alt);
            out.push_back('"');
            // Explicit, so a preview and the public page lay out identically
            // even before the image loads, and so a slow media origin cannot
            // reflow a rendered note.
            out.append(R"( loading="lazy")");
        }
        // dir and lang are allowed on every element: mixed EN/AR prose is the
        // normal case here, and without them a paragraph of Arabic containing a
        // Latin brand name renders with the punctuation in the wrong place.
        if (equals_ascii_ci(dir, "ltr") || equals_ascii_ci(dir, "rtl") ||
            equals_ascii_ci(dir, "auto")) {
            out.append(" dir=\"");
            append_attribute_value(out, dir);
            out.push_back('"');
        }
        if (is_valid_lang(lang)) {
            out.append(" lang=\"");
            append_attribute_value(out, lang);
            out.push_back('"');
        }

        out.push_back('>');
        if (is_void) { continue; }
        if (self_closing) {
            // A self-closed non-void element is just a start tag in HTML;
            // emitting the close immediately keeps the output balanced.
            out.append("</");
            out.append(element);
            out.push_back('>');
            continue;
        }
        open.push_back(element);
    }

    flush_text(html.size());
    while (!open.empty()) {
        out.append("</");
        out.append(open.back());
        out.push_back('>');
        open.pop_back();
    }

    return SanitizedHtml{std::move(out), HtmlVerdict::Ok};
}

}  // namespace anvil::input
