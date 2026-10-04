// The chat service against a live cluster: the rules of docs/22-chat.md that
// are not queries, each asserted where it can actually fail.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <future>
#include <memory>
#include <optional>
#include <vector>

#include "anvil/chat/activity_gate.h"
#include "anvil/chat/service.h"
#include "anvil/media/grant.h"
#include "anvil/media/service.h"
#include "anvil/core/uuid.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "anvil/input/json.h"
#include "testapp/chat_cards.h"
#include "testapp/chat_collections.h"
#include "testapp/chat_kinds.h"
#include "testapp/perms.h"

namespace {

namespace chat = anvil::chat;
using anvil::ErrorCode;
using anvil::Uuid;
using anvil::input::Reason;
using anvil::testfixture::scratch_names;

// The reference table with the group shrunk to four, so a capacity race needs
// four people rather than a thousand.
constexpr std::array<chat::ConversationKindSpec, 4> kKinds = [] {
    auto table = testapp::kChatKinds;
    table[1].max_members = 4;
    return table;
}();
static_assert(chat::kinds_are_well_formed(kKinds));

constexpr std::string_view kMedia = "media";
constexpr std::array<std::uint8_t, 64> kGrantKey = [] {
    std::array<std::uint8_t, 64> key{};
    for (std::size_t i = 0; i < key.size(); ++i) { key[i] = static_cast<std::uint8_t>(i * 3U); }
    return key;
}();
constexpr std::array<std::uint8_t, 32> kPepper{{7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                               7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7}};

[[nodiscard]] Uuid person() { return anvil::uuid::generate_v4(); }

[[nodiscard]] chat::Actor creator(const Uuid& user) {
    return chat::Actor{user, anvil::perm_mask(testapp::Perm::ChatCreateGroup,
                                              testapp::Perm::ChatCreateChannel)};
}

[[nodiscard]] chat::Actor plain(const Uuid& user) { return chat::Actor{user, anvil::PermSet{}}; }

class ChatServiceDb : public ::testing::Test {
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
        anvil::testfixture::clear_collection(**client_, kMedia);
        repo_ = std::make_unique<chat::ChatRepository>(scratch_names(), testapp::kChatCollections,
                                                       kKinds);
        media_ = std::make_unique<anvil::media::MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);
        build_service(true);
    }

    void build_service(bool reach_everyone) {
        chat::ChatHooks hooks{};
        if (reach_everyone) {
            hooks.may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; };
        }
        hooks.on_membership = [this](const chat::MembershipEvent& event) {
            events_.push_back(event);
        };
        service_ = std::make_unique<chat::ChatService>(deps(std::move(hooks)));
    }

    [[nodiscard]] chat::ChatServiceDeps deps(chat::ChatHooks hooks) {
        return chat::ChatServiceDeps{.repository = *repo_, .media = *media_, .grants = grants_,
                                     .kinds = kKinds, .cards = testapp::kChatCards,
                                     .invite_pepper = kPepper, .hooks = std::move(hooks)};
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }
    [[nodiscard]] const chat::ChatService& service() const { return *service_; }
    [[nodiscard]] const chat::ChatRepository& repo() const { return *repo_; }

    // A stored object in `ns`, as an upload would leave it, and the handle the
    // upload would have answered with.
    [[nodiscard]] std::pair<Uuid, std::string> uploaded(const Uuid& uploader,
                                                        anvil::fs::Ns ns = testapp::kChat) {
        const Uuid object = anvil::uuid::generate_v4();
        EXPECT_TRUE(media_->repository()
                        .insert(db(), anvil::media::NewMedia{.variants = {}, .sha256 = {},
                                                             .bytes = 10, .id = object,
                                                             .owner = uploader,
                                                             .uploader_ip = std::nullopt,
                                                             .width = 0, .height = 0, .ns = ns,
                                                             .mime = anvil::fs::Mime::Pdf})
                        .ok());
        const std::int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                          anvil::db::now_ms().time_since_epoch())
                                          .count();
        return {object, anvil::media::mint_upload_handle(grants_, ns, object, uploader, now_unix)};
    }

    [[nodiscard]] bool in_timer(const Uuid& conversation, std::uint32_t seconds) {
        auto session = db().start_session();
        bool done = false;
        anvil::repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            done = repo().set_timer(db(), *txn, conversation, seconds).ok();
        });
        return done;
    }

    [[nodiscard]] Uuid group(const Uuid& owner, std::vector<Uuid> members = {}) {
        const auto made = service().create(
            db(), creator(owner), chat::CreateConversation{"group", "Team", "", members, false});
        EXPECT_TRUE(made.ok()) << static_cast<int>(made.code());
        return made.value().conversation.id;
    }

    // The system events in the log, in order.
    [[nodiscard]] std::vector<chat::SystemEvent> log(const Uuid& conversation) {
        std::vector<chat::SystemEvent> out;
        const auto page = repo().history(db(), conversation, 0, 1'000, 100, anvil::db::now_ms());
        EXPECT_TRUE(page.ok());
        for (const chat::MessageRecord& m : page.value().messages) {
            if (m.system.has_value()) { out.push_back(m.system->event); }
        }
        return out;
    }

    std::unique_ptr<mongocxx::pool::entry>  client_;
    std::unique_ptr<chat::ChatRepository>   repo_;
    std::unique_ptr<anvil::media::MediaService> media_;
    anvil::media::GrantKeys                 grants_{1, kGrantKey};
    std::unique_ptr<chat::ChatService>      service_;
    std::vector<chat::MembershipEvent>      events_;
};

// --- creation -----------------------------------------------------------------------

TEST_F(ChatServiceDb, CreatingAGroupNeedsTheKindsBitAndATitle) {
    const Uuid owner = person();
    const auto refused =
        service().create(db(), plain(owner), chat::CreateConversation{"group", "T", "", {}, false});
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), ErrorCode::Forbidden);

    const auto untitled = service().create(db(), creator(owner),
                                           chat::CreateConversation{"group", "", "", {}, false});
    ASSERT_FALSE(untitled.ok());
    EXPECT_EQ(untitled.error().field, chat::kTitleField);
    EXPECT_EQ(untitled.error().detail, static_cast<std::uint16_t>(Reason::Required));

    const auto unknown = service().create(db(), creator(owner),
                                          chat::CreateConversation{"nonesuch", "T", "", {}, false});
    ASSERT_FALSE(unknown.ok());
    EXPECT_EQ(unknown.error().field, chat::kKindField);

    // A direct conversation is found, never created through this door.
    const auto direct = service().create(db(), creator(owner),
                                         chat::CreateConversation{"direct", "T", "", {}, false});
    ASSERT_FALSE(direct.ok());
}

TEST_F(ChatServiceDb, ARetriedCreateIsTheFirstConversationEvenWhenTheAttemptsRace) {
    const Uuid owner = person();
    chat::CreateConversation request{"group", "Team", "", {}, false};
    request.client_id[0] = 0x5A;
    request.client_id[15] = 0xA5;
    std::vector<std::future<anvil::Result<chat::CreatedConversation>>> racing;
    for (int i = 0; i < 4; ++i) {
        racing.push_back(std::async(std::launch::async, [&] {
            auto entry = anvil::db::MongoPool::instance().acquire();
            return service().create(*entry, creator(owner), request);
        }));
    }
    std::vector<Uuid> ids;
    int created = 0;
    for (auto& attempt : racing) {
        const auto made = attempt.get();
        ASSERT_TRUE(made.ok()) << static_cast<int>(made.code());
        ids.push_back(made.value().conversation.id);
        created += made.value().created ? 1 : 0;
    }
    EXPECT_EQ(created, 1) << "one attempt made it; the rest were answered with it";
    for (const Uuid& id : ids) { EXPECT_EQ(id, ids.front()); }
    const auto list = service().chat_list(db(), creator(owner), false, std::nullopt, 10);
    EXPECT_EQ(list.value().items.size(), 1U);

    // The same key from somebody else is their own first attempt.
    const auto theirs = service().create(db(), creator(person()), request);
    ASSERT_TRUE(theirs.ok());
    EXPECT_TRUE(theirs.value().created);
    EXPECT_NE(theirs.value().conversation.id, ids.front());
}

