// The sections CMS against a live cluster: the two documents per key, the
// versioned write, the whole-subdocument encoding, bootstrap's idempotence, the
// three-tier cache and the image checks the service makes.
//
// These cases cannot be unit tests. What they assert is a property of the
// SERVER — that a compound `_id` is a uniqueness constraint, that a duplicate
// insert is a distinguishable error code, that a versioned update matches
// nothing when the version has moved — and none of that is observable without
// one.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "anvil/media/repository.h"
#include "anvil/media/service.h"
#include "anvil/sections/bootstrap.h"
#include "anvil/sections/payload.h"
#include "anvil/sections/repository.h"
#include "anvil/sections/service.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/namespaces.h"
#include "testapp/sections.h"

namespace {

using anvil::ErrorCode;
using anvil::Locale;
using anvil::Uuid;
using anvil::fs::Format;
using anvil::fs::Mime;
using anvil::images::VariantRecord;
using anvil::media::MediaRepository;
using anvil::media::MediaService;
using anvil::media::NewMedia;
using anvil::testfixture::scratch_names;
namespace sec = anvil::sections;

constexpr std::string_view kMedia = "media";

// The nobody who writes a bootstrap row. A real deployment passes the operator
// or a service identity; nothing in these cases depends on which.
const Uuid& actor() {
    static const Uuid id = anvil::uuid::generate_v7();
    return id;
}

Locale en() { return *Locale::from_tag("en"); }
Locale ar() { return *Locale::from_tag("ar"); }

class SectionDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, testapp::kSectionsCollection);
        anvil::testfixture::clear_collection(**client_, kMedia);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static sec::SectionRepository sections() {
        return sec::SectionRepository{
            std::string{scratch_names().for_collection(testapp::kSectionsCollection)},
            testapp::kSectionsCollection};
    }

    [[nodiscard]] static const sec::SectionSpec& hero() {
        return *sec::find_section(testapp::kSections, "home.hero");
    }

    // By key, never by index. kDefaults is positionally identical to kSections —
    // defaults_match_registry asserts exactly that — but a test that spells the
    // position asserts something different the day a section is added.
    [[nodiscard]] static const sec::SectionDefaults& defaults_for(std::string_view key) {
        return *sec::find_defaults(testapp::kDefaults, key);
    }

    [[nodiscard]] static sec::SectionDocument seed_document(const sec::SectionSpec& spec,
                                                            const sec::SectionDefaults& d) {
        sec::SectionDocument doc{};
        doc.content = sec::canonicalise(spec, sec::default_content(spec, d));
        doc.etag = sec::content_etag(spec, doc.content);
        doc.updated_by = actor();
        doc.version = 1;
        return doc;
    }

    // A media row of a chosen size, with no files behind it. Everything these
    // cases assert about images is a property of the ROW — the namespace in the
    // filter and the dimensions against the ImageSpec — and none of it needs an
    // encoder.
    [[nodiscard]] Uuid store_image(std::uint32_t width, std::uint32_t height,
                                   anvil::fs::Ns ns = testapp::kContent,
                                   std::uint8_t hash_seed = 1) {
        const Uuid id = anvil::uuid::generate_v7();
        anvil::crypto::Digest256 sha{};
        sha[0] = hash_seed;
        sha[1] = static_cast<std::uint8_t>(width);
        const NewMedia row{
            .variants = {VariantRecord{2048, 320, 180, Format::Avif}},
            .sha256 = sha,
            .bytes = 123456,
            .id = id,
            .owner = actor(),
            .uploader_ip = std::nullopt,
            .width = width,
            .height = height,
            .ns = ns,
            .mime = Mime::Jpeg,
        };
        const MediaRepository media{std::string{scratch_names().for_collection(kMedia)}, kMedia};
        EXPECT_TRUE(media.insert(db(), row).ok());
        return id;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

// --- the repository ---------------------------------------------------------

TEST_F(SectionDb, PublishedAndDraftAreTwoDocumentsUnderOneKey) {
    const sec::SectionDocument doc = seed_document(hero(), defaults_for("home.hero"));
    ASSERT_TRUE(sections()
                    .insert_if_absent(db(), hero(), sec::SectionState::Published, doc, actor())
                    .value_or(false));
    ASSERT_TRUE(
        sections().insert_if_absent(db(), hero(), sec::SectionState::Draft, doc, actor())
            .value_or(false));

    // Two rows, and a read of one cannot see the other. That is what stops an
    // unpublished string leaking through a forgotten projection: it is not in
    // the document the public read looked at.
    const auto published = sections().find(db(), hero(), sec::SectionState::Published);
    const auto draft = sections().find(db(), hero(), sec::SectionState::Draft);
    ASSERT_TRUE(published.ok());
    ASSERT_TRUE(draft.ok());
    EXPECT_TRUE(published.value().has_value());
    EXPECT_TRUE(draft.value().has_value());
}

TEST_F(SectionDb, AKeyNeverWrittenIsAbsentRatherThanAnError) {
    const auto found = sections().find(db(), hero(), sec::SectionState::Published);
    ASSERT_TRUE(found.ok());
    EXPECT_FALSE(found.value().has_value());
}

TEST_F(SectionDb, InsertIfAbsentIsIdempotentAndNeverOverwrites) {
    sec::SectionDocument first = seed_document(hero(), defaults_for("home.hero"));
    ASSERT_TRUE(sections()
                    .insert_if_absent(db(), hero(), sec::SectionState::Published, first, actor())
                    .value_or(false));

    sec::SectionDocument second = first;
    second.content.set("headline", [] {
        sec::SectionValue v{};
        v.text[0] = "SECOND BOOT";
        v.text[1] = "إقلاع تاني";
        return v;
    }());

    // false, not an error: another instance got there first, or it was already
    // there. Both are the intended outcome of an idempotent bootstrap.
    const auto again =
        sections().insert_if_absent(db(), hero(), sec::SectionState::Published, second, actor());
    ASSERT_TRUE(again.ok());
    EXPECT_FALSE(again.value());

    // And the first write survived. A boot path that overwrote would turn every
    // deploy into a content wipe.
    const auto stored = sections().find(db(), hero(), sec::SectionState::Published);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    EXPECT_NE(stored.value()->content.find("headline")->value.primary(), "SECOND BOOT");
}

TEST_F(SectionDb, TheRoundTripPreservesEveryTypeAndEveryLocale) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const sec::SectionDocument doc = seed_document(hero(), defaults_for("home.hero"));
    ASSERT_TRUE(sections()
                    .insert_if_absent(db(), hero(), sec::SectionState::Published, doc, actor())
                    .value_or(false));

    const auto stored = sections().find(db(), hero(), sec::SectionState::Published);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());
    const sec::SectionContent& content = stored.value()->content;

    // A localised Text field: both locales, byte for byte.
    ASSERT_NE(content.find("headline"), nullptr);
    EXPECT_EQ(content.find("headline")->value.text[en().index()],
              "Where community meets exploration.");
    EXPECT_EQ(content.find("headline")->value.text[ar().index()],
              "المكان اللي بيجمع الناس والطريق.");
    // A non-localised Url, a Colour, and a Bool. Decoding never coerces: the
    // stored BSON type must be the one the registry declares.
    EXPECT_EQ(content.find("cta_href")->value.primary(), "#/events");
    EXPECT_EQ(content.find("accent")->value.primary(), "#134411");
    EXPECT_TRUE(content.find("show")->value.boolean);
    // And a Number, from the other section.
    const sec::SectionSpec& contact = *sec::find_section(testapp::kSections, "contact.info");
    const sec::SectionDocument seats = seed_document(contact, defaults_for("contact.info"));
    ASSERT_TRUE(
        sections()
            .insert_if_absent(db(), contact, sec::SectionState::Published, seats, actor())
            .value_or(false));
    const auto read_back = sections().find(db(), contact, sec::SectionState::Published);
    ASSERT_TRUE(read_back.ok());
    EXPECT_EQ(read_back.value()->content.find("seats")->value.number, 48);

    EXPECT_EQ(stored.value()->etag, doc.etag);
    EXPECT_EQ(stored.value()->version, 1);
    EXPECT_EQ(stored.value()->updated_by, actor());
}

