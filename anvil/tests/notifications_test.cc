// The notification seams: the topic registry, the preference masks, the template
// table and the substitution engine.
//
// Much of both seams' value is a static_assert in the reference application's own
// tables (tests/testapp/topics.h), so that part of this suite is a build. What is
// left is the runtime behaviour a table cannot assert about itself.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/notifications/shedding.h"
#include "anvil/notifications/template_spec.h"
#include "anvil/notifications/topic_spec.h"
#include "testapp/topics.h"

namespace {

using anvil::ErrorCode;
using anvil::Locale;
using anvil::PermSet;
namespace n = anvil::notifications;

using testapp::kTemplates;
using testapp::kTopics;
using testapp::Template;
using testapp::Topic;

[[nodiscard]] Locale en() { return *Locale::from_tag("en"); }
[[nodiscard]] Locale ar() { return *Locale::from_tag("ar"); }

[[nodiscard]] n::TopicCode code_of(Topic topic) noexcept {
    return static_cast<n::TopicCode>(topic);
}

[[nodiscard]] n::TemplateId id_of(Template tpl) noexcept {
    return static_cast<n::TemplateId>(tpl);
}

[[nodiscard]] const n::TopicSpec& spec_of(Topic topic) {
    const n::TopicSpec* spec = n::topic_spec(kTopics, code_of(topic));
    EXPECT_NE(spec, nullptr);
    return *spec;
}

}  // namespace

// --- the topic seam ---------------------------------------------------------

TEST(TopicSeam, AMalformedTableIsRejected) {
    using n::ClientType;
    using n::FanOut;
    using n::Scope;
    const auto inapp = n::channels(ClientType::InApp);

    static constexpr std::array<n::TopicSpec, 0> kEmpty{};
    const std::array<n::TopicSpec, 1> kNoKey{
        {{"", PermSet{}, 90, 0, 0, FanOut::Read, inapp, Scope::Global, false, true}}};
    const std::array<n::TopicSpec, 1> kOutOfRange{
        {{"a", PermSet{}, 90, 0, 64, FanOut::Read, inapp, Scope::Global, false, true}}};
    // A topic nothing can be delivered on publishes into nothing, and it looks
    // entirely correct while doing it.
    const std::array<n::TopicSpec, 1> kNoChannel{
        {{"a", PermSet{}, 90, 0, 0, FanOut::Read, n::kNoChannels, Scope::Global, false, true}}};
    // The sentinel means "whatever the topic declares", so a topic declaring it
    // declares nothing.
    const std::array<n::TopicSpec, 1> kSentinelChannel{
        {{"a", PermSet{}, 90, 0, 0, FanOut::Read, n::kDefaultChannels, Scope::Global, false,
          true}}};
    // Zero retention expires every notification the instant it is written.
    const std::array<n::TopicSpec, 1> kNoRetention{
        {{"a", PermSet{}, 0, 0, 0, FanOut::Read, inapp, Scope::Global, false, true}}};
    const std::array<n::TopicSpec, 2> kDuplicateCode{
        {{"a", PermSet{}, 90, 0, 0, FanOut::Read, inapp, Scope::Global, false, true},
         {"b", PermSet{}, 90, 0, 0, FanOut::Read, inapp, Scope::Global, false, true}}};
    const std::array<n::TopicSpec, 2> kDuplicateKey{
        {{"a", PermSet{}, 90, 0, 0, FanOut::Read, inapp, Scope::Global, false, true},
         {"a", PermSet{}, 90, 0, 1, FanOut::Read, inapp, Scope::Global, false, true}}};

    EXPECT_FALSE(n::topic_table_is_well_formed(kEmpty));
    EXPECT_FALSE(n::topic_table_is_well_formed(kNoKey));
    EXPECT_FALSE(n::topic_table_is_well_formed(kOutOfRange));
    EXPECT_FALSE(n::topic_table_is_well_formed(kNoChannel));
    EXPECT_FALSE(n::topic_table_is_well_formed(kSentinelChannel));
    EXPECT_FALSE(n::topic_table_is_well_formed(kNoRetention));
    EXPECT_FALSE(n::topic_table_is_well_formed(kDuplicateCode));
    EXPECT_FALSE(n::topic_table_is_well_formed(kDuplicateKey));

    EXPECT_TRUE(n::topic_table_is_well_formed(kTopics));
}