TEST_F(ChatServiceDb, ACreatedGroupHasItsOwnerItsMembersAndOneCreatedMessage) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a, a, owner});
    const auto mine = repo().find_member(db(), id, owner);
    ASSERT_TRUE(mine.ok() && mine.value().has_value());
    EXPECT_EQ(mine.value()->role, chat::Role::Owner);
    const auto theirs = repo().find_member(db(), id, a);
    ASSERT_TRUE(theirs.ok() && theirs.value().has_value());
    EXPECT_EQ(theirs.value()->role, chat::Role::Member);
    EXPECT_TRUE(theirs.value()->can_see(1)) << "a founding member sees the Created message";
    EXPECT_EQ(log(id), std::vector{chat::SystemEvent::Created});
}

TEST_F(ChatServiceDb, EncryptionFollowsTheKindAndNotOnlyTheRequest) {
    const Uuid owner = person();
    // "announce" is E2ee::Never: asking for encryption does not get it.
    const auto plaintext = service().create(
        db(), creator(owner), chat::CreateConversation{"announce", "News", "", {}, true});
    ASSERT_TRUE(plaintext.ok());
    EXPECT_FALSE(plaintext.value().conversation.encrypted);
    const auto optional = service().create(
        db(), creator(owner), chat::CreateConversation{"group", "Secret", "", {}, true});
    ASSERT_TRUE(optional.ok());
    EXPECT_TRUE(optional.value().conversation.encrypted);
}

// --- reach --------------------------------------------------------------------------

TEST_F(ChatServiceDb, WithNoReachPolicyNobodyCanBeReached) {
    build_service(false);
    const auto direct = service().open_direct(db(), plain(person()), "direct", person(), false);
    ASSERT_FALSE(direct.ok());
    EXPECT_EQ(direct.error().field, chat::kMembersField);
    EXPECT_EQ(direct.error().detail, static_cast<std::uint16_t>(Reason::NotAllowed));
}

TEST_F(ChatServiceDb, ABlockAndARefusingPolicyAnswerAlike) {
    const Uuid a = person();
    const Uuid b = person();
    ASSERT_TRUE(repo().block(db(), b, a).ok());
    const auto blocked = service().open_direct(db(), plain(a), "direct", b, false);
    build_service(false);
    const auto policy = service().open_direct(db(), plain(a), "direct", person(), false);
    ASSERT_FALSE(blocked.ok());
    ASSERT_FALSE(policy.ok());
    // Telling them apart would tell `a` that `b` blocked them.
    EXPECT_EQ(blocked.error().code, policy.error().code);
    EXPECT_EQ(blocked.error().field, policy.error().field);
    EXPECT_EQ(blocked.error().detail, policy.error().detail);
}

// --- direct -------------------------------------------------------------------------

TEST_F(ChatServiceDb, TwoPeopleOpeningEachOtherGetOneConversationWithOneCreatedMessage) {
    const Uuid a = person();
    const Uuid b = person();
    const auto open = [&](const Uuid& opener, const Uuid& other) {
        auto client = anvil::db::MongoPool::instance().acquire();
        return service().open_direct(*client, plain(opener), "direct", other, false);
    };
    auto first = std::async(std::launch::async, open, a, b);
    auto second = std::async(std::launch::async, open, b, a);
    const auto one = first.get();
    const auto two = second.get();
    ASSERT_TRUE(one.ok() && two.ok());
    EXPECT_EQ(one.value().conversation.id, two.value().conversation.id);
    const Uuid id = one.value().conversation.id;
    EXPECT_EQ(log(id), std::vector{chat::SystemEvent::Created});
    EXPECT_TRUE(repo().find_member(db(), id, a).value()->current());
    EXPECT_TRUE(repo().find_member(db(), id, b).value()->current());

    // The encrypted pair is a different conversation, never merged into this one.
    const auto secret = service().open_direct(db(), plain(a), "direct", b, true);
    ASSERT_TRUE(secret.ok());
    EXPECT_NE(secret.value().conversation.id, id);
    EXPECT_TRUE(secret.value().conversation.encrypted);
}

TEST_F(ChatServiceDb, NobodyLeavesOrIsAddedToADirectConversation) {
    const Uuid a = person();
    const Uuid b = person();
    const auto opened = service().open_direct(db(), plain(a), "direct", b, false);
    ASSERT_TRUE(opened.ok());
    const Uuid id = opened.value().conversation.id;
    EXPECT_EQ(service().remove_member(db(), plain(a), id, a).code(), ErrorCode::Forbidden);
    const std::array<Uuid, 1> third{person()};
    EXPECT_EQ(service().add_members(db(), plain(a), id, third).code(), ErrorCode::Forbidden);
}

// --- membership ---------------------------------------------------------------------

TEST_F(ChatServiceDb, AStrangerGetsNotFoundForEverythingAboutAConversation) {
    const Uuid id = group(person());
    const chat::Actor stranger = plain(person());
    const Uuid nowhere = person();
    for (const Uuid& target : {id, nowhere}) {
        EXPECT_EQ(service().state(db(), stranger, target).code(), ErrorCode::NotFound);
        EXPECT_EQ(service().members(db(), stranger, target, std::nullopt, 10).code(),
                  ErrorCode::NotFound);
        EXPECT_EQ(service().update_info(db(), stranger, target, "x", std::nullopt).code(),
                  ErrorCode::NotFound);
        EXPECT_EQ(service().remove_member(db(), stranger, target, stranger.user).code(),
                  ErrorCode::NotFound);
    }
}

TEST_F(ChatServiceDb, AMemberWithoutTheRightIsToldForbidden) {
    const Uuid owner = person();
    const Uuid member = person();
    const Uuid id = group(owner, {member});
    const std::array<Uuid, 1> newcomer{person()};
    // In "group" a plain member lacks AddMember.
    EXPECT_EQ(service().add_members(db(), plain(member), id, newcomer).code(),
              ErrorCode::Forbidden);
}

TEST_F(ChatServiceDb, TheMemberBoundIsExactUnderConcurrentAdds) {
    const Uuid owner = person();
    const Uuid id = group(owner, {person()});   // two of four
    // Three requests each adding one, racing for two places.
    std::vector<std::future<anvil::Status>> adds;
    for (int i = 0; i < 3; ++i) {
        adds.push_back(std::async(std::launch::async, [&, newcomer = person()] {
            auto client = anvil::db::MongoPool::instance().acquire();
            const std::array<Uuid, 1> one{newcomer};
            return service().add_members(*client, creator(owner), id, one);
        }));
    }
    int added = 0;
    for (auto& add : adds) {
        const anvil::Status outcome = add.get();
        if (outcome.ok()) {
            ++added;
        } else {
            EXPECT_EQ(outcome.error().detail, static_cast<std::uint16_t>(Reason::TooLong));
        }
    }
    EXPECT_EQ(added, 2);
}

TEST_F(ChatServiceDb, AStrangerCannotLearnWhoBlockedThemByAddingThem) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    const Uuid stranger = person();
    const Uuid blocker = person();
    ASSERT_TRUE(repo().block(db(), blocker, stranger).ok());
    const std::array<Uuid, 1> blocked{blocker};
    const std::array<Uuid, 1> free{person()};
    EXPECT_EQ(service().add_members(db(), plain(stranger), id, blocked).code(), ErrorCode::NotFound);
    EXPECT_EQ(service().add_members(db(), plain(stranger), id, free).code(), ErrorCode::NotFound);
}

TEST_F(ChatServiceDb, AddingSomebodyAlreadyThereRefusesTheWholeRequest) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const std::array<Uuid, 2> batch{person(), a};
    const anvil::Status outcome = service().add_members(db(), creator(owner), id, batch);
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().detail, static_cast<std::uint16_t>(Reason::NotAllowed));
    EXPECT_FALSE(repo().find_member(db(), id, batch[0]).value().has_value())
        << "all or nothing";
}

