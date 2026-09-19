// The identity layer against a live cluster: the user schema, the login
// projections, the session lifecycle, and refresh rotation.
//
// The assertions that matter here are the ones about what is INDISTINGUISHABLE.
// A disabled account, a locked one and a wrong password must produce the same
// outcome; a rotated credential replayed after its grace window must be detected
// rather than merely refused. Neither is observable from a unit test, because
// both are about what the database does under concurrency.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"
#include "anvil/identity/sessions.h"
#include "anvil/identity/users.h"
#include "app_fixture.h"
#include "db_fixture.h"

namespace {

using anvil::Uuid;
using anvil::UserStatus;
using anvil::UserType;
using anvil::identity::LoginIdentity;
using anvil::identity::NewSession;
using anvil::identity::NewUser;
using anvil::identity::PackedIp;
using anvil::identity::RefreshMatch;
using anvil::identity::SessionRepository;
using anvil::identity::UserAgentHash;
using anvil::identity::UserRepository;
using anvil::testfixture::scratch_names;

constexpr std::string_view kUsers = "users";
constexpr std::string_view kSessions = "user_sessions";

class AuthDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kUsers);
        anvil::testfixture::clear_collection(**client_, kSessions);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static UserRepository users() {
        return UserRepository{std::string{scratch_names().for_collection(kUsers)}, kUsers};
    }
    [[nodiscard]] static SessionRepository sessions() {
        return SessionRepository{std::string{scratch_names().for_collection(kSessions)},
                                 kSessions};
    }

    // One account, created through the repository so the schema under test is
    // the schema being asserted about.
    // NOT [[nodiscard]]: several cases create an account only so a lookup has
    // something to find, and have no use for its id.
    Uuid create_account(std::string_view local, UserStatus status,
                        std::string_view phone = {}) {
        const Uuid id = anvil::uuid::generate_v7();
        const std::string email = std::string{local} + "@example.test";
        const std::string username = std::string{local};
        const NewUser user{
            .id = id,
            .email_normalised = email,
            .email_display = email,
            .username_normalised = username,
            .username_display = username,
            .password_hash = "$argon2id$v=19$m=65536,t=3,p=4$c2FsdA$aGFzaA",
            .phone_e164 = phone,
            .locale = anvil::Locale{},
            .status = status,
        };
        EXPECT_TRUE(users().insert(db(), user).ok());
        return id;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

[[nodiscard]] PackedIp some_ip() noexcept {
    PackedIp ip{};
    ip[10] = 0xFF;
    ip[11] = 0xFF;
    ip[12] = 203;
    ip[14] = 113;
    ip[15] = 7;
    return ip;
}

[[nodiscard]] anvil::crypto::Digest256 some_digest(std::uint8_t byte) noexcept {
    anvil::crypto::Digest256 digest{};
    digest[0] = byte;
    return digest;
}

// --- the user schema --------------------------------------------------------

TEST_F(AuthDb, TheLoginProjectionCarriesTheHashAndTheAuthorityAndNothingElse) {
    const Uuid id = create_account("alice", UserStatus::Active);

    const auto found = users().find_for_login(db(), "alice@example.test", LoginIdentity::Email);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());

    const auto& record = *found.value();
    EXPECT_EQ(record.id, id);
    EXPECT_FALSE(record.password_hash.empty());
    EXPECT_EQ(record.status, UserStatus::Active);
    EXPECT_EQ(record.user_type, UserType::Client);
    // Epoch starts at 1, not 0, so "never bumped" and "no epoch recorded" stay
    // distinguishable in a token.
    EXPECT_EQ(record.perm_epoch, 1);
    EXPECT_EQ(record.failure_count, 0);
    EXPECT_FALSE(record.lock_until.has_value());
}

TEST_F(AuthDb, EachLoginIdentityResolvesThroughItsOwnField) {
    create_account("bob", UserStatus::Active, "+201000000001");

    for (const auto& [value, kind] :
         std::vector<std::pair<std::string, LoginIdentity>>{
             {"bob@example.test", LoginIdentity::Email},
             {"bob", LoginIdentity::Username},
             {"+201000000001", LoginIdentity::Phone}}) {
        const auto found = users().find_for_login(db(), value, kind);
        ASSERT_TRUE(found.ok()) << value;
        EXPECT_TRUE(found.value().has_value()) << value;
    }
}