TEST(TopicSeam, ATableMayNotOutgrowThePreferenceMasks) {
    // A 65th topic would fall silently outside every client's preferences and
    // would then be undisableable — so the table check refuses it rather than
    // letting it ship.
    std::vector<n::TopicSpec> oversized;
    for (std::size_t i = 0; i < n::kMaxTopicKinds + 1; ++i) {
        oversized.push_back({"k", PermSet{}, 90, 0, static_cast<n::TopicCode>(i),
                             n::FanOut::Read, n::channels(n::ClientType::InApp),
                             n::Scope::Global, false, true});
    }
    EXPECT_FALSE(n::topic_table_is_well_formed(oversized));
}

TEST(TopicSeam, AnUndeclaredCodeResolvesToNothing) {
    // The rolling-deploy state: a row written by a newer process names a topic
    // this build has never heard of. Refusing to interpret it is the correct
    // direction to fail.
    EXPECT_EQ(n::topic_spec(kTopics, 200), nullptr);
    EXPECT_TRUE(n::topic_key(kTopics, 200).empty());
    EXPECT_EQ(n::topic_by_key(kTopics, "no.such.topic"), nullptr);
}

TEST(TopicSeam, TheKeyRoundTripsInBothDirections) {
    for (const n::TopicSpec& spec : kTopics) {
        const n::TopicSpec* back = n::topic_by_key(kTopics, spec.key);
        ASSERT_NE(back, nullptr) << spec.key;
        EXPECT_EQ(back->code, spec.code);
        EXPECT_EQ(n::topic_key(kTopics, spec.code), spec.key);
    }
}

TEST(TopicSeam, ATopicRefIsTwentyFourBytesAndComparesWholly) {
    const anvil::Uuid subject{{1, 2, 3}};
    const n::TopicRef global = n::global_topic(code_of(Topic::ContentPublished));
    const n::TopicRef scoped = n::scoped_topic(code_of(Topic::FormSubmitted), subject);

    EXPECT_FALSE(n::is_scoped(global));
    EXPECT_TRUE(n::is_scoped(scoped));
    EXPECT_TRUE(n::same_topic(scoped, n::scoped_topic(code_of(Topic::FormSubmitted), subject)));
    // Same subject, different kind, and the reverse — both must be different
    // topics, or a subscription to one delivers the other.
    EXPECT_FALSE(n::same_topic(scoped, n::scoped_topic(code_of(Topic::FormAccepted), subject)));
    EXPECT_FALSE(n::same_topic(scoped, n::global_topic(code_of(Topic::FormSubmitted))));
}

// --- preferences ------------------------------------------------------------

TEST(NotificationPreferences, TheWireFormatIsFixedAndRoundTrips) {
    n::Preferences prefs = n::Preferences::all_enabled();
    prefs.set(code_of(Topic::ContentPublished), n::ClientType::Email, false);
    prefs.set(code_of(Topic::FormSubmitted), n::ClientType::WebPush, false);

    const std::array<std::uint8_t, n::kPrefBytes> bytes = prefs.to_bytes();
    EXPECT_EQ(n::Preferences::from_bytes(bytes), prefs);

    // Little-endian, low byte first, channel order = ClientType order. Fixed
    // explicitly because these bytes are persisted: byte 16 is the Email mask's
    // low byte, and clearing topic 0 clears its bit 0.
    EXPECT_EQ(bytes[16] & 0x01U, 0U);
    EXPECT_EQ(bytes[0] & 0x01U, 1U);
}