TEST_F(ChatServiceDb, ALeaverReadsUpToTheirDepartureAndNotTheMessageAnnouncingIt) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    ASSERT_TRUE(service().remove_member(db(), plain(a), id, a).ok());
    const auto gone = repo().find_member(db(), id, a);
    ASSERT_TRUE(gone.ok() && gone.value().has_value());
    ASSERT_TRUE(gone.value()->left_seq.has_value());
    EXPECT_FALSE(gone.value()->can_see(*gone.value()->left_seq));
    EXPECT_TRUE(gone.value()->can_see(*gone.value()->left_seq - 1));
    // A past member still has a state, so they can read what they were sent,
    // and the head they are shown stops where their range does.
    const auto seen = service().state(db(), plain(a), id);
    ASSERT_TRUE(seen.ok());
    EXPECT_EQ(seen.value().conversation.seq, *gone.value()->left_seq - 1);
    // Who is in it now is not theirs to know (docs/22 §7.1): phase 18 let a
    // past member list the members, which in an encrypted group is the very
    // set their removal rotated the keys away from.
    EXPECT_EQ(service().members(db(), plain(a), id, std::nullopt, 10).code(),
              ErrorCode::NotFound);
}

TEST_F(ChatServiceDb, TheLastOwnerLeavingHandsTheGroupToTheSeniorAdmin) {
    const Uuid owner = person();
    const Uuid first = person();
    const Uuid second = person();
    const Uuid id = group(owner, {first, second});
    ASSERT_TRUE(service().set_role(db(), creator(owner), id, second, chat::Role::Admin).ok());
    ASSERT_TRUE(service().remove_member(db(), creator(owner), id, owner).ok());
    // The admin, not the earlier-joined member: rank first, then seniority.
    EXPECT_EQ(repo().find_member(db(), id, second).value()->role, chat::Role::Owner);
    EXPECT_EQ(repo().find_member(db(), id, first).value()->role, chat::Role::Member);
    EXPECT_EQ(log(id).back(), chat::SystemEvent::OwnerSucceeded);
}

TEST_F(ChatServiceDb, NobodyRemovesSomebodyAboveThem) {
    const Uuid owner = person();
    const Uuid admin = person();
    const Uuid id = group(owner, {admin});
    ASSERT_TRUE(service().set_role(db(), creator(owner), id, admin, chat::Role::Admin).ok());
    EXPECT_EQ(service().remove_member(db(), plain(admin), id, owner).code(), ErrorCode::Forbidden);
    // An admin cannot crown anyone; only an owner hands ownership on.
    EXPECT_EQ(service().set_role(db(), plain(admin), id, owner, chat::Role::Member).code(),
              ErrorCode::Forbidden);
}

TEST_F(ChatServiceDb, EveryMembershipChangeIsAMessageAndIsReported) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner);
    events_.clear();
    const std::array<Uuid, 1> one{a};
    ASSERT_TRUE(service().add_members(db(), creator(owner), id, one).ok());
    ASSERT_TRUE(service().set_role(db(), creator(owner), id, a, chat::Role::Admin).ok());
    ASSERT_TRUE(service().update_info(db(), creator(owner), id, "Renamed", std::nullopt).ok());
    ASSERT_TRUE(service().remove_member(db(), creator(owner), id, a).ok());
    EXPECT_EQ(log(id), (std::vector{chat::SystemEvent::Created, chat::SystemEvent::MemberAdded,
                                    chat::SystemEvent::RoleChanged, chat::SystemEvent::InfoChanged,
                                    chat::SystemEvent::MemberRemoved}));
    ASSERT_EQ(events_.size(), 4U);
    EXPECT_EQ(events_.front().subject, a);
    EXPECT_EQ(service().state(db(), creator(owner), id).value().conversation.title, "Renamed");
}

// --- sending and history -------------------------------------------------------------

namespace {

[[nodiscard]] std::array<std::uint8_t, 16> cid(std::uint8_t seed) {
    std::array<std::uint8_t, 16> id{};
    id.fill(seed);
    return id;
}

[[nodiscard]] chat::SendMessage text(std::uint8_t seed, std::string_view body) {
    return chat::SendMessage{cid(seed), body, {}, std::nullopt, std::nullopt};
}

}  // namespace

TEST_F(ChatServiceDb, ANonMemberIsRefusedBeforeASeqIsSpent) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    const std::int64_t before = service().state(db(), creator(owner), id).value().conversation.seq;
    EXPECT_EQ(service().send(db(), plain(person()), id, text(1, "hi")).code(), ErrorCode::NotFound);
    EXPECT_EQ(service().state(db(), creator(owner), id).value().conversation.seq, before)
        << "a refusal after the $inc would also be a timing oracle on the conversation existing";
}

TEST_F(ChatServiceDb, AnAnnouncementsMemberCannotPost) {
    const Uuid owner = person();
    const Uuid reader = person();
    const std::array<Uuid, 1> members{reader};
    const auto made = service().create(
        db(), creator(owner), chat::CreateConversation{"announce", "News", "", members, false});
    ASSERT_TRUE(made.ok());
    const Uuid id = made.value().conversation.id;
    EXPECT_EQ(service().send(db(), plain(reader), id, text(1, "hi")).code(), ErrorCode::Forbidden);
    EXPECT_TRUE(service().send(db(), creator(owner), id, text(1, "news")).ok());
}

TEST_F(ChatServiceDb, ARetryIsTheSameMessageEvenWhenTheAttemptsRace) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    const auto first = service().send(db(), creator(owner), id, text(9, "once"));
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value().created);
    const auto again = service().send(db(), creator(owner), id, text(9, "once"));
    ASSERT_TRUE(again.ok());
    EXPECT_FALSE(again.value().created);
    EXPECT_EQ(again.value().seq, first.value().seq);

    std::vector<std::future<anvil::Result<chat::SentMessage>>> racing;
    for (int i = 0; i < 4; ++i) {
        racing.push_back(std::async(std::launch::async, [&] {
            auto client = anvil::db::MongoPool::instance().acquire();
            return service().send(*client, creator(owner), id, text(10, "raced"));
        }));
    }
    std::vector<std::int64_t> seqs;
    for (auto& attempt : racing) {
        const auto outcome = attempt.get();
        ASSERT_TRUE(outcome.ok()) << static_cast<int>(outcome.code());
        seqs.push_back(outcome.value().seq);
    }
    EXPECT_TRUE(std::all_of(seqs.begin(), seqs.end(), [&](std::int64_t s) { return s == seqs[0]; }))
        << "every attempt is answered with the one stored message";
}

TEST_F(ChatServiceDb, AMentionMustNameACurrentMember) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const std::array<chat::MentionSpan, 1> member{chat::MentionSpan{a, 0, 2}};
    EXPECT_TRUE(service().send(db(), creator(owner), id,
                               chat::SendMessage{cid(1), "@a hi", member, std::nullopt, std::nullopt}).ok());
    const std::array<chat::MentionSpan, 1> outsider{chat::MentionSpan{person(), 0, 2}};
    const auto refused = service().send(db(), creator(owner), id,
                                        chat::SendMessage{cid(2), "@b hi", outsider, std::nullopt, std::nullopt});
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().field, chat::kMentionsField);
}

TEST_F(ChatServiceDb, PlaintextIsRefusedInAnEncryptedConversation) {
    const Uuid owner = person();
    const auto made = service().create(
        db(), creator(owner), chat::CreateConversation{"group", "Secret", "", {}, true});
    ASSERT_TRUE(made.ok());
    const auto refused = service().send(db(), creator(owner), made.value().conversation.id,
                                        text(1, "in the clear"));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().field, chat::kBodyField);
}

TEST_F(ChatServiceDb, AKindsTextBoundIsTheOneEnforced) {
    const Uuid owner = person();
    const auto channel = service().create(
        db(), creator(owner), chat::CreateConversation{"channel", "Updates", "", {}, false});
    ASSERT_TRUE(channel.ok());
    const std::string long_post(2049, 'a');   // the channel kind takes 2048
    const auto refused = service().send(db(), creator(owner), channel.value().conversation.id,
                                        text(1, long_post));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().detail, static_cast<std::uint16_t>(Reason::TooLong));
}

