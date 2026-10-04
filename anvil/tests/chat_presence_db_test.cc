// Presence against a live Redis and MongoDB (chat/presence.h).
//
// The tracker is driven directly, as the hub drives it: came_online and
// went_offline are exactly the two calls a first and a last socket make. Every
// wait is a bounded poll; absence is asserted only after something that the
// same pass would have done first has been seen.

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "anvil/chat/presence.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "anvil/redis/redis_client.h"
#include "app_fixture.h"
#include "db_fixture.h"

namespace {

namespace chat = anvil::chat;
using anvil::ErrorCode;
using anvil::Uuid;

constexpr std::string_view kCollection = "chat_presence";

[[nodiscard]] bool eventually(const std::function<bool()>& condition) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until) {
        if (condition()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return condition();
}

// Who may see whom, set per case. A nil viewer is the "may anybody" question.
// Every question is remembered, so a case can wait until one has been asked.
class Visibility final {
public:
    void allow(const Uuid& viewer, const Uuid& subject) {
        const std::lock_guard lock{mutex_};
        pairs_.emplace(viewer, subject);
    }
    [[nodiscard]] bool may(const Uuid& viewer, const Uuid& subject) {
        const std::lock_guard lock{mutex_};
        asked_.emplace(viewer, subject);
        return pairs_.contains({viewer, subject});
    }
    [[nodiscard]] bool asked(const Uuid& viewer, const Uuid& subject) const {
        const std::lock_guard lock{mutex_};
        return asked_.contains({viewer, subject});
    }

private:
    mutable std::mutex              mutex_;
    std::set<std::pair<Uuid, Uuid>> pairs_;
    std::set<std::pair<Uuid, Uuid>> asked_;
};

class PresenceDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        ANVIL_REQUIRE_REDIS();
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
    }

    [[nodiscard]] std::unique_ptr<chat::PresenceTracker> tracker(bool enabled = true) {
        return std::make_unique<chat::PresenceTracker>(
            chat::PresenceConfig{
                .enabled = enabled,
                .may_see = [this](mongocxx::client&, const Uuid& viewer,
                                  const Uuid& subject) { return visibility_.may(viewer, subject); },
                .collection = kCollection,
                .databases = anvil::testfixture::scratch_names(),
                .refresh = std::chrono::seconds{1},
                .online_window = std::chrono::seconds{3},
                .last_seen_write = std::chrono::seconds{300}},
            anvil::redis::RedisClient::instance());
    }

    [[nodiscard]] chat::PresenceView view(const chat::PresenceTracker& presence,
                                          const Uuid& viewer, const Uuid& subject) {
        const auto seen = presence.view(db(), viewer, subject);
        EXPECT_TRUE(seen.ok());
        return seen.ok() ? seen.value() : chat::PresenceView{std::nullopt, false};
    }

    [[nodiscard]] bool stored(const Uuid& user) {
        return db()[std::string{anvil::testfixture::scratch_names().for_collection(kCollection)}]
                   [std::string{kCollection}]
                       .find_one(bsoncxx::builder::basic::make_document(
                           bsoncxx::builder::basic::kvp("_id", anvil::db::codec::uuid_bin(user))))
                       .has_value();
    }

    [[nodiscard]] static std::string key_of(const Uuid& user) {
        std::string key{chat::kPresenceKeyPrefix};
        for (const std::uint8_t byte : user) {
            constexpr std::string_view kHex = "0123456789abcdef";
            key.push_back(kHex[byte >> 4U]);
            key.push_back(kHex[byte & 0x0FU]);
        }
        return key;
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    std::unique_ptr<mongocxx::pool::entry> client_;
    Visibility                             visibility_;
};

TEST_F(PresenceDb, OffMeansNothingIsWrittenAndNothingIsAnswered) {
    const Uuid user = anvil::uuid::generate_v4();
    auto presence = tracker(false);
    presence->came_online(user);
    // Off builds no thread at all, so there is nothing that could still write.
    EXPECT_FALSE(static_cast<bool>(anvil::redis::RedisClient::instance().get(key_of(user))));
    const auto seen = presence->view(db(), user, user);
    ASSERT_FALSE(seen.ok());
    EXPECT_EQ(seen.code(), ErrorCode::NotFound);
}

