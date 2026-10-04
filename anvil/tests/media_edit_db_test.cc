// Image edits end to end against a live cluster (docs/21-image-edits.md §3, §6):
// a real upload, a real render, and the rows and reference counts they leave.
//
// The properties here are the ones only a server can show: that the same edit
// of the same source is one object even when two attempts race to the unique
// index, and that deleting an edit releases its source in the same transaction
// as the claim.

#include <gtest/gtest.h>

// Everything below renders, so a build without the image subsystem has nothing
// here to run.
#if ANVIL_HAS_VIPS

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <unistd.h>

#include <vips/vips.h>

#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/digest.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/upload.h"
#include "anvil/images/edit.h"
#include "anvil/images/probe.h"
#include "anvil/media/pipeline.h"
#include "anvil/media/service.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "namespaces.h"

namespace {

using anvil::ErrorCode;
using anvil::Uuid;
namespace fs = anvil::fs;
namespace images = anvil::images;
namespace media = anvil::media;
using anvil::testfixture::scratch_names;

constexpr std::string_view kMedia = "media";

class MediaEditDb : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        images::init("anvil_db_tests");
        std::array<char, 64> pattern{};
        std::snprintf(pattern.data(), pattern.size(), "/tmp/anvil-edit-XXXXXX");
        const char* made = ::mkdtemp(pattern.data());
        ASSERT_NE(made, nullptr);
        root_ = made;
        fs::Storage::init(root_);
    }

    static void TearDownTestSuite() {
        if (fs::Storage::initialised()) { fs::Storage::shutdown(); }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    void SetUp() override {
        if (!images::available()) { GTEST_SKIP() << "built without libvips"; }
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kMedia);
        service_ = std::make_unique<media::MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }
    [[nodiscard]] media::MediaService& service() { return *service_; }

    // An upload, through the same three stages a handler drives.
    [[nodiscard]] Uuid upload(int width, int height) {
        VipsImage* noise = nullptr;
        EXPECT_EQ(vips_gaussnoise(&noise, width, height, "mean", 128.0, "sigma", 60.0, nullptr), 0);
        VipsImage* grey = nullptr;
        EXPECT_EQ(vips_cast(noise, &grey, VIPS_FORMAT_UCHAR, nullptr), 0);
        g_object_unref(noise);
        VipsImage* rgb = nullptr;
        EXPECT_EQ(vips_bandjoin_const1(grey, &rgb, 90.0, nullptr), 0);
        g_object_unref(grey);
        void* buffer = nullptr;
        std::size_t size = 0;
        EXPECT_EQ(vips_image_write_to_buffer(rgb, ".png", &buffer, &size, nullptr), 0);
        g_object_unref(rgb);

        auto opened = fs::UploadSink::open(
            fs::Storage::instance(), fs::UploadLimits{images::kMaxBytes, 0}, testapp::kMedia);
        EXPECT_TRUE(opened.ok());
        fs::UploadSink sink = std::move(opened).value();
        EXPECT_TRUE(sink.write(std::span<const std::uint8_t>{static_cast<std::uint8_t*>(buffer),
                                                             size})
                        .ok());
        g_free(buffer);
        const anvil::Result<fs::UploadResult> finished = sink.finish("image/png");
        EXPECT_TRUE(finished.ok());
        const anvil::Result<media::ProcessedMedia> processed =
            media::process(testapp::kMedia, finished.value());
        EXPECT_TRUE(processed.ok());
        EXPECT_TRUE(service()
                        .record(db(), testapp::kMedia, anvil::uuid::generate_v4(),
                                processed.value(), finished.value().sha256, std::nullopt)
                        .ok());
        return processed.value().id;
    }

    [[nodiscard]] static std::vector<std::uint8_t> recipe_bytes(const images::Recipe& recipe) {
        std::vector<std::uint8_t> bytes;
        images::encode_recipe(recipe, bytes);
        return bytes;
    }

    [[nodiscard]] static images::Recipe crop_recipe(std::uint16_t x) {
        images::Recipe recipe{};
        recipe.crop = images::FixedRect{x, 0, 32768, 65535};
        return recipe;
    }

    // Prepare, render, record — the three stages on their three pools, run in
    // line.
    [[nodiscard]] anvil::Result<media::EditedMedia> edit(const Uuid& source,
                                                         const images::Recipe& recipe,
                                                         bool detach = false) {
        const std::vector<std::uint8_t> bytes = recipe_bytes(recipe);
        auto prepared = service().prepare_edit(db(), testapp::kMedia, source, bytes, detach);
        if (!prepared) { return prepared.error(); }
        if (const auto* existing = std::get_if<media::EditedMedia>(&prepared.value())) {
            return *existing;
        }
        const media::PreparedEdit& ready = std::get<media::PreparedEdit>(prepared.value());
        const anvil::Result<media::RenderedMedia> rendered = media::render(testapp::kMedia, ready);
        if (!rendered) { return rendered.error(); }
        return service().record_edit(db(), testapp::kMedia, anvil::uuid::generate_v4(), ready,
                                     rendered.value().media, rendered.value().sha256);
    }

    // The row, which every caller expects to exist; a missing one fails the
    // case here rather than as a null dereference further down.
    [[nodiscard]] media::MediaRecord row(const Uuid& id) {
        auto found = service().find(db(), testapp::kMedia, id);
        if (!found.ok() || !found.value().has_value()) {
            ADD_FAILURE() << "no media row";
            std::abort();
        }
        return *std::move(found).value();
    }

    [[nodiscard]] static bool master_exists(const Uuid& id) {
        return std::filesystem::exists(
            std::filesystem::path{root_} /
            std::string{fs::media_relative_path(testapp::kMedia, id).view()});
    }

    static inline std::string root_;
    std::unique_ptr<mongocxx::pool::entry> client_;
    std::unique_ptr<media::MediaService>   service_;
};