TEST_F(ChatServiceDb, CatchUpCrossesABurntSeqAndHistoryPagesBackwards) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    ASSERT_TRUE(service().send(db(), creator(owner), id, text(1, "one")).ok());
    // A crash between the $inc and the insert.
    ASSERT_TRUE(repo().allocate(db(), id, std::nullopt).ok());
    ASSERT_TRUE(service().send(db(), creator(owner), id, text(2, "two")).ok());
    ASSERT_TRUE(service().send(db(), creator(owner), id, text(3, "three")).ok());

    const auto caught = service().catch_up(db(), creator(owner), id, 1, 50);
    ASSERT_TRUE(caught.ok());
    std::vector<std::string> bodies;
    for (const auto& m : caught.value()) { bodies.push_back(m.body); }
    EXPECT_EQ(bodies, (std::vector<std::string>{"one", "two", "three"}));

    const auto newest = service().history(db(), creator(owner), id, std::nullopt, 2);
    ASSERT_TRUE(newest.ok());
    ASSERT_EQ(newest.value().messages.size(), 2U);
    EXPECT_EQ(newest.value().messages.back().body, "three");
    ASSERT_TRUE(newest.value().older.has_value());
    const auto older = service().history(db(), creator(owner), id, newest.value().older, 10);
    ASSERT_TRUE(older.ok());
    EXPECT_EQ(older.value().messages.back().body, "one");
    EXPECT_FALSE(older.value().older.has_value());
}

TEST_F(ChatServiceDb, ARemovedMemberReadsUpToTheRemovalAndNothingAfter) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    ASSERT_TRUE(service().send(db(), creator(owner), id, text(1, "before")).ok());
    ASSERT_TRUE(service().remove_member(db(), creator(owner), id, a).ok());
    ASSERT_TRUE(service().send(db(), creator(owner), id, text(2, "after")).ok());
    const auto seen = service().history(db(), plain(a), id, std::nullopt, 50);
    ASSERT_TRUE(seen.ok());
    std::vector<std::string> bodies;
    for (const auto& m : seen.value().messages) {
        if (m.kind == chat::MessageKind::Text) { bodies.push_back(m.body); }
    }
    EXPECT_EQ(bodies, std::vector<std::string>{"before"});
    EXPECT_EQ(service().send(db(), plain(a), id, text(3, "me?")).code(), ErrorCode::NotFound);
}

TEST_F(ChatServiceDb, AClearedChatStaysClearedForThatMemberOnly) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const auto sent = service().send(db(), creator(owner), id, text(1, "old"));
    ASSERT_TRUE(sent.ok());
    ASSERT_TRUE(repo().set_preferences(db(), id, a, {.muted_until = std::nullopt, .pinned = std::nullopt,
                                                      .archived = std::nullopt,
                                                      .hide_before = sent.value().seq,
                                                      .read_private = std::nullopt}).ok());
    ASSERT_TRUE(service().send(db(), creator(owner), id, text(2, "new")).ok());
    const auto mine = service().history(db(), plain(a), id, std::nullopt, 50);
    ASSERT_TRUE(mine.ok());
    ASSERT_EQ(mine.value().messages.size(), 1U);
    EXPECT_EQ(mine.value().messages.front().body, "new");
    const auto theirs = service().history(db(), creator(owner), id, std::nullopt, 50);
    EXPECT_GT(theirs.value().messages.size(), 2U);
}

TEST_F(ChatServiceDb, AMessageCarriesTheTimerAndTheRetentionItWasSentUnder) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    ASSERT_TRUE(in_timer(id, 86'400U));
    const auto sent = service().send(db(), creator(owner), id, text(1, "fleeting"));
    ASSERT_TRUE(sent.ok());
    const auto row = repo().find_message(db(), id, sent.value().seq, anvil::db::now_ms());
    ASSERT_TRUE(row.ok() && row.value().has_value());
    ASSERT_TRUE(row.value()->expires_at.has_value());
    EXPECT_EQ(*row.value()->expires_at, sent.value().sent_at + std::chrono::hours{24});
}

TEST_F(ChatServiceDb, TheActivityGateDecidesWhetherASendBumpsTheList) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    chat::ChatHooks hooks{};
    hooks.may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; };
    int asked = 0;
    hooks.claim_activity_bump = [&asked](const Uuid&) {
        ++asked;
        return false;
    };
    const chat::ChatService gated{deps(std::move(hooks))};
    const anvil::db::TimeMs before = repo().find_member(db(), id, owner).value()->activity;
    ASSERT_TRUE(gated.send(db(), creator(owner), id, text(1, "quiet")).ok());
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(repo().find_member(db(), id, owner).value()->activity, before);
}

// --- replies, edits, revocation ------------------------------------------------------

TEST_F(ChatServiceDb, AReplyCannotQuoteWhatTheSenderNeverSaw) {
    const Uuid owner = person();
    const Uuid late = person();
    const Uuid id = group(owner);
    const auto early = service().send(db(), creator(owner), id, text(1, "before you"));
    ASSERT_TRUE(early.ok());
    const std::array<Uuid, 1> one{late};
    ASSERT_TRUE(service().add_members(db(), creator(owner), id, one).ok());
    auto quoting = text(2, "re");
    quoting.reply_to = early.value().seq;
    const auto refused = service().send(db(), plain(late), id, quoting);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().field, chat::kReplyField);
    // The owner, who saw it, may.
    auto answer = text(3, "re");
    answer.reply_to = early.value().seq;
    const auto replied = service().send(db(), creator(owner), id, answer);
    ASSERT_TRUE(replied.ok());
    EXPECT_EQ(repo().find_message(db(), id, replied.value().seq, anvil::db::now_ms())
                  .value()->ref,
              early.value().seq);
}

TEST_F(ChatServiceDb, OnlyTheSenderEditsAndEditsAreNotChained) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const auto sent = service().send(db(), creator(owner), id, text(1, "draft"));
    ASSERT_TRUE(sent.ok());
    EXPECT_EQ(service().edit(db(), plain(a), id, sent.value().seq, "mine now", {}).code(),
              ErrorCode::NotFound);
    ASSERT_TRUE(service().edit(db(), creator(owner), id, sent.value().seq, "second", {}).ok());
    ASSERT_TRUE(service().edit(db(), creator(owner), id, sent.value().seq, "third", {}).ok());
    const auto row = repo().find_message(db(), id, sent.value().seq, anvil::db::now_ms());
    EXPECT_EQ(row.value()->body, "third");
    EXPECT_EQ(row.value()->edits, 2);
}

// --- the mutation catch-up (docs/22-chat.md §4.5) --------------------------------

TEST_F(ChatServiceDb, EditsRevokesAndReactionsAreCaughtUpByTheirOwnCounter) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const std::int64_t first = service().send(db(), creator(owner), id, text(1, "one")).value().seq;
    const std::int64_t second = service().send(db(), creator(owner), id, text(2, "two")).value().seq;
    const std::int64_t third = service().send(db(), creator(owner), id, text(3, "three")).value().seq;

    // A device that has caught up by seq holds all three, and nothing has
    // changed yet.
    EXPECT_EQ(service().state(db(), plain(a), id).value().conversation.mutations, 0);
    EXPECT_TRUE(service().changes(db(), plain(a), id, 0, 50).value().empty());

    ASSERT_TRUE(service().edit(db(), creator(owner), id, first, "one, edited", {}).ok());
    ASSERT_TRUE(service().react(db(), plain(a), id, third, "x").ok());
    ASSERT_TRUE(service().revoke(db(), creator(owner), id, second).ok());

    // The seq cursor sees none of it: nothing was allocated a seq.
    EXPECT_TRUE(service().catch_up(db(), plain(a), id, third, 50).value().empty());

    // The mutation cursor sees all three, in the order they changed, each
    // carrying the counter to move to.
    EXPECT_EQ(service().state(db(), plain(a), id).value().conversation.mutations, 3);
    const auto changed = service().changes(db(), plain(a), id, 0, 50);
    ASSERT_TRUE(changed.ok());
    ASSERT_EQ(changed.value().size(), 3U);
    EXPECT_EQ(changed.value()[0].seq, first);
    EXPECT_EQ(changed.value()[0].body, "one, edited");
    EXPECT_EQ(changed.value()[0].mutation, 1);
    EXPECT_EQ(changed.value()[1].seq, third);
    EXPECT_EQ(changed.value()[1].mutation, 2);
    EXPECT_EQ(changed.value()[2].seq, second);
    EXPECT_TRUE(changed.value()[2].revoked);
    EXPECT_EQ(changed.value()[2].mutation, 3);

    // A cursor in the middle reads the rest; one at the head reads nothing.
    const auto rest = service().changes(db(), plain(a), id, 2, 50);
    ASSERT_EQ(rest.value().size(), 1U);
    EXPECT_EQ(rest.value()[0].seq, second);
    EXPECT_TRUE(service().changes(db(), plain(a), id, 3, 50).value().empty());

    // A message changed twice is read once, at its latest number.
    ASSERT_TRUE(service().edit(db(), creator(owner), id, first, "one, again", {}).ok());
    const auto again = service().changes(db(), plain(a), id, 3, 50);
    ASSERT_EQ(again.value().size(), 1U);
    EXPECT_EQ(again.value()[0].seq, first);
    EXPECT_EQ(again.value()[0].mutation, 4);
    EXPECT_EQ(service().changes(db(), plain(a), id, 0, 50).value().size(), 3U);
}

