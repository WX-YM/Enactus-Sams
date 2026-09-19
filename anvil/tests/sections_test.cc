// The sections CMS, everything that needs neither a database nor Redis: the
// registry seam, the compile-time conformance checks, binding, merging,
// canonicalisation and serialisation.
//
// Much of the seam's value is a static_assert in the reference application's own
// table (tests/testapp/sections.h), so that part of this suite is a build. What
// is left is the runtime behaviour a table cannot assert about itself.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/locale.h"
#include "anvil/core/uuid.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "anvil/sections/content.h"
#include "anvil/sections/defaults.h"
#include "anvil/sections/payload.h"
#include "anvil/sections/registry.h"
#include "testapp/sections.h"

namespace {

using anvil::Locale;
using anvil::Uuid;
namespace sec = anvil::sections;

const sec::SectionSpec& hero() {
    const sec::SectionSpec* spec = sec::find_section(testapp::kSections, "home.hero");
    EXPECT_NE(spec, nullptr);
    return *spec;
}

const sec::SectionSpec& about() {
    const sec::SectionSpec* spec = sec::find_section(testapp::kSections, "home.about");
    EXPECT_NE(spec, nullptr);
    return *spec;
}

// By key, never by index. kDefaults is positionally identical to kSections —
// defaults_match_registry asserts exactly that — but a test that spells the
// position asserts something different the day a section is added, and says
// nothing about it.
const sec::SectionDefaults& defaults_for(std::string_view key) {
    const sec::SectionDefaults* d = sec::find_defaults(testapp::kDefaults, key);
    EXPECT_NE(d, nullptr);
    return *d;
}

Locale en() { return *Locale::from_tag("en"); }
Locale ar() { return *Locale::from_tag("ar"); }

// Parses one JSON body into an arena that outlives the returned document only as
// long as this object does — the same lifetime rule a request body carries.
class Body final {
public:
    explicit Body(std::string text)
        : text_{std::move(text)},
          document_{anvil::input::parse_json(text_, arena_, anvil::input::JsonLimits{})} {}

