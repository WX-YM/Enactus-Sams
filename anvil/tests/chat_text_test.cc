// The chat message validators: body text, mention spans, link previews and
// reactions (docs/22-chat.md §4.3, §4.6). One accepting and one refusing case
// per rule, so a refusal that starts accepting and an acceptance that starts
// refusing both fail here.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/chat/text.h"
#include "anvil/core/types.h"
#include "anvil/input/fields.h"
#include "testapp/chat_text_vectors.h"

namespace anvil::chat {
namespace {

using input::Reason;

[[nodiscard]] std::string repeat(std::string_view unit, std::size_t times) {
    std::string out;
    out.reserve(unit.size() * times);
    for (std::size_t i = 0; i < times; ++i) { out.append(unit); }
    return out;
}

[[nodiscard]] Uuid user(std::uint8_t tag) noexcept {
    Uuid id{};
    id[15] = tag;
    return id;
}

// Bidi controls are spelled as bytes: GCC's -Wbidi-chars flags an unpaired one
// even when it is written as a universal character name.
//
// U+0628 ARABIC LETTER BEH: one code point, two bytes.
constexpr std::string_view kBeh = "\u0628";

// --- message body -----------------------------------------------------------

TEST(ChatMessageText, AcceptsOrdinaryTextInAnyScript) {
    EXPECT_EQ(validate_message_text("hello", kMaxMessageCodePoints), Reason::Ok);
    EXPECT_EQ(validate_message_text("\u0645\u0631\u062D\u0628\u0627", kMaxMessageCodePoints),
              Reason::Ok);
    EXPECT_EQ(validate_message_text("line one\nline two\n\tindented", kMaxMessageCodePoints),
              Reason::Ok);
    EXPECT_EQ(validate_message_text("\U0001F44D great", kMaxMessageCodePoints), Reason::Ok);
}

TEST(ChatMessageText, BoundIsInCodePointsNotBytes) {
    // 4096 Arabic letters are 8192 bytes. A byte bound would have halved the
    // allowance for every Arabic writer.
    const std::string at_bound = repeat(kBeh, kMaxMessageCodePoints);
    ASSERT_EQ(at_bound.size(), 2U * kMaxMessageCodePoints);
    EXPECT_EQ(validate_message_text(at_bound, kMaxMessageCodePoints), Reason::Ok);

    const std::string over = repeat(kBeh, kMaxMessageCodePoints + 1U);
    EXPECT_EQ(validate_message_text(over, kMaxMessageCodePoints), Reason::TooLong);
}

TEST(ChatMessageText, AKindMayLowerTheBoundButNotRaiseIt) {
    EXPECT_EQ(validate_message_text(repeat("a", 10), 10), Reason::Ok);
    EXPECT_EQ(validate_message_text(repeat("a", 11), 10), Reason::TooLong);

    // Clamped to anvil's ceiling, not trusted.
    const std::string over_ceiling = repeat("a", kMaxMessageCodePoints + 1U);
    EXPECT_EQ(validate_message_text(over_ceiling, 1'000'000), Reason::TooLong);
    EXPECT_EQ(validate_message_text("a", 0), Reason::TooLong);
}

TEST(ChatMessageText, AHostileBodyIsRefusedOnLength) {
    const std::string megabyte(1U << 20U, 'a');
    EXPECT_EQ(validate_message_text(megabyte, kMaxMessageCodePoints), Reason::TooLong);
}

TEST(ChatMessageText, EmptyAndBlankAreRequired) {
    EXPECT_EQ(validate_message_text("", kMaxMessageCodePoints), Reason::Required);
    EXPECT_EQ(validate_message_text(" \n\t ", kMaxMessageCodePoints), Reason::Required);
    // No-break space, ideographic space, zero-width space and the Hangul
    // filler: each renders as nothing, which is the point of sending it.
    EXPECT_EQ(validate_message_text("\u00A0\u3000", kMaxMessageCodePoints), Reason::Required);
    EXPECT_EQ(validate_message_text("\u200B\u3164", kMaxMessageCodePoints), Reason::Required);
    EXPECT_EQ(validate_message_text(" x ", kMaxMessageCodePoints), Reason::Ok);
}

TEST(ChatMessageText, RefusesControlsOtherThanNewlineAndTab) {
    EXPECT_EQ(validate_message_text("a\x01" "b", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("a\x1B[2Jb", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("a\x7F", kMaxMessageCodePoints), Reason::BadCharset);
    // C1, policed by code point: U+0085 is a line break to some renderers.
    EXPECT_EQ(validate_message_text("a\u0085b", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("a\u009Bb", kMaxMessageCodePoints), Reason::BadCharset);
}

TEST(ChatMessageText, OneNewlineSpellingOnly) {
    EXPECT_EQ(validate_message_text("a\nb", kMaxMessageCodePoints), Reason::Ok);
    EXPECT_EQ(validate_message_text("a\rb", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("a\r\nb", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("a\u2028b", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("a\u2029b", kMaxMessageCodePoints), Reason::BadCharset);
}

TEST(ChatMessageText, RefusesInvalidUtf8) {
    EXPECT_EQ(validate_message_text("\xC3", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("\xC0\xAF", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("\xED\xA0\x80", kMaxMessageCodePoints), Reason::BadCharset);
    EXPECT_EQ(validate_message_text(std::string_view{"a\0b", 3}, kMaxMessageCodePoints),
              Reason::BadCharset);
    EXPECT_EQ(validate_message_text("\uFFFE", kMaxMessageCodePoints), Reason::BadCharset);
}

TEST(ChatMessageText, OverridesAreRefusedAndIsolatesAllowed) {
    constexpr std::uint32_t kMax = kMaxMessageCodePoints;
    EXPECT_EQ(validate_message_text("invoice\xE2\x80\xAE" "fdp.exe", kMax), Reason::BadCharset);
    EXPECT_EQ(validate_message_text("a\xE2\x80\xAA" "b", kMax), Reason::BadCharset);
    // The Prose policy: a mixed-direction sentence needs an isolate (U+2067
    // ... U+2069 around an Arabic word).
    const std::string isolated =
        "the word \xE2\x81\xA7\u0645\u0631\u062D\u0628\u0627\xE2\x81\xA9 here";
    EXPECT_EQ(validate_message_text(isolated, kMax), Reason::Ok);
}

TEST(ChatMessageText, NfcIsRequiredNotApplied) {
    EXPECT_EQ(validate_message_text("caf\u00E9", kMaxMessageCodePoints), Reason::Ok);
    EXPECT_EQ(validate_message_text("cafe\u0301", kMaxMessageCodePoints), Reason::BadFormat);
    // A composition exclusion: NFC would turn one code point into two, which
    // is exactly what would move a mention span.
    EXPECT_EQ(validate_message_text("\u0958", kMaxMessageCodePoints), Reason::BadFormat);
    // Arabic with tashkeel is already NFC.
    EXPECT_EQ(validate_message_text("\u0628\u064E\u0627\u0628", kMaxMessageCodePoints),
              Reason::Ok);
}

// --- mentions ---------------------------------------------------------------

TEST(ChatMentions, AcceptsSortedSpansInsideTheText) {
    // "مرحبا أحمد و سارة": the spans are code points, not bytes.
    const std::string_view text =
        "\u0645\u0631\u062D\u0628\u0627 \u0623\u062D\u0645\u062F \u0648 \u0633\u0627\u0631\u0629";
    const std::array<MentionSpan, 2> spans{{{user(1), 6, 4}, {user(2), 13, 4}}};
    ASSERT_EQ(validate_message_text(text, kMaxMessageCodePoints), Reason::Ok);
    EXPECT_EQ(validate_mentions(text, spans), Reason::Ok);
    EXPECT_EQ(validate_mentions(text, {}), Reason::Ok);
}

TEST(ChatMentions, TheSamePersonMayBeMentionedTwice) {
    const std::array<MentionSpan, 2> spans{{{user(1), 0, 2}, {user(1), 3, 2}}};
    EXPECT_EQ(validate_mentions("ab cd", spans), Reason::Ok);
}

TEST(ChatMentions, AdjacentSpansDoNotOverlap) {
    const std::array<MentionSpan, 2> spans{{{user(1), 0, 2}, {user(2), 2, 2}}};
    EXPECT_EQ(validate_mentions("abcd", spans), Reason::Ok);
}

TEST(ChatMentions, ASpanPastTheEndIsRefused) {
    const std::array<MentionSpan, 1> at_end{{{user(1), 3, 2}}};
    EXPECT_EQ(validate_mentions("abcde", at_end), Reason::Ok);
    const std::array<MentionSpan, 1> past_end{{{user(1), 4, 2}}};
    EXPECT_EQ(validate_mentions("abcde", past_end), Reason::OutOfRange);

    // Counted in code points: two Arabic letters are four bytes.
    const std::array<MentionSpan, 1> past_arabic{{{user(1), 0, 3}}};
    EXPECT_EQ(validate_mentions("\u0628\u0628", past_arabic), Reason::OutOfRange);
}

TEST(ChatMentions, AWrappingEndIsRefused) {
    const std::array<MentionSpan, 1> wraps{{{user(1), 0xFFFF'FFFFU, 2}}};
    EXPECT_EQ(validate_mentions("abc", wraps), Reason::OutOfRange);
}

TEST(ChatMentions, OverlappingAndUnsortedSpansAreRefused) {
    const std::array<MentionSpan, 2> overlapping{{{user(1), 0, 3}, {user(2), 2, 2}}};
    EXPECT_EQ(validate_mentions("abcdef", overlapping), Reason::BadFormat);
    const std::array<MentionSpan, 2> unsorted{{{user(1), 3, 2}, {user(2), 0, 2}}};
    EXPECT_EQ(validate_mentions("abcdef", unsorted), Reason::BadFormat);
}

TEST(ChatMentions, EmptySpansAndNilUsersAreRefused) {
    const std::array<MentionSpan, 1> empty{{{user(1), 1, 0}}};
    EXPECT_EQ(validate_mentions("abc", empty), Reason::BadFormat);
    const std::array<MentionSpan, 1> nil{{{kNilUuid, 0, 1}}};
    EXPECT_EQ(validate_mentions("abc", nil), Reason::Required);
}

TEST(ChatMentions, AtMostThirtyTwo) {
    const std::string text = repeat("a", 64);
    std::vector<MentionSpan> spans;
    for (std::uint32_t i = 0; i < kMaxMentions; ++i) { spans.push_back({user(1), i, 1}); }
    EXPECT_EQ(validate_mentions(text, spans), Reason::Ok);
    spans.push_back({user(1), 40, 1});
    EXPECT_EQ(validate_mentions(text, spans), Reason::TooLong);
}

// --- link preview -----------------------------------------------------------

[[nodiscard]] std::optional<input::FieldError> preview(std::string_view url,
                                                       std::string_view title = "",
                                                       std::string_view description = "") {
    return validate_link_preview(LinkPreview{url, title, description});
}

void expect_refused(const std::optional<input::FieldError>& error, std::string_view field,
                    Reason reason) {
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->field, field);
    EXPECT_EQ(error->reason, reason);
}

TEST(ChatLinkPreview, AcceptsAnAbsoluteHttpsPreview) {
    EXPECT_FALSE(preview("https://example.com/a?b=c#d", "A title",
                         "First line.\nSecond line.")
                     .has_value());
    EXPECT_FALSE(preview("HTTPS://Example.COM/").has_value()) << "the scheme is case-insensitive";
    EXPECT_FALSE(preview("https://xn--mgbh0fb.example/").has_value());
}

TEST(ChatLinkPreview, OnlyAbsoluteHttpsIsAllowed) {
    expect_refused(preview(""), kPreviewUrlField, Reason::Required);
    expect_refused(preview("javascript:alert(1)"), kPreviewUrlField, Reason::NotAllowed);
    expect_refused(preview("http://example.com/"), kPreviewUrlField, Reason::NotAllowed);
    expect_refused(preview("/relative/page"), kPreviewUrlField, Reason::NotAllowed);
    expect_refused(preview("#fragment"), kPreviewUrlField, Reason::NotAllowed);
    expect_refused(preview("mailto:a@example.com"), kPreviewUrlField, Reason::NotAllowed);
    expect_refused(preview("data:text/html,x"), kPreviewUrlField, Reason::NotAllowed);
}

TEST(ChatLinkPreview, TheAuthorityIsJudgedLikeAnyLink) {
    expect_refused(preview("https://bank.example@evil.example/"), kPreviewUrlField,
                   Reason::NotAllowed);
    expect_refused(preview("https://192.0.2.1/"), kPreviewUrlField, Reason::NotAllowed);
    expect_refused(preview("https://intranet/"), kPreviewUrlField, Reason::BadFormat);
}

TEST(ChatLinkPreview, TheUrlIsEncodedAscii) {
    expect_refused(preview("https://p\u0430ypal.example/"), kPreviewUrlField,
                   Reason::BadCharset);
    expect_refused(preview("https://example.com/a b"), kPreviewUrlField, Reason::BadCharset);
    expect_refused(preview("https://example.com/\r\nSet-Cookie:x"), kPreviewUrlField,
                   Reason::BadCharset);

    const std::string at_bound = "https://example.com/" + repeat("a", kMaxPreviewUrlBytes - 20U);
    ASSERT_EQ(at_bound.size(), kMaxPreviewUrlBytes);
    EXPECT_FALSE(preview(at_bound).has_value());
    expect_refused(preview(at_bound + "a"), kPreviewUrlField, Reason::TooLong);
}

TEST(ChatLinkPreview, TitleIsOneBoundedLine) {
    constexpr std::string_view kUrl = "https://example.com/";
    EXPECT_FALSE(preview(kUrl, repeat(kBeh, kMaxPreviewTitleCodePoints)).has_value());
    expect_refused(preview(kUrl, repeat(kBeh, kMaxPreviewTitleCodePoints + 1U)),
                   kPreviewTitleField, Reason::TooLong);
    expect_refused(preview(kUrl, "two\nlines"), kPreviewTitleField, Reason::BadCharset);
    expect_refused(preview(kUrl, "a\ttab"), kPreviewTitleField, Reason::BadCharset);
    expect_refused(preview(kUrl, "gnp.\xE2\x80\xAE" "exe"), kPreviewTitleField, Reason::BadCharset);
    expect_refused(preview(kUrl, "   "), kPreviewTitleField, Reason::Required);
}

TEST(ChatLinkPreview, DescriptionIsBoundedProse) {
    constexpr std::string_view kUrl = "https://example.com/";
    EXPECT_FALSE(
        preview(kUrl, "", repeat(kBeh, kMaxPreviewDescriptionCodePoints)).has_value());
    expect_refused(preview(kUrl, "", repeat(kBeh, kMaxPreviewDescriptionCodePoints + 1U)),
                   kPreviewDescriptionField, Reason::TooLong);
    expect_refused(preview(kUrl, "", "a\x01"), kPreviewDescriptionField, Reason::BadCharset);
    expect_refused(preview(kUrl, "", "cafe\u0301"), kPreviewDescriptionField,
                   Reason::BadFormat);
}

// --- reactions --------------------------------------------------------------

TEST(ChatReaction, AcceptsOneClusterOfAnyKind) {
    EXPECT_EQ(validate_reaction("\U0001F44D"), Reason::Ok);               // thumbs up
    EXPECT_EQ(validate_reaction("\U0001F44D\U0001F3FD"), Reason::Ok);     // with a skin tone
    EXPECT_EQ(validate_reaction("\u2764\uFE0F"), Reason::Ok);             // emoji presentation
    EXPECT_EQ(validate_reaction("1\uFE0F\u20E3"), Reason::Ok);            // keycap
    EXPECT_EQ(validate_reaction("\U0001F1EA\U0001F1EC"), Reason::Ok);     // a flag: two RIs
    // Whether it is an emoji is the application's question.
    EXPECT_EQ(validate_reaction("a"), Reason::Ok);
    EXPECT_EQ(validate_reaction("\u0628\u064E"), Reason::Ok);             // letter + tashkeel
}

TEST(ChatReaction, AZwjFamilyIsOneReaction) {
    // man ZWJ woman ZWJ girl ZWJ boy: seven code points, one cluster.
    constexpr std::string_view kFamily =
        "\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466";
    EXPECT_EQ(validate_reaction(kFamily), Reason::Ok);
}

TEST(ChatReaction, MoreThanOneClusterIsRefused) {
    EXPECT_EQ(validate_reaction("\U0001F44D\U0001F44D"), Reason::BadFormat);
    EXPECT_EQ(validate_reaction("ab"), Reason::BadFormat);
    // Three regional indicators are a flag and a stray.
    EXPECT_EQ(validate_reaction("\U0001F1EA\U0001F1EC\U0001F1EA"), Reason::BadFormat);
    // A ZWJ between two letters joins nothing.
    EXPECT_EQ(validate_reaction("a\u200Db"), Reason::BadFormat);
}

TEST(ChatReaction, BoundedInCodePointsAndBytes) {
    // One cluster, nine code points: a base and eight combining marks.
    const std::string nine = "a" + repeat("\u0301", 8);
    EXPECT_EQ(validate_reaction(nine), Reason::TooLong);
    const std::string eight = "a" + repeat("\u0301", 7);
    EXPECT_NE(validate_reaction(eight), Reason::TooLong);
    EXPECT_EQ(validate_reaction(repeat("a", kMaxReactionBytes + 1U)), Reason::TooLong);
}

TEST(ChatReaction, EmptyAndBlankAreRequired) {
    EXPECT_EQ(validate_reaction(""), Reason::Required);
    EXPECT_EQ(validate_reaction(" "), Reason::Required);
    EXPECT_EQ(validate_reaction("\u3164"), Reason::Required);
}

TEST(ChatReaction, IdentifierCharacterPolicyExceptAnInteriorZwj) {
    EXPECT_EQ(validate_reaction("\x01"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\n"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\xE2\x80\xAE\U0001F44D"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\xE2\x81\xA7\U0001F44D"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\u200E"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\u200B"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("a\u200C"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\u200D"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\U0001F44D\u200D"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\u200D\U0001F44D"), Reason::BadCharset);
}

TEST(ChatReaction, InvalidUtf8AndNonNfcAreRefused) {
    EXPECT_EQ(validate_reaction("\xF0\x9F\x91"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("\xED\xA0\x80"), Reason::BadCharset);
    EXPECT_EQ(validate_reaction("e\u0301"), Reason::BadFormat);
    EXPECT_EQ(validate_reaction("\u00E9"), Reason::Ok);
}

// --- the printed fixture -----------------------------------------------------------
//
// tests/testapp/chat_text_vectors.h is what testapp_emit_chat_text prints for
// hammer's composer validator: every case there is answered here exactly as it
// says, so the client and the server refuse the same text with the same field
// and reason.

TEST(ChatTextVectors, EveryPrintedCaseIsAnsweredAsItSays) {
    const auto all = testapp::text_vectors::cases();
    ASSERT_GT(all.size(), 100U);
    for (const testapp::text_vectors::TextCase& c : all) {
        SCOPED_TRACE(c.name);
        const auto [field, reason] = testapp::text_vectors::validate(c);
        EXPECT_EQ(reason, c.reason);
        EXPECT_EQ(field, c.field);
    }
}

}  // namespace
}  // namespace anvil::chat
