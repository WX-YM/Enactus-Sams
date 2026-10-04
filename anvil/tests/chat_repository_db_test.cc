// The chat repository against a live cluster (docs/22-chat.md §3, §4, §9.1).
//
// Every case asserts something only the SERVER provides: that the unique
// indexes are the constraints, that a conditional write's conditions live in
// its filter, that $max cannot walk a watermark backwards, and that the reads
// the service builds on come back in the order and the range it relies on.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/chat/repository.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/chat_collections.h"
#include "testapp/chat_kinds.h"
#include "testapp/namespaces.h"

namespace {

namespace chat = anvil::chat;
using anvil::ErrorCode;
using anvil::Uuid;
using anvil::db::TimeMs;
using anvil::testfixture::scratch_names;

constexpr chat::KindCode kDirect = static_cast<chat::KindCode>(testapp::ChatKind::Direct);
constexpr chat::KindCode kGroup = static_cast<chat::KindCode>(testapp::ChatKind::Group);

class ChatRepositoryDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        if (!anvil::testfixture::transactions_available()) {
            GTEST_SKIP() << "transactions need a replica set";
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        for (const std::string_view collection :
             {testapp::kChatCollections.conversations, testapp::kChatCollections.members,
              testapp::kChatCollections.messages, testapp::kChatCollections.reactions,
              testapp::kChatCollections.invites, testapp::kChatCollections.blocks}) {
            anvil::testfixture::clear_collection(**client_, collection);
        }
        repo_ = std::make_unique<chat::ChatRepository>(scratch_names(), testapp::kChatCollections,
                                                       testapp::kChatKinds);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }
    [[nodiscard]] const chat::ChatRepository& repo() const { return *repo_; }

    // Runs `body` in one transaction and answers its Status.
    //
    // A failure THROWS out of the callback rather than returning from it. A
    // write refused inside a transaction (a duplicate key, say) has already
    // aborted it on the server, and a callback that returns normally makes the
    // driver commit an aborted transaction and retry that for two minutes.
    struct Abort final {
        anvil::Failure failure;
    };
    template <typename Body>
    [[nodiscard]] anvil::Status in_txn(Body&& body) {
        try {
            auto session = db().start_session();
            anvil::repo::in_transaction(session, [&](mongocxx::client_session* txn) {
                const anvil::Status outcome = body(*txn);
                if (!outcome) { throw Abort{outcome.error()}; }
            });
        } catch (const Abort& aborted) {
            return aborted.failure;
        }
        return anvil::ok();
    }

    [[nodiscard]] Uuid group(const Uuid& creator) {
        chat::ConversationRecord row{};
        row.id = anvil::uuid::generate_v4();
        row.kind = kGroup;
        row.created_by = creator;
        row.created_at = anvil::db::now_ms();
        EXPECT_TRUE(in_txn([&](mongocxx::client_session& txn) {
                        return repo().insert_conversation(db(), txn, row);
                    }).ok());
        return row.id;
    }

    [[nodiscard]] anvil::Status add(const Uuid& conversation, const Uuid& user,
                                    std::int64_t joined, chat::Role role = chat::Role::Member) {
        chat::MemberRecord row{};
        row.id = anvil::uuid::generate_v4();
        row.conversation = conversation;
        row.user = user;
        row.role = role;
        row.joined_seq = joined;
        row.delivered = joined;
        row.read = joined;
        row.activity = anvil::db::now_ms();
        return in_txn([&](mongocxx::client_session& txn) { return repo().join(db(), txn, row); });
    }