TEST_F(ChatServiceDb, NothingThatChangesNothingMovesTheCounter) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const std::int64_t seq = service().send(db(), creator(owner), id, text(1, "hi")).value().seq;
    const auto counter = [&] { return service().state(db(), plain(a), id).value().conversation.mutations; };

    // Refused: not theirs, and a reaction taken back that was never there.
    EXPECT_EQ(service().edit(db(), plain(a), id, seq, "mine", {}).code(), ErrorCode::NotFound);
    EXPECT_EQ(service().revoke(db(), plain(a), id, seq).code(), ErrorCode::NotFound);
    ASSERT_TRUE(service().react(db(), plain(a), id, seq, std::nullopt).ok());
    EXPECT_EQ(counter(), 0) << "a refused mutation gives its number back with its transaction";

    ASSERT_TRUE(service().react(db(), plain(a), id, seq, "x").ok());
    ASSERT_TRUE(service().react(db(), plain(a), id, seq, "x").ok());
    EXPECT_EQ(counter(), 1) << "the same reaction twice is one change";
    ASSERT_TRUE(service().react(db(), plain(a), id, seq, std::nullopt).ok());
    EXPECT_EQ(counter(), 2) << "taking it back is another";
}

TEST_F(ChatServiceDb, APastMemberCatchesUpMutationsToWhereTheyLeftAndNoFurther) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const std::int64_t first = service().send(db(), creator(owner), id, text(1, "one")).value().seq;
    ASSERT_TRUE(service().edit(db(), creator(owner), id, first, "one, edited", {}).ok());
    ASSERT_TRUE(service().remove_member(db(), plain(a), id, a).ok());
    ASSERT_TRUE(service().edit(db(), creator(owner), id, first, "after they left", {}).ok());

    // How much it changed after them is not theirs, in the counter any more
    // than in the rows: the edit they were entitled to was already caught up.
    EXPECT_EQ(service().state(db(), plain(a), id).value().conversation.mutations, 1);
    EXPECT_TRUE(service().changes(db(), plain(a), id, 0, 50).value().empty());
    EXPECT_EQ(service().state(db(), creator(owner), id).value().conversation.mutations, 2);
}

TEST_F(ChatServiceDb, AMutationRacingAnotherCommitsInTheOrderItWasNumbered) {
    // A cursor moves past every number it reads, so a lower number committing
    // after a higher one has been read would be skipped for good. Inside the
    // transaction, two mutations of one conversation serialise on it.
    const Uuid owner = person();
    const Uuid id = group(owner, {});
    std::vector<std::int64_t> seqs;
    for (int i = 0; i < 8; ++i) {
        seqs.push_back(service()
                           .send(db(), creator(owner), id,
                                 text(static_cast<std::uint8_t>(10 + i), "m"))
                           .value()
                           .seq);
    }
    std::vector<std::future<bool>> racing;
    for (std::size_t i = 0; i < seqs.size(); ++i) {
        racing.push_back(std::async(std::launch::async, [&, i] {
            auto entry = anvil::db::MongoPool::instance().acquire();
            return service().react(*entry, creator(owner), id, seqs[i], "x").ok();
        }));
    }
    for (std::future<bool>& done : racing) { EXPECT_TRUE(done.get()); }
    const auto changed = service().changes(db(), creator(owner), id, 0, 50);
    ASSERT_EQ(changed.value().size(), seqs.size());
    for (std::size_t i = 0; i < changed.value().size(); ++i) {
        EXPECT_EQ(changed.value()[i].mutation, static_cast<std::int64_t>(i + 1))
            << "every number from one to eight, each on exactly one message";
    }
}

TEST_F(ChatServiceDb, AKindWithNoEditWindowRefusesEveryEdit) {
    const Uuid owner = person();
    const auto made = service().create(
        db(), creator(owner), chat::CreateConversation{"announce", "News", "", {}, false});
    ASSERT_TRUE(made.ok());
    const auto sent = service().send(db(), creator(owner), made.value().conversation.id, text(1, "x"));
    ASSERT_TRUE(sent.ok());
    EXPECT_EQ(service().edit(db(), creator(owner), made.value().conversation.id, sent.value().seq,
                             "y", {}).code(),
              ErrorCode::Forbidden);
}

TEST_F(ChatServiceDb, ARevokeTakesTheContentAndReleasesTheAttachmentOnce) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    // A stored object held by one message.
    const Uuid object = anvil::uuid::generate_v4();
    ASSERT_TRUE(media_->repository()
                    .insert(db(), anvil::media::NewMedia{.variants = {}, .sha256 = {}, .bytes = 10,
                                                         .id = object, .owner = owner,
                                                         .uploader_ip = std::nullopt, .width = 0,
                                                         .height = 0, .ns = testapp::kChat,
                                                         .mime = anvil::fs::Mime::Pdf})
                    .ok());
    chat::MessageRecord row{};
    row.id = anvil::uuid::generate_v7();
    row.sender = owner;
    row.seq = repo().allocate(db(), id, std::nullopt).value()->seq;
    row.kind = chat::MessageKind::Text;
    row.sent_at = anvil::db::now_ms();
    row.client_id = cid(1);
    row.attachments.push_back(chat::AttachmentRecord{.name = "a.pdf", .media = object,
                                                     .duration_ms = 0, .width = 0, .height = 0,
                                                     .ns = testapp::kChat,
                                                     .mime = anvil::fs::Mime::Pdf});
    {
        auto session = db().start_session();
        anvil::repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            ASSERT_TRUE(repo().insert_message(db(), *txn, row, id).ok());
            ASSERT_TRUE(media_->attach(db(), *txn, testapp::kChat, object).ok());
        });
    }
    ASSERT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 1);

    ASSERT_TRUE(service().revoke(db(), creator(owner), id, row.seq).ok());
    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 0);
    EXPECT_EQ(service().revoke(db(), creator(owner), id, row.seq).code(), ErrorCode::NotFound);
    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 0) << "released once";
    const auto kept = repo().find_message(db(), id, row.seq, anvil::db::now_ms());
    ASSERT_TRUE(kept.ok() && kept.value().has_value()) << "the row and its seq stay";
    EXPECT_TRUE(kept.value()->revoked);
}

TEST_F(ChatServiceDb, AModeratorRevokesOthersButNotWhatTheyNeverSaw) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid b = person();
    const Uuid late_admin = person();
    const Uuid id = group(owner, {a, b});
    const auto early = service().send(db(), plain(a), id, text(1, "early"));
    ASSERT_TRUE(early.ok());
    const std::array<Uuid, 1> admin{late_admin};
    ASSERT_TRUE(service().add_members(db(), creator(owner), id, admin).ok());
    ASSERT_TRUE(service().set_role(db(), creator(owner), id, late_admin, chat::Role::Admin).ok());
    const auto later = service().send(db(), plain(a), id, text(2, "later"));
    ASSERT_TRUE(later.ok());

    // A plain member cannot take back somebody else's message.
    EXPECT_EQ(service().revoke(db(), plain(b), id, later.value().seq).code(), ErrorCode::NotFound);
    EXPECT_EQ(service().revoke(db(), plain(late_admin), id, early.value().seq).code(),
              ErrorCode::NotFound);
    EXPECT_TRUE(service().revoke(db(), plain(late_admin), id, later.value().seq).ok());
}

// --- receipts and the chat list -------------------------------------------------------

