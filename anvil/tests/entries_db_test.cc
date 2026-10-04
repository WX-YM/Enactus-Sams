// Entries against a live cluster.
//
// Every case here asserts a property the SERVER provides and a unit test cannot
// see: that the unique slug index is the uniqueness constraint, that a
// conditional increment bounds a parent's children under the transaction, that
// a published read never carries the draft subtree, that the listing index
// returns pages in order, that the seed claim is once per kind across boots,
// and that media reference counts move with the write that changed them.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/entries/payload.h"
#include "anvil/entries/service.h"
#include "anvil/media/repository.h"
#include "anvil/media/service.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/entries.h"
#include "testapp/namespaces.h"

namespace {

using anvil::ErrorCode;
using anvil::Uuid;
using anvil::fs::Format;
using anvil::fs::Mime;
using anvil::images::VariantRecord;
using anvil::media::MediaRepository;
using anvil::media::MediaService;
using anvil::media::NewMedia;
using anvil::testfixture::scratch_names;
namespace ent = anvil::entries;
namespace sec = anvil::sections;

constexpr std::string_view kMedia = "media";

const Uuid& actor() {
    static const Uuid id = anvil::uuid::generate_v7();
    return id;
}

const Uuid& someone_else() {
    static const Uuid id = anvil::uuid::generate_v7();
    return id;
}

const ent::KindSpec& kind(std::string_view key) { return *ent::find_kind(testapp::kKinds, key); }
const ent::KindSpec& posts() { return kind("blog.post"); }
const ent::KindSpec& threads() { return kind("forum.thread"); }
const ent::KindSpec& replies() { return kind("forum.reply"); }
const ent::KindSpec& gallery() { return kind("gallery.item"); }

sec::SectionValue localized(std::string_view en, std::string_view ar) {
    sec::SectionValue value{};
    value.text[0] = std::string{en};
    value.text[1] = std::string{ar};
    return value;
}

sec::SectionValue plain(std::string_view text) {
    sec::SectionValue value{};
    value.primary() = std::string{text};
    return value;
}

sec::SectionContent post(std::string_view title) {
    sec::SectionContent content;
    content.set("title", localized(title, "عنوان"));
    return content;
}

sec::SectionContent item(std::string_view name, std::optional<Uuid> shot = std::nullopt) {
    sec::SectionContent content;
    content.set("name", localized(name, "اسم"));
    content.set("link", plain("/somewhere"));
    if (shot.has_value()) { content.set_image("shot", *shot); }
    return content;
}

sec::SectionContent one_line(std::string_view key, std::string_view text) {
    sec::SectionContent content;
    content.set(std::string{key}, plain(text));
    return content;
}

class EntryDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        ANVIL_REQUIRE_TRANSACTIONS();
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, testapp::kEntriesCollection);
        anvil::testfixture::clear_collection(**client_, kMedia);
        media_ = std::make_unique<MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);
        service_ = make_service([this](std::string_view kind) { heard_.emplace_back(kind); });
    }

    [[nodiscard]] std::unique_ptr<ent::EntryService> make_service(
        std::function<void(std::string_view)> hook = {}) {
        return std::make_unique<ent::EntryService>(
            std::string{scratch_names().for_collection(testapp::kEntriesCollection)},
            testapp::kEntriesCollection, testapp::kKinds, *media_,
            ent::EntryServiceConfig{.image_namespace = testapp::kContent,
                                    .channel_prefix = "anvil_t_ent",
                                    .on_invalidated = std::move(hook)});
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }
    [[nodiscard]] ent::EntryService& entries() { return *service_; }

    // A media row with no files behind it: everything asserted about images
    // here is a property of the row.
    [[nodiscard]] Uuid store_image(std::uint8_t seed) {
        const Uuid id = anvil::uuid::generate_v7();
        anvil::crypto::Digest256 sha{};
        sha[0] = seed;
        const NewMedia row{
            .variants = {VariantRecord{2048, 320, 180, Format::Avif}},
            .sha256 = sha,
            .bytes = 1000,
            .id = id,
            .owner = actor(),
            .uploader_ip = std::nullopt,
            .width = 1600,
            .height = 900,
            .ns = testapp::kContent,
            .mime = Mime::Jpeg,
        };
        EXPECT_TRUE(media_rows().insert(db(), row).ok());
        return id;
    }

    [[nodiscard]] std::int32_t refs(const Uuid& id) {
        return media_rows().find(db(), testapp::kContent, id).value()->refs;
    }

    [[nodiscard]] static MediaRepository media_rows() {
        return MediaRepository{std::string{scratch_names().for_collection(kMedia)}, kMedia};
    }

    [[nodiscard]] ent::EntryWriteOutcome create_post(std::string_view slug,
                                                     std::string_view title = "Hello") {
        ent::NewEntry entry{};
        entry.content = post(title);
        entry.slug = std::string{slug};
        const auto created = entries().create(db(), posts(), entry, actor());
        EXPECT_TRUE(created.ok());
        return created.value();
    }

    [[nodiscard]] ent::EntryWriteOutcome create_item(std::string_view slug,
                                                     std::optional<Uuid> shot = std::nullopt) {
        ent::NewEntry entry{};
        entry.content = item(slug, shot);
        entry.slug = std::string{slug};
        const auto created = entries().create(db(), gallery(), entry, actor());
        EXPECT_TRUE(created.ok()) << static_cast<int>(created.code());
        return created.value();
    }

    [[nodiscard]] std::vector<std::string> slugs(const ent::KindSpec& k, ent::EntryQuery query) {
        const auto page = entries().list(db(), k, query);
        EXPECT_TRUE(page.ok());
        std::vector<std::string> out;
        for (const ent::EntryDocument& e : page.value().entries) { out.push_back(e.slug); }
        return out;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
    std::unique_ptr<MediaService>          media_;
    std::unique_ptr<ent::EntryService>     service_;
    std::vector<std::string>               heard_;
};