    [[nodiscard]] anvil::Status send(const Uuid& conversation, const Uuid& sender,
                                     std::int64_t seq, std::string_view body,
                                     std::optional<TimeMs> expires = std::nullopt,
                                     std::uint8_t cid_seed = 0) {
        chat::MessageRecord row{};
        row.id = anvil::uuid::generate_v7();
        row.sender = sender;
        row.seq = seq;
        row.body = std::string{body};
        row.kind = chat::MessageKind::Text;
        row.sent_at = anvil::db::now_ms();
        row.expires_at = expires;
        row.client_id.fill(cid_seed == 0 ? static_cast<std::uint8_t>(seq) : cid_seed);
        return in_txn([&](mongocxx::client_session& txn) {
            return repo().insert_message(db(), txn, row, conversation);
        });
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
    std::unique_ptr<chat::ChatRepository>  repo_;
};

// --- conversations ------------------------------------------------------------------

TEST_F(ChatRepositoryDb, TwoPeopleOpeningEachOtherAtOnceGetOneConversation) {
    const Uuid a = anvil::uuid::generate_v4();
    const Uuid b = anvil::uuid::generate_v4();
    // The key is symmetric: A opening B and B opening A are the same pair.
    EXPECT_EQ(chat::direct_pair_key(a, b, false), chat::direct_pair_key(b, a, false));
    // And the encryption bit is in it, so one pair holds one of each mode.
    EXPECT_NE(chat::direct_pair_key(a, b, false), chat::direct_pair_key(a, b, true));

    const auto open = [&](const Uuid& opener) {
        auto client = anvil::db::MongoPool::instance().acquire();
        chat::ConversationRecord row{};
        row.id = anvil::uuid::generate_v4();
        row.kind = kDirect;
        row.created_by = opener;
        row.created_at = anvil::db::now_ms();
        row.direct_pair = chat::direct_pair_key(a, b, false);
        return repo().find_or_insert_direct(*client, row);
    };
    auto first = std::async(std::launch::async, open, a);
    auto second = std::async(std::launch::async, open, b);
    const auto one = first.get();
    const auto two = second.get();
    ASSERT_TRUE(one.ok()) << static_cast<int>(one.code());
    ASSERT_TRUE(two.ok()) << static_cast<int>(two.code());
    EXPECT_EQ(one.value().first.id, two.value().first.id);
    EXPECT_NE(one.value().second, two.value().second) << "exactly one of them created it";
}

TEST_F(ChatRepositoryDb, AllocationIsMonotonicAndAStaleFenceMatchesNothing) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const auto first = repo().allocate(db(), id, std::nullopt);
    const auto second = repo().allocate(db(), id, std::nullopt);
    ASSERT_TRUE(first.ok() && first.value().has_value());
    ASSERT_TRUE(second.ok() && second.value().has_value());
    EXPECT_EQ(second.value()->seq, first.value()->seq + 1);
    EXPECT_EQ(first.value()->kind, kGroup);

    // The device-set fence is in the filter: a sender encrypting against an
    // older set burns nothing and is told nothing but "no".
    const auto stale = repo().allocate(db(), id, std::int64_t{42});
    ASSERT_TRUE(stale.ok());
    EXPECT_FALSE(stale.value().has_value());
    const auto after = repo().allocate(db(), id, std::nullopt);
    ASSERT_TRUE(after.ok() && after.value().has_value());
    EXPECT_EQ(after.value()->seq, second.value()->seq + 1) << "a refused fence burnt a seq";

    EXPECT_FALSE(repo().allocate(db(), anvil::uuid::generate_v4(), std::nullopt).value().has_value());
}

// --- members ------------------------------------------------------------------------

TEST_F(ChatRepositoryDb, AMembershipIsRevivedOnRejoinAndRefusedWhileCurrent) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid user = anvil::uuid::generate_v4();
    ASSERT_TRUE(add(id, user, 1).ok());
    const anvil::Status twice = add(id, user, 2);
    ASSERT_FALSE(twice.ok());
    EXPECT_EQ(twice.code(), ErrorCode::Conflict);

    ASSERT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                    const auto left = repo().leave(db(), txn, id, user, 5, 0);
                    if (!left) { return left.error(); }
                    return left.value() ? anvil::ok() : anvil::fail(ErrorCode::NotFound);
                }).ok());
    const auto gone = repo().find_member(db(), id, user);
    ASSERT_TRUE(gone.ok() && gone.value().has_value());
    EXPECT_FALSE(gone.value()->current());
    EXPECT_TRUE(gone.value()->can_see(4));
    EXPECT_FALSE(gone.value()->can_see(5)) << "the range ends where they left";

    ASSERT_TRUE(add(id, user, 9).ok());
    const auto back = repo().find_member(db(), id, user);
    ASSERT_TRUE(back.ok() && back.value().has_value());
    EXPECT_TRUE(back.value()->current());
    EXPECT_EQ(back.value()->joined_seq, 9);
    EXPECT_FALSE(back.value()->can_see(7)) << "what was said while they were out stays unseen";
}