TEST(NotificationPreferences, ANewClientHasEveryBitSet) {
    // Adding a topic must not silently mute it for everyone who registered
    // earlier, which is what a default of "all enabled" buys.
    const n::Preferences fresh = n::Preferences::all_enabled();
    for (const n::TopicSpec& spec : kTopics) {
        EXPECT_TRUE(fresh.enabled(spec.code, n::ClientType::InApp)) << spec.key;
    }
    // And a bit position no topic uses yet is already on, so the topic added next
    // deploy arrives enabled rather than muted.
    EXPECT_TRUE(fresh.enabled(63, n::ClientType::InApp));
}

TEST(NotificationPreferences, OutOfRangeAccessIsFalseRatherThanUndefined) {
    n::Preferences prefs = n::Preferences::all_enabled();
    EXPECT_FALSE(prefs.enabled(64, n::ClientType::InApp));
    EXPECT_FALSE(prefs.enabled(255, n::ClientType::InApp));
    // A set past the end is a no-op rather than a write off the array.
    prefs.set(200, n::ClientType::InApp, false);
    EXPECT_TRUE(prefs.enabled(0, n::ClientType::InApp));
}

TEST(NotificationDelivery, AChannelTheTopicDoesNotDeclareIsNeverDelivered) {
    const n::TopicSpec& content = spec_of(Topic::ContentPublished);
    const n::Preferences all = n::Preferences::all_enabled();

    EXPECT_TRUE(n::should_deliver(content, n::ClientType::InApp, n::kDefaultChannels, all));
    // content.published declares InApp and WebPush; an Email client subscribed to
    // it is still not mailed.
    EXPECT_FALSE(n::should_deliver(content, n::ClientType::Email, n::kDefaultChannels, all));
    // An explicit mask narrows, and never widens past what it asks for.
    EXPECT_FALSE(n::should_deliver(content, n::ClientType::WebPush,
                                   n::channels(n::ClientType::InApp), all));
}

TEST(NotificationDelivery, ASecurityTopicIgnoresTheStoredPreferences) {
    const n::TopicSpec& security = spec_of(Topic::SessionNewDevice);
    const n::TopicSpec& optional = spec_of(Topic::FormAccepted);

    n::Preferences muted = n::Preferences::all_enabled();
    muted.set(security.code, n::ClientType::Email, false);
    muted.set(optional.code, n::ClientType::Email, false);

    // The exemption lives in the RULE, not in the stored mask — so a security
    // topic cannot be muted by a client row written by an older build, or by a
    // hand-edited document.
    EXPECT_TRUE(n::should_deliver(security, n::ClientType::Email, n::kDefaultChannels, muted));
    EXPECT_FALSE(n::should_deliver(optional, n::ClientType::Email, n::kDefaultChannels, muted));
}

// --- the template seam ------------------------------------------------------

TEST(TemplateSeam, AMalformedTableIsRejected) {
    const std::array<n::TemplateSpec, 1> kMissingLocale{
        {{{{"Title", ""}}, {{"Body", "نص"}}, 0, 0}}};
    const std::array<n::TemplateSpec, 1> kCountMismatch{
        {{{{"Title", "عنوان"}}, {{"{t} happened", "حدث {t}"}}, 0, 2}}};
    // The placeholder SET must be identical across locales: a translation that
    // drops `{t}` renders a sentence with a hole in it, in one language only.
    const std::array<n::TemplateSpec, 1> kLocaleDisagrees{
        {{{{"Title", "عنوان"}}, {{"{t} happened", "حدث شيء"}}, 0, 1}}};
    const std::array<n::TemplateSpec, 1> kUnclosedBrace{
        {{{{"Title", "عنوان"}}, {{"{t happened", "حدث {t}"}}, 0, 1}}};
    const std::array<n::TemplateSpec, 1> kNonLetterPlaceholder{
        {{{{"Title", "عنوان"}}, {{"{1} happened", "حدث {1}"}}, 0, 1}}};
    const std::array<n::TemplateSpec, 2> kDuplicateId{
        {{{{"A", "أ"}}, {{"a", "أ"}}, 0, 0}, {{{"B", "ب"}}, {{"b", "ب"}}, 0, 0}}};

    EXPECT_FALSE(n::template_table_is_well_formed(kMissingLocale));
    EXPECT_FALSE(n::template_table_is_well_formed(kCountMismatch));
    EXPECT_FALSE(n::template_table_is_well_formed(kLocaleDisagrees));
    EXPECT_FALSE(n::template_table_is_well_formed(kUnclosedBrace));
    EXPECT_FALSE(n::template_table_is_well_formed(kNonLetterPlaceholder));
    EXPECT_FALSE(n::template_table_is_well_formed(kDuplicateId));

    EXPECT_TRUE(n::template_table_is_well_formed(kTemplates));
}

