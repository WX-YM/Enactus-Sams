// The session service end to end: minting, the rotation budget, the two-tab
// race, replay detection, and what a revocation actually costs.
//
// These are the cases that cannot be written against the repository alone,
// because what they assert is a POLICY applied across two collections and a
// cache: a replay revokes the session AND bumps the epoch, a revocation ends the
// outstanding access token rather than waiting it out, and a rotation that loses
// its compare-and-swap is not an error.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include "anvil/auth/token.h"
#include "anvil/core/uuid.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/password_service.h"
#include "anvil/identity/prehash_service.h"
#include "anvil/identity/session_service.h"
#include "anvil/identity/users.h"
#include "app_fixture.h"
#include "db_fixture.h"

namespace {

using anvil::Uuid;
using anvil::UserStatus;
using anvil::UserType;
using anvil::identity::AuthzService;
using anvil::identity::IssuedSession;
using anvil::identity::NewUser;
using anvil::identity::PasswordService;
using anvil::identity::SessionPolicy;
using anvil::identity::SessionService;
using anvil::identity::UserAuthRecord;
using anvil::identity::UserRepository;
using anvil::identity::kRefreshReplayDetected;
using anvil::identity::pack_ip;
using anvil::testfixture::scratch_names;

constexpr std::string_view kUsers = "users";
constexpr std::string_view kSessions = "user_sessions";
constexpr std::string_view kUserAgent = "Mozilla/5.0 (test)";

// A 32-byte pepper and a 32-byte signing key. Fixed rather than random so a
// failure is reproducible.
constexpr std::array<std::uint8_t, 32> kPepper{0x11, 0x22, 0x33};
constexpr std::array<std::uint8_t, 32> kSigningKey{0x44, 0x55, 0x66};

class AuthService : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        // Every revocation path bumps the epoch, and that goes through the Redis
        // mirror. Without it these cases would assert half a mechanism.
        ANVIL_REQUIRE_REDIS();
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

