// The member cache against a live cluster (chat/member_cache.h).
//
// The cache is only as good as its validation, and its validation is a number
// another process writes. So the cases here run the real membership
// transactions through a second service — a second process, as far as the
// cache can tell — and ask the cache with the version a real allocate returns.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/chat/member_cache.h"
#include "anvil/chat/service.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"
#include "anvil/db/codec.h"
#include "anvil/media/grant.h"
#include "anvil/media/service.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/chat_cards.h"
#include "testapp/chat_collections.h"
#include "testapp/chat_kinds.h"
#include "testapp/perms.h"

namespace {

namespace chat = anvil::chat;
using anvil::ErrorCode;
using anvil::Uuid;
using anvil::testfixture::scratch_names;

constexpr std::string_view kMedia = "media";
constexpr std::array<std::uint8_t, 32> kPepper{{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                               9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}};

[[nodiscard]] Uuid person() { return anvil::uuid::generate_v4(); }

[[nodiscard]] chat::Actor owner_of(const Uuid& user) {
    return chat::Actor{user, anvil::perm_mask(testapp::Perm::ChatCreateGroup,
                                              testapp::Perm::ChatCreateChannel)};
}

[[nodiscard]] bool holds(const chat::MemberList& list, const Uuid& user) {
    return std::find(list.users.begin(), list.users.end(), user) != list.users.end();
}

class MemberCacheDb : public ::testing::Test {
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
              testapp::kChatCollections.messages}) {
            anvil::testfixture::clear_collection(**client_, collection);
        }
        repo_ = std::make_unique<chat::ChatRepository>(scratch_names(), testapp::kChatCollections,
                                                       testapp::kChatKinds);
        media_ = std::make_unique<anvil::media::MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);
        chat::ChatHooks hooks{};
        hooks.may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; };
        service_ = std::make_unique<chat::ChatService>(chat::ChatServiceDeps{
            .repository = *repo_, .media = *media_, .grants = grants_,
            .kinds = testapp::kChatKinds, .cards = testapp::kChatCards,
            .invite_pepper = kPepper, .hooks = std::move(hooks)});
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] Uuid group(const Uuid& owner, std::vector<Uuid> members) {
        const auto made = service_->create(
            db(), owner_of(owner), chat::CreateConversation{"group", "Team", "", members, false});
        EXPECT_TRUE(made.ok());
        return made.value().conversation.id;
    }

    // Step 3 of a send, exactly as ChatService::send takes it: the seq and the
    // membership version, from one $inc.
    [[nodiscard]] chat::Allocation allocate(const Uuid& conversation) {
        const auto alloc = repo_->allocate(db(), conversation, std::nullopt);
        EXPECT_TRUE(alloc.ok() && alloc.value().has_value());
        return *alloc.value();
    }

    [[nodiscard]] std::shared_ptr<const chat::MemberList> ask(chat::MemberCache& cache,
                                                              const Uuid& conversation,
                                                              const chat::Allocation& at) {
        auto list = cache.recipients(db(), *repo_, conversation, at.membership_version, at.seq);
        EXPECT_TRUE(list.ok()) << static_cast<int>(list.code());
        return list.ok() ? list.value() : std::make_shared<const chat::MemberList>();
    }

    std::unique_ptr<mongocxx::pool::entry>      client_;
    std::unique_ptr<chat::ChatRepository>       repo_;
    std::unique_ptr<anvil::media::MediaService> media_;
    anvil::media::GrantKeys grants_{1, anvil::crypto::random_array<64>()};
    std::unique_ptr<chat::ChatService>          service_;
};

TEST_F(MemberCacheDb, ASecondSendAtTheSameVersionCostsNoRead) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    chat::MemberCache cache{1U << 20U};

    const chat::Allocation first = allocate(id);
    const auto cold = ask(cache, id, first);
    EXPECT_EQ(cache.misses(), 1U);
    EXPECT_TRUE(holds(*cold, owner));
    EXPECT_TRUE(holds(*cold, a));

    // A row written behind the cache's back with no version bump is invisible
    // to it, which is how this case tells a hit from a read that happened to
    // agree.
    const Uuid smuggled = person();
    db()[std::string{scratch_names().for_collection(testapp::kChatCollections.members)}]
        [std::string{testapp::kChatCollections.members}]
            .insert_one(bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("c", anvil::db::codec::uuid_bin(id)),
                bsoncxx::builder::basic::kvp("u", anvil::db::codec::uuid_bin(smuggled)),
                bsoncxx::builder::basic::kvp("js", bsoncxx::types::b_int64{0})));

    const chat::Allocation second = allocate(id);
    ASSERT_EQ(second.membership_version, first.membership_version);
    const auto warm = ask(cache, id, second);
    EXPECT_EQ(cache.hits(), 1U);
    EXPECT_EQ(warm.get(), cold.get()) << "a hit is the same immutable list, not a copy";
    EXPECT_FALSE(holds(*warm, smuggled));
}