TEST(TemplateSeam, InvalidUtf8InALiteralIsABuildFailure) {
    // A literal reaches a reader with no validation step in between, so it is
    // proved where the mistake is made.
    const std::array<n::TemplateSpec, 1> kOverlongNul{
        {{{{"Title", std::string_view{"\xC0\x80", 2}}}, {{"body", "نص"}}, 0, 0}}};
    const std::array<n::TemplateSpec, 1> kLoneSurrogate{
        {{{{"Title", std::string_view{"\xED\xA0\x80", 3}}}, {{"body", "نص"}}, 0, 0}}};

    EXPECT_FALSE(n::template_table_is_well_formed(kOverlongNul));
    EXPECT_FALSE(n::template_table_is_well_formed(kLoneSurrogate));
}

TEST(TemplateSeam, ThePlaceholderOrderMayDifferBetweenLocales) {
    // Arabic phrasing legitimately puts `{n}` where English puts `{t}`, and a
    // check that demanded the same order would force an unnatural sentence.
    const std::array<n::TemplateSpec, 1> kReordered{
        {{{{"Title", "عنوان"}}, {{"{n} on {t}", "{t} بها {n}"}}, 0, 2}}};
    EXPECT_TRUE(n::template_table_is_well_formed(kReordered));
}

// --- rendering --------------------------------------------------------------

TEST(TemplateRender, ItRendersInTheReadersLocaleNotTheSenders) {
    const std::array<n::Param, 1> params{{n::Param::of('t', "Summer Hours")}};

    const anvil::Result<n::Rendered> english =
        n::render(kTemplates, id_of(Template::ContentPublished), en(), params);
    ASSERT_TRUE(english.ok());
    EXPECT_EQ(english.value().title, "New post");
    EXPECT_EQ(english.value().body, "Summer Hours was published");

    // The same stored row, the same params, a different reader. This is the whole
    // reason no rendered text is stored.
    const anvil::Result<n::Rendered> arabic =
        n::render(kTemplates, id_of(Template::ContentPublished), ar(), params);
    ASSERT_TRUE(arabic.ok());
    EXPECT_EQ(arabic.value().title, "منشور جديد");
    EXPECT_EQ(arabic.value().body, "تم نشر Summer Hours");
}

TEST(TemplateRender, ANumberIsFormattedWithoutAllocating) {
    const std::array<n::Param, 2> params{
        {n::Param::of('n', std::int64_t{3}), n::Param::of('t', "Membership")}};
    const anvil::Result<n::Rendered> out =
        n::render(kTemplates, id_of(Template::FormSubmitted), en(), params);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().body, "3 new submissions on Membership");
}

TEST(TemplateRender, TheMostNegativeInt64DoesNotOverflowOnNegation) {
    // -(-2^63) overflows in int64, and signed overflow is undefined behaviour —
    // so the magnitude is accumulated unsigned. Under UBSan this case is the
    // check.
    const std::array<n::Param, 2> params{
        {n::Param::of('n', std::numeric_limits<std::int64_t>::min()),
         n::Param::of('t', "x")}};
    const anvil::Result<n::Rendered> out =
        n::render(kTemplates, id_of(Template::FormSubmitted), en(), params);
    ASSERT_TRUE(out.ok());
    EXPECT_NE(out.value().body.find("-9223372036854775808"), std::string::npos);
}