TEST_F(SectionDb, AStaleVersionLosesRatherThanOverwriting) {
    ANVIL_REQUIRE_TRANSACTIONS();
    sec::SectionDocument doc = seed_document(hero(), defaults_for("home.hero"));
    ASSERT_TRUE(sections()
                    .insert_if_absent(db(), hero(), sec::SectionState::Published, doc, actor())
                    .value_or(false));

    auto session = db().start_session();
    session.start_transaction();
    const auto first = sections().update(db(), session, hero(), sec::SectionState::Published, 1,
                                         doc, actor());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 2);

    // The second writer still believes the document is at version 1. It matches
    // nothing, which is one winner and one VersionMismatch rather than a silent
    // lost update.
    const auto second = sections().update(db(), session, hero(), sec::SectionState::Published, 1,
                                          doc, actor());
    EXPECT_FALSE(second.ok());
    EXPECT_EQ(second.code(), ErrorCode::VersionMismatch);
    session.abort_transaction();
}

TEST_F(SectionDb, AFieldTheRegistryNoLongerDeclaresDisappearsOnTheNextWrite) {
    ANVIL_REQUIRE_TRANSACTIONS();
    sec::SectionDocument doc = seed_document(hero(), defaults_for("home.hero"));
    // A value with no registry entry, as a retired field would be. The codec is
    // the allow-list in both directions, so it is never written at all.
    doc.content.set("retired", sec::SectionValue{});
    ASSERT_TRUE(sections()
                    .insert_if_absent(db(), hero(), sec::SectionState::Published, doc, actor())
                    .value_or(false));

    const auto stored = sections().find(db(), hero(), sec::SectionState::Published);
    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(stored.value()->content.find("retired"), nullptr);

    // The write is a WHOLE subdocument rather than dotted $set paths, which is
    // what makes a removed field actually disappear from storage instead of
    // lingering behind an update that never mentions it. Written raw and then
    // rewritten through the codec, it is gone.
    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(sections()
                    .update(db(), session, hero(), sec::SectionState::Published, 1,
                            stored.value().value(), actor())
                    .ok());
    session.commit_transaction();

    const auto raw =
        db()[std::string{scratch_names().for_collection(testapp::kSectionsCollection)}]
            [std::string{testapp::kSectionsCollection}]
                .find_one(bsoncxx::builder::basic::make_document());
    ASSERT_TRUE(raw.has_value());
    const auto data = raw->view()["data"];
    ASSERT_TRUE(data);
    EXPECT_FALSE(data.get_document().value["retired"]);
}

