#pragma once

// Rich-text sanitisation.
//
// Every rich-text field an application stores — a section body, a long-form
// note, any payload authored as markup — is server-rendered HTML written by
// staff. Unsanitised, that is stored XSS whose audience is OTHER STAFF ON AN
// AUTHENTICATED ADMIN SESSION: a compromised account holding only the
// narrowest authoring permission escalates to whatever the admin who views its
// output can do.
//
// Four properties, each of which is a decision rather than an implementation
// detail:
//
//   1. SANITISE ON WRITE, STORE THE SANITISED FORM. Sanitising on read means
//      every render pays the cost and any renderer that forgets is a hole. A
//      renderer that re-escapes on output is defence in depth, but that is a
//      second layer, never the first.
//   2. ALLOW-LIST ONLY. An element or attribute not named here does not survive.
//      A deny-list is a list of the attacks that were thought of.
//   3. REJECT, DO NOT STRIP, for the hostile shapes. A body containing
//      `<script`, `<iframe` or an `on*` handler is not a formatting mistake to
//      be cleaned up quietly — it is a staff account doing something no editor
//      produces, and it earns a rejection and an audit row.
//   4. NO REGEX. std::regex is banned on every request path (ENGINEERING_RULES.md §5), and
//      an HTML sanitiser built on backtracking patterns is both the classic
//      ReDoS target and the classic bypass target. This is a single linear pass
//      with an explicit element stack.
//
// The output is well-formed by construction: every element this emits is
// closed, in order, by the writer rather than by trusting the input's own
// closing tags. Unbalanced input therefore cannot leave an element open across
// the end of the field and swallow the markup that follows it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace anvil::input {

enum class HtmlVerdict : std::uint8_t {
    Ok = 0,
    // A script or event-handler construct. The caller rejects the request AND
    // writes an audit record; it does not silently use a cleaned version.
    Hostile,
    // Nesting past the cap. A 5 000-deep <ul> is a stack-exhaustion attempt
    // against every renderer downstream, not a document.
    TooDeep,
    TooLong,
};

[[nodiscard]] constexpr bool is_ok(HtmlVerdict verdict) noexcept {
    return verdict == HtmlVerdict::Ok;
}

struct HtmlPolicy final {
    // The ONLY absolute origin `img[src]` may name, e.g.
    // "https://www.example.com". Site-relative sources are also accepted; an
    // image loaded from anywhere else is a tracking pixel at best and a
    // credential-leaking request at worst.
    std::string_view content_origin;
    // Bounds are in CODE POINTS, so an Arabic body gets the same allowance as
    // an English one. 20 000 is the note-body cap that also
    // bounds how long this may run on a request thread.
    std::size_t      max_code_points = 20000;
    std::size_t      max_depth = 16;
};

class SanitizedHtml;

// Allocation: one output string, reserved to the input's length up front. The
// result is never larger than a small constant factor of the input, because
// every construct that could expand — entities, added rel/target attributes —
// is bounded per element.
[[nodiscard]] SanitizedHtml sanitize_rich_text(std::string_view html,
                                               const HtmlPolicy& policy);

// Markup that went through the allow-list, plus the verdict that says whether
// it survived. The renderer's raw-insertion overload takes ONE of these and
// nothing else (docs/19-server-side-rendering.md §3), which is the whole reason
// the constructor is private: while this was an aggregate,
// `SanitizedHtml{attacker_controlled, HtmlVerdict::Ok}` compiled, and a type
// whose job is to assert something about its contents is decoration if anyone
// who has not done the work can mint one.
//
// Same shape, and the same reason, as identity::CapabilityScope.
class SanitizedHtml final {
public:
    SanitizedHtml() = delete;

    [[nodiscard]] std::string_view html() const noexcept { return html_; }
    [[nodiscard]] HtmlVerdict verdict() const noexcept { return verdict_; }

    // The bytes, moved out, for a caller that is about to store them. Only from
    // an rvalue: a value that has been emptied must not still be emittable, and
    // the ref-qualifier is what makes that a compile error rather than a blank
    // region on a page.
    [[nodiscard]] std::string into_html() && { return std::move(html_); }

private:
    friend SanitizedHtml sanitize_rich_text(std::string_view html,
                                            const HtmlPolicy& policy);

    SanitizedHtml(std::string html, HtmlVerdict verdict) noexcept
        : html_{std::move(html)}, verdict_{verdict} {}

    std::string html_;
    HtmlVerdict verdict_;
};

// Whether an `a[href]` or `img[src]` value is acceptable, exposed because the
// section registry's Url fields need the same rule without the surrounding
// markup (docs/12-sections-cms.md §3 rule 5).
[[nodiscard]] bool is_safe_link_target(std::string_view href) noexcept;
[[nodiscard]] bool is_safe_image_source(std::string_view src,
                                        std::string_view content_origin) noexcept;

}  // namespace anvil::input