TEST(TemplateRender, AMissingParameterRendersAsNothingNotAsTheTemplate) {
    // A row whose params were written by an older build. A shorter sentence is a
    // degradation; `{t}` in somebody's inbox is the template's internals leaking.
    const anvil::Result<n::Rendered> out =
        n::render(kTemplates, id_of(Template::ContentPublished), en(), {});
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().body, " was published");
    EXPECT_EQ(out.value().body.find('{'), std::string::npos);
}

TEST(TemplateRender, ThereIsNoExpressionLanguageToExploit) {
    // The grammar is one ASCII letter between braces and NOTHING else, and
    // anything else containing `{` is a BUILD failure rather than text that
    // renders literally. That is the stronger of the two positions and the right
    // one: a template that can read a property path can read one the reader is
    // not entitled to, and "it renders as literal text" is a claim about the
    // renderer that the next change to the renderer could quietly break.
    const std::array<n::TemplateSpec, 1> kPath{
        {{{{"T", "\xD8\xAA"}}, {{"{a.b}", "{a.b}"}}, 0, 0}}};
    const std::array<n::TemplateSpec, 1> kIndex{
        {{{{"T", "\xD8\xAA"}}, {{"{0}", "{0}"}}, 0, 0}}};
    const std::array<n::TemplateSpec, 1> kNested{
        {{{{"T", "\xD8\xAA"}}, {{"{{t}}", "{{t}}"}}, 0, 1}}};
    const std::array<n::TemplateSpec, 1> kEmptyBraces{
        {{{{"T", "\xD8\xAA"}}, {{"{}", "{}"}}, 0, 0}}};
    const std::array<n::TemplateSpec, 1> kLongName{
        {{{{"T", "\xD8\xAA"}}, {{"{total}", "{total}"}}, 0, 1}}};

    EXPECT_FALSE(n::template_table_is_well_formed(kPath));
    EXPECT_FALSE(n::template_table_is_well_formed(kIndex));
    EXPECT_FALSE(n::template_table_is_well_formed(kNested));
    EXPECT_FALSE(n::template_table_is_well_formed(kEmptyBraces));
    EXPECT_FALSE(n::template_table_is_well_formed(kLongName));
}

TEST(TemplateRender, AStrayClosingBraceIsProseAndAStrayOpeningBraceIsNot) {
    // The asymmetry is deliberate. A `}` in copy is punctuation in some language
    // and anvil does not police it; a `{` is the one character that starts the
    // only construct the scanner recognises, so it may not appear on its own.
    const std::array<n::TemplateSpec, 1> kClosing{
        {{{{"T", "\xD8\xAA"}}, {{"done} here", "done} here"}}, 0, 0}}};
    const std::array<n::TemplateSpec, 1> kOpening{
        {{{{"T", "\xD8\xAA"}}, {{"a { b", "a { b"}}, 0, 0}}};

    EXPECT_TRUE(n::template_table_is_well_formed(kClosing));
    EXPECT_FALSE(n::template_table_is_well_formed(kOpening));

    const anvil::Result<n::Rendered> out = n::render(kClosing, 0, en(), {});
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().body, "done} here");
}

TEST(TemplateRender, ADollarPrefixIsNotASecondSyntax) {
    // `${t}` is a `$` followed by an ordinary placeholder, not a shell
    // substitution — there is only one construct, and it is the only one.
    const std::array<n::TemplateSpec, 1> kDollar{
        {{{{"T", "\xD8\xAA"}}, {{"${t}", "${t}"}}, 0, 1}}};
    ASSERT_TRUE(n::template_table_is_well_formed(kDollar));

    const std::array<n::Param, 1> params{{n::Param::of('t', "X")}};
    const anvil::Result<n::Rendered> out = n::render(kDollar, 0, en(), params);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().body, "$X");
}