// --- bootstrap --------------------------------------------------------------

TEST_F(SectionDb, BootstrapCreatesEveryPublishedSectionAndIsIdempotent) {
    // No resolver: this build's section rows must be creatable whether or not it
    // can encode an image, so the slots come back as missing and the text lands
    // regardless.
    const auto first = sec::bootstrap_sections(db(), sections(), testapp::kSections,
                                               testapp::kDefaults, {}, actor());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().sections_created, testapp::kSections.size());
    EXPECT_EQ(first.value().sections_present, 0U);
    EXPECT_EQ(first.value().images_registered, 0U);
    EXPECT_EQ(first.value().images_missing, 2U);

    // Both declared slots are empty, because there was no resolver to fill them.
    // The two numbers agree here and only here: nothing existed before this
    // call, so what the resolver failed to produce and what storage lacks are
    // the same two slots.
    EXPECT_EQ(first.value().image_slots_unbound, 2U);

    // Every boot runs this, from every instance, concurrently. The second run
    // must create nothing and overwrite nothing.
    const auto second = sec::bootstrap_sections(db(), sections(), testapp::kSections,
                                                testapp::kDefaults, {}, actor());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().sections_created, 0U);
    EXPECT_EQ(second.value().sections_present, testapp::kSections.size());
    EXPECT_EQ(second.value().image_slots_unbound, 2U);

    // Published only: a fresh database has no drafts, and creating an empty one
    // would put a document in the draft state nobody authored.
    const auto draft = sections().find(db(), hero(), sec::SectionState::Draft);
    ASSERT_TRUE(draft.ok());
    EXPECT_FALSE(draft.value().has_value());
}

TEST_F(SectionDb, BootstrapRefusesTwoTablesThatWereNeverAssertedTogether) {
    // Positional correspondence is a compile-time property of the two tables,
    // but this takes spans and a caller can hand it two that were never
    // asserted. Walking by index across mismatched tables would write one
    // section's defaults into another section's document.
    const std::span<const sec::SectionDefaults> shortened{testapp::kDefaults.data(), 2};
    const auto report = sec::bootstrap_sections(db(), sections(), testapp::kSections, shortened,
                                                {}, actor());
    EXPECT_FALSE(report.ok());
}

TEST_F(SectionDb, DefaultContentResolutionCountsSlotsItCouldNotRegister) {
    sec::BootstrapReport report{};
    // A resolver that answers for one file and not the other, which is what a
    // half-installed defaults tree looks like. The section is still created,
    // with text and without that image — a missing decorative image must not
    // stop a deployment from booting with correct copy.
    const sec::DefaultImageResolver resolver =
        [](std::string_view file) -> std::optional<Uuid> {
        if (file == "home-hero.jpg") { return anvil::uuid::generate_v7(); }
        return std::nullopt;
    };
    const sec::SectionContent hero_content =
        sec::resolve_default_content(hero(), defaults_for("home.hero"), resolver, report);
    EXPECT_EQ(report.images_registered, 1U);
    EXPECT_NE(hero_content.find_image("hero"), nullptr);

    const sec::SectionSpec& about = *sec::find_section(testapp::kSections, "home.about");
    const sec::SectionContent about_content =
        sec::resolve_default_content(about, defaults_for("home.about"), resolver, report);
    EXPECT_EQ(report.images_missing, 1U);
    EXPECT_EQ(about_content.find_image("portrait"), nullptr);
}

// --- the count taken from storage -------------------------------------------
//
// Everything below asserts the same one property from three directions: the
// number an operator reads describes the DOCUMENTS, not the work this boot did.
// Every one of these shipped green before the count existed, because every one
// of them is a state `images_missing` reports as fine.

TEST_F(SectionDb, BootstrapCountsWhatStorageHoldsRatherThanWhatThisBootResolved) {
    // The reported defect, reproduced exactly: a database seeded BEFORE the
    // defaults tree existed, booting again after it arrives. The resolver now
    // answers for every file, so the old number says nothing is wrong — and
    // insert-if-absent did not touch either section, so every slot is still
    // empty.
    ASSERT_TRUE(sec::bootstrap_sections(db(), sections(), testapp::kSections,
                                        testapp::kDefaults, {}, actor())
                    .ok());

    const sec::DefaultImageResolver resolver = [](std::string_view) -> std::optional<Uuid> {
        return anvil::uuid::generate_v7();
    };
    const auto second = sec::bootstrap_sections(db(), sections(), testapp::kSections,
                                                testapp::kDefaults, resolver, actor());
    ASSERT_TRUE(second.ok());

    // Unchanged in meaning, and still right about what it measures: the resolver
    // was asked twice and failed neither time.
    EXPECT_EQ(second.value().images_missing, 0U);
    EXPECT_EQ(second.value().images_registered, 2U);
    EXPECT_EQ(second.value().sections_created, 0U);

    // The number that is about the site rather than about the boot.
    EXPECT_EQ(second.value().image_slots_unbound, 2U);
}