TEST_F(MemberCacheDb, AMemberRemovedOnAnotherProcessGetsNoWakeForTheNextMessage) {
    const Uuid owner = person();
    const Uuid leaving = person();
    const Uuid staying = person();
    const Uuid id = group(owner, {leaving, staying});

    // This process's cache, warm, with the member still in it.
    chat::MemberCache here{1U << 20U};
    const auto before = ask(here, id, allocate(id));
    ASSERT_TRUE(holds(*before, leaving));

    // The removal happens through another process's service and nothing
    // tells this cache about it — no invalidation crosses processes.
    chat::ChatHooks hooks{};
    hooks.may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; };
    const chat::ChatService elsewhere{chat::ChatServiceDeps{
        .repository = *repo_, .media = *media_, .grants = grants_, .kinds = testapp::kChatKinds,
        .cards = testapp::kChatCards, .invite_pepper = kPepper, .hooks = std::move(hooks)}};
    ASSERT_TRUE(elsewhere.remove_member(db(), owner_of(owner), id, leaving).ok());

    // The next send's allocate returns the version the removal bumped, so the
    // entry no longer matches and is read again.
    const chat::Allocation next = allocate(id);
    EXPECT_GT(next.membership_version, before->membership_version);
    const auto after = ask(here, id, next);
    EXPECT_EQ(here.misses(), 2U);
    EXPECT_FALSE(holds(*after, leaving)) << "a removed member would be woken";
    EXPECT_TRUE(holds(*after, staying));
    EXPECT_TRUE(holds(*after, owner));
}

TEST_F(MemberCacheDb, SomebodyWhoJoinedAfterTheAllocateIsNotWokenForIt) {
    const Uuid owner = person();
    const Uuid id = group(owner, {});

    // The send allocated, and THEN somebody joined — the window between step 3
    // and a cold cache's read.
    const chat::Allocation sent = allocate(id);
    const Uuid late = person();
    ASSERT_TRUE(service_->add_members(db(), owner_of(owner), id, std::vector{late}).ok());

    chat::MemberCache cache{1U << 20U};
    const auto list = ask(cache, id, sent);
    EXPECT_TRUE(holds(*list, owner));
    EXPECT_FALSE(holds(*list, late))
        << "a FromJoin member cannot see a message from before they joined, and a wake can "
           "carry it inline";

    // And the joiner is woken for what comes next.
    const auto next = ask(cache, id, allocate(id));
    EXPECT_TRUE(holds(*next, late));
}

TEST_F(MemberCacheDb, ALateSendAtAnOlderVersionDoesNotReplaceANewerEntry) {
    const Uuid owner = person();
    const Uuid id = group(owner, {});
    const chat::Allocation old = allocate(id);
    const Uuid joined = person();
    ASSERT_TRUE(service_->add_members(db(), owner_of(owner), id, std::vector{joined}).ok());
    const chat::Allocation current = allocate(id);

    chat::MemberCache cache{1U << 20U};
    (void)ask(cache, id, current);
    (void)ask(cache, id, old);  // a slow send, finishing after a faster one
    (void)ask(cache, id, current);
    EXPECT_EQ(cache.hits(), 1U) << "the older read evicted the entry every later send wants";
}

TEST_F(MemberCacheDb, TheBoundIsInBytesAndTheLeastRecentlyUsedGoFirst) {
    // Room for exactly two two-member entries.
    const std::size_t two = 2 * ((2 * sizeof(Uuid)) + chat::kMemberCacheEntryOverheadBytes);
    chat::MemberCache cache{two};
    const Uuid a = group(person(), {person()});
    const Uuid b = group(person(), {person()});
    const Uuid c = group(person(), {person()});

    const chat::Allocation at_a = allocate(a);
    (void)ask(cache, a, at_a);
    (void)ask(cache, b, allocate(b));
    EXPECT_EQ(cache.bytes(), two);
    (void)ask(cache, a, at_a);  // a is now the most recent
    (void)ask(cache, c, allocate(c));
    EXPECT_EQ(cache.entries(), 2U);
    EXPECT_LE(cache.bytes(), two);

    const std::uint64_t hits = cache.hits();
    (void)ask(cache, a, at_a);
    EXPECT_EQ(cache.hits(), hits + 1) << "the recently used entry was the one evicted";
}

TEST_F(MemberCacheDb, AConversationTooLargeToWakePerMemberIsRefusedAndNotKept) {
    const Uuid owner = person();
    const auto made = service_->create(db(), owner_of(owner),
                                       chat::CreateConversation{"channel", "News", "", {}, false});
    ASSERT_TRUE(made.ok());
    const Uuid id = made.value().conversation.id;

    std::vector<bsoncxx::document::value> followers;
    followers.reserve(chat::kMaxMembers);
    for (std::uint32_t i = 0; i < chat::kMaxMembers; ++i) {
        followers.push_back(bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("c", anvil::db::codec::uuid_bin(id)),
            bsoncxx::builder::basic::kvp("u", anvil::db::codec::uuid_bin(person())),
            bsoncxx::builder::basic::kvp("js", bsoncxx::types::b_int64{0})));
    }
    db()[std::string{scratch_names().for_collection(testapp::kChatCollections.members)}]
        [std::string{testapp::kChatCollections.members}]
            .insert_many(followers);

    chat::MemberCache cache{64U << 20U};
    const chat::Allocation at = allocate(id);
    const auto refused =
        cache.recipients(db(), *repo_, id, at.membership_version, at.seq);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), ErrorCode::PayloadTooLarge);
    EXPECT_EQ(cache.entries(), 0U);
}

}  // namespace