TEST(TemplateRender, AnUnusableParameterIsRefusedRatherThanRendered) {
    const std::array<n::Param, 1> invalid_utf8{
        {n::Param::of('t', std::string_view{"\xFF\xFE", 2})}};
    EXPECT_EQ(n::render(kTemplates, id_of(Template::ContentPublished), en(), invalid_utf8)
                  .code(),
              ErrorCode::ValidationFailed);

    // U+202E in a notification body spoofs the whole sentence, and the reader has
    // no way to see that it did.
    const std::array<n::Param, 1> override_mark{
        {n::Param::of('t', "safe\xE2\x80\xAEevil")}};
    EXPECT_EQ(n::render(kTemplates, id_of(Template::ContentPublished), en(), override_mark)
                  .code(),
              ErrorCode::ValidationFailed);

    // Code points, never bytes: 121 Arabic characters is 242 bytes and must fail
    // on the count rather than on the length.
    std::string long_arabic;
    for (int i = 0; i < static_cast<int>(n::kMaxParamCodePoints) + 1; ++i) {
        long_arabic += "\xD8\xA3";
    }
    const std::array<n::Param, 1> too_long{{n::Param::of('t', long_arabic)}};
    EXPECT_EQ(n::render(kTemplates, id_of(Template::ContentPublished), en(), too_long).code(),
              ErrorCode::ValidationFailed);

    // Exactly at the cap passes, so the bound is the one that was declared.
    std::string at_cap;
    for (int i = 0; i < static_cast<int>(n::kMaxParamCodePoints); ++i) { at_cap += "\xD8\xA3"; }
    const std::array<n::Param, 1> fits{{n::Param::of('t', at_cap)}};
    EXPECT_TRUE(n::render(kTemplates, id_of(Template::ContentPublished), en(), fits).ok());
}

TEST(TemplateRender, AnUndeclaredTemplateIsAnIntegrityFaultNotBadInput) {
    // The id came off a stored row this system wrote, so a build that cannot
    // resolve it has an integrity problem rather than bad input — and the two get
    // different HTTP statuses and different log lines.
    EXPECT_EQ(n::render(kTemplates, 200, en(), {}).code(), ErrorCode::Internal);
}

TEST(TemplateRender, TooManyParametersIsRefused) {
    std::vector<n::Param> many;
    for (std::size_t i = 0; i <= n::kMaxParams; ++i) {
        many.push_back(n::Param::of(static_cast<char>('a' + i), "x"));
    }
    EXPECT_EQ(n::render(kTemplates, id_of(Template::ContentPublished), en(), many).code(),
              ErrorCode::ValidationFailed);
}

// --- the storm breaker ------------------------------------------------------
//
// Every case below is a LOOP over the whole topic table rather than a named
// topic. A case that names one passes forever after somebody adds a seventh,
// and the property being asserted is a property of the table.

TEST(Shedding, TheTableHasATopicThatCannotBeSilenced) {
    // The loops below skip topics that do not apply to them, so this is what
    // stops the security case being vacuous the day nobody declares such a
    // topic — which would make it pass by testing nothing.
    std::size_t unsilenceable = 0;
    for (const n::TopicSpec& spec : kTopics) {
        if (!spec.user_optional) { ++unsilenceable; }
    }
    EXPECT_GT(unsilenceable, 0U);
}

TEST(Shedding, NoTopicTheReaderCannotSilenceIsEverDropped) {
    // The property the whole design turns on: a topic the reader is not allowed
    // to silence is a topic the storm breaker is not allowed to drop. Under any
    // pressure whatsoever it is only ever delayed.
    static constexpr std::array<float, 9> kPressures{
        {0.0F, 0.25F, 0.50F, 0.51F, 0.75F, 0.76F, 0.99F, 1.0F, 4.0F}};

    for (const n::TopicSpec& spec : kTopics) {
        if (spec.user_optional) { continue; }
        for (const float pressure : kPressures) {
            EXPECT_NE(n::shed_verdict(spec, n::ShedPolicy{}, pressure), n::ShedVerdict::Drop)
                << spec.key << " at " << pressure;
        }
    }
}

TEST(Shedding, BelowTheLowerWatermarkNothingIsShedAtAll) {
    const n::ShedPolicy policy{};
    for (const n::TopicSpec& spec : kTopics) {
        EXPECT_EQ(n::shed_verdict(spec, policy, 0.0F), n::ShedVerdict::Proceed) << spec.key;
        // AT the watermark, not merely below it. The comparison is `>`, so the
        // boundary value itself proceeds — and a test that only checked 0.0
        // would not notice it changing to `>=`.
        EXPECT_EQ(n::shed_verdict(spec, policy, policy.defer_dispatch_above),
                  n::ShedVerdict::Proceed)
            << spec.key;
    }
}