TEST_F(MediaEditDb, AnEditIsANewRowHoldingOneReferenceOnItsSource) {
    const Uuid source = upload(1200, 800);
    const anvil::Result<media::EditedMedia> edited = edit(source, crop_recipe(0));
    ASSERT_TRUE(edited.ok()) << edited.error().field;
    EXPECT_TRUE(edited.value().created);
    EXPECT_NE(edited.value().id, source);
    EXPECT_EQ(edited.value().width, 600U);
    EXPECT_EQ(edited.value().height, 800U);

    const media::MediaRecord derived = row(edited.value().id);
    ASSERT_TRUE(derived.source.has_value());
    EXPECT_EQ(*derived.source, source);
    EXPECT_EQ(derived.edit, recipe_bytes(crop_recipe(0)));
    EXPECT_FALSE(derived.variants.empty());
    EXPECT_EQ(derived.refs, 0);
    EXPECT_EQ(row(source).refs, 1);
    EXPECT_TRUE(master_exists(edited.value().id));
}

TEST_F(MediaEditDb, TheSameEditOfTheSameSourceIsTheSameObject) {
    const Uuid source = upload(1200, 800);
    const auto first = edit(source, crop_recipe(0));
    const auto again = edit(source, crop_recipe(0));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().id, first.value().id);
    EXPECT_FALSE(again.value().created);
    // Found before any render, so the reference was taken once.
    EXPECT_EQ(row(source).refs, 1);

    const auto different = edit(source, crop_recipe(32767));
    ASSERT_TRUE(different.ok());
    EXPECT_NE(different.value().id, first.value().id);
    EXPECT_EQ(row(source).refs, 2);
}

