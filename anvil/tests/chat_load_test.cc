// Phase 18's gate: the send path on ONE hot conversation, under load.
//
// Every send to a conversation $incs the same document for its seq, so a busy
// group is a single-document hotspot by construction, and the question is what
// that costs: whether contention on it surfaces as failures, as duplicates or
// as lost messages, and how many sends a second it sustains. The number is
// RECORDED, not asserted — it is what phase 19's wakes are measured against,
// and a timing threshold is the flaky kind of test docs/16 rules out. What is
// asserted is what only load can falsify:
//
//   1. No send fails while the database is answering.
//   2. Every send is stored exactly once, under a seq nobody else holds.
//   3. One sender's seqs ascend: a client that sent A then B sees A before B.
//
// The activity gate is the Redis one when Redis is up, as a deployment runs
// it: without it every send rewrites every member's chat-list row, which is a
// different (and much slower) path from the one an application ships.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "anvil/chat/activity_gate.h"
#include "anvil/chat/repository.h"
#include "anvil/chat/service.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"
#include "anvil/media/grant.h"
#include "anvil/media/service.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/chat_cards.h"
#include "testapp/chat_collections.h"
#include "testapp/chat_kinds.h"
#include "testapp/perms.h"

namespace anvil::chat {
namespace {

using testfixture::scratch_names;

// Eight senders is a busy group, not a broadcast: the shape that stresses the
// seq document hardest is many writers, and eight is the widest the shared
// test pool is sized for (tests/db_fixture.h).
constexpr int kSenders = 8;
constexpr int kPerSender = 250;
constexpr int kMembers = 16;

constexpr std::string_view kMedia = "media";
constexpr std::array<std::uint8_t, 32> kPepper{};

class ChatLoad : public ::testing::Test {
protected:
    void SetUp() override {
        if (!testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << testfixture::test_uri();
        }
        if (!testfixture::transactions_available()) {
            GTEST_SKIP() << "transactions need a replica set";
        }
        testfixture::ensure_indexes();
        repository_ = std::make_unique<ChatRepository>(scratch_names(),
                                                       testapp::kChatCollections,
                                                       testapp::kChatKinds);
        media_ = std::make_unique<media::MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);
        ChatHooks hooks{};
        hooks.may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; };
        gated_ = testfixture::redis_ready();
        if (gated_) { hooks.claim_activity_bump = redis_activity_gate(); }
        service_ = std::make_unique<ChatService>(ChatServiceDeps{
            .repository = *repository_,
            .media = *media_,
            .grants = grants_,
            .kinds = testapp::kChatKinds,
            .cards = testapp::kChatCards,
            .invite_pepper = kPepper,
            .hooks = std::move(hooks)});
    }

    std::unique_ptr<ChatRepository>      repository_;
    std::unique_ptr<media::MediaService> media_;
    media::GrantKeys grants_{1, crypto::random_array<media::GrantKeys::kKeyBytes>()};
    std::unique_ptr<ChatService>         service_;
    bool                                 gated_{false};
};

TEST_F(ChatLoad, OneHotConversationLosesNothingAndRecordsItsThroughput) {
    std::vector<Uuid> people;
    people.reserve(kMembers);
    for (int i = 0; i < kMembers; ++i) { people.push_back(uuid::generate_v4()); }
    const Uuid owner = people.front();
    const std::vector<Uuid> others(people.begin() + 1, people.end());

    auto setup = db::MongoPool::instance().acquire();
    const Result<CreatedConversation> made = service_->create(
        *setup, Actor{owner, perm_mask(testapp::Perm::ChatCreateGroup)},
        CreateConversation{"group", "Hot", "", others, false});
    ASSERT_TRUE(made.ok()) << static_cast<int>(made.code());
    const Uuid conversation = made.value().conversation.id;

    std::atomic<int> failed{0};
    std::atomic<int> retried{0};
    std::array<std::vector<std::int64_t>, kSenders> seqs{};
    std::vector<std::thread> senders;
    senders.reserve(kSenders);

    const auto started = std::chrono::steady_clock::now();
    for (int s = 0; s < kSenders; ++s) {
        senders.emplace_back([&, s] {
            auto entry = db::MongoPool::instance().acquire();
            const Actor sender{people[static_cast<std::size_t>(s)], PermSet{}};
            std::vector<std::int64_t>& mine = seqs[static_cast<std::size_t>(s)];
            mine.reserve(kPerSender);
            for (int i = 0; i < kPerSender; ++i) {
                SendMessage message{};
                message.client_id = crypto::random_array<16>();
                const std::string body = "message " + std::to_string(i);
                message.body = body;
                const Result<SentMessage> sent =
                    service_->send(*entry, sender, conversation, message);
                if (!sent) {
                    failed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (!sent.value().created) { retried.fetch_add(1, std::memory_order_relaxed); }
                mine.push_back(sent.value().seq);
            }
        });
    }
    for (std::thread& sender : senders) { sender.join(); }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // 1. Nothing failed, and nothing was answered as somebody else's retry.
    EXPECT_EQ(failed.load(), 0);
    EXPECT_EQ(retried.load(), 0);

    // 2 and 3. Every seq is distinct across senders, and ascends per sender.
    std::set<std::int64_t> all;
    for (const std::vector<std::int64_t>& mine : seqs) {
        EXPECT_TRUE(std::is_sorted(mine.begin(), mine.end()));
        EXPECT_TRUE(std::adjacent_find(mine.begin(), mine.end()) == mine.end());
        all.insert(mine.begin(), mine.end());
    }
    constexpr int kTotal = kSenders * kPerSender;
    EXPECT_EQ(all.size(), static_cast<std::size_t>(kTotal));

    // And the log holds each of them once: read back as a member's device
    // would, by catch-up from the start, so a stored row the read cannot see
    // counts as lost.
    std::size_t stored = 0;
    std::int64_t cursor = 0;
    const Actor reader{owner, PermSet{}};
    for (;;) {
        const Result<std::vector<MessageRecord>> page =
            service_->catch_up(*setup, reader, conversation, cursor, kMaxHistoryPage);
        ASSERT_TRUE(page.ok());
        const std::vector<MessageRecord>& rows = page.value();
        if (rows.empty()) { break; }
        for (const MessageRecord& row : rows) {
            if (row.kind == MessageKind::Text) {
                ++stored;
                EXPECT_EQ(all.count(row.seq), 1U) << row.seq;
            }
        }
        cursor = rows.back().seq;
    }
    EXPECT_EQ(stored, static_cast<std::size_t>(kTotal));

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double per_second = kTotal / seconds;
    RecordProperty("sends", kTotal);
    RecordProperty("senders", kSenders);
    RecordProperty("members", kMembers);
    RecordProperty("sends_per_second", static_cast<int>(per_second));
    RecordProperty("activity_gate", gated_ ? "redis" : "none");
    std::printf("chat hot conversation: %d sends by %d senders to %d members in %.2f s, "
                "%.0f sends/s, activity gate %s\n",
                kTotal, kSenders, kMembers, seconds, per_second, gated_ ? "redis" : "none");
}

}  // namespace
}  // namespace anvil::chat