TEST(Shedding, BetweenTheWatermarksEveryTopicIsOnlyDelayed) {
    const n::ShedPolicy policy{};
    for (const n::TopicSpec& spec : kTopics) {
        EXPECT_EQ(n::shed_verdict(spec, policy, 0.60F), n::ShedVerdict::DeferDispatch)
            << spec.key;
        EXPECT_EQ(n::shed_verdict(spec, policy, policy.drop_optional_above),
                  n::ShedVerdict::DeferDispatch)
            << spec.key;
    }
}

TEST(Shedding, AboveTheDropWatermarkOnlyTheSilenceableOnesAreLost) {
    const n::ShedPolicy policy{};
    for (const n::TopicSpec& spec : kTopics) {
        EXPECT_EQ(n::shed_verdict(spec, policy, 0.90F),
                  spec.user_optional ? n::ShedVerdict::Drop : n::ShedVerdict::DeferDispatch)
            << spec.key;
    }
}

TEST(Shedding, APressureNobodyCanReadNeverCostsANotification) {
    // What a caller whose pool reports zero capacity hands this: 0 / 0. Every
    // comparison against it is false, and the negated form is what turns that
    // into Proceed rather than into whichever branch happens to be last.
    const float unreadable = std::numeric_limits<float>::quiet_NaN();
    for (const n::TopicSpec& spec : kTopics) {
        EXPECT_EQ(n::shed_verdict(spec, n::ShedPolicy{}, unreadable), n::ShedVerdict::Proceed)
            << spec.key;
    }
}

TEST(Shedding, APolicyThatWouldDropBeforeItDefersIsMalformed) {
    EXPECT_TRUE(n::shed_policy_is_well_formed(n::ShedPolicy{}));

    // Dropping engaging before deferring throws a notification away while the
    // outbox path it could have used is still idle.
    EXPECT_FALSE(n::shed_policy_is_well_formed(
        n::ShedPolicy{.drop_optional_above = 0.40F, .defer_dispatch_above = 0.80F}));
    // A fraction of a queue's capacity is not a fraction if it is outside [0, 1].
    EXPECT_FALSE(n::shed_policy_is_well_formed(
        n::ShedPolicy{.drop_optional_above = 1.50F, .defer_dispatch_above = 0.50F}));
    EXPECT_FALSE(n::shed_policy_is_well_formed(
        n::ShedPolicy{.drop_optional_above = 0.75F, .defer_dispatch_above = -0.10F}));
    // A watermark computed from a division somebody did not check.
    EXPECT_FALSE(n::shed_policy_is_well_formed(
        n::ShedPolicy{.drop_optional_above = std::numeric_limits<float>::quiet_NaN(),
                      .defer_dispatch_above = 0.50F}));
    EXPECT_FALSE(n::shed_policy_is_well_formed(
        n::ShedPolicy{.drop_optional_above = 0.75F,
                      .defer_dispatch_above = std::numeric_limits<float>::quiet_NaN()}));
}

TEST(Shedding, EqualWatermarksAreLegalAndMeanNoDeferralBand) {
    // A deployment that would rather drop than build a backlog. Legal, and the
    // band between the two simply has no width.
    const n::ShedPolicy policy{.drop_optional_above = 0.75F, .defer_dispatch_above = 0.75F};
    ASSERT_TRUE(n::shed_policy_is_well_formed(policy));
    for (const n::TopicSpec& spec : kTopics) {
        EXPECT_EQ(n::shed_verdict(spec, policy, 0.75F), n::ShedVerdict::Proceed) << spec.key;
        EXPECT_EQ(n::shed_verdict(spec, policy, 0.76F),
                  spec.user_optional ? n::ShedVerdict::Drop : n::ShedVerdict::DeferDispatch)
            << spec.key;
    }
}