TEST_F(MediaEditDb, ALostRaceOnTheUniqueIndexAnswersWithTheWinner) {
    // Two attempts that both looked, both found nothing, and both rendered: the
    // index decides, and the loser's files — under its own fresh id — are gone.
    const Uuid source = upload(1200, 800);
    const std::vector<std::uint8_t> bytes = recipe_bytes(crop_recipe(0));
    auto a = service().prepare_edit(db(), testapp::kMedia, source, bytes, false);
    auto b = service().prepare_edit(db(), testapp::kMedia, source, bytes, false);
    ASSERT_TRUE(a.ok() && b.ok());
    const auto& ready_a = std::get<media::PreparedEdit>(a.value());
    const auto& ready_b = std::get<media::PreparedEdit>(b.value());
    const auto rendered_a = media::render(testapp::kMedia, ready_a);
    const auto rendered_b = media::render(testapp::kMedia, ready_b);
    ASSERT_TRUE(rendered_a.ok() && rendered_b.ok());

    const auto won = service().record_edit(db(), testapp::kMedia, anvil::uuid::generate_v4(),
                                           ready_a, rendered_a.value().media,
                                           rendered_a.value().sha256);
    const auto lost = service().record_edit(db(), testapp::kMedia, anvil::uuid::generate_v4(),
                                            ready_b, rendered_b.value().media,
                                            rendered_b.value().sha256);
    ASSERT_TRUE(won.ok());
    ASSERT_TRUE(lost.ok()) << static_cast<int>(lost.error().code);
    EXPECT_TRUE(won.value().created);
    EXPECT_FALSE(lost.value().created);
    EXPECT_EQ(lost.value().id, won.value().id);
    EXPECT_FALSE(master_exists(rendered_b.value().media.id));
    // The loser's reference never committed: its insert and its $inc were one
    // transaction.
    EXPECT_EQ(row(source).refs, 1);
}

TEST_F(MediaEditDb, AnEditOfAnEditIsRefused) {
    const Uuid source = upload(1200, 800);
    const auto first = edit(source, crop_recipe(0));
    ASSERT_TRUE(first.ok());
    images::Recipe flip{};
    flip.flip = true;
    const auto chained = edit(first.value().id, flip);
    ASSERT_FALSE(chained.ok());
    EXPECT_EQ(chained.error().code, ErrorCode::ValidationFailed);
    EXPECT_EQ(chained.error().field, images::kFaultNotSource);
}

TEST_F(MediaEditDb, AMissingSourceIsTheStealthNotFound) {
    images::Recipe flip{};
    flip.flip = true;
    const auto missing = edit(anvil::uuid::generate_v4(), flip);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    // Carrying no field: a missing object and a forbidden one must not be
    // told apart by what the refusal names.
    EXPECT_TRUE(missing.error().field.empty());
}

TEST_F(MediaEditDb, ARecipeThatDoesNotFitItsSourceIsRefusedBeforeAnyRender) {
    const Uuid source = upload(400, 400);
    images::Recipe tiny{};
    tiny.crop = images::FixedRect{0, 0, 16384, 65535};  // 100 px wide
    const auto refused = edit(source, tiny);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().field, images::kFaultTooSmall);
    EXPECT_EQ(row(source).refs, 0);
}

TEST_F(MediaEditDb, DeletingAnEditReleasesItsSource) {
    const Uuid source = upload(1200, 800);
    const auto edited = edit(source, crop_recipe(0));
    ASSERT_TRUE(edited.ok());
    ASSERT_EQ(row(source).refs, 1);

    ASSERT_TRUE(service().purge(db(), testapp::kMedia, edited.value().id).ok());
    EXPECT_EQ(row(source).refs, 0);
    EXPECT_FALSE(master_exists(edited.value().id));
}

TEST_F(MediaEditDb, TheSweeperCollectsAnEditAndThenItsSource) {
    const Uuid source = upload(1200, 800);
    const auto edited = edit(source, crop_recipe(0));
    ASSERT_TRUE(edited.ok());
    const anvil::db::TimeMs later = anvil::db::now_ms() + std::chrono::minutes{1};

    // The source is referenced by the edit, so the only thing unreferenced is
    // the edit; collecting it releases the source, which is then collectable.
    const auto first = service().collect_one_unreferenced(db(), later);
    ASSERT_TRUE(first.ok() && first.value());
    EXPECT_FALSE(master_exists(edited.value().id));
    EXPECT_EQ(row(source).refs, 0);

    const auto second = service().collect_one_unreferenced(db(), later);
    ASSERT_TRUE(second.ok() && second.value());
    EXPECT_FALSE(master_exists(source));
}