TEST_F(ChatRepositoryDb, AWatermarkNeverMovesBackwards) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid user = anvil::uuid::generate_v4();
    ASSERT_TRUE(add(id, user, 1).ok());
    ASSERT_TRUE(repo().advance(db(), id, user, 10, 8).ok());
    const auto stale = repo().advance(db(), id, user, 4, 3);
    ASSERT_TRUE(stale.ok() && stale.value().has_value());
    EXPECT_EQ(stale.value()->delivered, 10);
    EXPECT_EQ(stale.value()->read, 8);
    // A person who is not a member is not advanced, and is told so.
    EXPECT_FALSE(repo().advance(db(), id, anvil::uuid::generate_v4(), 1, 1).value().has_value());
}

TEST_F(ChatRepositoryDb, TheChatListIsMostRecentFirstAndItsCursorSkipsNothing) {
    const Uuid user = anvil::uuid::generate_v4();
    std::vector<Uuid> conversations;
    for (int i = 0; i < 5; ++i) {
        conversations.push_back(group(user));
        ASSERT_TRUE(add(conversations.back(), user, 1).ok());
    }
    // Bump them in a known order, two in the SAME millisecond, which is the
    // case a single-key cursor gets wrong.
    const TimeMs base = anvil::db::now_ms();
    ASSERT_TRUE(repo().bump_activity(db(), conversations[0], base + std::chrono::milliseconds{50}).ok());
    ASSERT_TRUE(repo().bump_activity(db(), conversations[1], base + std::chrono::milliseconds{40}).ok());
    ASSERT_TRUE(repo().bump_activity(db(), conversations[2], base + std::chrono::milliseconds{40}).ok());
    ASSERT_TRUE(repo().bump_activity(db(), conversations[3], base + std::chrono::milliseconds{30}).ok());
    ASSERT_TRUE(repo().bump_activity(db(), conversations[4], base + std::chrono::milliseconds{20}).ok());

    std::vector<Uuid> seen;
    std::optional<std::pair<TimeMs, Uuid>> cursor;
    for (int page = 0; page < 5; ++page) {
        const auto rows = repo().list_memberships(db(), user, false, cursor, 2);
        ASSERT_TRUE(rows.ok());
        if (rows.value().empty()) { break; }
        for (const chat::MemberRecord& row : rows.value()) { seen.push_back(row.conversation); }
        cursor = std::pair{rows.value().back().activity, rows.value().back().id};
    }
    ASSERT_EQ(seen.size(), 5U);
    EXPECT_EQ(seen.front(), conversations[0]);
    EXPECT_EQ(seen.back(), conversations[4]);
}

TEST_F(ChatRepositoryDb, TheActivityBumpSkipsSomebodyWhoLeft) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid stays = anvil::uuid::generate_v4();
    const Uuid left = anvil::uuid::generate_v4();
    ASSERT_TRUE(add(id, stays, 1).ok());
    ASSERT_TRUE(add(id, left, 1).ok());
    ASSERT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                    return repo().leave(db(), txn, id, left, 2, 0).ok() ? anvil::ok()
                                                                    : anvil::fail(ErrorCode::Internal);
                }).ok());
    const TimeMs later = anvil::db::now_ms() + std::chrono::hours{1};
    ASSERT_TRUE(repo().bump_activity(db(), id, later).ok());
    EXPECT_EQ(repo().find_member(db(), id, stays).value()->activity, later);
    EXPECT_NE(repo().find_member(db(), id, left).value()->activity, later)
        << "a conversation somebody left must not climb their chat list";
}

// --- messages -----------------------------------------------------------------------

TEST_F(ChatRepositoryDb, ARetriedSendIsTheSameMessage) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid sender = anvil::uuid::generate_v4();
    ASSERT_TRUE(send(id, sender, 1, "hello", std::nullopt, 7).ok());
    const anvil::Status retry = send(id, sender, 2, "hello", std::nullopt, 7);
    ASSERT_FALSE(retry.ok());
    EXPECT_EQ(retry.code(), ErrorCode::Conflict);

    std::array<std::uint8_t, 16> cid{};
    cid.fill(7);
    const auto first = repo().find_by_client_id(db(), id, sender, cid);
    ASSERT_TRUE(first.ok() && first.value().has_value());
    EXPECT_EQ(first.value()->seq, 1);
}

