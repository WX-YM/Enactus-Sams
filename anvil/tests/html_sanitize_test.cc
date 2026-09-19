// The rich-text sanitiser.
//
// The distinction this suite is really about is REJECT versus STRIP. A `<div>`
// is a formatting mistake and its content survives; a `<script>` or an `on*`
// handler is a staff account doing something no editor produces, and it earns a
// rejection plus an audit row. A test that only checked "no script survives"
// would pass for an implementation that silently cleaned both, which would lose
// the signal.

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "anvil/input/html.h"

namespace anvil::input {
namespace {

constexpr std::string_view kContentOrigin = "https://www.example.test";

[[nodiscard]] SanitizedHtml run(std::string_view html) {
    return sanitize_rich_text(html, HtmlPolicy{kContentOrigin, 20000, 16});
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// --- hostile input is rejected, never cleaned ------------------------------

TEST(HtmlSanitize, ScriptElementIsRejectedRatherThanStripped) {
    EXPECT_EQ(run("<p>hi</p><script>alert(1)</script>").verdict(), HtmlVerdict::Hostile);
    EXPECT_EQ(run("<SCRIPT>alert(1)</SCRIPT>").verdict(), HtmlVerdict::Hostile);
}

TEST(HtmlSanitize, EventHandlerAttributeIsRejected) {
    // The case names explicitly.
    EXPECT_EQ(run("<img src=x onerror=alert(1)>").verdict(), HtmlVerdict::Hostile);
    EXPECT_EQ(run("<p ONCLICK=\"x\">t</p>").verdict(), HtmlVerdict::Hostile);
    // `on` alone is a legitimate (if unknown) attribute name, not a handler.
    EXPECT_EQ(run("<p on=\"x\">t</p>").verdict(), HtmlVerdict::Ok);
}

TEST(HtmlSanitize, ForeignContentAndFrameElementsAreRejected) {
    for (const std::string_view hostile : {"<iframe src=x></iframe>", "<object data=x>",
                                           "<embed src=x>", "<svg><g/></svg>", "<math></math>",
                                           "<style>p{}</style>", "<base href=x>",
                                           "<link rel=x>", "<meta charset=x>"}) {
        EXPECT_EQ(run(hostile).verdict(), HtmlVerdict::Hostile) << hostile;
    }
}

TEST(HtmlSanitize, DangerousSchemesAreRejectedIncludingObfuscatedOnes) {
    EXPECT_EQ(run(R"HTML(<a href="javascript:alert(1)">x</a>)HTML").verdict(), HtmlVerdict::Hostile);
    // Leading whitespace and an embedded tab are both live in a browser, and a
    // naive prefix test on the raw value misses both.
    EXPECT_EQ(run("<a href=\" javascript:alert(1)\">x</a>").verdict(), HtmlVerdict::Hostile);
    EXPECT_EQ(run("<a href=\"java\tscript:alert(1)\">x</a>").verdict(), HtmlVerdict::Hostile);
    EXPECT_EQ(run(R"HTML(<a href="JaVaScRiPt:alert(1)">x</a>)HTML").verdict(), HtmlVerdict::Hostile);
    EXPECT_EQ(run(R"HTML(<img src="data:text/html,<script>">)HTML").verdict(), HtmlVerdict::Hostile);
}

TEST(HtmlSanitize, NestingPastTheCapIsRejectedRatherThanTruncated) {
    std::string deep;
    for (int i = 0; i < 64; ++i) { deep.append("<ul><li>"); }
    EXPECT_EQ(run(deep).verdict(), HtmlVerdict::TooDeep);
}

TEST(HtmlSanitize, LengthIsBoundedInCodePointsNotBytes) {
    // 20 000 Arabic code points is ~40 000 bytes. A byte bound would reject it
    // and give Arabic authors half the allowance.
    std::string arabic;
    for (int i = 0; i < 19000; ++i) { arabic.append("\xd8\xa8"); }
    EXPECT_EQ(run(arabic).verdict(), HtmlVerdict::Ok);

    std::string too_long;
    for (int i = 0; i < 20001; ++i) { too_long.append("\xd8\xa8"); }
    EXPECT_EQ(run(too_long).verdict(), HtmlVerdict::TooLong);
}

// --- benign input is preserved ---------------------------------------------

TEST(HtmlSanitize, AllowedElementsSurviveAndAreLowercased) {
    const SanitizedHtml result = run("<P><STRONG>bold</STRONG> and <em>italic</em></P>");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "<p><strong>bold</strong> and <em>italic</em></p>");
}

TEST(HtmlSanitize, UnknownElementsAreStrippedButTheirTextSurvives) {
    const SanitizedHtml result = run("<div class=x><span>kept</span></div>");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "kept");
}

TEST(HtmlSanitize, ArabicPassesThroughAsRawUtf8) {
    const SanitizedHtml result = run("<p>قهوة</p>");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "<p>قهوة</p>");
}

