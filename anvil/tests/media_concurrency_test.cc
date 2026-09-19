// What happens to a stored object when several requests touch it at once.
//
// Every case here is about an operation that LOOKS safe written as a read
// followed by a write, and is not. The reference counting is the clearest of
// them: `find` then `update` loses one of two concurrent attaches, which is a
// file the garbage collector is then entitled to remove while something still
// points at it.

#include <gtest/gtest.h>

#include <atomic>
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
using anvil::media::MediaRepository;
using anvil::media::NewMedia;
using anvil::testfixture::scratch_names;

constexpr std::string_view kMedia = "media";

[[nodiscard]] MediaRepository media() {
    return MediaRepository{std::string{scratch_names().for_collection(kMedia)}, kMedia};
}

[[nodiscard]] anvil::crypto::Digest256 digest_of(std::uint8_t seed) noexcept {
    anvil::crypto::Digest256 sha{};
    sha[0] = seed;
    return sha;
}

[[nodiscard]] NewMedia row_with(Ns ns, const Uuid& id, const anvil::crypto::Digest256& sha) {
    return NewMedia{
        .variants = {VariantRecord{2048, 320, 180, Format::Avif}},
        .sha256 = sha,
        .bytes = 1024,
        .id = id,
        .owner = anvil::uuid::generate_v7(),
        .uploader_ip = std::nullopt,
        .width = 640,
        .height = 360,
        .ns = ns,
        .mime = Mime::Jpeg,
    };
}