TEST_F(MediaEditDb, ADetachedEditHoldsNothingAndCannotBeReopened) {
    const Uuid source = upload(1200, 800);
    const auto detached = edit(source, crop_recipe(0), true);
    ASSERT_TRUE(detached.ok());
    EXPECT_TRUE(detached.value().created);
    const media::MediaRecord derived = row(detached.value().id);
    EXPECT_FALSE(derived.source.has_value());
    EXPECT_TRUE(derived.edit.empty());
    EXPECT_EQ(row(source).refs, 0);

    // Nothing links it to its recipe, so it is not found again — and it is a
    // source in its own right now, which is the point of detaching.
    const auto again = edit(source, crop_recipe(0), true);
    ASSERT_TRUE(again.ok());
    EXPECT_NE(again.value().id, detached.value().id);
    images::Recipe flip{};
    flip.flip = true;
    EXPECT_TRUE(edit(detached.value().id, flip).ok());
}

TEST_F(MediaEditDb, AnUploadNeverDeduplicatesOntoAnEdit) {
    const Uuid source = upload(1200, 800);
    const auto edited = edit(source, crop_recipe(0));
    ASSERT_TRUE(edited.ok());
    const media::MediaRecord derived = row(edited.value().id);
    const auto duplicate =
        service().find_duplicate(db(), testapp::kMedia, derived.owner, derived.sha256);
    ASSERT_TRUE(duplicate.ok());
    EXPECT_FALSE(duplicate.value().has_value());
}

// --- the stored-file class ---------------------------------------------------

TEST_F(MediaEditDb, AStoredFileIsPublishedAsSentAndCannotBeDecodedOrEdited) {
    auto opened = fs::UploadSink::open(fs::Storage::instance(),
                                       fs::UploadLimits{1 << 20, 0}, testapp::kChat);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    std::vector<std::uint8_t> pdf{'%', 'P', 'D', 'F', '-', '1', '.', '7', '\n'};
    pdf.resize(1024, 0x20);
    ASSERT_TRUE(sink.write(pdf).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("application/pdf");
    ASSERT_TRUE(finished.ok());

    // Never handed to a decoder, even by a caller that forgot to branch.
    const auto decoded = media::process(testapp::kChat, finished.value());
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code, ErrorCode::Internal);

    const auto stored = media::store_file(sink, finished.value());
    ASSERT_TRUE(stored.ok());
    EXPECT_TRUE(stored.value().variants.empty());
    EXPECT_EQ(stored.value().master_bytes, pdf.size());
    ASSERT_TRUE(service()
                    .record(db(), testapp::kChat, anvil::uuid::generate_v4(), stored.value(),
                            finished.value().sha256, std::nullopt)
                    .ok());

    // The master on disk is the bytes that arrived, byte for byte.
    const fs::Fd master =
        fs::Storage::instance().open_media(testapp::kChat, stored.value().id, fs::kMasterVariant);
    ASSERT_TRUE(master.valid());
    std::vector<std::uint8_t> read_back(pdf.size() + 1);
    const ssize_t got = ::read(master.get(), read_back.data(), read_back.size());
    ASSERT_EQ(got, static_cast<ssize_t>(pdf.size()));
    read_back.resize(pdf.size());
    EXPECT_EQ(read_back, pdf);

    // A document has no frame to crop.
    images::Recipe flip{};
    flip.flip = true;
    const auto prepared = service().prepare_edit(db(), testapp::kChat, stored.value().id,
                                                 recipe_bytes(flip), false);
    ASSERT_FALSE(prepared.ok());
    EXPECT_EQ(prepared.error().code, ErrorCode::UnsupportedMedia);
}

