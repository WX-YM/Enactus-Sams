// The entry registry's compile-time checks, the slug rule, the members an entry
// request carries beside its content, and the JSON an editor reads. No database:
// what the server enforces is tests/entries_db_test.cc.

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/entries/payload.h"
#include "anvil/entries/registry.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "testapp/entries.h"

namespace {

using anvil::Uuid;
namespace ent = anvil::entries;
namespace sec = anvil::sections;

class Body final {
public:
    explicit Body(std::string text)
        : text_{std::move(text)},
          document_{anvil::input::parse_json(text_, arena_, anvil::input::JsonLimits{})} {}

    [[nodiscard]] const anvil::input::JsonValue& root() const { return document_.root(); }
    [[nodiscard]] bool ok() const { return document_.ok(); }

private:
    std::string                text_;
    anvil::input::BodyArena    arena_;
    anvil::input::JsonDocument document_;
};

const ent::KindSpec& gallery() { return *ent::find_kind(testapp::kKinds, "gallery.item"); }

// --- the table checks, each refusal proved against a table that breaks it ----------

inline constexpr std::array<sec::FieldSpec, 1> kTitle{{
    {"title", {{"Title", "العنوان"}}, {}, 60, sec::FieldType::Text, true, true},
}};

constexpr ent::KindSpec root(std::string_view key, std::string_view parent = "",
                             std::uint32_t capacity = 10,
                             ent::Ordering ordering = ent::Ordering::Newest) {
    return ent::KindSpec{{key, "/", kTitle, {}}, {}, parent, capacity,
                         ent::Workflow::Immediate, ordering, ent::SlugRule::None};
}

static_assert(ent::kinds_are_well_formed(std::array{root("a.x"), root("b.x", "a.x")}));
static_assert(!ent::kinds_are_well_formed(std::array{root("b.x"), root("a.x")}),
              "unsorted");
static_assert(!ent::kinds_are_well_formed(std::array{root("a.x", "nope")}),
              "a parent that is not declared");
static_assert(!ent::kinds_are_well_formed(std::array{root("a.x", "b.x"), root("b.x", "a.x")}),
              "a parent chain that loops");
static_assert(!ent::kinds_are_well_formed(std::array{root("a.x", "a.x")}), "its own parent");
static_assert(!ent::kinds_are_well_formed(std::array{root("a.x", "", 0)}), "zero capacity");
static_assert(!ent::kinds_are_well_formed(
                  std::array{root("a.x", "", ent::kMaxManualCapacity + 1, ent::Ordering::Manual)}),
              "a manual scope too large to reorder in one transaction");
static_assert(ent::kinds_are_well_formed(
    std::array{root("a.x", "", ent::kMaxManualCapacity + 1, ent::Ordering::Newest)}));

inline constexpr std::array<ent::FlagSpec, 2> kTwice{{
    {"pinned", {{"Pinned", "متثبت"}}},
    {"pinned", {{"Again", "تاني"}}},
}};
inline constexpr std::array<ent::FlagSpec, 1> kDotted{{{"a.b", {{"Dotted", "نقطة"}}}}};
static_assert(!ent::kinds_are_well_formed(std::array{ent::KindSpec{
                  {"a.x", "/", kTitle, {}}, kTwice, "", 10, ent::Workflow::Immediate,
                  ent::Ordering::Newest, ent::SlugRule::None}}),
              "a flag declared twice");
static_assert(!ent::kinds_are_well_formed(std::array{ent::KindSpec{
                  {"a.x", "/", kTitle, {}}, kDotted, "", 10, ent::Workflow::Immediate,
                  ent::Ordering::Newest, ent::SlugRule::None}}),
              "a flag name that reads as a path");

// Seeds: a slug the rule refuses, a seed on a child kind, and too many for the kind.
inline constexpr std::array<sec::DefaultField, 2> kItem{{
    {"name", {{"Name", "اسم"}}},
    {"link", {{"/x", ""}}},
}};
inline constexpr std::array<sec::DefaultImage, 1> kShot{{{"shot", "x.png"}}};
inline constexpr std::array<ent::EntrySeed, 1> kBadSlug{{{"Bad Slug", {"gallery.item", kItem, kShot}, 0}}};
static_assert(!ent::seeds_match_kinds(testapp::kKinds, std::array{ent::KindSeeds{"gallery.item", kBadSlug}}));
inline constexpr std::array<ent::EntrySeed, 1> kBadFlag{{{"ok", {"gallery.item", kItem, kShot}, 2}}};
static_assert(!ent::seeds_match_kinds(testapp::kKinds, std::array{ent::KindSeeds{"gallery.item", kBadFlag}}),
              "a bit the kind does not declare");
inline constexpr std::array<ent::EntrySeed, 6> kTooMany{{
    {"a", {"gallery.item", kItem, kShot}, 0}, {"b", {"gallery.item", kItem, kShot}, 0},
    {"c", {"gallery.item", kItem, kShot}, 0}, {"d", {"gallery.item", kItem, kShot}, 0},
    {"e", {"gallery.item", kItem, kShot}, 0}, {"f", {"gallery.item", kItem, kShot}, 0},
}};
static_assert(!ent::seeds_match_kinds(testapp::kKinds, std::array{ent::KindSeeds{"gallery.item", kTooMany}}));
inline constexpr std::array<sec::DefaultField, 1> kReply{{{"text", {{"Hi", ""}}}}};
inline constexpr std::array<ent::EntrySeed, 1> kChild{{{"", {"forum.reply", kReply, {}}, 0}}};
static_assert(!ent::seeds_match_kinds(testapp::kKinds, std::array{ent::KindSeeds{"forum.reply", kChild}}),
              "a child kind cannot be seeded: its parent id is not a compile-time fact");

// --- slugs -----------------------------------------------------------------------------

static_assert(ent::is_wellformed_slug("portfolio"));
static_assert(ent::is_wellformed_slug("garden-cafe-2026"));
static_assert(!ent::is_wellformed_slug(""));
static_assert(!ent::is_wellformed_slug("-lead"));
static_assert(!ent::is_wellformed_slug("trail-"));
static_assert(!ent::is_wellformed_slug("dou--ble"));
static_assert(!ent::is_wellformed_slug("Upper"));
static_assert(!ent::is_wellformed_slug("sp ace"));
static_assert(!ent::is_wellformed_slug("dot.ted"));
static_assert(!ent::is_wellformed_slug("\xd9\x85\xd8\xab\xd8\xa7\xd9\x84"), "ASCII only");
static_assert(ent::is_wellformed_slug(std::string_view{
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}));
static_assert(!ent::is_wellformed_slug(std::string_view{
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}));

// --- flags -----------------------------------------------------------------------------

static_assert(ent::flag_bit(*ent::find_kind(testapp::kKinds, "blog.post"), "pinned") == 1);
static_assert(ent::flag_bit(*ent::find_kind(testapp::kKinds, "blog.post"), "featured") == 2);
static_assert(ent::flag_bit(*ent::find_kind(testapp::kKinds, "blog.post"), "nope") == 0);
static_assert(ent::declared_flags(*ent::find_kind(testapp::kKinds, "blog.post")) == 3);

TEST(EntryPayload, FlagsBindToBitsToSetAndClear) {
    const Body body{R"({"flags":{"pinned":true}})"};
    ASSERT_TRUE(body.ok());
    ent::FlagSet set = 0;
    ent::FlagSet clear = 0;
    EXPECT_FALSE(ent::bind_flags(gallery(), body.root().find("flags"), set, clear).has_value());
    EXPECT_EQ(set, 1);
    EXPECT_EQ(clear, 0);

    const Body off{R"({"flags":{"pinned":false}})"};
    EXPECT_FALSE(ent::bind_flags(gallery(), off.root().find("flags"), set, clear).has_value());
    EXPECT_EQ(set, 0);
    EXPECT_EQ(clear, 1);
}

TEST(EntryPayload, AnUnknownFlagIsRefusedWithoutEchoingItsName) {
    const Body body{R"({"flags":{"admin":true}})"};
    ent::FlagSet set = 0;
    ent::FlagSet clear = 0;
    const auto error = ent::bind_flags(gallery(), body.root().find("flags"), set, clear);
    ASSERT_TRUE(error.has_value());
    EXPECT_TRUE(error->field.empty());
}

TEST(EntryPayload, AFlagMustBeABooleanAndNamedOnce) {
    ent::FlagSet set = 0;
    ent::FlagSet clear = 0;
    const Body text{R"({"flags":{"pinned":"yes"}})"};
    const auto error = ent::bind_flags(gallery(), text.root().find("flags"), set, clear);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->field, "pinned");