// --- the editorial workflow --------------------------------------------------------

TEST_F(EntryDb, AnEditorialEntryIsInvisibleUntilPublished) {
    const ent::EntryWriteOutcome created = create_post("hello");

    const auto reader = entries().find(db(), posts(), created.id, ent::Stage::Published);
    ASSERT_TRUE(reader.ok());
    EXPECT_FALSE(reader.value().has_value());
    EXPECT_FALSE(entries().find_by_slug(db(), posts(), "hello", ent::Stage::Published)
                     .value()
                     .has_value());
    EXPECT_TRUE(slugs(posts(), {.stage = ent::Stage::Published}).empty());

    const auto editor = entries().find(db(), posts(), created.id, ent::Stage::Working);
    ASSERT_TRUE(editor.ok());
    ASSERT_TRUE(editor.value().has_value());
    EXPECT_FALSE(editor.value()->live());
    EXPECT_TRUE(editor.value()->draft.has_value());
    // A draft save changes nothing a reader sees, so nobody is told.
    EXPECT_TRUE(heard_.empty());

    ASSERT_TRUE(entries().publish(db(), posts(), created.id, created.version, actor()).ok());
    EXPECT_EQ(slugs(posts(), {.stage = ent::Stage::Published}),
              std::vector<std::string>{"hello"});
    EXPECT_EQ(heard_, std::vector<std::string>{"blog.post"});
}

TEST_F(EntryDb, APublishedReadNeverCarriesTheDraft) {
    const ent::EntryWriteOutcome created = create_post("leak", "Public title");
    const auto published =
        entries().publish(db(), posts(), created.id, created.version, actor());
    ASSERT_TRUE(published.ok());

    ent::EntryEdit edit{};
    edit.patch = post("SECRET DRAFT TITLE");
    ASSERT_TRUE(entries()
                    .write(db(), posts(), created.id, published.value().version, edit, actor())
                    .ok());

    for (const auto& read :
         {entries().find(db(), posts(), created.id, ent::Stage::Published),
          entries().find_by_slug(db(), posts(), "leak", ent::Stage::Published)}) {
        ASSERT_TRUE(read.ok());
        ASSERT_TRUE(read.value().has_value());
        EXPECT_FALSE(read.value()->draft.has_value());
        EXPECT_EQ(read.value()->published->content.find("title")->value.text[0], "Public title");
    }
    const auto page = entries().list(db(), posts(), {.stage = ent::Stage::Published});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().entries.size(), 1U);
    EXPECT_FALSE(page.value().entries[0].draft.has_value());

    // The serialised form too: the string appears nowhere in the reader's bytes.
    const std::string json =
        ent::serialize_page(page.value(), ent::ImageLinks{.base = "/m", .suffix = ""});
    EXPECT_EQ(json.find("SECRET"), std::string::npos);
}