    [[nodiscard]] UserAuthRecord create(std::string_view local, UserType type,
                                        UserStatus status = UserStatus::Active) {
        const Uuid id = anvil::uuid::generate_v7();
        const std::string email = std::string{local} + "@example.test";
        const std::string username{local};
        EXPECT_TRUE(users()
                        .insert(db(), NewUser{.id = id,
                                              .email_normalised = email,
                                              .email_display = email,
                                              .username_normalised = username,
                                              .username_display = username,
                                              .password_hash = "$argon2id$x",
                                              .phone_e164 = {},
                                              .locale = anvil::Locale{},
                                              .status = status})
                        .ok());
        if (type != UserType::Client) {
            auto session = db().start_session();
            session.start_transaction();
            EXPECT_TRUE(users()
                            .set_user_type(db(), session, id, 1, type, anvil::PermSet{},
                                           anvil::PermSet{})
                            .ok());
            session.commit_transaction();
        }
        const auto found = users().find_for_login(
            db(), email, anvil::identity::LoginIdentity::Email);
        EXPECT_TRUE(found.ok());
        EXPECT_TRUE(found.value().has_value());
        return *found.value();
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

[[nodiscard]] AuthzService& authz() {
    static AuthzService service{std::string{scratch_names().for_collection(kUsers)}, kUsers};
    return service;
}

[[nodiscard]] std::shared_ptr<const anvil::auth::TokenKeys> keys() {
    static const auto shared =
        std::make_shared<const anvil::auth::TokenKeys>(std::uint8_t{1}, kSigningKey);
    return shared;
}

[[nodiscard]] SessionService make_service(SessionPolicy policy = {},
                                          anvil::identity::SessionsRevoked on_revoked = {}) {
    return SessionService{std::string{scratch_names().for_collection(kUsers)},
                          kSessions,
                          kUsers,
                          kPepper,
                          keys(),
                          authz(),
                          policy,
                          std::move(on_revoked)};
}

// What a revocation hook was told, one entry per call.
struct Revocation final {
    Uuid              user;
    std::vector<Uuid> sessions;
};

[[nodiscard]] std::vector<Uuid> sorted(std::vector<Uuid> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

// --- minting ----------------------------------------------------------------

TEST_F(AuthService, CreatingASessionMintsBothCredentials) {
    const UserAuthRecord user = create("mint", UserType::Client);
    const SessionService service = make_service();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.5"), kUserAgent,
                                       anvil::db::now_ms());
    ASSERT_TRUE(issued.ok());

    EXPECT_FALSE(issued.value().access_token.empty());
    EXPECT_EQ(issued.value().refresh_token.size(), anvil::identity::kRefreshTokenLength);
    EXPECT_GT(issued.value().access_expires_in_seconds, 0);
    EXPECT_GT(issued.value().refresh_expires_in_seconds,
              issued.value().access_expires_in_seconds);
}

TEST_F(AuthService, TheAccessTokenCarriesTheAuthorityTheFilterWillRead) {
    const UserAuthRecord user = create("claims", UserType::Staff);
    const SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.6"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());

    const anvil::auth::TokenResult decoded = anvil::auth::decode(
        issued.value().access_token, *keys(),
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.claims.user_id, user.id);
    EXPECT_EQ(decoded.claims.session_id, issued.value().session_id);
    EXPECT_EQ(decoded.claims.user_type, UserType::Staff);
    // The epoch the token was minted with. The filter compares it against the
    // authority, and a mismatch denies immediately.
    EXPECT_EQ(decoded.claims.perm_epoch, static_cast<std::uint64_t>(user.perm_epoch));
}

TEST_F(AuthService, APrivilegedSessionGetsAShorterAccessLifetime) {
    const SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const UserAuthRecord client_user = create("short-client", UserType::Client);
    const UserAuthRecord staff_user = create("short-staff", UserType::Staff);

    const auto as_client =
        service.create(db(), client_user, pack_ip("203.0.113.7"), kUserAgent, now);
    const auto as_staff =
        service.create(db(), staff_user, pack_ip("203.0.113.7"), kUserAgent, now);
    ASSERT_TRUE(as_client.ok());
    ASSERT_TRUE(as_staff.ok());

    EXPECT_LT(as_staff.value().access_expires_in_seconds,
              as_client.value().access_expires_in_seconds);
    // Privileged sessions do not slide either: the refresh window equals the
    // absolute cap, so at the interval they re-authenticate.
    EXPECT_LT(as_staff.value().refresh_expires_in_seconds,
              as_client.value().refresh_expires_in_seconds);
}

TEST_F(AuthService, ANewDeviceIsReportedOnceAndNotOnEverySignIn) {
    const UserAuthRecord user = create("device", UserType::Client);
    const SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto first = service.create(db(), user, pack_ip("203.0.113.8"), kUserAgent, now);
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value().from_new_device);
    EXPECT_FALSE(first.value().new_device_label.empty());

    const auto second = service.create(db(), user, pack_ip("203.0.113.8"), kUserAgent, now);
    ASSERT_TRUE(second.ok());
    // The device already has a live session. Re-alerting would be a stream of
    // alerts about nothing, which is how a security notification stops being
    // read.
    EXPECT_FALSE(second.value().from_new_device);
    EXPECT_TRUE(second.value().new_device_label.empty());

    const auto elsewhere =
        service.create(db(), user, pack_ip("203.0.113.8"), "a different browser", now);
    ASSERT_TRUE(elsewhere.ok());
    EXPECT_TRUE(elsewhere.value().from_new_device);
}

TEST_F(AuthService, TheConcurrentSessionCapEvictsInTheRequestThatCausedIt) {
    const UserAuthRecord user = create("capped", UserType::Client);
    SessionPolicy policy{};
    policy.max_concurrent_sessions = 3;
    const SessionService service = make_service(policy);
    const anvil::db::TimeMs now = anvil::db::now_ms();

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(
            service
                .create(db(), user, pack_ip("203.0.113.9"),
                        "browser " + std::to_string(i), now + std::chrono::seconds{i})
                .ok());
    }

    const auto live = service.list(db(), user.id, Uuid{}, now + std::chrono::seconds{10});
    ASSERT_TRUE(live.ok());
    // Unbounded session rows are unbounded write amplification and a storage
    // leak an attacker controls.
    EXPECT_LE(live.value().size(), static_cast<std::size_t>(policy.max_concurrent_sessions));
}

// --- refresh ----------------------------------------------------------------

