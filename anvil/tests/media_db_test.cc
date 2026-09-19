// The media collection against a live cluster: namespace scoping, reference
// counting, deduplication, and the two claims that make deletion safe under
// concurrency.
//
// The concurrency cases are here rather than in a separate suite because what
// they assert is a property of the SERVER's atomicity rather than of this
// process's threads: `delete_if_unreferenced` and `clear_refs_if` are correct
// precisely because the expected count is in the filter, and that can only be
// demonstrated against a real server.

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/media/repository.h"
#include "app_fixture.h"
#include "db_fixture.h"

namespace {

using anvil::Uuid;
using anvil::fs::Format;
using anvil::fs::Mime;
using anvil::fs::Ns;
using anvil::images::VariantRecord;
using anvil::media::MediaCursor;
using anvil::media::MediaRecord;
using anvil::media::MediaRepository;
using anvil::media::NewMedia;
using anvil::media::attached_refs;
using anvil::media::is_pinned;
using anvil::media::kPinnedRefs;
using anvil::testfixture::scratch_names;

constexpr std::string_view kMedia = "media";

class MediaDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kMedia);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }
    [[nodiscard]] static MediaRepository media() {
        return MediaRepository{std::string{scratch_names().for_collection(kMedia)}, kMedia};
    }

    [[nodiscard]] Uuid store(Ns ns, std::uint8_t hash_seed,
                             std::optional<std::array<std::uint8_t, 16>> ip = std::nullopt) {
        const Uuid id = anvil::uuid::generate_v7();
        anvil::crypto::Digest256 sha{};
        sha[0] = hash_seed;
        const NewMedia row{
            .variants = {VariantRecord{2048, 320, 180, Format::Avif},
                         VariantRecord{4096, 640, 360, Format::Webp}},
            .sha256 = sha,
            .bytes = 123456,
            .id = id,
            .owner = anvil::uuid::generate_v7(),
            .uploader_ip = ip,
            .width = 1600,
            .height = 900,
            .ns = ns,
            .mime = Mime::Jpeg,
        };
        EXPECT_TRUE(media().insert(db(), row).ok());
        return id;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

[[nodiscard]] std::array<std::uint8_t, 16> v4(std::uint8_t last) noexcept {
    std::array<std::uint8_t, 16> ip{};
    ip[10] = 0xFF;
    ip[11] = 0xFF;
    ip[12] = 203;
    ip[14] = 113;
    ip[15] = last;
    return ip;
}

// --- namespace scoping ------------------------------------------------------

TEST_F(MediaDb, ARowRoundTripsIncludingItsVariants) {
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x01);

    const auto found = media().find(db(), ns, id);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());

    const MediaRecord& row = *found.value();
    EXPECT_EQ(row.id, id);
    EXPECT_EQ(row.ns, ns);
    EXPECT_EQ(row.bytes, 123456U);
    EXPECT_EQ(row.width, 1600U);
    EXPECT_EQ(row.height, 900U);
    EXPECT_EQ(row.mime, Mime::Jpeg);
    EXPECT_EQ(row.refs, 0);
    ASSERT_EQ(row.variants.size(), 2U);
    EXPECT_EQ(row.variants[0].width, 320);
    EXPECT_EQ(row.variants[0].format, Format::Avif);
    EXPECT_EQ(row.variants[1].width, 640);
    EXPECT_EQ(row.variants[1].format, Format::Webp);
}

TEST_F(MediaDb, AnIdFromOneNamespaceDoesNotResolveThroughAnother) {
    // The namespace is in EVERY filter rather than checked by a caller, which is
    // what makes it impossible to forget. A handler holding another API's media
    // id must get nothing back, indistinguishable from an id that never existed.
    const Ns content = *Ns::from_index(0);
    const Ns guest = *Ns::from_index(2);
    const Uuid id = store(content, 0x02);

    const auto wrong = media().find(db(), guest, id);
    ASSERT_TRUE(wrong.ok());
    EXPECT_FALSE(wrong.value().has_value());

    const auto missing = media().find(db(), guest, anvil::uuid::generate_v7());
    ASSERT_TRUE(missing.ok());
    EXPECT_FALSE(missing.value().has_value());
}