TEST_F(SectionDb, TheCountFallsAsSlotsAreActuallyBound) {
    ASSERT_TRUE(sec::bootstrap_sections(db(), sections(), testapp::kSections,
                                        testapp::kDefaults, {}, actor())
                    .ok());

    // One slot filled the way a staff member fills one — through the ordinary
    // versioned write, which is the only path that ever binds an image into a
    // section that already exists.
    const Uuid picture = store_image(1920, 1080);
    sec::SectionContent patch;
    patch.set_image("hero", picture);
    const auto stored = sections().find(db(), hero(), sec::SectionState::Published);
    ASSERT_TRUE(stored.ok());
    ASSERT_TRUE(stored.value().has_value());

    sec::SectionDocument next = stored.value().value();
    next.content = sec::canonicalise(hero(), sec::merge(next.content, patch));
    next.etag = sec::content_etag(hero(), next.content);
    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(sections()
                    .update(db(), session, hero(), sec::SectionState::Published,
                            next.version, next, actor())
                    .ok());
    session.commit_transaction();

    // One of the two slots now resolves. A count of SECTIONS whose media map is
    // empty would also say one here — which is why the case below exists.
    const auto after = sections().count_unbound_image_slots(db(), testapp::kSections,
                                                            sec::SectionState::Published);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value(), 1U);
}

// A registry of its own, because no section in the reference application
// declares two slots and the difference between counting SECTIONS and counting
// SLOTS is invisible until one does. A section with two slots and one bound is
// one gap on the page; "sections whose media map is empty" calls it zero.
namespace two_slots {

constexpr std::array<sec::FieldSpec, 0> kNoFields{};

constexpr std::array<sec::ImageSpec, 2> kPair{{
    {"left", {{"Left", "يسار"}}, 0, 0, 0, 0},
    {"right", {{"Right", "يمين"}}, 0, 0, 0, 0},
}};

constexpr std::array<sec::SectionSpec, 1> kRegistry{{
    {"gallery.pair", "/gallery", kNoFields, kPair},
}};

}  // namespace two_slots

TEST_F(SectionDb, ASlotIsCountedEvenWhereItsNeighbourInTheSameSectionIsBound) {
    const sec::SectionSpec& pair = two_slots::kRegistry[0];

    // Nothing stored at all. Absent and empty are the same gap on the page, so
    // a document that never arrived counts every slot it would have held.
    const auto absent = sections().count_unbound_image_slots(db(), two_slots::kRegistry,
                                                             sec::SectionState::Published);
    ASSERT_TRUE(absent.ok());
    EXPECT_EQ(absent.value(), 2U);

    sec::SectionDocument document{};
    document.content.set_image("left", anvil::uuid::generate_v7());
    document.content = sec::canonicalise(pair, document.content);
    document.etag = sec::content_etag(pair, document.content);
    document.updated_by = actor();
    document.version = 1;
    ASSERT_TRUE(sections()
                    .insert_if_absent(db(), pair, sec::SectionState::Published, document,
                                      actor())
                    .ok());

    const auto half = sections().count_unbound_image_slots(db(), two_slots::kRegistry,
                                                           sec::SectionState::Published);
    ASSERT_TRUE(half.ok());
    EXPECT_EQ(half.value(), 1U);
}

TEST_F(SectionDb, ASlotHoldingSomethingThatIsNotAMediaIdCountsAsUnbound) {
    // A slot a renderer cannot resolve to a media id is the same gap on the page
    // as a slot with nothing in it, and counting mere PRESENCE would report it
    // as bound — the class of answer this count exists to stop giving.
    //
    // Written through the raw collection because no encoder in this library will
    // produce it: the shape comes from a hand-edit, a restored backup, or a
    // migration that wrote the string form of an id.
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;
    using bsoncxx::builder::basic::sub_document;

    const Uuid real_id = anvil::uuid::generate_v7();
    // The id as a STRING — the 36-character spelling CLAUDE.md §2.3 bans, which
    // is exactly what a migration written without reading it produces. Bound to
    // a local because the view appended below borrows it.
    const std::string id_as_text = anvil::uuid::to_string(real_id);

    db()[std::string{scratch_names().for_collection(testapp::kSectionsCollection)}]
        [std::string{testapp::kSectionsCollection}]
            .insert_one(
                make_document(
                    kvp("_id",
                        [](sub_document id) {
                            id.append(kvp("k", bsoncxx::types::b_string{"gallery.pair"}));
                            id.append(kvp("s", bsoncxx::types::b_int32{0}));
                        }),
                    kvp("media",
                        [&](sub_document media) {
                            media.append(kvp(
                                "left",
                                bsoncxx::types::b_string{anvil::db::codec::key_of(id_as_text)}));
                            media.append(
                                kvp("right", anvil::db::codec::uuid_bin(real_id)));
                        }))
                    .view());

    const auto counted = sections().count_unbound_image_slots(db(), two_slots::kRegistry,
                                                              sec::SectionState::Published);
    ASSERT_TRUE(counted.ok());
    EXPECT_EQ(counted.value(), 1U);
}