TEST_F(MediaEditDb, AnImageIsNeverStoredAsAFile) {
    auto opened = fs::UploadSink::open(fs::Storage::instance(),
                                       fs::UploadLimits{1 << 20, 0}, testapp::kChat);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    std::vector<std::uint8_t> png{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png.resize(256, 0x42);
    ASSERT_TRUE(sink.write(png).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("image/png");
    ASSERT_TRUE(finished.ok());
    // Stored as sent, it would keep its metadata, which is the thing the image
    // path exists to strip.
    const auto stored = media::store_file(sink, finished.value());
    ASSERT_FALSE(stored.ok());
    EXPECT_FALSE(sink.published());
}

// --- the sealed class -------------------------------------------------------------

TEST_F(MediaEditDb, ASealedBlobIsPublishedAsSentAndNothingElseMayTouchIt) {
    // Ciphertext that happens to open like a PDF: opaque all the same.
    std::vector<std::uint8_t> blob{'%', 'P', 'D', 'F', '-', '1', '.', '7', '\n'};
    for (std::size_t i = blob.size(); i < 4096; ++i) {
        blob.push_back(static_cast<std::uint8_t>((i * 197U + 3U) & 0xFFU));
    }
    auto opened = fs::UploadSink::open(fs::Storage::instance(),
                                       fs::UploadLimits{1 << 20, 0}, testapp::kSealed);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    ASSERT_TRUE(sink.write(blob).ok());
    const anvil::Result<fs::UploadResult> finished =
        sink.finish_sealed(anvil::crypto::sha256(blob));
    ASSERT_TRUE(finished.ok());

    // Neither the decoder nor the file stage takes it, and refusing does not
    // publish.
    const auto decoded = media::process(testapp::kSealed, finished.value());
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code, ErrorCode::Internal);
    const auto as_file = media::store_file(sink, finished.value());
    ASSERT_FALSE(as_file.ok());
    EXPECT_FALSE(sink.published());

    const auto stored = media::store_sealed(sink, finished.value());
    ASSERT_TRUE(stored.ok());
    EXPECT_TRUE(stored.value().variants.empty());
    EXPECT_EQ(stored.value().width, 0U);
    EXPECT_EQ(stored.value().height, 0U);
    EXPECT_EQ(stored.value().mime, fs::Mime::Sealed);
    const Uuid owner = anvil::uuid::generate_v4();
    ASSERT_TRUE(service()
                    .record(db(), testapp::kSealed, owner, stored.value(),
                            finished.value().sha256, std::nullopt)
                    .ok());

    // Byte for byte what arrived.
    const fs::Fd master = fs::Storage::instance().open_media(testapp::kSealed, stored.value().id,
                                                             fs::kMasterVariant);
    ASSERT_TRUE(master.valid());
    std::vector<std::uint8_t> read_back(blob.size() + 1);
    const ssize_t got = ::read(master.get(), read_back.data(), read_back.size());
    ASSERT_EQ(got, static_cast<ssize_t>(blob.size()));
    read_back.resize(blob.size());
    EXPECT_EQ(read_back, blob);

    // The row reads back as Sealed, and the same bytes from the same owner are
    // never found again: the namespace does not deduplicate.
    const auto found = service().find(db(), testapp::kSealed, stored.value().id);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->mime, fs::Mime::Sealed);
    const auto duplicate =
        service().find_duplicate(db(), testapp::kSealed, owner, finished.value().sha256);
    ASSERT_TRUE(duplicate.ok());
    EXPECT_FALSE(duplicate.value().has_value());

    // Ciphertext has no frame to edit.
    images::Recipe flip{};
    flip.flip = true;
    const auto prepared = service().prepare_edit(db(), testapp::kSealed, stored.value().id,
                                                 recipe_bytes(flip), false);
    ASSERT_FALSE(prepared.ok());
    EXPECT_EQ(prepared.error().code, ErrorCode::UnsupportedMedia);
}

TEST_F(MediaEditDb, AFileIsNeverStoredAsSealed) {
    auto opened = fs::UploadSink::open(fs::Storage::instance(),
                                       fs::UploadLimits{1 << 20, 0}, testapp::kChat);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    std::vector<std::uint8_t> pdf{'%', 'P', 'D', 'F', '-', '1', '.', '7', '\n'};
    pdf.resize(256, 0x20);
    ASSERT_TRUE(sink.write(pdf).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("application/pdf");
    ASSERT_TRUE(finished.ok());
    const auto stored = media::store_sealed(sink, finished.value());
    ASSERT_FALSE(stored.ok());
    EXPECT_EQ(stored.error().code, ErrorCode::Internal);
    EXPECT_FALSE(sink.published());
}

}  // namespace

#endif  // ANVIL_HAS_VIPS