TEST_F(MediaDb, DeduplicationIsScopedToOneNamespace) {
    // Sharing a FILE across namespaces would let an upload into one resolve
    // through a handler for another, which is the whole thing the scoping exists
    // to prevent.
    const Ns content = *Ns::from_index(0);
    const Ns guest = *Ns::from_index(2);
    store(content, 0x03);

    anvil::crypto::Digest256 sha{};
    sha[0] = 0x03;

    const auto same_ns = media().find_by_hash(db(), content, sha);
    ASSERT_TRUE(same_ns.ok());
    EXPECT_TRUE(same_ns.value().has_value());

    const auto other_ns = media().find_by_hash(db(), guest, sha);
    ASSERT_TRUE(other_ns.ok());
    EXPECT_FALSE(other_ns.value().has_value());
}

TEST_F(MediaDb, AListingCoversOneNamespaceAndPaginatesByItsCursor) {
    const Ns content = *Ns::from_index(0);
    const Ns other = *Ns::from_index(1);
    for (std::uint8_t i = 0; i < 5; ++i) { store(content, static_cast<std::uint8_t>(0x10 + i)); }
    store(other, 0x20);

    std::vector<Uuid> seen;
    std::optional<MediaCursor> cursor;
    for (int page = 0; page < 5; ++page) {
        const auto rows = media().list_namespace(db(), content, cursor, 2);
        ASSERT_TRUE(rows.ok());
        if (rows.value().empty()) { break; }
        for (const MediaRecord& row : rows.value()) {
            EXPECT_EQ(row.ns, content);
            seen.push_back(row.id);
        }
        const MediaRecord& last = rows.value().back();
        cursor = MediaCursor{last.created_at, last.id};
    }

    EXPECT_EQ(seen.size(), 5U);
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
}

// --- reference counting -----------------------------------------------------

TEST_F(MediaDb, AttachAndReleaseMoveTheCountInsideTheCallersTransaction) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x30);

    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, 1).ok());
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, 1).ok());
    session.commit_transaction();

    const auto after = media().find(db(), ns, id);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value()->refs, 2);
}

TEST_F(MediaDb, ACountCommittedByAnAbortedTransactionDoesNotSurvive) {
    // A count that commits while the document referencing it aborts is a leak no
    // amount of sweeping can distinguish from a live reference. This is why
    // attach takes the OWNING document's session rather than opening its own.
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x31);

    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, 1).ok());
    session.abort_transaction();

    const auto after = media().find(db(), ns, id);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value()->refs, 0);
}

TEST_F(MediaDb, AttachingToARowThatDoesNotExistIsNotFound) {
    // An attach that referenced nothing means the owning document is about to
    // point at a file that is not there, and the transaction is the last place
    // that can be prevented.
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);

    auto session = db().start_session();
    session.start_transaction();
    const anvil::Status attached =
        media().adjust_refs(db(), session, ns, anvil::uuid::generate_v7(), 1);
    session.abort_transaction();

    ASSERT_FALSE(attached.ok());
    EXPECT_EQ(attached.error().code, anvil::ErrorCode::NotFound);
}

// --- the two claims ---------------------------------------------------------

TEST_F(MediaDb, DeletingAnUnreferencedRowClaimsItAndReturnsWhatToUnlink) {
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x40);

    const auto claimed = media().delete_if_unreferenced(db(), ns, id);
    ASSERT_TRUE(claimed.ok());
    ASSERT_TRUE(claimed.value().has_value());
    // The variant list comes back with the claim, because the files are
    // enumerated from the row and never from a readdir glob — a glob is racy,
    // slow, and will happily delete a neighbour that shares a prefix.
    EXPECT_EQ(claimed.value()->variants.size(), 2U);

    const auto gone = media().find(db(), ns, id);
    ASSERT_TRUE(gone.ok());
    EXPECT_FALSE(gone.value().has_value());
}