    // Two answers to one question never reach the binder.
    const Body twice{R"({"flags":{"pinned":true,"pinned":false}})"};
    EXPECT_FALSE(twice.ok());
}

TEST(EntryPayload, AnOrderIsBoundedAndEveryIdParses) {
    const std::string a = anvil::uuid::to_string(anvil::uuid::generate_v7());
    const std::string b = anvil::uuid::to_string(anvil::uuid::generate_v7());
    const Body body{R"({"order":[")" + a + R"(",")" + b + R"("]})"};
    std::vector<Uuid> out;
    EXPECT_FALSE(ent::bind_order(body.root().find("order"), 5, out).has_value());
    EXPECT_EQ(out.size(), 2U);
    EXPECT_TRUE(ent::bind_order(body.root().find("order"), 1, out).has_value());

    const Body bad{R"({"order":["not-an-id"]})"};
    EXPECT_TRUE(ent::bind_order(bad.root().find("order"), 5, out).has_value());
    const Body absent{R"({})"};
    EXPECT_TRUE(ent::bind_order(absent.root().find("order"), 5, out).has_value());
}

TEST(EntryPayload, ASlugBindsOnlyWhenWellFormed) {
    std::string out;
    const Body good{R"({"slug":"garden-cafe"})"};
    EXPECT_FALSE(ent::bind_slug(good.root().find("slug"), out).has_value());
    EXPECT_EQ(out, "garden-cafe");
    const Body bad{R"({"slug":"Garden Cafe"})"};
    EXPECT_TRUE(ent::bind_slug(bad.root().find("slug"), out).has_value());
    const Body number{R"({"slug":7})"};
    EXPECT_TRUE(ent::bind_slug(number.root().find("slug"), out).has_value());
}