TEST_F(AuthService, ARefreshInsideTheBudgetMintsNoNewRefreshToken) {
    const UserAuthRecord user = create("budget", UserType::Client);
    const SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.10"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());

    SessionService refresher = make_service();
    const auto refreshed =
        refresher.refresh(db(), issued.value().refresh_token, now + std::chrono::minutes{5});
    ASSERT_TRUE(refreshed.ok());

    EXPECT_FALSE(refreshed.value().access_token.empty());
    // Empty means NOT rotated, which is the common case: between rotations the
    // client keeps the token it holds and no session write occurs at all.
    EXPECT_TRUE(refreshed.value().refresh_token.empty());
}

TEST_F(AuthService, ARefreshPastTheBudgetRotates) {
    const UserAuthRecord user = create("rotates", UserType::Client);
    SessionPolicy policy{};
    policy.rotation_interval = std::chrono::seconds{1};
    const SessionService service = make_service(policy);
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.11"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());

    SessionService refresher = make_service(policy);
    const auto refreshed =
        refresher.refresh(db(), issued.value().refresh_token, now + std::chrono::seconds{5});
    ASSERT_TRUE(refreshed.ok());
    EXPECT_EQ(refreshed.value().refresh_token.size(), anvil::identity::kRefreshTokenLength);
    EXPECT_NE(refreshed.value().refresh_token, issued.value().refresh_token);
}

TEST_F(AuthService, TwoTabsRefreshingAtOnceBothSucceed) {
    // The loser presents a token the winner just rotated away. Inside the grace
    // window that is a RACE, not a replay, and signing the second tab out is the
    // failure this window exists to prevent.
    const UserAuthRecord user = create("tabs", UserType::Client);
    SessionPolicy policy{};
    policy.rotation_interval = std::chrono::seconds{1};
    policy.rotation_grace = std::chrono::seconds{60};
    const SessionService service = make_service(policy);
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.12"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());
    const std::string original = issued.value().refresh_token;

    SessionService first = make_service(policy);
    const auto winner = first.refresh(db(), original, now + std::chrono::seconds{5});
    ASSERT_TRUE(winner.ok());
    ASSERT_FALSE(winner.value().refresh_token.empty());

    SessionService second = make_service(policy);
    const auto loser = second.refresh(db(), original, now + std::chrono::seconds{6});
    ASSERT_TRUE(loser.ok()) << "the losing tab must not be signed out";
    EXPECT_FALSE(loser.value().access_token.empty());
}

TEST_F(AuthService, AReplayedTokenRevokesTheSessionAndBumpsTheEpoch) {
    // The single highest-signal event the identity layer produces: an old
    // refresh token presented after its grace window closed means a rotated
    // credential was replayed, which means it leaked.
    const UserAuthRecord user = create("replayed", UserType::Client);
    SessionPolicy policy{};
    policy.rotation_interval = std::chrono::seconds{1};
    policy.rotation_grace = std::chrono::seconds{1};
    const SessionService service = make_service(policy);
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.13"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());
    const std::string original = issued.value().refresh_token;

    SessionService rotator = make_service(policy);
    const auto rotated = rotator.refresh(db(), original, now + std::chrono::seconds{5});
    ASSERT_TRUE(rotated.ok());
    ASSERT_FALSE(rotated.value().refresh_token.empty());

    const auto epoch_before = users().find_permissions(db(), user.id);
    ASSERT_TRUE(epoch_before.ok());

    SessionService replayer = make_service(policy);
    const auto replayed = replayer.refresh(db(), original, now + std::chrono::seconds{60});
    ASSERT_FALSE(replayed.ok());
    EXPECT_EQ(replayed.error().code, anvil::ErrorCode::Unauthenticated);
    // Named, so the caller can write the high-severity audit row rather than
    // logging it as an ordinary expired login.
    EXPECT_EQ(replayed.error().field, kRefreshReplayDetected);

    // The whole session is gone, and the epoch bump kills its outstanding access
    // token too rather than waiting out the access lifetime.
    const auto epoch_after = users().find_permissions(db(), user.id);
    ASSERT_TRUE(epoch_after.ok());
    EXPECT_GT(epoch_after.value()->perm_epoch, epoch_before.value()->perm_epoch);

    SessionService after = make_service(policy);
    const auto rotated_token_now =
        after.refresh(db(), rotated.value().refresh_token, now + std::chrono::seconds{61});
    ASSERT_FALSE(rotated_token_now.ok()) << "the replay must revoke the whole session";
}