    [[nodiscard]] const anvil::input::JsonValue& root() const { return document_.root(); }
    [[nodiscard]] bool ok() const { return document_.ok(); }

private:
    std::string                 text_;
    anvil::input::BodyArena       arena_;
    anvil::input::JsonDocument  document_;
};

sec::SectionContent bound(const sec::SectionSpec& spec, std::string json,
                          std::optional<sec::BindError>& error) {
    Body body{std::move(json)};
    EXPECT_TRUE(body.ok());
    sec::SectionContent out;
    error = sec::bind_data(spec, body.root().find("data"), sec::BindPolicy{"https://cdn.test"},
                           out);
    if (!error) {
        error = sec::bind_images(spec, body.root().find("images"), out);
    }
    return out;
}

// --- the registry seam ------------------------------------------------------

TEST(SectionRegistry, LookupIsABinarySearchOverTheApplicationsTable) {
    EXPECT_EQ(sec::find_section(testapp::kSections, "home.hero")->key, "home.hero");
    EXPECT_EQ(sec::find_section(testapp::kSections, "contact.info")->key, "contact.info");
    // A key that is not in the table resolves to nothing. The caller turns that
    // into a stealth 404 WITHOUT creating a cache entry — a miss that populated
    // one would be unbounded growth keyed on attacker input.
    EXPECT_EQ(sec::find_section(testapp::kSections, "home.nope"), nullptr);
    EXPECT_EQ(sec::find_section(testapp::kSections, ""), nullptr);
}

TEST(SectionRegistry, SectionIndexIsThePositionTheCacheIsKeyedOn) {
    for (std::size_t i = 0; i < testapp::kSections.size(); ++i) {
        EXPECT_EQ(sec::section_index(testapp::kSections, &testapp::kSections[i]), i);
    }
}

TEST(SectionRegistry, PrefixMatchingRequiresTheDot) {
    EXPECT_TRUE(sec::key_has_prefix("home.hero", "home"));
    EXPECT_TRUE(sec::key_has_prefix("home", "home"));
    // Without the dot requirement a request for "home" would also match a future
    // "homepage.x", and a page read would answer with another page's sections.
    EXPECT_FALSE(sec::key_has_prefix("homepage.x", "home"));
    EXPECT_FALSE(sec::key_has_prefix("home", "home.hero"));
}

TEST(SectionRegistry, KeysAreCompileTimeIdentifiersAndNothingElse) {
    EXPECT_TRUE(sec::is_wellformed_key("home.hero"));
    EXPECT_TRUE(sec::is_wellformed_key("yard_now"));
    EXPECT_TRUE(sec::is_wellformed_key("a1.b2"));
    EXPECT_FALSE(sec::is_wellformed_key(""));
    EXPECT_FALSE(sec::is_wellformed_key(".home"));
    EXPECT_FALSE(sec::is_wellformed_key("home."));
    EXPECT_FALSE(sec::is_wellformed_key("home..hero"));
    EXPECT_FALSE(sec::is_wellformed_key("Home.Hero"));
    // The characters that would matter if a key ever reached a path, a query or
    // a Redis key. It never does — it is compared against the table and either
    // matches or is rejected — and this is the second lock on that door.
    EXPECT_FALSE(sec::is_wellformed_key("home/hero"));
    EXPECT_FALSE(sec::is_wellformed_key("home$hero"));
    EXPECT_FALSE(sec::is_wellformed_key(std::string(49, 'a')));
}

TEST(SectionRegistry, ChoiceMembershipExcludesTheEmptyString) {
    EXPECT_TRUE(sec::is_choice(testapp::kIcons, "coffee"));
    EXPECT_TRUE(sec::is_choice(testapp::kIcons, "star"));
    EXPECT_FALSE(sec::is_choice(testapp::kIcons, "rocket"));
    // "none" is a legitimate stored value handled by the caller: whether a field
    // may be blank is `required`, not membership.
    EXPECT_FALSE(sec::is_choice(testapp::kIcons, ""));
}

// --- the compile-time validators, exercised at runtime ----------------------
//
// These run in a static_assert over the application's table, where a failure is
// a build error and prints no detail. Running them here is what says WHICH case
// each one catches.

TEST(SectionCompileTimeChecks, Utf8ValidatorRejectsWhatTheRuntimeOneDoes) {
    EXPECT_TRUE(sec::ct::is_valid_utf8("العنوان"));
    EXPECT_TRUE(sec::ct::is_valid_utf8(""));
    EXPECT_FALSE(sec::ct::is_non_empty_utf8(""));
    // An overlong NUL. Two spellings of one character is how a filter and a
    // consumer disagree about what a string says.
    EXPECT_FALSE(sec::ct::is_valid_utf8(std::string_view{"\xC0\x80", 2}));
    // A surrogate half, which is not a character at all.
    EXPECT_FALSE(sec::ct::is_valid_utf8(std::string_view{"\xED\xA0\x80", 3}));
    // An embedded NUL: legal UTF-8 structurally, and a truncation primitive
    // everywhere it is later handled as a C string.
    EXPECT_FALSE(sec::ct::is_valid_utf8(std::string_view{"a\0b", 3}));
    EXPECT_FALSE(sec::ct::is_valid_utf8("\xE2\x82"));       // truncated
    EXPECT_FALSE(sec::ct::is_valid_utf8("\xFF"));           // not a lead byte
}

TEST(SectionCompileTimeChecks, CodePointsNotBytes) {
    // Seven Arabic characters, fourteen bytes. A byte bound would give an Arabic
    // author half the allowance of an English one.
    EXPECT_EQ(sec::ct::count_code_points("العنوان"), 7U);
    EXPECT_EQ(sec::ct::count_code_points("Headline"), 8U);
}

TEST(SectionCompileTimeChecks, DefaultUrlsAreStricterThanRuntimeOnes) {
    EXPECT_TRUE(sec::ct::is_safe_default_url("#/events"));
    EXPECT_TRUE(sec::ct::is_safe_default_url("/about"));
    EXPECT_TRUE(sec::ct::is_safe_default_url("https://example.com/x"));
    // A compiled-in default has no excuse to be any of these.
    EXPECT_FALSE(sec::ct::is_safe_default_url("http://example.com"));
    EXPECT_FALSE(sec::ct::is_safe_default_url("javascript:alert(1)"));
    EXPECT_FALSE(sec::ct::is_safe_default_url("//evil.example"));
    EXPECT_FALSE(sec::ct::is_safe_default_url(""));
}

TEST(SectionCompileTimeChecks, ColourIsLowercaseSixDigitHex) {
    EXPECT_TRUE(sec::ct::is_hex_color("#134411"));
    EXPECT_FALSE(sec::ct::is_hex_color("#FFF"));
    EXPECT_FALSE(sec::ct::is_hex_color("#FFFFFF"));
    EXPECT_FALSE(sec::ct::is_hex_color("134411"));
}

// --- binding ----------------------------------------------------------------

TEST(SectionBind, AnUnknownKeyIsAnErrorAndNeverASilentDrop) {
    std::optional<sec::BindError> error;
    (void)bound(hero(), R"({"data":{"nope":"x"}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::NotAllowed);
    // Reported with an EMPTY field name, so a client cannot choose what appears
    // in a response or in a log line.
    EXPECT_TRUE(error->field.empty());
}

TEST(SectionBind, ALocalisedFieldNeedsEveryDeclaredLocale) {
    std::optional<sec::BindError> error;
    (void)bound(hero(), R"({"data":{"headline":{"en":"Hi"}}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::NotAllowed);

    // An extra member is refused too: a client that can send a key nobody reads
    // is a client discovering what this endpoint accepts.
    (void)bound(hero(), R"({"data":{"headline":{"en":"Hi","ar":"أهلا","fr":"Salut"}}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::NotAllowed);
}

TEST(SectionBind, ALocalisedPairIsStoredPerLocale) {
    std::optional<sec::BindError> error;
    const sec::SectionContent content =
        bound(hero(), R"({"data":{"headline":{"en":"Hi","ar":"أهلا"}}})", error);
    EXPECT_FALSE(error.has_value());
    const sec::SectionField* field = content.find("headline");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->value.text[en().index()], "Hi");
    EXPECT_EQ(field->value.text[ar().index()], "أهلا");
}

TEST(SectionBind, BlankingEveryLocaleClearsAnOptionalFieldAndNotARequiredOne) {
    std::optional<sec::BindError> error;
    // `subline` is optional. Without this, check_text's one-code-point minimum
    // means a staff member can fill a box but never empty one, and the only way
    // to take a line off the site is a deploy.
    const sec::SectionContent cleared =
        bound(hero(), R"({"data":{"subline":{"en":"","ar":""}}})", error);
    EXPECT_FALSE(error.has_value());
    // PRESENT in the patch, carrying an empty value in every locale — not
    // absent. An absent field is one the merge leaves alone, so dropping it here
    // would make "clear this line" do nothing at all.
    const sec::SectionField* subline = cleared.find("subline");
    ASSERT_NE(subline, nullptr);
    EXPECT_TRUE(subline->value.text[en().index()].empty());
    EXPECT_TRUE(subline->value.text[ar().index()].empty());

    // And the merge is where that matters: the stored value is replaced rather
    // than kept.
    const sec::SectionContent base = sec::default_content(hero(), defaults_for("home.hero"));
    ASSERT_NE(base.find("subline"), nullptr);
    EXPECT_FALSE(base.find("subline")->value.primary().empty());
    EXPECT_TRUE(sec::merge(base, cleared).find("subline")->value.primary().empty());

    // HALF blank is not a clear — it is a half-translated field, and it is
    // refused rather than filled in from the other locale.
    (void)bound(hero(), R"({"data":{"subline":{"en":"only english","ar":""}}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->field, "subline");
}

TEST(SectionBind, TypeBeforeValue) {
    std::optional<sec::BindError> error;
    // A Bool field handed a string, a Number field handed a string, and a
    // localised field handed a bare string. Which member of SectionValue is live
    // is decided by the registry, never by inspecting what arrived.
    (void)bound(hero(), R"({"data":{"show":"true"}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::BadFormat);

    (void)bound(hero(), R"({"data":{"headline":"Hi"}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::BadFormat);

    const sec::SectionSpec& contact = *sec::find_section(testapp::kSections, "contact.info");
    (void)bound(contact, R"({"data":{"seats":"48"}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::BadFormat);
}

TEST(SectionBind, AUrlFieldRefusesAStoredOpenRedirectAndStoredXss) {
    std::optional<sec::BindError> error;
    (void)bound(hero(), R"J({"data":{"cta_href":"javascript:alert(1)"}})J", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->field, "cta_href");

    const sec::SectionContent ok = bound(hero(), R"({"data":{"cta_href":"/events"}})", error);
    EXPECT_FALSE(error.has_value());
    ASSERT_NE(ok.find("cta_href"), nullptr);
    EXPECT_EQ(ok.find("cta_href")->value.primary(), "/events");
}

TEST(SectionBind, AChoiceFieldAcceptsOnlyItsOwnAllowListOrNothing) {
    std::optional<sec::BindError> error;
    const sec::SectionContent chosen = bound(about(), R"({"data":{"icon":"coffee"}})", error);
    EXPECT_FALSE(error.has_value());
    ASSERT_NE(chosen.find("icon"), nullptr);
    EXPECT_EQ(chosen.find("icon")->value.primary(), "coffee");

    // A value that is not in the list is a validation error at the moment it is
    // typed, rather than a saved value that renders as nothing at all.
    (void)bound(about(), R"({"data":{"icon":"rocket"}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::NotAllowed);

    // Blank is "no icon", and it reaches the allow-list ahead of check_text so
    // that it does not come back as TooShort.
    const sec::SectionContent blank = bound(about(), R"({"data":{"icon":""}})", error);
    EXPECT_FALSE(error.has_value());
    ASSERT_NE(blank.find("icon"), nullptr);
    EXPECT_TRUE(blank.find("icon")->value.primary().empty());
}

TEST(SectionBind, RichTextIsSanitisedOnWriteAndHostileMarkupIsRefused) {
    std::optional<sec::BindError> error;
    const sec::SectionContent clean = bound(
        about(), R"({"data":{"body":{"en":"<p>hello</p>","ar":"<p>أهلا</p>"}}})", error);
    EXPECT_FALSE(error.has_value());
    ASSERT_NE(clean.find("body"), nullptr);
    EXPECT_NE(clean.find("body")->value.primary().find("<p>"), std::string::npos);

    // REJECTED, not cleaned. A staff account submitting a script construct is a
    // signal worth an audit record, and a cleaned version silently tells the
    // submitter it worked.
    (void)bound(about(),
                R"({"data":{"body":{"en":"<script>x</script>","ar":"<p>أهلا</p>"}}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::NotAllowed);
}

TEST(SectionBind, AnUnknownImageSlotIsRefusedWithNoEcho) {
    std::optional<sec::BindError> error;
    (void)bound(hero(),
                R"({"images":{"nope":"00000000-0000-4000-8000-000000000001"}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->reason, anvil::input::Reason::NotAllowed);
    EXPECT_TRUE(error->field.empty());
}

TEST(SectionBind, AnImageSlotTakesAUuidAndNothingElse) {
    std::optional<sec::BindError> error;
    (void)bound(hero(), R"({"images":{"hero":"not-a-uuid"}})", error);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->field, "hero");

    const sec::SectionContent ok =
        bound(hero(), R"({"images":{"hero":"00000000-0000-4000-8000-000000000001"}})", error);
    EXPECT_FALSE(error.has_value());
    EXPECT_NE(ok.find_image("hero"), nullptr);
}

// --- merge, required, canonicalise ------------------------------------------

TEST(SectionMerge, RequiredIsCheckedAgainstTheMergedResultAndNotThePatch) {
    std::optional<sec::BindError> error;
    const sec::SectionContent base = sec::default_content(hero(), defaults_for("home.hero"));

    // A patch touching only the optional sub-line. Checked against the patch
    // alone this is missing `headline` and `cta_href`; checked against the merge
    // it is complete, which is what makes a partial update possible at all.
    const sec::SectionContent patch =
        bound(hero(), R"({"data":{"subline":{"en":"New","ar":"جديد"}}})", error);
    ASSERT_FALSE(error.has_value());
    EXPECT_TRUE(sec::check_required(hero(), patch).has_value());
    EXPECT_FALSE(sec::check_required(hero(), sec::merge(base, patch)).has_value());
}

TEST(SectionMerge, ARequiredLocalisedFieldMissingOneLocaleIsIncomplete) {
    sec::SectionContent content = sec::default_content(hero(), defaults_for("home.hero"));
    EXPECT_FALSE(sec::check_required(hero(), content).has_value());
    // One locale emptied. A page half-translated at render time is worse than
    // one that refused the write.
    for (sec::SectionField& field : content.fields) {
        if (field.key == "headline") { field.value.text[ar().index()].clear(); }
    }
    const std::optional<sec::BindError> missing = sec::check_required(hero(), content);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->field, "headline");
}

TEST(SectionMerge, CanonicaliseDropsWhatTheRegistryNoLongerDeclares) {
    sec::SectionContent content = sec::default_content(hero(), defaults_for("home.hero"));
    content.set("retired_field", sec::SectionValue{});
    content.set_image("retired_slot", Uuid{});

    const sec::SectionContent canonical = sec::canonicalise(hero(), content);
    EXPECT_EQ(canonical.find("retired_field"), nullptr);
    EXPECT_EQ(canonical.find_image("retired_slot"), nullptr);
    EXPECT_NE(canonical.find("headline"), nullptr);
}

TEST(SectionMerge, CanonicaliseIsRegistryOrderSoTwoInstancesAgreeByteForByte) {
    const sec::SectionContent base = sec::default_content(hero(), defaults_for("home.hero"));

    // The same content assembled in the opposite order. Two instances that
    // received the same patch with its keys in a different order must produce
    // identical bytes, or the etag is a function of the request rather than of
    // the content.
    sec::SectionContent reversed;
    for (auto it = base.fields.rbegin(); it != base.fields.rend(); ++it) {
        reversed.set(it->key, it->value);
    }

    const sec::SectionContent a = sec::canonicalise(hero(), base);
    const sec::SectionContent b = sec::canonicalise(hero(), reversed);
    ASSERT_EQ(a.fields.size(), b.fields.size());
    for (std::size_t i = 0; i < a.fields.size(); ++i) {
        EXPECT_EQ(a.fields[i].key, b.fields[i].key);
    }
    EXPECT_EQ(sec::serialize(hero(), a, 1, en(), "https://cdn.test/content").json,
              sec::serialize(hero(), b, 1, en(), "https://cdn.test/content").json);
    EXPECT_EQ(sec::content_etag(hero(), a), sec::content_etag(hero(), b));
}

// --- defaults ---------------------------------------------------------------

TEST(SectionDefaults, EveryDeclaredFieldGetsAValue) {
    // The one place an index is the right spelling: positional correspondence is
    // what bootstrap relies on, so this walks both tables the way bootstrap does.
    for (std::size_t i = 0; i < testapp::kSections.size(); ++i) {
        const sec::SectionContent content =
            sec::default_content(testapp::kSections[i], testapp::kDefaults[i]);
        EXPECT_EQ(content.fields.size(), testapp::kSections[i].fields.size())
            << testapp::kSections[i].key;
        // A complete, renderable section: a fresh deployment must not come up
        // with a required field missing.
        EXPECT_FALSE(sec::check_required(testapp::kSections[i], content).has_value())
            << testapp::kSections[i].key;
    }
}

TEST(SectionDefaults, NonTextTypesAreConvertedRatherThanStoredAsLiterals) {
    const sec::SectionContent contact =
        sec::default_content(*sec::find_section(testapp::kSections, "contact.info"),
                             defaults_for("contact.info"));
    ASSERT_NE(contact.find("seats"), nullptr);
    EXPECT_EQ(contact.find("seats")->value.number, 48);

    const sec::SectionContent hero_content = sec::default_content(hero(), defaults_for("home.hero"));
    ASSERT_NE(hero_content.find("show"), nullptr);
    EXPECT_TRUE(hero_content.find("show")->value.boolean);
}

TEST(SectionDefaults, FindDefaultsResolvesByKey) {
    EXPECT_NE(sec::find_defaults(testapp::kDefaults, "home.hero"), nullptr);
    EXPECT_EQ(sec::find_defaults(testapp::kDefaults, "home.nope"), nullptr);
}

// --- serialisation ----------------------------------------------------------

TEST(SectionSerialize, OneLocalePerPayload) {
    const sec::SectionContent content = sec::default_content(hero(), defaults_for("home.hero"));
    const sec::SerializedSection english =
        sec::serialize(hero(), content, 7, en(), "https://cdn.test/content");
    const sec::SerializedSection arabic =
        sec::serialize(hero(), content, 7, ar(), "https://cdn.test/content");

    EXPECT_NE(english.json.find("\"lang\":\"en\""), std::string::npos);
    EXPECT_NE(arabic.json.find("\"lang\":\"ar\""), std::string::npos);
    EXPECT_NE(english.json.find("Where community meets exploration."), std::string::npos);
    // Returning every locale would multiply the payload for content all but one
    // of which is discarded on arrival.
    EXPECT_EQ(english.json.find("المكان اللي بيجمع الناس والطريق."), std::string::npos);
    EXPECT_NE(arabic.json.find("المكان اللي بيجمع الناس والطريق."), std::string::npos);
    EXPECT_NE(english.etag, arabic.etag);
    EXPECT_EQ(english.version, 7);
}

TEST(SectionSerialize, NonLatinTextIsRawUtf8AndNeverEscaped) {
    const sec::SectionContent content = sec::default_content(hero(), defaults_for("home.hero"));
    const sec::SerializedSection arabic =
        sec::serialize(hero(), content, 1, ar(), "https://cdn.test/content");
    // \uXXXX would cost six bytes per character instead of two.
    EXPECT_EQ(arabic.json.find("\\u"), std::string::npos);
}

TEST(SectionSerialize, EveryComponentOfAnImageUrlIsServerGenerated) {
    sec::SectionContent content = sec::default_content(hero(), defaults_for("home.hero"));
    const Uuid id = *anvil::uuid::parse("11111111-2222-4333-8444-555555555555");
    content.set_image("hero", id);

    const sec::SerializedSection payload =
        sec::serialize(hero(), sec::canonicalise(hero(), content), 1, en(),
                       "https://cdn.test/content");
    EXPECT_NE(payload.json.find("https://cdn.test/content/11111111-2222-4333-8444-555555555555"),
              std::string::npos);
}

TEST(SectionSerialize, TheContentEtagMovesWhenAnyLocaleMoves) {
    const sec::SectionContent base =
        sec::canonicalise(hero(), sec::default_content(hero(), defaults_for("home.hero")));
    sec::SectionContent changed = base;
    for (sec::SectionField& field : changed.fields) {
        // A change confined to the NON-default locale. A digest taken over one
        // rendering would miss it, and a writer comparing etags would decide
        // nothing had changed.
        if (field.key == "headline") { field.value.text[ar().index()] = "مختلف"; }
    }
    EXPECT_NE(sec::content_etag(hero(), base), sec::content_etag(hero(), changed));
}

TEST(SectionSerialize, ImageSpecsCarryABilingualLabelAndANullAspectForNoConstraint) {
    const std::string hero_specs = sec::serialize_image_specs(hero());
    EXPECT_NE(hero_specs.find("\"aspect\":{\"num\":16,\"den\":9}"), std::string::npos);
    EXPECT_NE(hero_specs.find("\"min_width\":1920"), std::string::npos);
    EXPECT_NE(hero_specs.find("بانر رئيسي"), std::string::npos);

    // 0/0 serialises as null, never as `{"num":0,"den":0}`: an object is always
    // truthy, so the latter prints "shaped 0:0" beside a slot that asks nothing.
    const std::string about_specs = sec::serialize_image_specs(about());
    EXPECT_NE(about_specs.find("\"aspect\":null"), std::string::npos);
}

TEST(SectionSerialize, TheRegistryEndpointCarriesTheChoicesWithTheField) {
    const std::string registry = sec::serialize_registry(testapp::kSections);
    EXPECT_NE(registry.find("\"key\":\"home.hero\""), std::string::npos);
    EXPECT_NE(registry.find("\"type\":\"Choice\""), std::string::npos);
    // An editor holding its own copy of the allow-list would drift the first
    // time a value is added, and the failure would be a staff member choosing a
    // value the server refuses.
    EXPECT_NE(registry.find(R"("options":["calendar","coffee","map-pin","star"])"),
              std::string::npos);
    EXPECT_NE(registry.find("\"localized\":true"), std::string::npos);
    EXPECT_NE(registry.find("\"max_cp\":80"), std::string::npos);
    // The choices travel only with the field that has them.
    EXPECT_EQ(registry.find("\"type\":\"Text\",\"max_cp\":80,\"localized\":true,"
                            "\"required\":true,\"options\""),
              std::string::npos);
}

// --- the buffers the bound protects -----------------------------------------

TEST(SectionIndex, ContentIsIndexedIntoRegistryOrderInOnePass) {
    const sec::SectionContent content =
        sec::canonicalise(hero(), sec::default_content(hero(), defaults_for("home.hero")));
    const sec::FieldIndex index = sec::index_content(hero(), content);
    for (std::size_t i = 0; i < hero().fields.size(); ++i) {
        ASSERT_NE(index.fields[i], nullptr) << hero().fields[i].key;
        EXPECT_EQ(index.fields[i]->key, hero().fields[i].key);
    }
    // A slot the content does not carry is a null entry rather than a gap that
    // shifts every later index.
    EXPECT_EQ(index.images[0], nullptr);
}

}  // namespace