// --- the service ------------------------------------------------------------

class SectionServiceDb : public SectionDb {
protected:
    void SetUp() override {
        SectionDb::SetUp();
        if (::testing::Test::IsSkipped()) { return; }
        media_ = std::make_unique<MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);
        service_ = make_service();
        ASSERT_TRUE(sec::bootstrap_sections(db(), sections(), testapp::kSections,
                                            testapp::kDefaults, {}, actor())
                        .ok());
    }

    // One more instance of the SAME deployment: same collection, same registry,
    // same cache prefix and therefore the same invalidation channel. Two of
    // these is what a second process looks like from in here, which is the only
    // way to drive the subscriber's path.
    [[nodiscard]] std::unique_ptr<sec::SectionService> make_service(
        std::function<void(std::string_view)> hook = {}) {
        return std::make_unique<sec::SectionService>(
            std::string{scratch_names().for_collection(testapp::kSectionsCollection)},
            testapp::kSectionsCollection, testapp::kSections, *media_,
            sec::SectionServiceConfig{.content_origin = "https://cdn.test",
                                      .image_url_base = "https://cdn.test/content",
                                      .cache_prefix = cache_prefix(),
                                      .redis_ttl = std::chrono::seconds{30},
                                      .image_namespace = testapp::kContent,
                                      .on_invalidated = std::move(hook)});
    }

    // One prefix per process, so a run against a shared Redis cannot read
    // another run's cached sections — the same reason the database name is
    // per-run.
    [[nodiscard]] static const std::string& cache_prefix() {
        static const std::string prefix = "anvil_t_sec_" + anvil::testfixture::scratch_database();
        return prefix;
    }

    std::unique_ptr<MediaService>        media_;
    std::unique_ptr<sec::SectionService> service_;
};

TEST_F(SectionServiceDb, PeekIsEmptyUntilSomethingFillsIt) {
    // nullptr means "not cached here", NEVER "does not exist" — which is why a
    // miss falls through to load() rather than to a 404.
    EXPECT_EQ(service_->peek(hero(), en()), nullptr);
}

TEST_F(SectionServiceDb, LoadPopulatesTierOneForTheLocaleItWasAskedFor) {
    const auto payload = service_->load(db(), hero(), en());
    ASSERT_TRUE(payload.ok());
    ASSERT_NE(payload.value(), nullptr);
    EXPECT_NE(payload.value()->json.find("\"lang\":\"en\""), std::string::npos);

    ASSERT_NE(service_->peek(hero(), en()), nullptr);
    // And only that locale: a payload carries one, so filling English must not
    // pretend Arabic is cached.
    EXPECT_EQ(service_->peek(hero(), ar()), nullptr);
}

TEST_F(SectionServiceDb, PreloadFillsEverySectionInEveryLocale) {
    const auto loaded = service_->preload(db());
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value(), testapp::kSections.size() * anvil::kLocaleCount);
    for (const sec::SectionSpec& spec : testapp::kSections) {
        for (const Locale locale : anvil::kAllLocales) {
            EXPECT_NE(service_->peek(spec, locale), nullptr) << spec.key;
        }
    }
}

TEST_F(SectionServiceDb, AWriteReplacesTierOneRatherThanLeavingItCold) {
    ANVIL_REQUIRE_TRANSACTIONS();
    ASSERT_TRUE(service_->preload(db()).ok());

    sec::SectionContent patch;
    sec::SectionValue headline{};
    headline.text[en().index()] = "Changed";
    headline.text[ar().index()] = "اتغير";
    patch.set("headline", headline);

    const auto outcome =
        service_->write(db(), hero(), sec::SectionState::Published, 1, patch, actor());
    ASSERT_TRUE(outcome.ok()) << static_cast<int>(outcome.code());
    EXPECT_EQ(outcome.value().version, 2);

    // Serialised once, here, so the first reader after a write does not pay for
    // it — and so the etag the client was handed is the one a conditional GET
    // will compare against.
    const auto cached = service_->peek(hero(), en());
    ASSERT_NE(cached, nullptr);
    EXPECT_NE(cached->json.find("Changed"), std::string::npos);
    EXPECT_EQ(cached->version, 2);
}

TEST_F(SectionServiceDb, RequiredIsEnforcedAgainstTheMergedDocument) {
    ANVIL_REQUIRE_TRANSACTIONS();
    // A patch that touches only an optional field is complete once merged, even
    // though on its own it names no required field at all.
    sec::SectionContent partial;
    sec::SectionValue subline{};
    subline.text[en().index()] = "New line";
    subline.text[ar().index()] = "سطر جديد";
    partial.set("subline", subline);
    EXPECT_TRUE(
        service_->write(db(), hero(), sec::SectionState::Published, 1, partial, actor()).ok());

    // Clearing a REQUIRED field is still refused, because the check runs against
    // the result rather than against the patch.
    sec::SectionContent clearing;
    clearing.set("headline", sec::SectionValue{});
    const auto refused =
        service_->write(db(), hero(), sec::SectionState::Published, 2, clearing, actor());
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), ErrorCode::ValidationFailed);
}