TEST_F(AuthService, ARefreshAgainstADisabledAccountRevokesRatherThanRenewing) {
    const UserAuthRecord user = create("disabled-later", UserType::Client);
    const SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.14"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());

    ANVIL_REQUIRE_TRANSACTIONS();
    auto session = db().start_session();
    session.start_transaction();
    ASSERT_TRUE(users().set_status(db(), session, user.id, UserStatus::Disabled, now).ok());
    session.commit_transaction();

    SessionService refresher = make_service();
    const auto refreshed =
        refresher.refresh(db(), issued.value().refresh_token, now + std::chrono::minutes{1});
    ASSERT_FALSE(refreshed.ok());
    EXPECT_EQ(refreshed.error().code, anvil::ErrorCode::Unauthenticated);
}

TEST_F(AuthService, AMalformedRefreshTokenIsRejectedOnLengthBeforeAnythingHashesIt) {
    SessionService service = make_service();
    for (const std::string& candidate :
         {std::string{}, std::string{"short"}, std::string(4096, 'A')}) {
        const auto refreshed = service.refresh(db(), candidate, anvil::db::now_ms());
        ASSERT_FALSE(refreshed.ok());
        EXPECT_EQ(refreshed.error().code, anvil::ErrorCode::Unauthenticated);
    }
}

TEST_F(AuthService, AnUnknownRefreshTokenFailsWithoutRevealingWhetherItEverExisted) {
    SessionService service = make_service();
    const std::string well_formed(anvil::identity::kRefreshTokenLength, 'A');
    const auto refreshed = service.refresh(db(), well_formed, anvil::db::now_ms());
    ASSERT_FALSE(refreshed.ok());
    EXPECT_EQ(refreshed.error().code, anvil::ErrorCode::Unauthenticated);
    EXPECT_NE(refreshed.error().field, kRefreshReplayDetected);
}

// --- revocation -------------------------------------------------------------

TEST_F(AuthService, RevokingASessionBumpsTheEpoch) {
    const UserAuthRecord user = create("revoke", UserType::Client);
    SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = service.create(db(), user, pack_ip("203.0.113.15"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());

    const auto before = users().find_permissions(db(), user.id);
    ASSERT_TRUE(before.ok());

    ASSERT_TRUE(service.revoke(db(), issued.value().session_id, user.id).ok());

    // Without the bump, the revoked session's outstanding access token keeps
    // working until it expires — which is the whole window a sign-out is
    // supposed to close.
    const auto after = users().find_permissions(db(), user.id);
    ASSERT_TRUE(after.ok());
    EXPECT_GT(after.value()->perm_epoch, before.value()->perm_epoch);

    SessionService refresher = make_service();
    const auto refreshed = refresher.refresh(db(), issued.value().refresh_token,
                                             now + std::chrono::minutes{1});
    EXPECT_FALSE(refreshed.ok());
}

TEST_F(AuthService, RevokingEverythingCostsOneEpochBumpAndNotOnePerSession) {
    const UserAuthRecord user = create("revoke-all", UserType::Client);
    SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(service
                        .create(db(), user, pack_ip("203.0.113.16"),
                                "browser " + std::to_string(i), now)
                        .ok());
    }

    const auto before = users().find_permissions(db(), user.id);
    ASSERT_TRUE(before.ok());

    const auto revoked = service.revoke_all(db(), user.id);
    ASSERT_TRUE(revoked.ok());
    EXPECT_EQ(revoked.value(), 3);

    const auto after = users().find_permissions(db(), user.id);
    ASSERT_TRUE(after.ok());
    // The epoch is per USER, not per session: N increments would be N writes to
    // say the same thing once.
    EXPECT_EQ(after.value()->perm_epoch, before.value()->perm_epoch + 1);
}