// --- the editor's JSON ---------------------------------------------------------------

TEST(EntryPayload, TheKindTableCarriesConfigurationFlagsAndShape) {
    const std::string json = ent::serialize_kinds(testapp::kKinds);
    const Body body{json};
    ASSERT_TRUE(body.ok()) << json;
    const anvil::input::JsonValue* kinds = body.root().find("kinds");
    ASSERT_NE(kinds, nullptr);
    ASSERT_EQ(kinds->elements().size(), testapp::kKinds.size());

    const anvil::input::JsonValue& reply = kinds->elements()[1];
    EXPECT_EQ(reply.find("key")->as_string(), "forum.reply");
    EXPECT_EQ(reply.find("parent")->as_string(), "forum.thread");
    EXPECT_EQ(reply.find("workflow")->as_string(), "immediate");
    EXPECT_EQ(reply.find("ordering")->as_string(), "oldest");

    const anvil::input::JsonValue& item = kinds->elements()[3];
    EXPECT_EQ(item.find("slug")->as_string(), "unique");
    EXPECT_EQ(item.find("flags")->elements()[0].find("key")->as_string(), "pinned");
    // The shape is the section registry's own element, so one editor control
    // serves both.
    EXPECT_EQ(item.find("shape")->find("images")->elements()[0].find("slot")->as_string(),
              "shot");
}

TEST(EntryPayload, AnEntryCarriesEveryLocaleAndEveryFlagByName) {
    ent::EntryDocument entry{};
    entry.kind = &gallery();
    entry.id = anvil::uuid::generate_v7();
    entry.created_by = entry.id;
    entry.slug = "first";
    entry.flags = 1;
    entry.version = 3;
    ent::EntryContent copy{};
    sec::SectionValue name{};
    name.text[0] = "First";
    name.text[1] = "الأول";
    copy.content.set("name", name);
    copy.updated_by = entry.id;
    entry.draft = copy;

    const std::string json =
        ent::serialize_entry(entry, ent::ImageLinks{.base = "/media", .suffix = "/thumb"});
    const Body body{json};
    ASSERT_TRUE(body.ok()) << json;
    EXPECT_EQ(body.root().find("slug")->as_string(), "first");
    EXPECT_EQ(body.root().find("flags")->find("pinned")->as_bool(), true);
    EXPECT_TRUE(body.root().find("published")->is_null());
    const anvil::input::JsonValue* data = body.root().find("draft")->find("data");
    EXPECT_EQ(data->find("name")->find("ar")->as_string(), "الأول");
    // A field the copy does not hold is present, as null.
    EXPECT_TRUE(data->find("link")->is_null());
    EXPECT_TRUE(body.root().find("draft")->find("images")->find("shot")->is_null());
}

}  // namespace