TEST_F(EntryDb, UnpublishTakesItOffTheSiteAndKeepsTheDraft) {
    const ent::EntryWriteOutcome created = create_post("gone");
    const auto published = entries().publish(db(), posts(), created.id, created.version, actor());
    ASSERT_TRUE(published.ok());
    ASSERT_TRUE(
        entries().unpublish(db(), posts(), created.id, published.value().version, actor()).ok());

    EXPECT_FALSE(
        entries().find(db(), posts(), created.id, ent::Stage::Published).value().has_value());
    const auto editor = entries().find(db(), posts(), created.id, ent::Stage::Working);
    ASSERT_TRUE(editor.value().has_value());
    EXPECT_TRUE(editor.value()->draft.has_value());
}

TEST_F(EntryDb, AStaleVersionIsRefused) {
    const ent::EntryWriteOutcome created = create_post("stale");
    ent::EntryEdit edit{};
    edit.patch = post("First");
    ASSERT_TRUE(entries().write(db(), posts(), created.id, created.version, edit, actor()).ok());
    edit.patch = post("Second");
    EXPECT_EQ(entries().write(db(), posts(), created.id, created.version, edit, actor()).code(),
              ErrorCode::VersionMismatch);
}

TEST_F(EntryDb, ARequiredFieldIsRequiredOnCreate) {
    ent::NewEntry entry{};
    entry.slug = "empty";
    const auto created = entries().create(db(), posts(), entry, actor());
    EXPECT_EQ(created.code(), ErrorCode::ValidationFailed);
}

// --- identity ------------------------------------------------------------------------

TEST_F(EntryDb, ASlugIsUniqueWithinItsKind) {
    (void)create_post("same");
    ent::NewEntry again{};
    again.content = post("Other");
    again.slug = "same";
    EXPECT_EQ(entries().create(db(), posts(), again, actor()).code(), ErrorCode::Conflict);

    // A different kind is a different namespace of slugs.
    (void)create_item("same");
}

TEST_F(EntryDb, ASlugRuleIsEnforcedBothWays) {
    ent::NewEntry bad{};
    bad.content = post("Bad");
    bad.slug = "Not A Slug";
    EXPECT_EQ(entries().create(db(), posts(), bad, actor()).code(), ErrorCode::ValidationFailed);

    ent::NewEntry thread{};
    thread.content = one_line("subject", "Hi");
    thread.slug = "threads-have-none";
    EXPECT_EQ(entries().create(db(), threads(), thread, actor()).code(),
              ErrorCode::ValidationFailed);
}

TEST_F(EntryDb, AnIdIsNotFoundUnderAnotherKind) {
    const ent::EntryWriteOutcome created = create_post("mine");
    EXPECT_FALSE(
        entries().find(db(), gallery(), created.id, ent::Stage::Working).value().has_value());
}

TEST_F(EntryDb, ASlugMovesWithAnEdit) {
    const ent::EntryWriteOutcome created = create_post("before");
    ent::EntryEdit edit{};
    edit.slug = "after";
    ASSERT_TRUE(entries().write(db(), posts(), created.id, created.version, edit, actor()).ok());
    EXPECT_TRUE(
        entries().find_by_slug(db(), posts(), "after", ent::Stage::Working).value().has_value());
    EXPECT_FALSE(
        entries().find_by_slug(db(), posts(), "before", ent::Stage::Working).value().has_value());
}

// --- order and pages -------------------------------------------------------------------