TEST_F(AuthDb, AnIdentifierResolvedAgainstTheWrongFieldMatchesNothing) {
    // The branch is chosen from the identifier's SHAPE before the query is
    // issued. Getting it wrong must return nothing rather than quietly falling
    // back to a scan across all three.
    create_account("carol", UserStatus::Active);
    const auto found = users().find_for_login(db(), "carol@example.test",
                                              LoginIdentity::Username);
    ASSERT_TRUE(found.ok());
    EXPECT_FALSE(found.value().has_value());
}

TEST_F(AuthDb, EveryNonActiveStatusIsReadBackExactlyAsStored) {
    // The login path must treat all three identically, and it can only do that
    // if the decode never coerces one into another.
    const Uuid disabled = create_account("d1", UserStatus::Disabled);
    const Uuid locked = create_account("d2", UserStatus::Locked);
    const Uuid pending = create_account("d3", UserStatus::PendingVerification);

    for (const auto& [id, expected] : std::vector<std::pair<Uuid, UserStatus>>{
             {disabled, UserStatus::Disabled},
             {locked, UserStatus::Locked},
             {pending, UserStatus::PendingVerification}}) {
        const auto found = users().find_permissions(db(), id);
        ASSERT_TRUE(found.ok());
        ASSERT_TRUE(found.value().has_value());
        EXPECT_EQ(found.value()->status, expected);
    }
}

TEST_F(AuthDb, ADuplicateIdentityIsAConflictAndNotASecondRow) {
    create_account("dup", UserStatus::Active);

    const NewUser again{
        .id = anvil::uuid::generate_v7(),
        .email_normalised = "dup@example.test",
        .email_display = "dup@example.test",
        .username_normalised = "someone-else",
        .username_display = "someone-else",
        .password_hash = "$argon2id$v=19$m=65536,t=3,p=4$c2FsdA$aGFzaA",
        .phone_e164 = {},
        .locale = anvil::Locale{},
        .status = UserStatus::Active,
    };
    const anvil::Status inserted = users().insert(db(), again);
    // The SERVER rejects it, through a unique index. A check-then-insert in code
    // loses to two concurrent registrations; an index does not.
    ASSERT_FALSE(inserted.ok());
    EXPECT_EQ(inserted.error().code, anvil::ErrorCode::Conflict);
}

TEST_F(AuthDb, SeveralAccountsMayHaveNoPhoneAtAll) {
    // A unique index treats a missing field as null, so without the partial
    // filter the SECOND phoneless account would be rejected — which is every
    // account, for a deployment that does not collect phone numbers.
    create_account("p1", UserStatus::Active);
    create_account("p2", UserStatus::Active);
    create_account("p3", UserStatus::Active);

    const auto found = users().find_for_login(db(), "p3@example.test", LoginIdentity::Email);
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(found.value().has_value());
}

TEST_F(AuthDb, LoginFailuresAccumulateAndClear) {
    const Uuid id = create_account("failing", UserStatus::Active);

    const auto first = users().record_login_failure(db(), id, std::nullopt);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 1);

    const auto second = users().record_login_failure(db(), id, std::nullopt);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value(), 2);

    ASSERT_TRUE(users().clear_login_failures(db(), id).ok());
    const auto after = users().find_for_login(db(), "failing@example.test",
                                              LoginIdentity::Email);
    ASSERT_TRUE(after.ok());
    ASSERT_TRUE(after.value().has_value());
    EXPECT_EQ(after.value()->failure_count, 0);
    EXPECT_FALSE(after.value()->lock_until.has_value());
}

TEST_F(AuthDb, RecordingAFailureForNoSuchAccountIsNotAnError) {
    // The caller reached this because a credential did not match, and whether
    // the account exists is precisely what it must not learn — including by
    // observing an error it would not otherwise get.
    const auto result = users().record_login_failure(db(), anvil::uuid::generate_v7(),
                                                     std::nullopt);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 0);
}