class MediaConcurrency : public ::testing::Test {
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

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(MediaConcurrency, ConcurrentUploadsOfIdenticalBytesConvergeWithoutCorruptingEither) {
    // Two uploads of the same file racing each other. Both write a row with the
    // same hash, because the deduplication lookup happens BEFORE the transcode
    // and neither saw the other's row yet. What must not happen is either row
    // becoming unreadable or the two becoming indistinguishable: each is a
    // complete object with its own id and its own files, and the sweeper
    // collects whichever ends up unreferenced.
    const Ns ns = *Ns::from_index(0);
    const anvil::crypto::Digest256 sha = digest_of(0x01);

    constexpr int kUploaders = 8;
    std::vector<Uuid> ids(kUploaders);
    std::atomic<int> stored{0};
    std::vector<std::thread> threads;
    threads.reserve(kUploaders);

    for (int i = 0; i < kUploaders; ++i) {
        ids[static_cast<std::size_t>(i)] = anvil::uuid::generate_v7();
        threads.emplace_back([&ids, &stored, i, ns, sha] {
            auto client = anvil::db::MongoPool::instance().acquire();
            if (media().insert(*client, row_with(ns, ids[static_cast<std::size_t>(i)], sha)).ok()) {
                stored.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) { thread.join(); }

    EXPECT_EQ(stored.load(), kUploaders);

    // Every row is independently readable and complete. A dedup lookup answers
    // with ONE of them — which one does not matter, because they are the same
    // bytes — and the rest are collected once nothing references them.
    for (const Uuid& id : ids) {
        const auto found = media().find(db(), ns, id);
        ASSERT_TRUE(found.ok());
        ASSERT_TRUE(found.value().has_value()) << anvil::uuid::to_string(id);
        EXPECT_EQ(found.value()->sha256, sha);
        EXPECT_EQ(found.value()->variants.size(), 1U);
    }

    const auto deduped = media().find_by_hash(db(), ns, sha);
    ASSERT_TRUE(deduped.ok());
    EXPECT_TRUE(deduped.value().has_value());
}

TEST_F(MediaConcurrency, ConcurrentAttachesAreAllCounted) {
    // THE case a read-then-write loses. Eight transactions each add one
    // reference; a `find` followed by an `update` of read+1 would report
    // anything from one to eight, and any number below eight is a file the
    // collector may remove while something still points at it.
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = anvil::uuid::generate_v7();
    ASSERT_TRUE(media().insert(db(), row_with(ns, id, digest_of(0x02))).ok());

    constexpr int kAttachers = 8;
    std::atomic<int> committed{0};
    std::vector<std::thread> threads;
    threads.reserve(kAttachers);
    for (int i = 0; i < kAttachers; ++i) {
        threads.emplace_back([&committed, id, ns] {
            auto client = anvil::db::MongoPool::instance().acquire();
            try {
                auto session = client->start_session();
                session.with_transaction([&](mongocxx::client_session* txn) {
                    if (!media().adjust_refs(*client, *txn, ns, id, 1)) {
                        throw std::runtime_error{"attach failed"};
                    }
                });
                committed.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception&) {
                // Counted by its absence below.
            }
        });
    }
    for (std::thread& thread : threads) { thread.join(); }

    const auto found = media().find(db(), ns, id);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->refs, committed.load());
    EXPECT_EQ(committed.load(), kAttachers);
}

TEST_F(MediaConcurrency, AttachesAndReleasesNetOutToZero) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = anvil::uuid::generate_v7();
    ASSERT_TRUE(media().insert(db(), row_with(ns, id, digest_of(0x03))).ok());

    constexpr int kPairs = 8;
    std::vector<std::thread> threads;
    threads.reserve(kPairs * 2);
    for (int i = 0; i < kPairs; ++i) {
        for (const std::int32_t delta : {1, -1}) {
            threads.emplace_back([id, ns, delta] {
                auto client = anvil::db::MongoPool::instance().acquire();
                try {
                    auto session = client->start_session();
                    session.with_transaction([&](mongocxx::client_session* txn) {
                        if (!media().adjust_refs(*client, *txn, ns, id, delta)) {
                            throw std::runtime_error{"adjust failed"};
                        }
                    });
                } catch (const std::exception&) {
                }
            });
        }
    }
    for (std::thread& thread : threads) { thread.join(); }

    const auto found = media().find(db(), ns, id);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    // $inc composes in any order. A read-modify-write does not.
    EXPECT_EQ(found.value()->refs, 0);
}

TEST_F(MediaConcurrency, AnAttachLandingDuringADeleteLeavesTheRowIntact) {
    // The claim is conditional on the count still being zero, so the two
    // outcomes are "deleted, and nothing referenced it" or "not deleted, and the
    // caller must not unlink". There is no third outcome in which a referenced
    // row loses its files.
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);

    constexpr int kRounds = 16;
    int deleted = 0;
    int survived = 0;

    for (int round = 0; round < kRounds; ++round) {
        const Uuid id = anvil::uuid::generate_v7();
        ASSERT_TRUE(media()
                        .insert(db(), row_with(ns, id,
                                               digest_of(static_cast<std::uint8_t>(0x10 + round))))
                        .ok());

        std::atomic<bool> claimed{false};
        std::atomic<bool> attached{false};

        std::thread deleter{[&claimed, id, ns] {
            auto client = anvil::db::MongoPool::instance().acquire();
            const auto result = media().delete_if_unreferenced(*client, ns, id);
            claimed.store(result.ok() && result.value().has_value());
        }};
        std::thread attacher{[&attached, id, ns] {
            auto client = anvil::db::MongoPool::instance().acquire();
            try {
                auto session = client->start_session();
                session.with_transaction([&](mongocxx::client_session* txn) {
                    if (!media().adjust_refs(*client, *txn, ns, id, 1)) {
                        throw std::runtime_error{"row gone"};
                    }
                });
                attached.store(true);
            } catch (const std::exception&) {
                attached.store(false);
            }
        }};
        deleter.join();
        attacher.join();

        const auto found = media().find(db(), ns, id);
        ASSERT_TRUE(found.ok());

        if (claimed.load()) {
            // The delete won. The attach must have failed, or a live reference
            // would be pointing at files that were just unlinked.
            EXPECT_FALSE(attached.load());
            EXPECT_FALSE(found.value().has_value());
            ++deleted;
        } else {
            // The delete lost. The row is intact and carries the reference.
            ASSERT_TRUE(found.value().has_value());
            EXPECT_TRUE(attached.load());
            EXPECT_EQ(found.value()->refs, 1);
            ++survived;
        }
    }

    // Both branches are reachable — the point of running the race repeatedly is
    // that neither outcome is wrong, only the third one would be.
    EXPECT_EQ(deleted + survived, kRounds);
}

TEST_F(MediaConcurrency, OnlyOneOfManyConcurrentDeletesClaimsTheRow) {
    // Two callers unlinking the same files is not merely wasteful: the second
    // one may unlink a file the first has already replaced under a reused id.
    // The atomic claim means exactly one caller is ever told to unlink.
    const Ns ns = *Ns::from_index(0);
    const Uuid id = anvil::uuid::generate_v7();
    ASSERT_TRUE(media().insert(db(), row_with(ns, id, digest_of(0x04))).ok());

    constexpr int kDeleters = 8;
    std::atomic<int> claims{0};
    std::vector<std::thread> threads;
    threads.reserve(kDeleters);
    for (int i = 0; i < kDeleters; ++i) {
        threads.emplace_back([&claims, id, ns] {
            auto client = anvil::db::MongoPool::instance().acquire();
            const auto result = media().delete_if_unreferenced(*client, ns, id);
            if (result.ok() && result.value().has_value()) {
                claims.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) { thread.join(); }

    EXPECT_EQ(claims.load(), 1);
}

TEST_F(MediaConcurrency, OnlyOneConcurrentClearRefsMatchesTheExpectedCount) {
    // The expected count is the interlock. Several sweepers that each read the
    // same count and then wrote unconditionally would all "succeed"; only one
    // can match.
    ANVIL_REQUIRE_TRANSACTIONS();
    const Ns ns = *Ns::from_index(0);
    const Uuid id = anvil::uuid::generate_v7();
    ASSERT_TRUE(media().insert(db(), row_with(ns, id, digest_of(0x05))).ok());

    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(media().adjust_refs(db(), session, ns, id, 5).ok());
    session.commit_transaction();

    constexpr int kSweepers = 8;
    std::atomic<int> matched{0};
    std::vector<std::thread> threads;
    threads.reserve(kSweepers);
    for (int i = 0; i < kSweepers; ++i) {
        threads.emplace_back([&matched, id, ns] {
            auto client = anvil::db::MongoPool::instance().acquire();
            const auto result = media().clear_refs_if(*client, ns, id, 5);
            if (result.ok() && result.value()) {
                matched.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) { thread.join(); }

    EXPECT_EQ(matched.load(), 1);

    const auto found = media().find(db(), ns, id);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value()->refs, 0);
}

}  // namespace