TEST_F(ChatServiceDb, AReceiptPastTheHeadIsClampedSoItCannotPreReadTheFuture) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const auto sent = service().send(db(), creator(owner), id, text(1, "now"));
    ASSERT_TRUE(sent.ok());
    const auto after = service().receipts(db(), plain(a), id, 0, 1'000'000'000);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().read, sent.value().seq);
    EXPECT_GE(after.value().delivered, after.value().read) << "reading implies delivery";

    const auto next = service().send(db(), creator(owner), id, text(2, "later"));
    ASSERT_TRUE(next.ok());
    const auto readers = service().readers(db(), creator(owner), id, next.value().seq);
    ASSERT_TRUE(readers.ok());
    const std::vector<Uuid>& read = *readers.value().read_by;
    EXPECT_TRUE(std::find(read.begin(), read.end(), a) == read.end())
        << "a receipt for the future would mark every later message read";
    const std::vector<Uuid>& held = readers.value().delivered_to;
    EXPECT_TRUE(std::find(held.begin(), held.end(), a) == held.end())
        << "nor delivered: delivered is clamped to the head too";
}

TEST_F(ChatServiceDb, PrivateReadReceiptsAreWithheldFromOthers) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const auto sent = service().send(db(), creator(owner), id, text(1, "x"));
    ASSERT_TRUE(sent.ok());
    ASSERT_TRUE(service().preferences(db(), plain(a), id, {.read_private = true}).ok());
    ASSERT_TRUE(service().receipts(db(), plain(a), id, sent.value().seq, sent.value().seq).ok());
    const auto readers = service().readers(db(), creator(owner), id, sent.value().seq);
    ASSERT_TRUE(readers.ok());
    const std::vector<Uuid>& read = *readers.value().read_by;
    EXPECT_TRUE(std::find(read.begin(), read.end(), a) == read.end());
    // Delivered is not a read, and is not withheld: their device holds it.
    const std::vector<Uuid>& held = readers.value().delivered_to;
    EXPECT_TRUE(std::find(held.begin(), held.end(), a) != held.end());
    // The reader's own count still moved.
    const auto list = service().chat_list(db(), plain(a), false, std::nullopt, 10);
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().items.size(), 1U);
    EXPECT_EQ(list.value().items.front().unread, 0);
}

TEST_F(ChatServiceDb, ADeliveredKindShowsWhoHoldsAMessageAndNotWhoReadIt) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid b = person();
    const std::array<Uuid, 2> members{a, b};
    const auto made = service().create(
        db(), creator(owner), chat::CreateConversation{"announce", "News", "", members, false});
    ASSERT_TRUE(made.ok());
    const Uuid id = made.value().conversation.id;
    const std::int64_t seq = service().send(db(), creator(owner), id, text(1, "news")).value().seq;
    // a's device holds it and a has read it; b's has neither.
    ASSERT_TRUE(service().receipts(db(), plain(a), id, seq, seq).ok());
    const auto readers = service().readers(db(), creator(owner), id, seq);
    ASSERT_TRUE(readers.ok());
    EXPECT_FALSE(readers.value().read_by.has_value()) << "this kind does not show reads";
    EXPECT_EQ(readers.value().delivered_to, std::vector<Uuid>{a});
}

TEST_F(ChatServiceDb, DeliveredIsAWatermarkSoOneAskAnswersEveryOlderMessage) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const std::int64_t first = service().send(db(), creator(owner), id, text(1, "1")).value().seq;
    const std::int64_t second = service().send(db(), creator(owner), id, text(2, "2")).value().seq;
    ASSERT_TRUE(service().receipts(db(), plain(a), id, second, 0).ok());
    for (const std::int64_t seq : {first, second}) {
        const auto readers = service().readers(db(), creator(owner), id, seq);
        ASSERT_TRUE(readers.ok());
        EXPECT_TRUE(readers.value().read_by->empty());
        EXPECT_NE(std::find(readers.value().delivered_to.begin(),
                            readers.value().delivered_to.end(), a),
                  readers.value().delivered_to.end());
    }
}

TEST_F(ChatServiceDb, AKindWithoutReceiptsAnswersNeither) {
    const Uuid owner = person();
    const auto made = service().create(
        db(), creator(owner), chat::CreateConversation{"channel", "Feed", "", {}, false});
    ASSERT_TRUE(made.ok());
    EXPECT_EQ(service().readers(db(), creator(owner), made.value().conversation.id, 1).code(),
              ErrorCode::Forbidden);
}

TEST_F(ChatServiceDb, TheChatListCountsUnreadBySubtractionAndCapsIt) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    for (std::uint8_t i = 1; i <= 3; ++i) {
        ASSERT_TRUE(service().send(db(), creator(owner), id, text(i, "m")).ok());
    }
    const auto list = service().chat_list(db(), plain(a), false, std::nullopt, 10);
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().items.size(), 1U);
    EXPECT_EQ(list.value().items.front().unread, 3);

    for (int i = 0; i < chat::kUnreadCap + 5; ++i) {
        ASSERT_TRUE(repo().allocate(db(), id, std::nullopt).ok());
    }
    const auto many = service().chat_list(db(), plain(a), false, std::nullopt, 10);
    EXPECT_EQ(many.value().items.front().unread, chat::kUnreadCap);
}

TEST_F(ChatServiceDb, PinsComeFirstAndAreBounded) {
    const Uuid me = person();
    std::vector<Uuid> conversations;
    for (int i = 0; i <= chat::kMaxPins; ++i) { conversations.push_back(group(me)); }
    for (int i = 0; i < chat::kMaxPins; ++i) {
        ASSERT_TRUE(service().preferences(db(), creator(me), conversations[static_cast<std::size_t>(i)],
                                          {.pinned = true}).ok());
    }
    const auto over = service().preferences(db(), creator(me), conversations.back(),
                                                     {.pinned = true});
    ASSERT_FALSE(over.ok());
    EXPECT_EQ(over.error().field, chat::kPinnedField);
    const auto list = service().chat_list(db(), creator(me), false, std::nullopt, 50);
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().pinned.size(), static_cast<std::size_t>(chat::kMaxPins));
}

TEST_F(ChatServiceDb, TheRedisGateLetsOneBumpThroughPerWindow) {
    ANVIL_REQUIRE_REDIS();
    const auto gate = chat::redis_activity_gate(std::chrono::milliseconds{2000});
    const Uuid id = person();
    EXPECT_TRUE(gate(id));
    EXPECT_FALSE(gate(id));
    EXPECT_FALSE(gate(id));
    EXPECT_TRUE(gate(person())) << "the window is per conversation";
}

// --- reactions ----------------------------------------------------------------------

TEST_F(ChatServiceDb, OneReactionPerPersonTalliedWithTheViewersOwnMarked) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid b = person();
    const Uuid id = group(owner, {a, b});
    const auto sent = service().send(db(), creator(owner), id, text(1, "nice"));
    ASSERT_TRUE(sent.ok());
    const std::int64_t seq = sent.value().seq;
    ASSERT_TRUE(service().react(db(), plain(a), id, seq, "👍").ok());
    ASSERT_TRUE(service().react(db(), plain(b), id, seq, "👍").ok());
    ASSERT_TRUE(service().react(db(), plain(b), id, seq, "❤").ok()) << "a second replaces the first";

    const std::array<std::int64_t, 1> page{seq};
    const auto tallies = service().reactions(db(), plain(a), id, page);
    ASSERT_TRUE(tallies.ok());
    ASSERT_EQ(tallies.value().size(), 2U);
    for (const auto& tally : tallies.value()) {
        EXPECT_EQ(tally.count, 1);
        EXPECT_EQ(tally.mine, tally.reaction == "👍");
    }
    ASSERT_TRUE(service().react(db(), plain(a), id, seq, std::nullopt).ok());
    EXPECT_EQ(service().reactions(db(), plain(a), id, page).value().size(), 1U);
}