TEST_F(AuthDb, AnEpochBumpIsAtomicAndMonotonic) {
    const Uuid id = create_account("bumped", UserStatus::Active);

    std::int64_t previous = 1;
    for (int i = 0; i < 5; ++i) {
        const auto bumped = users().bump_perm_epoch(db(), id);
        ASSERT_TRUE(bumped.ok());
        EXPECT_GT(bumped.value(), previous);
        previous = bumped.value();
    }
}

TEST_F(AuthDb, BumpingTheEpochOfNoSuchAccountIsNotFound) {
    const auto bumped = users().bump_perm_epoch(db(), anvil::uuid::generate_v7());
    ASSERT_FALSE(bumped.ok());
    EXPECT_EQ(bumped.error().code, anvil::ErrorCode::NotFound);
}

TEST_F(AuthDb, ARehashOnlyLandsWhenTheStoredHashIsStillTheOneVerified) {
    const Uuid id = create_account("rehash", UserStatus::Active);
    const auto before = users().find_for_login(db(), "rehash@example.test",
                                               LoginIdentity::Email);
    ASSERT_TRUE(before.ok());
    const std::string original = before.value()->password_hash;

    // A password change lands between the verify and the rehash.
    ASSERT_TRUE(users().set_password_hash(db(), id, 1, "$argon2id$NEWER").ok());

    // The background rehash carries the hash it verified. It must match nothing.
    ASSERT_TRUE(users().replace_password_hash(db(), id, original, "$argon2id$STALE").ok());

    const auto after = users().find_for_login(db(), "rehash@example.test",
                                              LoginIdentity::Email);
    ASSERT_TRUE(after.ok());
    // The newer credential survives. Without the compare-and-swap the rehash
    // would resurrect the password the person just replaced.
    EXPECT_EQ(after.value()->password_hash, "$argon2id$NEWER");
}

TEST_F(AuthDb, AVersionedWriteRefusesAStaleVersion) {
    const Uuid id = create_account("versioned", UserStatus::Active);

    const auto first = users().set_permissions(db(), id, 1, anvil::PermSet{}, anvil::PermSet{});
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 2);

    // The same version again: a second editor holding the row as it was.
    const auto stale = users().set_permissions(db(), id, 1, anvil::PermSet{}, anvil::PermSet{});
    ASSERT_FALSE(stale.ok());
    EXPECT_EQ(stale.error().code, anvil::ErrorCode::VersionMismatch);
}

TEST_F(AuthDb, AStaleVersionAndAMissingRowAreIndistinguishable) {
    // Telling a caller "your version is stale" versus "it is gone" on an
    // administrative route is an existence oracle, and the remedy for both is
    // the same: re-read and retry.
    const auto gone = users().set_permissions(db(), anvil::uuid::generate_v7(), 1,
                                              anvil::PermSet{}, anvil::PermSet{});
    ASSERT_FALSE(gone.ok());
    EXPECT_EQ(gone.error().code, anvil::ErrorCode::VersionMismatch);
}

TEST_F(AuthDb, TheSweepTakesOnlyUnverifiedAccountsOlderThanTheWatermark) {
    create_account("active", UserStatus::Active);
    const Uuid pending = create_account("pending", UserStatus::PendingVerification);

    const anvil::db::TimeMs future = anvil::db::now_ms() + std::chrono::hours{1};
    const auto first = users().delete_one_stale_pending(db(), future);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value().has_value());
    EXPECT_EQ(*first.value(), pending);

    // The active account is left alone, and a second pass finds nothing.
    const auto second = users().delete_one_stale_pending(db(), future);
    ASSERT_TRUE(second.ok());
    EXPECT_FALSE(second.value().has_value());
}