TEST_F(MediaDb, AReferencedRowIsNotClaimedAndTheCallerLearnsNotToUnlink) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x41);

    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, 1).ok());
    session.commit_transaction();

    const auto claimed = media().delete_if_unreferenced(db(), ns, id);
    ASSERT_TRUE(claimed.ok());
    // nullopt means "not claimed", never "deleted anyway".
    EXPECT_FALSE(claimed.value().has_value());

    const auto still_there = media().find(db(), ns, id);
    ASSERT_TRUE(still_there.ok());
    EXPECT_TRUE(still_there.value().has_value());
}

TEST_F(MediaDb, TwoSweepersRacingProduceOneDeletionAndOneNothingToDo) {
    // The claim and the delete are ONE operation, so the loser learns it must
    // not unlink rather than unlinking files the winner already accounted for.
    const Ns ns = *Ns::from_index(0);
    store(ns, 0x50);

    const anvil::db::TimeMs future = anvil::db::now_ms() + std::chrono::hours{1};

    constexpr int kSweepers = 8;
    std::vector<int> claimed(kSweepers, 0);
    std::vector<std::thread> threads;
    threads.reserve(kSweepers);
    for (int i = 0; i < kSweepers; ++i) {
        threads.emplace_back([&claimed, i, future] {
            auto client = anvil::db::MongoPool::instance().acquire();
            const auto result = media().claim_unreferenced(*client, future);
            if (result.ok() && result.value().has_value()) { claimed[i] = 1; }
        });
    }
    for (std::thread& thread : threads) { thread.join(); }

    int winners = 0;
    for (const int one : claimed) { winners += one; }
    EXPECT_EQ(winners, 1);
}

TEST_F(MediaDb, TheSweeperRespectsTheGracePeriod) {
    // A row created moments ago and not yet attached is an upload whose owning
    // document is still being written, not an orphan.
    const Ns ns = *Ns::from_index(0);
    store(ns, 0x51);

    const anvil::db::TimeMs past = anvil::db::now_ms() - std::chrono::hours{1};
    const auto too_new = media().claim_unreferenced(db(), past);
    ASSERT_TRUE(too_new.ok());
    EXPECT_FALSE(too_new.value().has_value());
}

TEST_F(MediaDb, ClearRefsOnlyLandsWhenTheCountIsStillWhatTheSweeperSaw) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x60);

    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, 3).ok());
    session.commit_transaction();

    // The sweeper decided the row was unreferenced by asking the owning
    // collection, and an attach landed between that question and this write.
    const auto stale = media().clear_refs_if(db(), ns, id, 2);
    ASSERT_TRUE(stale.ok());
    EXPECT_FALSE(stale.value());

    const auto unchanged = media().find(db(), ns, id);
    ASSERT_TRUE(unchanged.ok());
    // Without the interlock this would be a live file collected a day later.
    EXPECT_EQ(unchanged.value()->refs, 3);

    const auto matched = media().clear_refs_if(db(), ns, id, 3);
    ASSERT_TRUE(matched.ok());
    EXPECT_TRUE(matched.value());
}

// --- the pin ----------------------------------------------------------------

TEST_F(MediaDb, APinnedRowReadsAsPinnedAndReportsItsOrdinaryCount) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x70);

    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, kPinnedRefs + 3).ok());
    session.commit_transaction();

    const auto found = media().find(db(), ns, id);
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(is_pinned(*found.value()));
    // The pin is ADDED rather than replacing the count, so the ordinary count
    // comes back out by subtraction — a row rendering "used in 1000003 places"
    // is the bug this exists to avoid.
    EXPECT_EQ(attached_refs(*found.value()), 3);
}