TEST_F(ChatServiceDb, AReactionIsOneClusterOnAVisibleLiveMessage) {
    const Uuid owner = person();
    const Uuid late = person();
    const Uuid id = group(owner);
    const auto early = service().send(db(), creator(owner), id, text(1, "old"));
    ASSERT_TRUE(early.ok());
    const std::array<Uuid, 1> one{late};
    ASSERT_TRUE(service().add_members(db(), creator(owner), id, one).ok());
    EXPECT_EQ(service().react(db(), plain(late), id, early.value().seq, "👍").code(),
              ErrorCode::NotFound);
    EXPECT_EQ(service().react(db(), creator(owner), id, early.value().seq, "👍👍").error().field,
              chat::kReactionField);
    ASSERT_TRUE(service().revoke(db(), creator(owner), id, early.value().seq).ok());
    EXPECT_EQ(service().react(db(), creator(owner), id, early.value().seq, "👍").code(),
              ErrorCode::NotFound);
    // And a tally is never a way to ask about a message out of range.
    const std::array<std::int64_t, 1> hidden{early.value().seq};
    EXPECT_TRUE(service().reactions(db(), plain(late), id, hidden).value().empty());
}

// --- disappearing messages ----------------------------------------------------------

TEST_F(ChatServiceDb, OnlyADeclaredTimerIsSetAndItReachesOnlyLaterMessages) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const auto before = service().send(db(), creator(owner), id, text(1, "kept"));
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(service().set_timer(db(), creator(owner), id, 3'600U).error().field,
              chat::kTimerField);
    EXPECT_EQ(service().set_timer(db(), plain(a), id, 86'400U).code(), ErrorCode::Forbidden);
    ASSERT_TRUE(service().set_timer(db(), creator(owner), id, 86'400U).ok());
    EXPECT_EQ(log(id).back(), chat::SystemEvent::TimerChanged);
    const auto after = service().send(db(), creator(owner), id, text(2, "fleeting"));
    ASSERT_TRUE(after.ok());
    const anvil::db::TimeMs now = anvil::db::now_ms();
    EXPECT_FALSE(repo().find_message(db(), id, before.value().seq, now).value()->expires_at.has_value())
        << "a timer does not reach back into what was already said";
    EXPECT_TRUE(repo().find_message(db(), id, after.value().seq, now).value()->expires_at.has_value());
    ASSERT_TRUE(service().set_timer(db(), creator(owner), id, 0).ok()) << "off is always allowed";
}

TEST_F(ChatServiceDb, TheSweeperRemovesTheExpiredWithTheirReferencesAndReactions) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    const Uuid object = anvil::uuid::generate_v4();
    ASSERT_TRUE(media_->repository()
                    .insert(db(), anvil::media::NewMedia{.variants = {}, .sha256 = {}, .bytes = 10,
                                                         .id = object, .owner = owner,
                                                         .uploader_ip = std::nullopt, .width = 0,
                                                         .height = 0, .ns = testapp::kChat,
                                                         .mime = anvil::fs::Mime::Pdf})
                    .ok());
    const anvil::db::TimeMs past = anvil::db::now_ms() - std::chrono::minutes{1};
    std::vector<std::int64_t> seqs;
    for (std::uint8_t i = 1; i <= 3; ++i) {
        chat::MessageRecord row{};
        row.id = anvil::uuid::generate_v7();
        row.sender = owner;
        row.seq = repo().allocate(db(), id, std::nullopt).value()->seq;
        row.kind = chat::MessageKind::Text;
        row.sent_at = past;
        row.client_id = cid(i);
        row.body = "gone";
        row.expires_at = past;
        if (i == 1) {
            row.attachments.push_back(chat::AttachmentRecord{.name = {}, .media = object,
                                                             .duration_ms = 0, .width = 0,
                                                             .height = 0, .ns = testapp::kChat,
                                                             .mime = anvil::fs::Mime::Pdf});
        }
        auto session = db().start_session();
        anvil::repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            ASSERT_TRUE(repo().insert_message(db(), *txn, row, id).ok());
            if (i == 1) { ASSERT_TRUE(media_->attach(db(), *txn, testapp::kChat, object).ok()); }
        });
        // Straight to the repository: the message is already past its expiry,
        // which the service would refuse to react to.
        auto reacting = db().start_session();
        anvil::repo::in_transaction(reacting, [&](mongocxx::client_session* txn) {
            ASSERT_TRUE(repo().set_reaction(db(), *txn, id, row.seq, owner, "👍").ok());
        });
        seqs.push_back(row.seq);
    }
    const auto kept = service().send(db(), creator(owner), id, text(9, "stays"));
    ASSERT_TRUE(kept.ok());

    const auto first = service().sweep_expired(db(), 2);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 2) << "bounded by the batch";
    const auto second = service().sweep_expired(db(), 10);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value(), 1);
    EXPECT_EQ(service().sweep_expired(db(), 10).value(), 0);

    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 0)
        << "the reference left with the message";
    EXPECT_TRUE(repo().reactions(db(), id, seqs, owner, 100).value().empty());
    EXPECT_TRUE(repo().find_message(db(), id, kept.value().seq, anvil::db::now_ms()).value().has_value());
}

// --- invites and blocks --------------------------------------------------------------

TEST_F(ChatServiceDb, AnInviteJoinsOnceAndThenAnswersLikeEveryDeadLink) {
    const Uuid owner = person();
    const Uuid member = person();
    const Uuid id = group(owner, {member});
    EXPECT_EQ(service().create_invite(db(), plain(member), id, 1, std::chrono::hours{1}).code(),
              ErrorCode::Forbidden);
    const auto token = service().create_invite(db(), creator(owner), id, 1, std::chrono::hours{1});
    ASSERT_TRUE(token.ok());

    // A member following the link spends nothing.
    ASSERT_TRUE(service().join(db(), plain(member), token.value()).ok());
    const Uuid joiner = person();
    const auto joined = service().join(db(), plain(joiner), token.value());
    ASSERT_TRUE(joined.ok());
    EXPECT_EQ(joined.value().conversation.id, id);
    EXPECT_EQ(log(id).back(), chat::SystemEvent::JoinedByInvite);

    // Spent, unknown and malformed are one answer.
    EXPECT_EQ(service().join(db(), plain(person()), token.value()).code(), ErrorCode::NotFound);
    EXPECT_EQ(service().join(db(), plain(person()), std::string(43, 'A')).code(), ErrorCode::NotFound);
    EXPECT_EQ(service().join(db(), plain(person()), "short").code(), ErrorCode::NotFound);
}

TEST_F(ChatServiceDb, ARevokedLinkAndAFullGroupAnswerLikeAnUnknownLink) {
    const Uuid owner = person();
    const Uuid id = group(owner, {person(), person()});   // three of four
    const auto revoked = service().create_invite(db(), creator(owner), id, 10, std::chrono::hours{1});
    ASSERT_TRUE(revoked.ok());
    ASSERT_TRUE(service().revoke_invite(db(), creator(owner), id, revoked.value()).value());
    EXPECT_EQ(service().join(db(), plain(person()), revoked.value()).code(), ErrorCode::NotFound);

    const auto open = service().create_invite(db(), creator(owner), id, 10, std::chrono::hours{1});
    ASSERT_TRUE(open.ok());
    ASSERT_TRUE(service().join(db(), plain(person()), open.value()).ok());   // the fourth
    EXPECT_EQ(service().join(db(), plain(person()), open.value()).code(), ErrorCode::NotFound);
}

TEST_F(ChatServiceDb, ABlockedSendersDirectMessageIsShownOnlyToTheSender) {
    const Uuid a = person();
    const Uuid b = person();
    const auto opened = service().open_direct(db(), plain(a), "direct", b, false);
    ASSERT_TRUE(opened.ok());
    const Uuid id = opened.value().conversation.id;
    ASSERT_TRUE(service().block(db(), plain(b), a).ok());

    // `a` is not told: the send succeeds like any other.
    const auto sent = service().send(db(), plain(a), id, text(1, "hello?"));
    ASSERT_TRUE(sent.ok());
    const auto bodies = [&](const Uuid& viewer) {
        std::vector<std::string> out;
        const auto page = service().history(db(), plain(viewer), id, std::nullopt, 50);
        EXPECT_TRUE(page.ok());
        for (const auto& m : page.value().messages) {
            if (m.kind == chat::MessageKind::Text) { out.push_back(m.body); }
        }
        return out;
    };
    EXPECT_EQ(bodies(a), std::vector<std::string>{"hello?"});
    EXPECT_TRUE(bodies(b).empty());
    EXPECT_TRUE(service().catch_up(db(), plain(b), id, 0, 50).value().size() <= 1U)
        << "only the Created message";
    // The one who blocked knows they did, and is refused.
    EXPECT_EQ(service().send(db(), plain(b), id, text(2, "no")).code(), ErrorCode::Forbidden);
    // And nobody can open a new one past it.
    EXPECT_FALSE(service().open_direct(db(), plain(a), "direct", b, true).ok());
}