TEST_F(AuthDb, NamesOfIssuesNoQueryForAnEmptyInputAndOmitsUnknownIds) {
    const Uuid known = create_account("named", UserStatus::Active);

    const auto none = users().names_of(db(), {});
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());

    const std::array<Uuid, 2> ids{known, anvil::uuid::generate_v7()};
    const auto some = users().names_of(db(), ids);
    ASSERT_TRUE(some.ok());
    // An id naming no account is ABSENT rather than an error: a log outlives the
    // accounts it names.
    ASSERT_EQ(some.value().size(), 1U);
    EXPECT_EQ(some.value()[0].id, known);
    EXPECT_EQ(some.value()[0].name, "named");
}

// --- sessions ---------------------------------------------------------------

TEST_F(AuthDb, ASessionIsFoundByItsRefreshHashAndNotByAnythingElse) {
    const Uuid user = create_account("sess", UserStatus::Active);
    const Uuid session_id = anvil::uuid::generate_v7();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const NewSession session{
        .id = session_id,
        .user_id = user,
        .refresh_hash = some_digest(0x11),
        .now = now,
        .expires_at = now + std::chrono::hours{24},
        .abs_expiry = now + std::chrono::hours{72},
        .ip = some_ip(),
        .user_agent_hash = UserAgentHash{},
        .user_type = UserType::Client,
    };
    ASSERT_TRUE(sessions().insert(db(), session).ok());

    const auto found = sessions().find_by_refresh_hash(db(), some_digest(0x11), now);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->match, RefreshMatch::Current);
    EXPECT_EQ(found.value()->session.id, session_id);

    const auto missing = sessions().find_by_refresh_hash(db(), some_digest(0x22), now);
    ASSERT_TRUE(missing.ok());
    EXPECT_FALSE(missing.value().has_value());
}

TEST_F(AuthDb, AnExpiredSessionIsUnfindableEvenBeforeTheTtlMonitorReachesIt) {
    // The whole reason every query carries an explicit expiry predicate: the
    // monitor runs roughly every 60 seconds, so an expired row is still a
    // document and would still authenticate.
    const Uuid user = create_account("expired", UserStatus::Active);
    const anvil::db::TimeMs now = anvil::db::now_ms();
    const anvil::db::TimeMs past = now - std::chrono::hours{2};

    const NewSession session{
        .id = anvil::uuid::generate_v7(),
        .user_id = user,
        .refresh_hash = some_digest(0x33),
        .now = past,
        .expires_at = past + std::chrono::minutes{1},   // already over
        .abs_expiry = now + std::chrono::hours{72},
        .ip = some_ip(),
        .user_agent_hash = UserAgentHash{},
        .user_type = UserType::Client,
    };
    ASSERT_TRUE(sessions().insert(db(), session).ok());

    const auto found = sessions().find_by_refresh_hash(db(), some_digest(0x33), now);
    ASSERT_TRUE(found.ok());
    EXPECT_FALSE(found.value().has_value());
}

TEST_F(AuthDb, RotationIsACompareAndSwapSoOnlyOneOfTwoTabsWins) {
    const Uuid user = create_account("rotate", UserStatus::Active);
    const Uuid session_id = anvil::uuid::generate_v7();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const NewSession session{
        .id = session_id,
        .user_id = user,
        .refresh_hash = some_digest(0x44),
        .now = now,
        .expires_at = now + std::chrono::hours{24},
        .abs_expiry = now + std::chrono::hours{72},
        .ip = some_ip(),
        .user_agent_hash = UserAgentHash{},
        .user_type = UserType::Client,
    };
    ASSERT_TRUE(sessions().insert(db(), session).ok());

    const auto winner = sessions().rotate_refresh_hash(
        db(), session_id, some_digest(0x44), some_digest(0x55),
        now + std::chrono::minutes{1}, now + std::chrono::hours{48}, now);
    ASSERT_TRUE(winner.ok());
    EXPECT_TRUE(winner.value());

    // The loser presents the hash the winner just rotated away. It matches
    // nothing, which is NOT an error — the loser keeps the token it holds and
    // reads the winner's grace window.
    const auto loser = sessions().rotate_refresh_hash(
        db(), session_id, some_digest(0x44), some_digest(0x66),
        now + std::chrono::minutes{1}, now + std::chrono::hours{48}, now);
    ASSERT_TRUE(loser.ok());
    EXPECT_FALSE(loser.value());
}