TEST_F(SectionServiceDb, TwoWritersAtOneVersionProduceOneWinner) {
    ANVIL_REQUIRE_TRANSACTIONS();
    sec::SectionContent patch;
    sec::SectionValue subline{};
    subline.text[en().index()] = "One";
    subline.text[ar().index()] = "واحد";
    patch.set("subline", subline);

    EXPECT_TRUE(
        service_->write(db(), hero(), sec::SectionState::Published, 1, patch, actor()).ok());
    const auto loser =
        service_->write(db(), hero(), sec::SectionState::Published, 1, patch, actor());
    EXPECT_FALSE(loser.ok());
    EXPECT_EQ(loser.code(), ErrorCode::VersionMismatch);
}

TEST_F(SectionServiceDb, TheFirstDraftIsCreatedFromTheLiveSection) {
    ANVIL_REQUIRE_TRANSACTIONS();
    // Bootstrap seeds a Published document per section and no Draft of any of
    // them, so without this branch the first "save as draft" of every section in
    // the registry answers NotFound — the state a fresh database is in is the
    // state where the feature does not work.
    sec::SectionContent patch;
    sec::SectionValue subline{};
    subline.text[en().index()] = "Draft line";
    subline.text[ar().index()] = "سطر مسودة";
    patch.set("subline", subline);

    const auto created =
        service_->write(db(), hero(), sec::SectionState::Draft, 0, patch, actor());
    ASSERT_TRUE(created.ok()) << static_cast<int>(created.code());
    EXPECT_EQ(created.value().version, 1);

    const auto draft = service_->read_document(db(), hero(), sec::SectionState::Draft);
    ASSERT_TRUE(draft.ok());
    ASSERT_TRUE(draft.value().has_value());
    // A draft is a draft OF the live page: anything the patch does not mention
    // reads as the site reads today rather than as empty.
    EXPECT_EQ(draft.value()->content.find("subline")->value.primary(), "Draft line");
    EXPECT_EQ(draft.value()->content.find("headline")->value.primary(),
              "Where community meets exploration.");

    // A draft has no public reader, so it is not cached and publishes no
    // invalidation.
    EXPECT_EQ(service_->peek(hero(), en()), nullptr);
}

TEST_F(SectionServiceDb, AFirstDraftIsOnlyCreatedFromVersionZero) {
    ANVIL_REQUIRE_TRANSACTIONS();
    // A caller that believes a draft already exists at some version is working
    // from a stale read, and creating one underneath it would silently discard
    // whatever it thought it was editing.
    const auto refused =
        service_->write(db(), hero(), sec::SectionState::Draft, 3, sec::SectionContent{},
                        actor());
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), ErrorCode::VersionMismatch);
}

TEST_F(SectionServiceDb, AMissingPublishedSectionIsNotFoundRatherThanCreated) {
    ANVIL_REQUIRE_TRANSACTIONS();
    anvil::testfixture::clear_collection(db(), testapp::kSectionsCollection);
    const auto refused = service_->write(db(), hero(), sec::SectionState::Published, 1,
                                         sec::SectionContent{}, actor());
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), ErrorCode::NotFound);
}

// --- images -----------------------------------------------------------------

TEST_F(SectionServiceDb, AnImageMustMeetItsSlotsMinimumDimensions) {
    sec::SectionContent content;
    content.set_image("hero", store_image(200, 200));
    const anvil::Status refused = service_->verify_images(db(), hero(), content);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), ErrorCode::ValidationFailed);

    sec::SectionContent ok;
    ok.set_image("hero", store_image(1920, 1080, testapp::kContent, 2));
    EXPECT_TRUE(service_->verify_images(db(), hero(), ok).ok());
}

TEST_F(SectionServiceDb, TheAspectRatioIsCheckedWithATolerance) {
    // 1920x1081 is 16:9 to within a rounding error of one pixel. Rejecting it
    // would be a defect dressed as a control: cropping and resampling do not
    // land on an exact ratio for every source size.
    sec::SectionContent near;
    near.set_image("hero", store_image(1920, 1081, testapp::kContent, 3));
    EXPECT_TRUE(service_->verify_images(db(), hero(), near).ok());

    // 4:3 at the same width is not 16:9 by any tolerance.
    sec::SectionContent wrong;
    wrong.set_image("hero", store_image(1920, 1440, testapp::kContent, 4));
    EXPECT_FALSE(service_->verify_images(db(), hero(), wrong).ok());
}

TEST_F(SectionServiceDb, ASlotDeclaringNoRatioAcceptsAnyShape) {
    // 0/0 is how the registry says "any shape". Without the explicit skip the
    // cross multiplication compares against zero and rejects everything.
    const sec::SectionSpec& about = *sec::find_section(testapp::kSections, "home.about");
    sec::SectionContent tall;
    tall.set_image("portrait", store_image(600, 1800, testapp::kContent, 5));
    EXPECT_TRUE(service_->verify_images(db(), about, tall).ok());
}