TEST_F(ChatServiceDb, ABlockTouchesNoGroup) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    ASSERT_TRUE(service().block(db(), plain(a), owner).ok());
    ASSERT_TRUE(service().send(db(), creator(owner), id, text(1, "still here")).ok());
    const auto seen = service().history(db(), plain(a), id, std::nullopt, 50);
    ASSERT_TRUE(seen.ok());
    EXPECT_EQ(seen.value().messages.back().body, "still here");
}

// --- attachments ----------------------------------------------------------------------

TEST_F(ChatServiceDb, AnAttachmentIsReferencedWithTheMessageAndForwardedWithoutAnId) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid first = group(owner, {a});
    const Uuid second = group(owner);
    const auto [object, handle] = uploaded(owner);

    const std::array<chat::OutgoingAttachment, 1> upload{chat::OutgoingAttachment{
        .handle = handle, .forward = std::nullopt, .name = "contract.pdf", .duration_ms = 0,
        .width = 0, .height = 0}};
    chat::SendMessage message = text(1, "");
    message.attachments = upload;
    const auto sent = service().send(db(), creator(owner), first, message);
    ASSERT_TRUE(sent.ok()) << static_cast<int>(sent.code()) << " " << sent.error().field;
    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 1);

    const std::array<chat::OutgoingAttachment, 1> forward{chat::OutgoingAttachment{
        .handle = {},
        .forward = chat::OutgoingAttachment::Forward{first, sent.value().seq, 0},
        .name = {}, .duration_ms = 0, .width = 0, .height = 0}};
    chat::SendMessage forwarded = text(2, "fwd");
    forwarded.attachments = forward;
    ASSERT_TRUE(service().send(db(), creator(owner), second, forwarded).ok());
    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 2)
        << "one object, two references, nothing uploaded again";
    const auto row = repo().find_message(db(), first, sent.value().seq, anvil::db::now_ms());
    EXPECT_EQ(row.value()->attachments.front().name, "contract.pdf");
    EXPECT_EQ(row.value()->attachments.front().mime, anvil::fs::Mime::Pdf)
        << "the type comes from the stored object, not from the client";

    ASSERT_TRUE(service().revoke(db(), creator(owner), first, sent.value().seq).ok());
    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 1);
}

TEST_F(ChatServiceDb, AHandleOpensOnlyForItsUploaderAndOnlyInTheKindsNamespace) {
    const Uuid owner = person();
    const Uuid a = person();
    const Uuid id = group(owner, {a});
    const auto [object, handle] = uploaded(owner);
    const std::array<chat::OutgoingAttachment, 1> theirs{chat::OutgoingAttachment{
        .handle = handle, .forward = std::nullopt, .name = {}, .duration_ms = 0, .width = 0,
        .height = 0}};
    chat::SendMessage stolen = text(1, "mine now");
    stolen.attachments = theirs;
    const auto refused = service().send(db(), plain(a), id, stolen);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().field, chat::kAttachmentsField);
    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 0);

    const auto [elsewhere, public_handle] = uploaded(owner, testapp::kMedia);
    const std::array<chat::OutgoingAttachment, 1> wrong_ns{chat::OutgoingAttachment{
        .handle = public_handle, .forward = std::nullopt, .name = {}, .duration_ms = 0,
        .width = 0, .height = 0}};
    chat::SendMessage misplaced = text(2, "x");
    misplaced.attachments = wrong_ns;
    EXPECT_EQ(service().send(db(), creator(owner), id, misplaced).error().field,
              chat::kAttachmentsField);
    (void)elsewhere;
}

TEST_F(ChatServiceDb, AForwardOfSomethingTheSenderCannotSeeIsRefused) {
    const Uuid owner = person();
    const Uuid outsider = person();
    const Uuid secret = group(owner);
    const Uuid mine = group(outsider);
    const auto [object, handle] = uploaded(owner);
    const std::array<chat::OutgoingAttachment, 1> upload{chat::OutgoingAttachment{
        .handle = handle, .forward = std::nullopt, .name = {}, .duration_ms = 0, .width = 0,
        .height = 0}};
    chat::SendMessage message = text(1, "");
    message.attachments = upload;
    const auto sent = service().send(db(), creator(owner), secret, message);
    ASSERT_TRUE(sent.ok());
    const std::array<chat::OutgoingAttachment, 1> forward{chat::OutgoingAttachment{
        .handle = {}, .forward = chat::OutgoingAttachment::Forward{secret, sent.value().seq, 0},
        .name = {}, .duration_ms = 0, .width = 0, .height = 0}};
    chat::SendMessage lifted = text(2, "");
    lifted.attachments = forward;
    EXPECT_EQ(service().send(db(), creator(outsider), mine, lifted).error().field,
              chat::kAttachmentsField);
    EXPECT_EQ(media_->find(db(), testapp::kChat, object).value()->refs, 1);
}

// --- cards --------------------------------------------------------------------------

TEST_F(ChatServiceDb, ACardIsBoundByItsKindsBinderAndStoredCanonically) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    anvil::input::BodyArena arena;
    const auto parsed = anvil::input::parse_json(
        R"({ "options" : ["yes", "no"], "question": "Lunch?" })", arena);
    ASSERT_TRUE(parsed.ok());
    chat::SendMessage poll = text(1, "");
    poll.card = chat::SendMessage::Card{"poll", &parsed.root()};
    const auto sent = service().send(db(), creator(owner), id, poll);
    ASSERT_TRUE(sent.ok()) << sent.error().field;
    const auto row = repo().find_message(db(), id, sent.value().seq, anvil::db::now_ms());
    ASSERT_TRUE(row.value()->card.has_value());
    EXPECT_EQ(row.value()->kind, chat::MessageKind::Card);
    EXPECT_EQ(row.value()->card->body, R"({"question":"Lunch?","options":["yes","no"]})")
        << "the binder's canonical form, whatever spacing and order arrived";
    // A card can be taken back like any message, and its body goes with it.
    ASSERT_TRUE(service().revoke(db(), creator(owner), id, sent.value().seq).ok());
    EXPECT_FALSE(repo().find_message(db(), id, sent.value().seq, anvil::db::now_ms())
                     .value()->card.has_value());
}

TEST_F(ChatServiceDb, ACardTheBinderRefusesNamesItsFieldAndAnUnknownKindIsRefused) {
    const Uuid owner = person();
    const Uuid id = group(owner);
    anvil::input::BodyArena arena;
    const auto one_option = anvil::input::parse_json(R"({"question":"?","options":["only"]})", arena);
    ASSERT_TRUE(one_option.ok());
    chat::SendMessage poll = text(1, "");
    poll.card = chat::SendMessage::Card{"poll", &one_option.root()};
    const auto refused = service().send(db(), creator(owner), id, poll);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().field, testapp::kPollOptions);
    EXPECT_EQ(refused.error().detail, static_cast<std::uint16_t>(Reason::TooShort));

    poll.card = chat::SendMessage::Card{"survey", &one_option.root()};
    EXPECT_EQ(service().send(db(), creator(owner), id, poll).error().field, chat::kCardField);
}

// --- channels -----------------------------------------------------------------------

TEST_F(ChatServiceDb, OnlyAChannelCanBeFollowedAndFollowingTwiceIsFollowing) {
    const Uuid owner = person();
    const auto channel = service().create(
        db(), creator(owner), chat::CreateConversation{"channel", "Updates", "", {}, false});
    ASSERT_TRUE(channel.ok());
    const chat::Actor fan = plain(person());
    ASSERT_TRUE(service().follow(db(), fan, channel.value().conversation.id).ok());
    ASSERT_TRUE(service().follow(db(), fan, channel.value().conversation.id).ok());
    // A follower cannot list who else follows.
    EXPECT_EQ(service().members(db(), fan, channel.value().conversation.id, std::nullopt, 10).code(),
              ErrorCode::Forbidden);
    // A group is never joinable by knowing its id.
    EXPECT_EQ(service().follow(db(), fan, group(owner)).code(), ErrorCode::NotFound);
}

}  // namespace