TEST_F(AuthDb, ThePreviousHashIsAGraceWindowInsideItAndAReplayOutsideIt) {
    const Uuid user = create_account("replay", UserStatus::Active);
    const Uuid session_id = anvil::uuid::generate_v7();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const NewSession session{
        .id = session_id,
        .user_id = user,
        .refresh_hash = some_digest(0x77),
        .now = now,
        .expires_at = now + std::chrono::hours{24},
        .abs_expiry = now + std::chrono::hours{72},
        .ip = some_ip(),
        .user_agent_hash = UserAgentHash{},
        .user_type = UserType::Client,
    };
    ASSERT_TRUE(sessions().insert(db(), session).ok());

    const anvil::db::TimeMs grace_ends = now + std::chrono::seconds{60};
    ASSERT_TRUE(sessions()
                    .rotate_refresh_hash(db(), session_id, some_digest(0x77),
                                         some_digest(0x88), grace_ends,
                                         now + std::chrono::hours{48}, now)
                    .ok());

    // Inside the window: a concurrent tab.
    const auto inside = sessions().find_by_refresh_hash(db(), some_digest(0x77), now);
    ASSERT_TRUE(inside.ok());
    ASSERT_TRUE(inside.value().has_value());
    EXPECT_EQ(inside.value()->match, RefreshMatch::PreviousGrace);

    // Outside it: a rotated credential was replayed, which means it leaked. The
    // lookup must SAY so rather than matching nothing — matching nothing would
    // report the single highest-signal event in the system as an ordinary
    // expired login.
    const auto outside =
        sessions().find_by_refresh_hash(db(), some_digest(0x77),
                                        grace_ends + std::chrono::seconds{1});
    ASSERT_TRUE(outside.ok());
    ASSERT_TRUE(outside.value().has_value());
    EXPECT_EQ(outside.value()->match, RefreshMatch::PreviousStale);
}

TEST_F(AuthDb, RevokingIsScopedToTheOwner) {
    const Uuid owner = create_account("owner", UserStatus::Active);
    const Uuid stranger = create_account("stranger", UserStatus::Active);
    const Uuid session_id = anvil::uuid::generate_v7();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    ASSERT_TRUE(sessions()
                    .insert(db(), NewSession{.id = session_id,
                                             .user_id = owner,
                                             .refresh_hash = some_digest(0x99),
                                             .now = now,
                                             .expires_at = now + std::chrono::hours{24},
                                             .abs_expiry = now + std::chrono::hours{72},
                                             .ip = some_ip(),
                                             .user_agent_hash = UserAgentHash{},
                                             .user_type = UserType::Client})
                    .ok());

    // A session id is not a secret — it travels in every access token — so
    // without the owner in the filter, presenting one would revoke somebody
    // else's session.
    ASSERT_TRUE(sessions().revoke(db(), session_id, stranger).ok());
    const auto still_live = sessions().find_by_id(db(), session_id, now);
    ASSERT_TRUE(still_live.ok());
    EXPECT_TRUE(still_live.value().has_value());

    ASSERT_TRUE(sessions().revoke(db(), session_id, owner).ok());
    const auto revoked = sessions().find_by_id(db(), session_id, now);
    ASSERT_TRUE(revoked.ok());
    EXPECT_FALSE(revoked.value().has_value());
}