TEST_F(ChatRepositoryDb, HistoryCrossesAGapAndHidesWhatExpired) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid sender = anvil::uuid::generate_v4();
    const TimeMs past = anvil::db::now_ms() - std::chrono::minutes{1};
    // Seq 3 was burnt by a crash between allocation and insert; seq 5 expired
    // and the sweeper has not reached it yet.
    for (const std::int64_t seq : {1, 2, 4, 6, 7}) { ASSERT_TRUE(send(id, sender, seq, "m").ok()); }
    ASSERT_TRUE(send(id, sender, 5, "gone", past).ok());

    const auto newest = repo().history(db(), id, 0, 100, 3, anvil::db::now_ms());
    ASSERT_TRUE(newest.ok());
    ASSERT_EQ(newest.value().messages.size(), 3U);
    EXPECT_EQ(newest.value().messages.front().seq, 4) << "oldest first within a page";
    EXPECT_EQ(newest.value().messages.back().seq, 7);
    ASSERT_TRUE(newest.value().older.has_value());

    const auto older =
        repo().history(db(), id, 0, *newest.value().older, 3, anvil::db::now_ms());
    ASSERT_TRUE(older.ok());
    ASSERT_EQ(older.value().messages.size(), 2U);
    EXPECT_EQ(older.value().messages.front().seq, 1);
    EXPECT_FALSE(older.value().older.has_value());

    const auto caught_up = repo().after(db(), id, 2, 100, 10, anvil::db::now_ms());
    ASSERT_TRUE(caught_up.ok());
    ASSERT_EQ(caught_up.value().size(), 3U);
    EXPECT_EQ(caught_up.value().front().seq, 4);
    EXPECT_FALSE(repo().find_message(db(), id, 5, anvil::db::now_ms()).value().has_value())
        << "an expired message is not readable before the sweeper reaches it";
}

TEST_F(ChatRepositoryDb, AnEditsConditionsAreInItsFilter) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid sender = anvil::uuid::generate_v4();
    ASSERT_TRUE(send(id, sender, 1, "first").ok());
    const TimeMs now = anvil::db::now_ms();

    const auto edit = [&](const Uuid& who, std::string_view body, TimeMs not_before) {
        std::optional<chat::Mutated> out;
        EXPECT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                        auto result = repo().edit(db(), txn, id, 1, who, body, {}, not_before,
                                                  now, 7);
                        if (!result) { return result.error(); }
                        out = result.value();
                        return anvil::ok();
                    }).ok());
        return out;
    };
    EXPECT_FALSE(edit(anvil::uuid::generate_v4(), "x", now - std::chrono::hours{1}).has_value())
        << "only the sender edits";
    EXPECT_FALSE(edit(sender, "x", now + std::chrono::hours{1}).has_value())
        << "a closed window matches nothing";
    const std::optional<chat::Mutated> mutated = edit(sender, "second", now - std::chrono::hours{1});
    ASSERT_TRUE(mutated.has_value());
    EXPECT_EQ(mutated->sender, sender);
    EXPECT_FALSE(mutated->hidden_from_peer);
    const auto edited = repo().find_message(db(), id, 1, now);
    ASSERT_TRUE(edited.ok() && edited.value().has_value());
    EXPECT_EQ(edited.value()->body, "second");
    EXPECT_EQ(edited.value()->edits, 1);
    EXPECT_EQ(edited.value()->mutation, 7) << "the counter the edit was numbered with";
}

TEST_F(ChatRepositoryDb, ARevokeKeepsTheSeqAndHandsBackTheAttachmentsOnce) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid sender = anvil::uuid::generate_v4();
    chat::MessageRecord row{};
    row.id = anvil::uuid::generate_v7();
    row.sender = sender;
    row.seq = 1;
    row.body = "look";
    row.kind = chat::MessageKind::Text;
    row.sent_at = anvil::db::now_ms();
    row.attachments.push_back(chat::AttachmentRecord{.name = "x.pdf",
                                                     .media = anvil::uuid::generate_v4(),
                                                     .duration_ms = 0,
                                                     .width = 0,
                                                     .height = 0,
                                                     .ns = testapp::kChat,
                                                     .mime = anvil::fs::Mime::Pdf});
    ASSERT_TRUE(in_txn([&](mongocxx::client_session& txn) {
                    return repo().insert_message(db(), txn, row, id);
                }).ok());

    std::optional<chat::Revoked> revoked;
    ASSERT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                    auto result = repo().revoke(db(), txn, id, 1, sender, std::nullopt, 1);
                    if (!result) { return result.error(); }
                    revoked = result.value();
                    return anvil::ok();
                }).ok());
    ASSERT_TRUE(revoked.has_value());
    ASSERT_EQ(revoked->attachments.size(), 1U);
    EXPECT_EQ(revoked->attachments.front().media, row.attachments.front().media);

    const auto after = repo().find_message(db(), id, 1, anvil::db::now_ms());
    ASSERT_TRUE(after.ok() && after.value().has_value()) << "the row and its seq stay";
    EXPECT_TRUE(after.value()->revoked);
    EXPECT_TRUE(after.value()->body.empty());
    EXPECT_TRUE(after.value()->attachments.empty());

    // A second revoke matches nothing, so the references are released once.
    ASSERT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                    auto result = repo().revoke(db(), txn, id, 1, sender, std::nullopt, 1);
                    if (!result) { return result.error(); }
                    EXPECT_FALSE(result.value().has_value());
                    return anvil::ok();
                }).ok());
}