TEST_F(SectionServiceDb, AnIdFromAnotherNamespaceIsIndistinguishableFromAbsent) {
    // The namespace is part of the LOOKUP, so an id uploaded through another API
    // cannot be attached to a section slot — and the answer is the same NotFound
    // a nonexistent id gets, rather than an oracle over every namespace at once.
    sec::SectionContent other;
    other.set_image("hero", store_image(1920, 1080, testapp::kGuest, 6));
    const anvil::Status refused = service_->verify_images(db(), hero(), other);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), ErrorCode::NotFound);

    sec::SectionContent absent;
    absent.set_image("hero", anvil::uuid::generate_v7());
    EXPECT_EQ(service_->verify_images(db(), hero(), absent).code(), ErrorCode::NotFound);
}

TEST_F(SectionServiceDb, ReferenceCountsMoveWithTheSectionWriteThatChangedThem) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const MediaRepository media{std::string{scratch_names().for_collection(kMedia)}, kMedia};
    const Uuid first = store_image(1920, 1080, testapp::kContent, 7);
    const Uuid second = store_image(1920, 1080, testapp::kContent, 8);

    sec::SectionContent attach;
    attach.set_image("hero", first);
    ASSERT_TRUE(
        service_->write(db(), hero(), sec::SectionState::Published, 1, attach, actor()).ok());
    EXPECT_EQ(media.find(db(), testapp::kContent, first).value()->refs, 1);

    // Swapping the slot attaches the new id and releases the old one, in ONE
    // transaction with the write. A count committed on its own is wrong the
    // moment the section write aborts.
    sec::SectionContent swap;
    swap.set_image("hero", second);
    ASSERT_TRUE(
        service_->write(db(), hero(), sec::SectionState::Published, 2, swap, actor()).ok());
    EXPECT_EQ(media.find(db(), testapp::kContent, first).value()->refs, 0);
    EXPECT_EQ(media.find(db(), testapp::kContent, second).value()->refs, 1);
}

TEST_F(SectionServiceDb, RewritingTheSameImageDoesNotDoubleCountIt) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const MediaRepository media{std::string{scratch_names().for_collection(kMedia)}, kMedia};
    const Uuid id = store_image(1920, 1080, testapp::kContent, 9);

    sec::SectionContent attach;
    attach.set_image("hero", id);
    ASSERT_TRUE(
        service_->write(db(), hero(), sec::SectionState::Published, 1, attach, actor()).ok());
    // The same slot, the same id, a second write. Counting it again would leak a
    // reference the collector can never reclaim.
    ASSERT_TRUE(
        service_->write(db(), hero(), sec::SectionState::Published, 2, attach, actor()).ok());
    EXPECT_EQ(media.find(db(), testapp::kContent, id).value()->refs, 1);
}

// --- invalidation -----------------------------------------------------------

TEST_F(SectionServiceDb, InvalidationDropsTheLocalEntryForEveryLocale) {
    ASSERT_TRUE(service_->preload(db()).ok());
    ASSERT_NE(service_->peek(hero(), en()), nullptr);
    ASSERT_NE(service_->peek(hero(), ar()), nullptr);

    service_->invalidate_local(hero().key);
    EXPECT_EQ(service_->peek(hero(), en()), nullptr);
    EXPECT_EQ(service_->peek(hero(), ar()), nullptr);

    // Another section is untouched: invalidation is per key, not a flush.
    const sec::SectionSpec& about = *sec::find_section(testapp::kSections, "home.about");
    EXPECT_NE(service_->peek(about, en()), nullptr);
}

TEST_F(SectionServiceDb, AKeyFromAnotherProcessGetsTheSameAllowListTreatmentAsARequest) {
    ASSERT_TRUE(service_->preload(db()).ok());
    // The message on the invalidation channel is data from another process. A
    // key that is not in the registry is discarded rather than acted on.
    service_->invalidate_local("not.a.section");
    service_->invalidate_local("");
    EXPECT_NE(service_->peek(hero(), en()), nullptr);
}

TEST_F(SectionServiceDb, ARedisOutageDegradesFreshnessAndNothingElse) {
    ANVIL_REQUIRE_TRANSACTIONS();
    // publish_invalidation never throws and never fails a request. With no Redis
    // configured in this process it takes the catch, and the local drop — the
    // one step that cannot fail, and the one that matters most on a
    // single-instance deployment — still happens.
    ASSERT_TRUE(service_->preload(db()).ok());
    service_->publish_invalidation(hero().key);
    EXPECT_EQ(service_->peek(hero(), en()), nullptr);

    // And a write still succeeds with the cache tier unavailable.
    sec::SectionContent patch;
    sec::SectionValue subline{};
    subline.text[en().index()] = "Still writable";
    subline.text[ar().index()] = "لسه بيتكتب";
    patch.set("subline", subline);
    EXPECT_TRUE(
        service_->write(db(), hero(), sec::SectionState::Published, 1, patch, actor()).ok());
}

// --- the invalidation hook --------------------------------------------------
//
// A cache ABOVE this library's cache cannot be told a key changed: both
// invalidate_local and the Redis subscriber are internal. The first consumer's
// workaround was to drop its own derived snapshot from its own write handler,
// which misses every write that happened on another instance — the exact bug
// the Redis channel exists to fix, reintroduced one layer up. These cases pin
// the three properties that make the hook a replacement for it rather than a
// version of it.