TEST_F(AuthService, EveryRevocationNamesTheSessionsItEndedToTheHook) {
    const UserAuthRecord user = create("hooked", UserType::Client);
    std::vector<Revocation> told;
    SessionPolicy policy{};
    policy.max_concurrent_sessions = 3;
    SessionService service = make_service(
        policy, [&told](mongocxx::client&, const Uuid& user_id, std::span<const Uuid> ended) {
            told.push_back(Revocation{user_id, {ended.begin(), ended.end()}});
        });
    const anvil::db::TimeMs now = anvil::db::now_ms();
    const auto sign_in = [&](int i) {
        const auto issued = service.create(db(), user, pack_ip("203.0.113.18"),
                                           "browser " + std::to_string(i),
                                           now + std::chrono::seconds{i});
        EXPECT_TRUE(issued.ok());
        return issued.ok() ? issued.value().session_id : Uuid{};
    };

    // A sign-out names its one session.
    const Uuid first = sign_in(0);
    ASSERT_TRUE(service.revoke(db(), first, user.id).ok());
    ASSERT_EQ(told.size(), 1U);
    EXPECT_EQ(told[0].user, user.id);
    EXPECT_EQ(told[0].sessions, std::vector<Uuid>{first});

    // An eviction past the cap names the one it pushed out.
    const Uuid a = sign_in(1);
    const Uuid b = sign_in(2);
    const Uuid c = sign_in(3);
    EXPECT_EQ(told.size(), 1U);
    const Uuid d = sign_in(4);
    ASSERT_EQ(told.size(), 2U);
    EXPECT_EQ(told[1].sessions, std::vector<Uuid>{a});

    // A password change names every session but the one it was made in.
    ASSERT_EQ(service.revoke_others(db(), user.id, d).value(), 2);
    ASSERT_EQ(told.size(), 3U);
    EXPECT_EQ(sorted(told[2].sessions), sorted({b, c}));

    // Signing out everywhere names what is left, and only that.
    ASSERT_EQ(service.revoke_all(db(), user.id).value(), 1);
    ASSERT_EQ(told.size(), 4U);
    EXPECT_EQ(told[3].sessions, std::vector<Uuid>{d});

    // Nothing left to end is nothing to say.
    ASSERT_EQ(service.revoke_all(db(), user.id).value(), 0);
    EXPECT_EQ(told.size(), 4U);
}

TEST_F(AuthService, AHookThatThrowsDoesNotFailTheRevocation) {
    const UserAuthRecord user = create("throwing", UserType::Client);
    SessionService service =
        make_service({}, [](mongocxx::client&, const Uuid&, std::span<const Uuid>) {
            throw std::runtime_error{"the chat database is down"};
        });
    const anvil::db::TimeMs now = anvil::db::now_ms();
    ASSERT_TRUE(service.create(db(), user, pack_ip("203.0.113.19"), kUserAgent, now).ok());

    // The sessions are revoked and the epoch bumped before the hook is asked,
    // and what follows a sign-out must not be able to fail it.
    const auto revoked = service.revoke_all(db(), user.id);
    ASSERT_TRUE(revoked.ok());
    EXPECT_EQ(revoked.value(), 1);
    EXPECT_TRUE(service.list(db(), user.id, Uuid{}, now).value().empty());
}

TEST_F(AuthService, TheSessionListingDisclosesNeitherTheAddressNorTheUserAgent) {
    const UserAuthRecord user = create("listing", UserType::Client);
    SessionService service = make_service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued =
        service.create(db(), user, pack_ip("203.0.113.17"), kUserAgent, now);
    ASSERT_TRUE(issued.ok());

    const auto listed = service.list(db(), user.id, issued.value().session_id, now);
    ASSERT_TRUE(listed.ok());
    ASSERT_EQ(listed.value().size(), 1U);

    const auto& view = listed.value()[0];
    EXPECT_TRUE(view.is_current);
    // Coarsened: a feature for killing a session must not become a self-service
    // intelligence surface.
    EXPECT_EQ(view.coarse_ip, "203.0.113.0/24");
    EXPECT_NE(view.device_label, kUserAgent);
    EXPECT_FALSE(view.device_label.empty());
}

// --- password hashing -------------------------------------------------------