TEST_F(EntryDb, ATimeOrderedKindPagesNewestFirstWithoutRepeats) {
    std::vector<Uuid> made;
    for (int i = 0; i < 5; ++i) {
        ent::NewEntry entry{};
        entry.content = one_line("subject", "thread " + std::to_string(i));
        const auto created = entries().create(db(), threads(), entry, actor());
        ASSERT_TRUE(created.ok());
        made.push_back(created.value().id);
    }
    std::reverse(made.begin(), made.end());

    std::vector<Uuid> seen;
    std::optional<ent::EntryCursor> after;
    for (int guard = 0; guard < 10; ++guard) {
        const auto page = entries().list(
            db(), threads(), {.after = after, .limit = 2, .stage = ent::Stage::Published});
        ASSERT_TRUE(page.ok());
        for (const ent::EntryDocument& e : page.value().entries) { seen.push_back(e.id); }
        if (!page.value().next.has_value()) { break; }
        after = page.value().next;
    }
    EXPECT_EQ(seen, made);
}

TEST_F(EntryDb, AManualKindKeepsTheOrderStaffSet) {
    const ent::EntryWriteOutcome a = create_item("a");
    const ent::EntryWriteOutcome b = create_item("b");
    const ent::EntryWriteOutcome c = create_item("c");
    EXPECT_EQ(slugs(gallery(), {.stage = ent::Stage::Working}),
              (std::vector<std::string>{"a", "b", "c"}));

    const std::array<Uuid, 3> reversed{c.id, b.id, a.id};
    ASSERT_TRUE(entries().reorder(db(), gallery(), std::nullopt, reversed).ok());
    EXPECT_EQ(slugs(gallery(), {.stage = ent::Stage::Working}),
              (std::vector<std::string>{"c", "b", "a"}));

    // A caller looking at a stale list — here, one missing an entry — is told
    // so rather than having the entry it did not name dropped to the end.
    const std::array<Uuid, 2> partial{a.id, b.id};
    EXPECT_EQ(entries().reorder(db(), gallery(), std::nullopt, partial).code(),
              ErrorCode::Conflict);
    const std::array<Uuid, 3> twice{a.id, a.id, b.id};
    EXPECT_EQ(entries().reorder(db(), gallery(), std::nullopt, twice).code(),
              ErrorCode::Conflict);
}

TEST_F(EntryDb, ARootKindStopsAtItsCapacity) {
    for (int i = 0; i < 5; ++i) { (void)create_item("item-" + std::to_string(i)); }
    ent::NewEntry sixth{};
    sixth.content = item("six");
    sixth.slug = "six";
    EXPECT_EQ(entries().create(db(), gallery(), sixth, actor()).code(), ErrorCode::Conflict);
}

// --- flags ---------------------------------------------------------------------------------

TEST_F(EntryDb, AFlagFiltersAListing) {
    const ent::EntryWriteOutcome a = create_item("a");
    const ent::EntryWriteOutcome b = create_item("b");
    ASSERT_TRUE(entries().publish(db(), gallery(), a.id, a.version, actor()).ok());
    ASSERT_TRUE(entries().publish(db(), gallery(), b.id, b.version, actor()).ok());

    const ent::FlagSet pinned = ent::flag_bit(gallery(), "pinned");
    ASSERT_TRUE(entries().set_flags(db(), gallery(), b.id, pinned, 0).ok());
    EXPECT_EQ(slugs(gallery(), {.flags_all = pinned, .stage = ent::Stage::Published}),
              std::vector<std::string>{"b"});

    ASSERT_TRUE(entries().set_flags(db(), gallery(), b.id, 0, pinned).ok());
    EXPECT_TRUE(slugs(gallery(), {.flags_all = pinned, .stage = ent::Stage::Published}).empty());
}

TEST_F(EntryDb, AFlagToggleDoesNotStaleAnOpenEditor) {
    const ent::EntryWriteOutcome a = create_item("a");
    ASSERT_TRUE(
        entries().set_flags(db(), gallery(), a.id, ent::flag_bit(gallery(), "pinned"), 0).ok());
    ent::EntryEdit edit{};
    edit.patch = item("renamed");
    EXPECT_TRUE(entries().write(db(), gallery(), a.id, a.version, edit, actor()).ok());
}

TEST_F(EntryDb, AnUndeclaredOrContradictoryFlagIsRefused) {
    const ent::EntryWriteOutcome a = create_item("a");
    EXPECT_EQ(entries().set_flags(db(), gallery(), a.id, 1, 1).code(),
              ErrorCode::ValidationFailed);
    EXPECT_EQ(entries().set_flags(db(), gallery(), a.id, 1U << 5U, 0).code(),
              ErrorCode::ValidationFailed);
}