TEST_F(SectionServiceDb, TheHookRunsAfterTheEntryIsAlreadyGone) {
    // The ordering IS the feature. A consumer re-reads on the callback, so a
    // hook called before the drop would hand it back the stale value it was
    // being told about — and it would look correct in every test that only
    // counted calls.
    std::vector<std::string>    seen;
    bool                        saw_stale = false;
    sec::SectionService*        observed = nullptr;

    std::unique_ptr<sec::SectionService> service = make_service([&](std::string_view key) {
        seen.emplace_back(key);
        saw_stale = saw_stale || observed->peek(hero(), en()) != nullptr;
    });
    observed = service.get();

    ASSERT_TRUE(service->preload(db()).ok());
    ASSERT_NE(service->peek(hero(), en()), nullptr);

    service->invalidate_local(hero().key);

    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0], "home.hero");
    EXPECT_FALSE(saw_stale) << "the hook ran before the local entry was dropped";
}

TEST_F(SectionServiceDb, AKeyTheRegistryDoesNotDeclareNeverReachesTheHook) {
    // The allow-list applies to the hook as well as to the cache. A published
    // key is data from another process, and handing an application an arbitrary
    // string from the wire would make the callback an injection surface for
    // whatever it keys its own cache on.
    std::atomic<int> fired{0};
    std::unique_ptr<sec::SectionService> service =
        make_service([&fired](std::string_view) { fired.fetch_add(1); });

    ASSERT_TRUE(service->preload(db()).ok());
    service->invalidate_local("not.a.section");
    service->invalidate_local("");
    service->invalidate_local("home.hero.extra");

    EXPECT_EQ(fired.load(), 0);
}

TEST_F(SectionServiceDb, AHookThatThrowsCostsTheKeyAndNothingElse) {
    // invalidate_local is noexcept and the subscriber thread calls it. An
    // exception let out of a consumer's callback would be std::terminate — one
    // application's bug taking the process down and stopping every other
    // instance's invalidations with it.
    std::unique_ptr<sec::SectionService> service = make_service([](std::string_view) {
        throw std::runtime_error{"a consumer's callback had a bad day"};
    });

    ASSERT_TRUE(service->preload(db()).ok());
    ASSERT_NE(service->peek(hero(), en()), nullptr);

    service->invalidate_local(hero().key);

    // The drop happened first, so the throw cost the callback and not the cache.
    EXPECT_EQ(service->peek(hero(), en()), nullptr);
    // And the next key is unaffected: nothing latched.
    const sec::SectionSpec& about = *sec::find_section(testapp::kSections, "home.about");
    ASSERT_NE(service->peek(about, en()), nullptr);
    service->invalidate_local(about.key);
    EXPECT_EQ(service->peek(about, en()), nullptr);
}

TEST_F(SectionServiceDb, AStaffWriteReachesTheHookThroughThePathItAlreadyTakes) {
    ANVIL_REQUIRE_TRANSACTIONS();
    // The local half. write() publishes, publish_invalidation drops, and the
    // hook is on that path rather than bolted to write() — which is what makes
    // it fire for a draft promotion, a restore, or anything else that ever
    // learns to invalidate.
    std::vector<std::string> seen;
    std::unique_ptr<sec::SectionService> service =
        make_service([&seen](std::string_view key) { seen.emplace_back(key); });

    ASSERT_TRUE(service->preload(db()).ok());

    sec::SectionContent patch;
    sec::SectionValue subline{};
    subline.text[en().index()] = "A new line";
    subline.text[ar().index()] = "سطر جديد";
    patch.set("subline", subline);
    ASSERT_TRUE(
        service->write(db(), hero(), sec::SectionState::Published, 1, patch, actor()).ok());

    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0], "home.hero");
}

TEST_F(SectionServiceDb, TheHookFiresOnTheSubscribersPathAndNotOnlyWhereTheWriteHappened) {
    ANVIL_REQUIRE_REDIS();
    // The half that decides whether this is a hook or the workaround it
    // replaces. `other` is a second instance of the same deployment: it did not
    // perform the write and hears about it only through Redis.
    std::atomic<int> fired{0};
    std::unique_ptr<sec::SectionService> other =
        make_service([&fired](std::string_view) { fired.fetch_add(1, std::memory_order_release); });
    ASSERT_TRUE(other->preload(db()).ok());
    other->start_invalidation_listener();

    // SUBSCRIBE is asynchronous and a message published before it lands is a
    // message nobody receives, so this publishes until it is heard rather than
    // sleeping once and hoping. The deadline is what turns a broken hook into a
    // failure instead of a hang.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (fired.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        service_->publish_invalidation(hero().key);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    other->stop_invalidation_listener();

    EXPECT_GT(fired.load(std::memory_order_acquire), 0)
        << "a write on another instance never reached this one's hook";
    EXPECT_EQ(other->peek(hero(), en()), nullptr);
}

TEST_F(SectionServiceDb, NoHookIsTheDefaultAndCostsOneBranch) {
    // The configuration every existing consumer already has. An empty
    // std::function must be a branch rather than a call through a null target.
    ASSERT_TRUE(service_->preload(db()).ok());
    service_->invalidate_local(hero().key);
    EXPECT_EQ(service_->peek(hero(), en()), nullptr);
}

}  // namespace