TEST_F(ChatRepositoryDb, TheSweeperClaimsOnlyWhatHasExpired) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const Uuid sender = anvil::uuid::generate_v4();
    const TimeMs now = anvil::db::now_ms();
    ASSERT_TRUE(send(id, sender, 1, "old", now - std::chrono::minutes{5}).ok());
    ASSERT_TRUE(send(id, sender, 2, "later", now + std::chrono::hours{1}).ok());
    ASSERT_TRUE(send(id, sender, 3, "forever").ok());

    std::vector<std::int64_t> claimed;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                        auto row = repo().claim_expired(db(), txn, now);
                        if (!row) { return row.error(); }
                        if (row.value().has_value()) { claimed.push_back(row.value()->seq); }
                        return anvil::ok();
                    }).ok());
    }
    ASSERT_EQ(claimed.size(), 1U);
    EXPECT_EQ(claimed.front(), 1);
}

// --- invites and blocks -------------------------------------------------------------

TEST_F(ChatRepositoryDb, AnInviteRunsOutAndThenAnswersLikeAnyUnknownLink) {
    const Uuid id = group(anvil::uuid::generate_v4());
    const anvil::crypto::Digest256 digest = anvil::crypto::sha256(std::string_view{"token"});
    ASSERT_TRUE(repo().insert_invite(db(), digest, id, 2,
                                     anvil::db::now_ms() + std::chrono::hours{1},
                                     anvil::uuid::generate_v4())
                    .ok());
    const auto redeem = [&]() {
        std::optional<Uuid> found;
        EXPECT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                        auto result = repo().redeem_invite(db(), txn, digest, anvil::db::now_ms());
                        if (!result) { return result.error(); }
                        found = result.value();
                        return anvil::ok();
                    }).ok());
        return found;
    };
    EXPECT_EQ(redeem(), id);
    EXPECT_EQ(redeem(), id);
    EXPECT_FALSE(redeem().has_value()) << "the cap is in the filter";

    const anvil::crypto::Digest256 expired = anvil::crypto::sha256(std::string_view{"old"});
    ASSERT_TRUE(repo().insert_invite(db(), expired, id, 5,
                                     anvil::db::now_ms() - std::chrono::minutes{1},
                                     anvil::uuid::generate_v4())
                    .ok());
    std::optional<Uuid> stale;
    ASSERT_TRUE(in_txn([&](mongocxx::client_session& txn) -> anvil::Status {
                    auto result = repo().redeem_invite(db(), txn, expired, anvil::db::now_ms());
                    if (!result) { return result.error(); }
                    stale = result.value();
                    return anvil::ok();
                }).ok());
    EXPECT_FALSE(stale.has_value()) << "an expired link is refused before the TTL monitor runs";
}

TEST_F(ChatRepositoryDb, ABlockIsVisibleFromEitherSide) {
    const Uuid a = anvil::uuid::generate_v4();
    const Uuid b = anvil::uuid::generate_v4();
    EXPECT_FALSE(repo().blocked_between(db(), a, b).value());
    ASSERT_TRUE(repo().block(db(), a, b).ok());
    ASSERT_TRUE(repo().block(db(), a, b).ok()) << "blocking twice is one block";
    EXPECT_TRUE(repo().blocked_between(db(), a, b).value());
    EXPECT_TRUE(repo().blocked_between(db(), b, a).value());
    ASSERT_TRUE(repo().unblock(db(), a, b).ok());
    EXPECT_FALSE(repo().blocked_between(db(), b, a).value());
}

}  // namespace