// --- parents -------------------------------------------------------------------------------

TEST_F(EntryDb, AParentBoundsItsChildrenExactly) {
    ent::NewEntry thread{};
    thread.content = one_line("subject", "Topic");
    const auto parent = entries().create(db(), threads(), thread, actor());
    ASSERT_TRUE(parent.ok());
    const Uuid id = parent.value().id;

    std::vector<ent::EntryWriteOutcome> made;
    for (int i = 0; i < 3; ++i) {
        ent::NewEntry reply{};
        reply.content = one_line("text", "reply " + std::to_string(i));
        reply.parent = id;
        const auto created = entries().create(db(), replies(), reply, actor());
        ASSERT_TRUE(created.ok());
        made.push_back(created.value());
    }
    ent::NewEntry fourth{};
    fourth.content = one_line("text", "one too many");
    fourth.parent = id;
    const auto refused = entries().create(db(), replies(), fourth, actor());
    EXPECT_EQ(refused.code(), ErrorCode::Conflict);

    EXPECT_EQ(entries().find(db(), threads(), id, ent::Stage::Working).value()->children, 3);
    EXPECT_EQ(entries().remove(db(), threads(), id, parent.value().version).code(),
              ErrorCode::Conflict);

    ASSERT_TRUE(entries().remove(db(), replies(), made[0].id, made[0].version).ok());
    EXPECT_EQ(entries().find(db(), threads(), id, ent::Stage::Working).value()->children, 2);

    const auto listed = entries().list(db(), replies(), {.parent = id});
    ASSERT_TRUE(listed.ok());
    EXPECT_EQ(listed.value().entries.size(), 2U);
    EXPECT_EQ(entries().list(db(), replies(), {}).code(), ErrorCode::ValidationFailed);
}

TEST_F(EntryDb, AChildNeedsAParentThatExists) {
    ent::NewEntry reply{};
    reply.content = one_line("text", "orphan");
    reply.parent = anvil::uuid::generate_v7();
    EXPECT_EQ(entries().create(db(), replies(), reply, actor()).code(), ErrorCode::NotFound);
    reply.parent.reset();
    EXPECT_EQ(entries().create(db(), replies(), reply, actor()).code(),
              ErrorCode::ValidationFailed);
}

// --- authorship ----------------------------------------------------------------------------

TEST_F(EntryDb, AGuardedWriteByAnotherAuthorLooksLikeNothingIsThere) {
    ent::NewEntry thread{};
    thread.content = one_line("subject", "Mine");
    const auto created = entries().create(db(), threads(), thread, actor());
    ASSERT_TRUE(created.ok());

    ent::EntryEdit edit{};
    edit.patch = one_line("subject", "Theirs");
    const ent::WriteGuard theirs{.author = someone_else()};
    EXPECT_EQ(entries()
                  .write(db(), threads(), created.value().id, created.value().version, edit,
                         someone_else(), theirs)
                  .code(),
              ErrorCode::NotFound);
    EXPECT_EQ(entries().remove(db(), threads(), created.value().id, created.value().version, theirs)
                  .code(),
              ErrorCode::NotFound);

    const ent::WriteGuard mine{.author = actor()};
    EXPECT_TRUE(entries()
                    .write(db(), threads(), created.value().id, created.value().version, edit,
                           actor(), mine)
                    .ok());
}

// --- media -----------------------------------------------------------------------------------

TEST_F(EntryDb, AnEntryHoldsOneReferencePerDistinctImage) {
    const Uuid first = store_image(1);
    const Uuid second = store_image(2);
    const ent::EntryWriteOutcome created = create_item("shot", first);
    EXPECT_EQ(refs(first), 1);

    // Both copies name `first` after a publish, and it is still ONE reference.
    const auto published =
        entries().publish(db(), gallery(), created.id, created.version, actor());
    ASSERT_TRUE(published.ok());
    EXPECT_EQ(refs(first), 1);

    // The draft moves to `second`; the published copy still shows `first`, so
    // the entry holds both.
    ent::EntryEdit edit{};
    edit.patch.set_image("shot", second);
    const auto edited =
        entries().write(db(), gallery(), created.id, published.value().version, edit, actor());
    ASSERT_TRUE(edited.ok());
    EXPECT_EQ(refs(first), 1);
    EXPECT_EQ(refs(second), 1);

    const auto republished =
        entries().publish(db(), gallery(), created.id, edited.value().version, actor());
    ASSERT_TRUE(republished.ok());
    EXPECT_EQ(refs(first), 0);
    EXPECT_EQ(refs(second), 1);

    ASSERT_TRUE(
        entries().remove(db(), gallery(), created.id, republished.value().version).ok());
    EXPECT_EQ(refs(second), 0);
}