TEST_F(PresenceDb, AFirstSocketIsOnlineAtOnceAndALastOneLeavesALastSeen) {
    const Uuid user = anvil::uuid::generate_v4();
    auto presence = tracker();
    presence->came_online(user);
    EXPECT_TRUE(eventually([&] { return view(*presence, user, user).online; }));
    presence->went_offline(user);
    EXPECT_TRUE(eventually([&] { return !view(*presence, user, user).online; }));
    const chat::PresenceView left = view(*presence, user, user);
    ASSERT_TRUE(left.last_seen.has_value());
    EXPECT_LE(anvil::db::now_ms() - *left.last_seen, std::chrono::seconds{10});
}

TEST_F(PresenceDb, AViewerTheHookRefusesSeesExactlyWhatNeverSeenLooksLike) {
    const Uuid subject = anvil::uuid::generate_v4();
    const Uuid friendly = anvil::uuid::generate_v4();
    const Uuid stranger = anvil::uuid::generate_v4();
    visibility_.allow(friendly, subject);
    auto presence = tracker();
    presence->came_online(subject);
    ASSERT_TRUE(eventually([&] { return view(*presence, friendly, subject).online; }));

    const chat::PresenceView refused = view(*presence, stranger, subject);
    const chat::PresenceView never = view(*presence, friendly, anvil::uuid::generate_v4());
    EXPECT_FALSE(refused.online);
    EXPECT_FALSE(refused.last_seen.has_value());
    EXPECT_EQ(refused.online, never.online);
    EXPECT_EQ(refused.last_seen, never.last_seen);
    presence->went_offline(subject);
}

TEST_F(PresenceDb, LastSeenIsStoredOnlyForAnAccountSomebodyMaySee) {
    const Uuid hidden = anvil::uuid::generate_v4();
    const Uuid visible = anvil::uuid::generate_v4();
    // "May anybody see `visible`": the nil viewer.
    visibility_.allow(Uuid{}, visible);
    auto presence = tracker();
    presence->came_online(hidden);
    presence->came_online(visible);
    presence->went_offline(hidden);
    presence->went_offline(visible);
    // Both are due in one pass, written in the order of their ids rather than
    // the order they left. So the wait is for the whole pass: the hidden one's
    // question asked, and the visible one's write counted, which follows its
    // row by a moment.
    ASSERT_TRUE(eventually([&] {
        return visibility_.asked(Uuid{}, hidden) && stored(visible) &&
               presence->last_seen_writes() >= 1U;
    }));
    EXPECT_FALSE(stored(hidden)) << "a last seen nobody may be shown was stored";
}

TEST_F(PresenceDb, AHeartbeatOlderThanTheWindowReadsOffline) {
    const Uuid user = anvil::uuid::generate_v4();
    auto presence = tracker();
    // The key a process that died an hour ago left behind.
    const auto stale = anvil::db::now_ms() - std::chrono::hours{1};
    anvil::redis::RedisClient::instance().set(
        key_of(user), std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         stale.time_since_epoch())
                                         .count()),
        std::chrono::seconds{60});
    const chat::PresenceView seen = view(*presence, user, user);
    EXPECT_FALSE(seen.online) << "a process that died must not leave its accounts online";
    ASSERT_TRUE(seen.last_seen.has_value());
}

TEST_F(PresenceDb, AKeyThatDoesNotParseIsNoAnswer) {
    const Uuid user = anvil::uuid::generate_v4();
    auto presence = tracker();
    anvil::redis::RedisClient::instance().set(key_of(user), "not-a-time",
                                              std::chrono::seconds{60});
    const chat::PresenceView seen = view(*presence, user, user);
    EXPECT_FALSE(seen.online);
    EXPECT_FALSE(seen.last_seen.has_value());
}

TEST_F(PresenceDb, TurningItOnWithAnUndeclaredCollectionFailsAtBoot) {
    EXPECT_THROW((chat::PresenceTracker{
                     chat::PresenceConfig{.enabled = true,
                                          .may_see = {},
                                          .collection = "no_such_collection",
                                          .databases = anvil::testfixture::scratch_names()},
                     anvil::redis::RedisClient::instance()}),
                 std::invalid_argument);
}

}  // namespace