TEST(HtmlSanitize, DirAndLangSurviveBecauseMixedScriptProseNeedsThem) {
    const SanitizedHtml result = run(R"(<p dir="rtl" lang="ar">قهوة</p>)");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_TRUE(contains(result.html(), R"(dir="rtl")"));
    EXPECT_TRUE(contains(result.html(), R"(lang="ar")"));
}

TEST(HtmlSanitize, StyleAndSrcsetAreDroppedWithoutRejectingTheDocument) {
    const SanitizedHtml result =
        run(R"(<p style="x" class="y" data-z="1">t</p>)");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "<p>t</p>");
}

// --- links ------------------------------------------------------------------

TEST(HtmlSanitize, ExternalLinksGetNoopenerAndTarget) {
    const SanitizedHtml result = run(R"(<a href="https://example.test/x">go</a>)");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_TRUE(contains(result.html(), R"(rel="noopener noreferrer nofollow")"));
    EXPECT_TRUE(contains(result.html(), R"(target="_blank")"));
}

TEST(HtmlSanitize, SiteRelativeLinksSurviveWithoutTargetBlank) {
    const SanitizedHtml result = run(R"(<a href="/menu">menu</a>)");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), R"(<a href="/menu">menu</a>)");
}

TEST(HtmlSanitize, ProtocolRelativeLinksAreDroppedNotFollowed) {
    // `//evil.test/x` inherits the page scheme: an absolute off-site link
    // wearing a relative-looking prefix. The anchor goes, its text stays.
    const SanitizedHtml result = run(R"(<a href="//evil.test/x">t</a>)");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "t");
}

TEST(HtmlSanitize, HttpLinksAreDroppedBecauseOnlyHttpsIsAllowed) {
    const SanitizedHtml result = run(R"(<a href="http://example.test/">t</a>)");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "t");
}

// --- images -----------------------------------------------------------------

TEST(HtmlSanitize, ImagesMustComeFromTheMediaOrigin) {
    const SanitizedHtml allowed =
        run(R"(<img src="https://www.example.test/media/content/x" alt="a">)");
    ASSERT_EQ(allowed.verdict(), HtmlVerdict::Ok);
    EXPECT_TRUE(contains(allowed.html(), "<img src="));

    // A prefix match alone would accept this. The '/' check is what stops it.
    const SanitizedHtml lookalike =
        run(R"(<img src="https://www.example.test.evil.test/x" alt="a">)");
    ASSERT_EQ(lookalike.verdict(), HtmlVerdict::Ok);
    EXPECT_FALSE(contains(lookalike.html(), "<img"));
}

TEST(HtmlSanitize, AltTextIsEscapedIntoTheAttribute) {
    const SanitizedHtml result =
        run(R"HTML(<img src="/media/x" alt="a &quot;b&quot; <c>">)HTML");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_FALSE(contains(result.html(), "<c>"));
    EXPECT_TRUE(contains(result.html(), "&lt;c&gt;"));
}

// --- structure --------------------------------------------------------------

TEST(HtmlSanitize, OutputIsBalancedEvenWhenTheInputIsNot) {
    const SanitizedHtml unclosed = run("<p>one<p>two");
    ASSERT_EQ(unclosed.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(unclosed.html(), "<p>one<p>two</p></p>");

    const SanitizedHtml stray = run("</p>text");
    ASSERT_EQ(stray.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(stray.html(), "text");
}

TEST(HtmlSanitize, BareAngleBracketsBecomeTextRatherThanElements) {
    const SanitizedHtml result = run("a < b and c > d");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "a &lt; b and c &gt; d");
}

TEST(HtmlSanitize, CommentsAreDroppedWholeIncludingTheirContents) {
    const SanitizedHtml result = run("before<!-- <script>x</script> -->after");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_EQ(result.html(), "beforeafter");
}

TEST(HtmlSanitize, KnownEntitiesSurviveAndLoneAmpersandsAreEscaped) {
    const SanitizedHtml result = run("a &amp; b &nosuch; c & d &#x27;");
    ASSERT_EQ(result.verdict(), HtmlVerdict::Ok);
    EXPECT_TRUE(contains(result.html(), "a &amp; b"));
    EXPECT_TRUE(contains(result.html(), "&amp;nosuch;"));
    EXPECT_TRUE(contains(result.html(), "c &amp; d"));
    EXPECT_TRUE(contains(result.html(), "&#x27;"));
}

TEST(HtmlSanitize, SanitisingIsIdempotent) {
    // Storage holds the sanitised form and every renderer sanitises again as
    // defence in depth (anvil/http/html_writer.h, append_sanitized). A second
    // pass that changed the output would make a stored value render differently
    // from what was saved.
    for (const std::string_view input :
         {"<p>plain</p>", R"(<a href="https://example.test/">x</a>)",
          R"(<img src="https://www.example.test/x" alt="&amp;">)", "a &lt; b",
          "<ul><li>one</li><li>two</li></ul>"}) {
        const SanitizedHtml once = run(input);
        ASSERT_EQ(once.verdict(), HtmlVerdict::Ok) << input;
        const SanitizedHtml twice = run(once.html());
        ASSERT_EQ(twice.verdict(), HtmlVerdict::Ok) << input;
        EXPECT_EQ(once.html(), twice.html()) << input;
    }
}

// --- the shared link predicate ---------------------------------------------

TEST(HtmlSanitize, SafeLinkTargetMatchesTheSectionUrlRule) {
    // The same predicate section Url fields use.
    EXPECT_TRUE(is_safe_link_target("/menu"));
    EXPECT_TRUE(is_safe_link_target("#anchor"));
    EXPECT_TRUE(is_safe_link_target("https://example.test/x"));
    EXPECT_TRUE(is_safe_link_target("mailto:a@example.test"));
    EXPECT_FALSE(is_safe_link_target("javascript:alert(1)"));
    EXPECT_FALSE(is_safe_link_target("//evil.test"));
    EXPECT_FALSE(is_safe_link_target("http://example.test"));
    EXPECT_FALSE(is_safe_link_target(""));
}

TEST(HtmlSanitize, NoInputCrashesTheScanner) {
    // Truncated tags, unterminated quotes and stray delimiters all end the scan
    // rather than reading past the buffer.
    for (const std::string_view input : {"<", "<a", "<a href=", "<a href=\"", "<!--", "<!",
                                         "</", "</p", "&", "&#", "&#x", "<p ", "<img src=\"x"}) {
        const SanitizedHtml result = run(input);
        EXPECT_TRUE(result.verdict() == HtmlVerdict::Ok ||
                    result.verdict() == HtmlVerdict::Hostile)
            << input;
    }
}

}  // namespace
}  // namespace anvil::input