TEST_F(MediaDb, APinnedRowBelowThePinIsStillPinned) {
    // A release with no matching attach walks a pinned row DOWN. An `>=` test
    // would call those unpinned and then report a number near the pin as their
    // ordinary count, which is the same bug wearing a different number.
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0x71);

    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, kPinnedRefs - 1).ok());
    session.commit_transaction();

    const auto found = media().find(db(), ns, id);
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(is_pinned(*found.value()));
    // Clamped at zero: "used in -1 places" is not a thing to put on a screen.
    EXPECT_EQ(attached_refs(*found.value()), 0);
}

// --- usage and purge --------------------------------------------------------

TEST_F(MediaDb, UsageCountsEveryRowButGroupsOnlyTheOnesWithAnAddress) {
    const Ns ns = *Ns::from_index(2);
    store(ns, 0x80, v4(1));
    store(ns, 0x81, v4(1));
    store(ns, 0x82, v4(2));
    store(ns, 0x83);   // no address: an account-backed upload

    const auto usage = media().usage(db(), ns, 10);
    ASSERT_TRUE(usage.ok());

    // The totals cover EVERY row. Answering "how much is this namespace worth"
    // from the per-address group would report a number that silently excludes
    // every account-backed upload.
    EXPECT_EQ(usage.value().count, 4);
    EXPECT_EQ(usage.value().bytes, 4 * 123456);

    ASSERT_EQ(usage.value().top.size(), 2U);
    // Ordered by bytes: the question is "who is filling the disk", and the
    // answer is the first few rows or it is not an answer.
    EXPECT_EQ(usage.value().top[0].count, 2);
    EXPECT_EQ(usage.value().top[0].ip, v4(1));
}

TEST_F(MediaDb, UsageOfAnEmptyNamespaceIsZeroAndNotAnError) {
    const auto usage = media().usage(db(), *Ns::from_index(1), 10);
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage.value().count, 0);
    EXPECT_EQ(usage.value().bytes, 0);
    EXPECT_TRUE(usage.value().top.empty());
}

TEST_F(MediaDb, ListingByAddressIsScopedToOneNamespace) {
    const Ns guest = *Ns::from_index(2);
    const Ns content = *Ns::from_index(0);
    store(guest, 0x90, v4(9));
    store(content, 0x91, v4(9));

    const auto rows = media().list_by_ip(db(), guest, v4(9), 100);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_EQ(rows.value()[0].ns, guest);
}

TEST_F(MediaDb, AnAddressIsAbsentRatherThanZeroWhenItWasNotRecorded) {
    // The unspecified address is a real address, and decoding "we did not record
    // this" into it would make every account-backed upload look like one sender.
    const Ns ns = *Ns::from_index(0);
    const Uuid id = store(ns, 0xA0);

    const auto found = media().find(db(), ns, id);
    ASSERT_TRUE(found.ok());
    EXPECT_FALSE(found.value()->uploader_ip.has_value());
}

TEST_F(MediaDb, ScanAfterWalksEveryRowInIdOrderWithoutSkipping) {
    const Ns ns = *Ns::from_index(0);
    for (std::uint8_t i = 0; i < 7; ++i) { store(ns, static_cast<std::uint8_t>(0xB0 + i)); }

    std::vector<Uuid> seen;
    std::optional<Uuid> cursor;
    for (int page = 0; page < 10; ++page) {
        const auto rows = media().scan_after(db(), cursor, 3);
        ASSERT_TRUE(rows.ok());
        if (rows.value().empty()) { break; }
        for (const MediaRecord& row : rows.value()) { seen.push_back(row.id); }
        cursor = rows.value().back().id;
    }

    EXPECT_EQ(seen.size(), 7U);
    // Strictly ascending, which is what makes the cursor a cursor rather than a
    // skip.
    EXPECT_TRUE(std::is_sorted(seen.begin(), seen.end()));
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
}

}  // namespace