TEST_F(AuthDb, RevokeOthersKeepsTheCallersOwnSession) {
    const Uuid user = create_account("keeper", UserStatus::Active);
    const anvil::db::TimeMs now = anvil::db::now_ms();

    std::vector<Uuid> ids;
    for (std::uint8_t i = 0; i < 3; ++i) {
        const Uuid id = anvil::uuid::generate_v7();
        ids.push_back(id);
        ASSERT_TRUE(sessions()
                        .insert(db(), NewSession{.id = id,
                                                 .user_id = user,
                                                 .refresh_hash = some_digest(
                                                     static_cast<std::uint8_t>(0xA0 + i)),
                                                 .now = now,
                                                 .expires_at = now + std::chrono::hours{24},
                                                 .abs_expiry = now + std::chrono::hours{72},
                                                 .ip = some_ip(),
                                                 .user_agent_hash = UserAgentHash{},
                                                 .user_type = UserType::Client})
                        .ok());
    }

    const auto revoked = sessions().revoke_all_except(db(), user, ids[1]);
    ASSERT_TRUE(revoked.ok());
    EXPECT_EQ(revoked.value(), 2);

    const auto live = sessions().list_for_user(db(), user, now, 10);
    ASSERT_TRUE(live.ok());
    ASSERT_EQ(live.value().size(), 1U);
    // Signing somebody out of the browser they are changing their password in is
    // the one outcome nobody wants from a password change.
    EXPECT_EQ(live.value()[0].id, ids[1]);
}

TEST_F(AuthDb, DeviceCountsForSeveralUsersComeBackInOneRoundTrip) {
    const Uuid one = create_account("dev1", UserStatus::Active);
    const Uuid two = create_account("dev2", UserStatus::Active);
    const Uuid none = create_account("dev3", UserStatus::Active);
    const anvil::db::TimeMs now = anvil::db::now_ms();

    std::uint8_t seed = 0xB0;
    const auto add_session = [&](const Uuid& user) {
        ASSERT_TRUE(sessions()
                        .insert(db(), NewSession{.id = anvil::uuid::generate_v7(),
                                                 .user_id = user,
                                                 .refresh_hash = some_digest(seed++),
                                                 .now = now,
                                                 .expires_at = now + std::chrono::hours{24},
                                                 .abs_expiry = now + std::chrono::hours{72},
                                                 .ip = some_ip(),
                                                 .user_agent_hash = UserAgentHash{},
                                                 .user_type = UserType::Client})
                        .ok());
    };
    add_session(one);
    add_session(two);
    add_session(two);

    const std::array<Uuid, 3> ids{one, two, none};
    const auto counts = sessions().count_live_for_users(db(), ids, now);
    ASSERT_TRUE(counts.ok());

    // A user with no live session is ABSENT rather than present with zero: the
    // aggregation has nothing to group for them, and inventing a row would mean
    // the caller could not tell "none" from "not asked".
    ASSERT_EQ(counts.value().size(), 2U);
    std::int32_t for_two = 0;
    for (const auto& row : counts.value()) {
        EXPECT_NE(row.user_id, none);
        if (row.user_id == two) { for_two = row.live; }
    }
    EXPECT_EQ(for_two, 2);
}

TEST_F(AuthDb, OverflowSessionsReportsTheOldestBeyondTheCap) {
    const Uuid user = create_account("capped", UserStatus::Active);
    const anvil::db::TimeMs now = anvil::db::now_ms();

    std::vector<Uuid> ordered;
    for (std::uint8_t i = 0; i < 5; ++i) {
        const Uuid id = anvil::uuid::generate_v7();
        ordered.push_back(id);
        ASSERT_TRUE(sessions()
                        .insert(db(), NewSession{
                                          .id = id,
                                          .user_id = user,
                                          .refresh_hash =
                                              some_digest(static_cast<std::uint8_t>(0xC0 + i)),
                                          // Ascending last_seen, so "oldest" is
                                          // unambiguous.
                                          .now = now + std::chrono::minutes{i},
                                          .expires_at = now + std::chrono::hours{24},
                                          .abs_expiry = now + std::chrono::hours{72},
                                          .ip = some_ip(),
                                          .user_agent_hash = UserAgentHash{},
                                          .user_type = UserType::Client})
                        .ok());
    }

    const auto overflow = sessions().overflow_sessions(db(), user, now, 3);
    ASSERT_TRUE(overflow.ok());
    ASSERT_EQ(overflow.value().size(), 2U);
    EXPECT_EQ(overflow.value()[0], ordered[0]);
    EXPECT_EQ(overflow.value()[1], ordered[1]);

    // Under the cap there is nothing to evict.
    const auto none = sessions().overflow_sessions(db(), user, now, 10);
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());
}

}  // namespace