TEST_F(EntryDb, AnImageFromNowhereIsRefusedAndCountsNothing) {
    ent::NewEntry entry{};
    entry.content = item("ghost", anvil::uuid::generate_v7());
    entry.slug = "ghost";
    EXPECT_EQ(entries().create(db(), gallery(), entry, actor()).code(), ErrorCode::NotFound);
    EXPECT_TRUE(slugs(gallery(), {.stage = ent::Stage::Working}).empty());
}

// --- the fresh database ----------------------------------------------------------------------

TEST_F(EntryDb, SeedsLandOnceAndAnEmptiedKindStaysEmpty) {
    const Uuid first = store_image(11);
    const Uuid second = store_image(12);
    const sec::DefaultImageResolver resolve = [&](std::string_view file) -> std::optional<Uuid> {
        if (file == "first.png") { return first; }
        if (file == "second.png") { return second; }
        return std::nullopt;
    };

    const auto seeded = entries().bootstrap(db(), testapp::kSeeds, resolve, actor());
    ASSERT_TRUE(seeded.ok());
    EXPECT_EQ(seeded.value().kinds_seeded, 1U);
    EXPECT_EQ(seeded.value().entries_created, 2U);
    EXPECT_EQ(seeded.value().images_registered, 2U);
    EXPECT_EQ(slugs(gallery(), {.stage = ent::Stage::Published}),
              (std::vector<std::string>{"first", "second"}));
    EXPECT_EQ(slugs(gallery(), {.flags_all = ent::flag_bit(gallery(), "pinned"),
                                .stage = ent::Stage::Published}),
              std::vector<std::string>{"first"});
    EXPECT_EQ(refs(first), 1);

    // A second boot — on this instance or another — changes nothing.
    const auto again = make_service()->bootstrap(db(), testapp::kSeeds, resolve, actor());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().kinds_seeded, 0U);
    EXPECT_EQ(again.value().kinds_already_seeded, 1U);
    EXPECT_EQ(slugs(gallery(), {.stage = ent::Stage::Working}).size(), 2U);

    // Staff empty it; the next deploy does not refill it.
    const auto held = entries().list(db(), gallery(), {.stage = ent::Stage::Working});
    ASSERT_TRUE(held.ok());
    for (const ent::EntryDocument& e : held.value().entries) {
        ASSERT_TRUE(entries().remove(db(), gallery(), e.id, e.version).ok());
    }
    ASSERT_TRUE(entries().bootstrap(db(), testapp::kSeeds, resolve, actor()).ok());
    EXPECT_TRUE(slugs(gallery(), {.stage = ent::Stage::Working}).empty());
}

// --- invalidation ----------------------------------------------------------------------------

TEST_F(EntryDb, OnlyWhatReadersSeeWakesTheHook) {
    ent::NewEntry thread{};
    thread.content = one_line("subject", "Live at once");
    ASSERT_TRUE(entries().create(db(), threads(), thread, actor()).ok());
    EXPECT_EQ(heard_, std::vector<std::string>{"forum.thread"});

    heard_.clear();
    const ent::EntryWriteOutcome draft = create_post("quiet");
    ent::EntryEdit edit{};
    edit.patch = post("Still quiet");
    ASSERT_TRUE(entries().write(db(), posts(), draft.id, draft.version, edit, actor()).ok());
    EXPECT_TRUE(heard_.empty());

    // A name no table declares is dropped, as a message from another process
    // naming one would be.
    entries().invalidate_local("not.a.kind");
    EXPECT_TRUE(heard_.empty());
}

}  // namespace
