// The escaping half is the security half, so it is asserted per CONTEXT rather
// than in aggregate: a writer that is right about element text and wrong about
// attribute values is wrong, and an aggregate assertion over both hides it.
//
// The raw-insertion rule is asserted by COMPILE failures rather than by a
// runtime test, and there are two of them because one alone proves nothing:
// that append_sanitized cannot be handed a string, and that a SanitizedHtml
// cannot be minted from one. The second is the one that matters — without a
// closed constructor the first is a speed bump rather than a guarantee.

#include "anvil/http/html_writer.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "anvil/input/html.h"

#if ANVIL_HAS_EGY
#include "anvil/locale_egy/phone_html.h"
#endif

namespace anvil::http {
namespace {

constexpr std::string_view kContentOrigin = "https://www.example.test";

[[nodiscard]] input::SanitizedHtml sanitize(std::string_view html,
                                            std::size_t max_code_points = 20000) {
    return input::sanitize_rich_text(html,
                                     input::HtmlPolicy{kContentOrigin, max_code_points, 16});
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// --- element text ----------------------------------------------------------

TEST(HtmlWriter, TextEscapingLeavesNothingThatCanOpenAnElement) {
    // A NUL in the middle, because the parameter is a string_view: a writer
    // taking const char* would silently emit the prefix and call it a day.
    const std::string hostile = std::string{"<script>x</script> & \" ' "} + '\0' + "tail";

    std::string out;
    append_html_text(out, hostile);

    EXPECT_FALSE(contains(out, "<"));
    EXPECT_FALSE(contains(out, ">"));
    EXPECT_TRUE(contains(out, "&lt;script&gt;"));
    EXPECT_TRUE(contains(out, "&amp;"));
    // Everything after the NUL is still there, which is the property the NUL is
    // in this input to prove.
    EXPECT_TRUE(contains(out, std::string{'\0'} + "tail"));
}

TEST(HtmlWriter, TextEscapingLeavesTheQuoteCharactersAlone) {
    // Not an omission: in element text a quote is an ordinary character, and
    // the attribute writer emits its own quotes, so no call shape lands this
    // inside an attribute. Asserted so a later "tighten it up" has to argue.
    std::string out;
    append_html_text(out, R"(it's "fine")");
    EXPECT_EQ(out, R"(it's "fine")");
}

TEST(HtmlWriter, AmpersandIsEscapedBeforeWhatFollowsIt) {
    // The double-escaping window: an escaper that replaced `<` first and `&`
    // second would turn `<` into `&amp;lt;`. One byte at a time closes it.
    std::string out;
    append_html_text(out, "&lt;");
    EXPECT_EQ(out, "&amp;lt;");
}

// --- attribute values ------------------------------------------------------

TEST(HtmlWriter, AnAttributeValueStaysInsideTheQuotesTheWriterEmits) {
    std::string out;
    append_html_attr(out, "title", R"(" onmouseover="alert(1))");
    EXPECT_EQ(out, R"HTML( title="&quot; onmouseover=&quot;alert(1)")HTML");
}

TEST(HtmlWriter, TheAttributeSetIsTheTextSetPlusBothQuoteCharacters) {
    std::string out;
    append_html_attr(out, "data-x", R"(<&>"')");
    EXPECT_EQ(out, R"( data-x="&lt;&amp;&gt;&quot;&#x27;")");
}

TEST(HtmlWriter, AnAttributeNameOutsideTheAllowedSetEmitsNothing) {
    // The one shape that would otherwise be an injection: a name built from
    // request data. Escaping the name would not help — `onload` is a perfectly
    // well-formed attribute name — so it fails closed instead.
    for (const std::string_view name : {std::string_view{"x onload=alert(1)"},
                                        std::string_view{"a\"b"}, std::string_view{""},
                                        std::string_view{"a>b"}}) {
        std::string out;
        append_html_attr(out, name, "value");
        EXPECT_TRUE(out.empty()) << name;
    }
}

// --- URL-valued attributes -------------------------------------------------

TEST(HtmlWriter, AUrlAttributeEmitsNothingForWhatTheLinkRuleRejects) {
    // Asserted against is_safe_link_target's OWN answer rather than against a
    // fresh list: the point of delegating is that there is one list, and a test
    // carrying a second copy is the drift it was meant to prevent.
    for (const std::string_view url :
         {std::string_view{"javascript:alert(1)"}, std::string_view{"//evil.test/x"},
          std::string_view{"http://example.test/"}, std::string_view{"data:text/html,<x>"},
          std::string_view{""}, std::string_view{"/ok"}, std::string_view{"#frag"},
          std::string_view{"https://example.test/x"}, std::string_view{"mailto:a@b.test"}}) {
        std::string out;
        const bool emitted = append_url_attr(out, "href", url);
        EXPECT_EQ(emitted, input::is_safe_link_target(url)) << url;
        EXPECT_EQ(out.empty(), !emitted) << url;
    }
}

TEST(HtmlWriter, TheProtocolRelativeUrlIsRefused) {
    // Named explicitly even though the loop above covers it: it is the case a
    // reimplementation forgets, and this test is also documentation.
    std::string out;
    EXPECT_FALSE(append_url_attr(out, "href", "//evil.test/x"));
    EXPECT_TRUE(out.empty());
}

TEST(HtmlWriter, AnAcceptedUrlIsStillEscapedOnTheWayOut) {
    // Delegating the DECISION is not delegating the ESCAPE. This is a
    // site-relative path, which the link rule correctly accepts, and emitting it
    // raw would close the attribute and open an event handler.
    std::string out;
    EXPECT_TRUE(append_url_attr(out, "href", R"(/x" onmouseover="alert(1))"));
    EXPECT_EQ(out, R"HTML( href="/x&quot; onmouseover=&quot;alert(1)")HTML");
}

TEST(HtmlWriter, AQueryStringAmpersandBecomesAnEntity) {
    std::string out;
    EXPECT_TRUE(append_url_attr(out, "href", "/search?a=1&b=2"));
    EXPECT_EQ(out, R"( href="/search?a=1&amp;b=2")");
}

// --- raw insertion ---------------------------------------------------------

TEST(HtmlWriter, SanitisedMarkupIsTheOnlyThingAppendedVerbatim) {
    const input::SanitizedHtml safe = sanitize("<p><strong>bold</strong></p>");
    ASSERT_EQ(safe.verdict(), input::HtmlVerdict::Ok);

    std::string out;
    append_sanitized(out, safe);
    EXPECT_EQ(out, "<p><strong>bold</strong></p>");
}

TEST(HtmlWriter, AppendSanitizedCannotBeHandedAString) {
    // Compile-time, because the rule is a type-system rule: a function that
    // cannot be handed a request byte cannot be made to accept one by a
    // refactor that was not thinking about it.
    static_assert(!std::is_invocable_v<decltype(append_sanitized)&, std::string&,
                                       std::string_view>,
                  "append_sanitized must not accept a string_view");
    static_assert(!std::is_invocable_v<decltype(append_sanitized)&, std::string&, std::string>,
                  "append_sanitized must not accept a std::string");
    static_assert(!std::is_invocable_v<decltype(append_sanitized)&, std::string&, const char*>,
                  "append_sanitized must not accept a string literal");
    static_assert(std::is_invocable_v<decltype(append_sanitized)&, std::string&,
                                      const input::SanitizedHtml&>,
                  "append_sanitized must accept what the sanitiser produced");
    SUCCEED();
}

TEST(HtmlWriter, SanitizedHtmlCannotBeMintedFromArbitraryBytes) {
    // The one that matters. Without this, the assertion above is a speed bump:
    // a caller who wanted to insert attacker bytes would write
    // `SanitizedHtml{attacker_controlled, HtmlVerdict::Ok}` and the compiler
    // would agree.
    static_assert(!std::is_aggregate_v<input::SanitizedHtml>,
                  "an aggregate is brace-initialisable by anyone");
    static_assert(!std::is_constructible_v<input::SanitizedHtml, std::string,
                                           input::HtmlVerdict>,
                  "only sanitize_rich_text may produce one");
    static_assert(!std::is_default_constructible_v<input::SanitizedHtml>,
                  "a default-constructed one would carry an Ok verdict over no work");
    SUCCEED();
}

// --- re-sanitisation at render ---------------------------------------------

TEST(HtmlWriter, AValueThatFailsTheSecondPassIsNotInTheBytes) {
    // The write-time pass and the render-time pass disagree here, which is the
    // whole reason there are two of them: a page bounds what it puts in one
    // (docs/19-server-side-rendering.md §5), and this body was stored under the
    // field's much larger cap. A test where both passes agree proves nothing.
    const std::string stored_source = "<p>" + std::string(3000, 'a') + "</p>";

    const input::SanitizedHtml at_write = sanitize(stored_source);
    ASSERT_EQ(at_write.verdict(), input::HtmlVerdict::Ok);

    const input::SanitizedHtml at_render = sanitize(at_write.html(), 500);
    ASSERT_EQ(at_render.verdict(), input::HtmlVerdict::TooLong);

    std::string out;
    append_sanitized(out, at_render);
    EXPECT_EQ(out, kWithheldContent);
    EXPECT_FALSE(contains(out, "aaaa"));
}

TEST(HtmlWriter, StoredMarkupThatBypassedTheWritePathIsWithheldNotEmitted) {
    // A stored value that fails re-sanitisation means something bypassed the
    // write path — and the write path is exactly what an attacker who has
    // reached the database no longer has to go through.
    const input::SanitizedHtml at_render = sanitize("<p>ok</p><script>alert(1)</script>");
    ASSERT_EQ(at_render.verdict(), input::HtmlVerdict::Hostile);

    std::string out;
    append_sanitized(out, at_render);
    EXPECT_EQ(out, kWithheldContent);
    EXPECT_FALSE(contains(out, "script"));
}

TEST(HtmlWriter, TheWithheldGapIsVisibleRatherThanSilent) {
    // A blank region with no explanation is indistinguishable from a content
    // bug, and gets "fixed" by whoever finds it next.
    EXPECT_TRUE(contains(kWithheldContent, "withheld"));
    EXPECT_TRUE(kWithheldContent.starts_with("<!--"));
    EXPECT_TRUE(kWithheldContent.ends_with("-->"));
}

// --- composition -----------------------------------------------------------

TEST(HtmlWriter, APageIsOneReservedStringAndOneAllocation) {
    const input::SanitizedHtml body = sanitize("<p>hello</p>");

    std::string out;
    out.reserve(256);
    const char* const before = out.data();

    out.append("<!doctype html><html><body><a");
    ASSERT_TRUE(append_url_attr(out, "href", "/menu"));
    append_html_attr(out, "title", R"(Ali's "menu")");
    out.push_back('>');
    append_html_text(out, "Ali & Co <3");
    out.append("</a>");
    append_sanitized(out, body);
    out.append("</body></html>");

    EXPECT_EQ(out.data(), before) << "the reserve was not enough and the page reallocated";
    EXPECT_EQ(out,
              "<!doctype html><html><body>"
              R"(<a href="/menu" title="Ali&#x27;s &quot;menu&quot;">Ali &amp; Co &lt;3</a>)"
              "<p>hello</p></body></html>");
}

#if ANVIL_HAS_EGY

// --- a tel: href ------------------------------------------------------------
//
// The report behind these: a handler passed a `tel:` URL to append_url_attr,
// which refused it and said nothing, and a contact page shipped with a phone
// number that was not tappable on the device every visitor is holding.

[[nodiscard]] input::PhoneEgy validated(std::string_view typed) {
    input::PhoneEgy phone{};
    EXPECT_EQ(input::validate_phone_egy(typed, phone), input::Reason::Ok) << typed;
    return phone;
}

TEST(HtmlWriterTel, TheGeneralWriterStillRefusesATelUrl) {
    // The decision NOT taken, pinned. Adding `tel:` to is_safe_link_target would
    // have closed the same report, and is the one thing that must not happen:
    // every scheme on that list is one somebody argued for, and this one arrives
    // as a type instead.
    std::string out;
    EXPECT_FALSE(append_url_attr(out, "href", "tel:+201012345678"));
    EXPECT_TRUE(out.empty());
}

TEST(HtmlWriterTel, AValidatedNumberEmitsOneWholeQuotedHref) {
    std::string out;
    EXPECT_TRUE(append_tel_attr(out, validated("0101 234 5678")));
    EXPECT_EQ(out, R"( href="tel:+201012345678")");
}

TEST(HtmlWriterTel, EverySpellingOfOneNumberReachesOneAnchor) {
    // The normalisation the validator already does, carried all the way to the
    // page. This is what storing E.164 and nothing else is FOR: six ways of
    // writing one number are one link, so a renderer never has to decide which
    // spelling is the real one.
    static constexpr std::array<std::string_view, 6> kSpellings{{
        "01012345678",
        "0101 234 5678",
        "+201012345678",
        "00201012345678",
        "201012345678",
        "٠١٠١٢٣٤٥٦٧٨",
    }};

    for (const std::string_view spelling : kSpellings) {
        std::string out;
        ASSERT_TRUE(append_tel_attr(out, validated(spelling))) << spelling;
        EXPECT_EQ(out, R"( href="tel:+201012345678")") << spelling;
    }
}

TEST(HtmlWriterTel, APhoneNoValidatorFilledEmitsNothing) {
    // The gap the type alone cannot close. PhoneEgy is an aggregate behind an
    // out-parameter API, so this compiles and has to: a caller that ignored the
    // Reason is holding thirteen NUL bytes of a perfectly well-typed number, and
    // `tel:` followed by thirteen NULs is an href worse than no href at all.
    const input::PhoneEgy unfilled{};
    std::string out;
    EXPECT_FALSE(append_tel_attr(out, unfilled));
    EXPECT_TRUE(out.empty());
}

TEST(HtmlWriterTel, EveryByteOfTheLayoutIsRefusedWhenItIsWrong) {
    // One corruption per rule, because a check that passes in aggregate can be
    // missing any single one of them. Each of these is a value no scanner
    // produces and every one of them is reachable by a hand-edit, a `memcpy`
    // from a wider buffer, or a struct somebody filled from a database column.
    struct Case final {
        std::string_view why;
        std::size_t      index;
        char             byte;
    };
    static constexpr std::array<Case, 6> kCorruptions{{
        {"no leading plus", 0, ' '},
        {"not the Egyptian calling code", 2, '1'},
        {"a landline rather than a mobile", 3, '2'},
        {"an operator prefix nobody was assigned", 4, '3'},
        {"a letter among the subscriber digits", 9, 'x'},
        // The one that would be ESCAPED rather than refused if the digit check
        // were ever dropped. It must fail the check, not survive it quoted.
        {"a byte from the attribute escape set", 9, '"'},
    }};

    for (const Case& corruption : kCorruptions) {
        input::PhoneEgy phone = validated("+201012345678");
        phone.e164[corruption.index] = corruption.byte;
        if (corruption.index == 4) {
            // Keep the two members in agreement, so the operator TABLE is the
            // only rule left that can refuse this one. The cross-check between
            // them has its own case below.
            phone.operator_digit = static_cast<std::uint8_t>(corruption.byte - '0');
        }
        std::string out;
        EXPECT_FALSE(append_tel_attr(out, phone)) << corruption.why;
        EXPECT_TRUE(out.empty()) << corruption.why;
    }
}

TEST(HtmlWriterTel, TheOperatorMemberHasToAgreeWithTheDigitItCameFrom) {
    // Two members describing one fact is two members that can disagree, and the
    // disagreement is the signal that neither came from the scanner. Both halves
    // here are individually legal — `2` is Orange, `0` is Vodafone — so nothing
    // but the cross-check refuses this.
    input::PhoneEgy phone = validated("+201012345678");
    ASSERT_EQ(phone.operator_digit, 0U);
    phone.e164[4] = '2';

    std::string out;
    EXPECT_FALSE(append_tel_attr(out, phone));
    EXPECT_TRUE(out.empty());
}

TEST(HtmlWriterTel, AnAnchorIsOneAppendChainWithNoAllocationAfterTheReserve) {
    // The same property the page case below asserts, for the shape a contact
    // page actually writes: the URI is built in automatic storage and the
    // attribute lands in the caller's buffer.
    std::string out;
    out.reserve(128);
    const char* const before = out.data();

    out.append("<a");
    ASSERT_TRUE(append_tel_attr(out, validated("01012345678")));
    out.push_back('>');
    append_html_text(out, "0101 234 5678");
    out.append("</a>");

    EXPECT_EQ(out.data(), before) << "the reserve was not enough and the anchor reallocated";
    EXPECT_EQ(out, R"(<a href="tel:+201012345678">0101 234 5678</a>)");
}

#endif  // ANVIL_HAS_EGY

}  // namespace
}  // namespace anvil::http