TEST(PasswordServiceShedding, ShedsThroughTheReturnValueAndNeverReEntrantly) {
    // A caller that sheds simply returns; it never has to reason about whether
    // its own continuation already ran on the stack beneath it. That is why
    // shedding is a return value rather than a callback invocation.
    //
    // The pools come from the fixture because Pools::init is once per BINARY —
    // see tests/app_fixture.h.
    ASSERT_TRUE(anvil::testfixture::pools_ready());

    const PasswordService hasher{anvil::auth::kDefaultArgon2Params};

    std::atomic<int> callbacks{0};
    std::atomic<int> shed{0};
    constexpr int kAttempts = 32;
    for (int i = 0; i < kAttempts; ++i) {
        const anvil::Status queued = hasher.hash_async(
            "correct horse battery staple",
            [&callbacks](PasswordService::HashResult) {
                callbacks.fetch_add(1, std::memory_order_relaxed);
            });
        if (!queued) {
            EXPECT_EQ(queued.error().code, anvil::ErrorCode::ServiceUnavailable);
            shed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // A pool one thread wide with a queue of one cannot absorb 32 concurrent
    // hashes, and the excess must be REFUSED rather than queued: hash_pool's
    // size is the memory cap, and a queued request still holds its 64 MiB when
    // it finally runs.
    EXPECT_GT(shed.load(), 0);

    // Every accepted task eventually runs its callback exactly once. Drained
    // rather than slept on, so the assertion is about the pool and not about a
    // timer.
    while (callbacks.load() + shed.load() < kAttempts) {
        std::this_thread::yield();
    }
    EXPECT_EQ(callbacks.load() + shed.load(), kAttempts);
}

// --- client prehash (docs/05 §12) --------------------------------------------------

namespace {

[[nodiscard]] anvil::auth::PrehashPolicy prehash_test_policy() {
    anvil::crypto::Key256 pepper;
    anvil::crypto::Key256 salt_key;
    pepper.mutable_span()[0] = 1;
    salt_key.mutable_span()[0] = 2;
    return anvil::auth::PrehashPolicy{
        .client = {.memory_kib = 8192, .iterations = 2, .parallelism = 1},
        .server = anvil::auth::PrehashKeyedDigestStage{.key_id = "k1", .key = std::move(pepper)},
        .retired_peppers = {},
        .salt_key = std::move(salt_key),
    };
}

// Blocks the test thread, never a pool thread, until `done` flips.
void wait_for(const std::atomic<bool>& done) {
    while (!done.load(std::memory_order_acquire)) { std::this_thread::yield(); }
}

}  // namespace

TEST(PrehashService, EnrollsAndVerifiesOffTheCallingThread) {
    ASSERT_TRUE(anvil::testfixture::pools_ready());
    const anvil::identity::PrehashService service{prehash_test_policy()};

    anvil::auth::PrehashKey k;
    k.mutable_span()[7] = 0x5A;
    anvil::auth::PrehashKey same;
    same.mutable_span()[7] = 0x5A;

    const std::thread::id caller = std::this_thread::get_id();
    std::string record;
    std::thread::id ran_on;
    std::atomic<bool> enrolled{false};
    ASSERT_TRUE(service.enroll_async(
        std::move(k), service.hasher().answer_for(std::nullopt, 0, "user@example.com"),
        [&](anvil::identity::PrehashService::EnrollResult result) {
            ran_on = std::this_thread::get_id();
            record = result.ok() ? result.value() : std::string{};
            enrolled.store(true, std::memory_order_release);
        }));
    wait_for(enrolled);
    EXPECT_NE(ran_on, caller) << "a callback on the caller's stack breaks the shedding contract";
    ASSERT_FALSE(record.empty());

    anvil::crypto::VerifyOutcome outcome = anvil::crypto::VerifyOutcome::Malformed;
    std::atomic<bool> verified{false};
    ASSERT_TRUE(service.verify_async(record, std::move(same),
                                     [&](anvil::identity::PrehashService::VerifyResult result) {
                                         if (result.ok()) { outcome = result.value().outcome; }
                                         verified.store(true, std::memory_order_release);
                                     }));
    wait_for(verified);
    EXPECT_EQ(outcome, anvil::crypto::VerifyOutcome::Match);
}

TEST(PrehashService, AMissingAccountIsAMismatchAfterTheSameWork) {
    ASSERT_TRUE(anvil::testfixture::pools_ready());
    const anvil::identity::PrehashService service{prehash_test_policy()};

    anvil::crypto::VerifyOutcome outcome = anvil::crypto::VerifyOutcome::Match;
    std::atomic<bool> verified{false};
    ASSERT_TRUE(service.verify_async({}, anvil::auth::PrehashKey{},
                                     [&](anvil::identity::PrehashService::VerifyResult result) {
                                         if (result.ok()) { outcome = result.value().outcome; }
                                         verified.store(true, std::memory_order_release);
                                     }));
    wait_for(verified);
    EXPECT_EQ(outcome, anvil::crypto::VerifyOutcome::Mismatch);
}

}  // namespace
