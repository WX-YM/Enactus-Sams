// The built-in account flows (anvil/accounts/service.h), end to end against a
// live MongoDB and Redis: registration, contact verification, sign-in, lockout,
// password reset and change, under both hashing modes.
//
// Every case drives the service exactly as its HTTP handlers do, and a client
// credential is derived the way a browser derives one — NFC, then Argon2id
// under the salt the service's own salt route answered — never borrowed from
// the code under test. Deliveries are captured instead of sent.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

#include "anvil/accounts/service.h"
#include "anvil/auth/password.h"
#include "anvil/auth/token.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/argon2.h"
#include "anvil/crypto/base64url.h"
#include "anvil/i18n/normalize.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/session_service.h"
#include "anvil/identity/users.h"
#include "anvil/identity/verification.h"
#include "app_fixture.h"
#include "db_fixture.h"

namespace {

namespace acc = anvil::accounts;
using acc::AccountAnswer;
using acc::CodeDelivery;
using acc::CodePurpose;
using anvil::ErrorCode;
using anvil::identity::LoginIdentity;
using anvil::testfixture::scratch_names;

constexpr std::string_view kUsers = "users";
constexpr std::string_view kSessions = "user_sessions";
constexpr std::string_view kVerifications = "email_verifications";

constexpr std::array<std::uint8_t, 32> kSessionPepper{0x11, 0x22, 0x33};
constexpr std::array<std::uint8_t, 32> kSigningKey{0x44, 0x55, 0x66};
constexpr std::array<std::uint8_t, 32> kCodePepper{0x77};
constexpr std::array<std::uint8_t, 32> kIndexKey{0x88};

// The cheapest client stage the prehash policy accepts, so the suite measures
// the flows rather than Argon2.
constexpr anvil::crypto::Argon2Params kClientParams{.memory_kib = 8192, .iterations = 2,
                                                    .parallelism = 1};

// Email and username required — the reference catalogue's unique indexes over
// both are not partial — and a phone optional, whose index is.
inline constexpr std::array<acc::IdentifierSpec, 3> kIdentifiers{{
    {LoginIdentity::Email, true, true},
    {LoginIdentity::Username, true, true},
    {LoginIdentity::Phone, false, true},
}};
inline constexpr std::array<acc::ProfileFieldSpec, 2> kProfile{{
    {"given_name", anvil::input::kPersonNameRules, true},
    {"family_name", anvil::input::kPersonNameRules, false},
}};
inline constexpr acc::AccountSchema kSchema{kIdentifiers, kProfile, LoginIdentity::Email,
                                            acc::Activation::AfterVerification};
static_assert(acc::account_schema_is_well_formed(kSchema));

inline constexpr std::array<acc::AccountRoute, 2> kClientRoutesWithSignIn{{
    {acc::AccountRole::Salt, "auth.prehash"}, {acc::AccountRole::SignIn, "auth.login"}}};
inline constexpr std::array<acc::AccountRoute, 1> kServerRoutes{{
    {acc::AccountRole::SignIn, "auth.login"}}};

// Generous: rate limiting has its own suite, and a shared Redis budget spent by
// an earlier case would fail a later one for a reason it is not about.
constexpr anvil::http::RateLimitRule kWide{"acct-test", std::chrono::minutes{1}, 100000};

[[nodiscard]] acc::AccountBudgets wide_budgets() {
    return acc::AccountBudgets{kWide, kWide, kWide, kWide, kWide, kWide, kWide};
}

[[nodiscard]] anvil::crypto::Key256 key_of(std::uint8_t first) {
    anvil::crypto::Key256 key;
    for (std::size_t i = 0; i < key.size(); ++i) {
        key.mutable_span()[i] = static_cast<std::uint8_t>(first + i);
    }
    return key;
}

// A session is always a Client account at creation. The lockout policy: three
// failures lock for an hour.
[[nodiscard]] acc::LockPolicy three_strikes() {
    return [](std::int32_t failures, anvil::db::TimeMs now) -> std::optional<anvil::db::TimeMs> {
        if (failures < 3) { return std::nullopt; }
        return now + std::chrono::hours{1};
    };
}

// What was "sent", in order, safe to read from the test thread.
class Outbox final {
public:
    acc::DeliverCode sink() {
        return [this](CodeDelivery delivery) {
            const std::lock_guard lock{mutex_};
            sent_.push_back(std::move(delivery));
            changed_.notify_all();
        };
    }

    // Waits for the n-th delivery; a flow delivers AFTER answering.
    [[nodiscard]] std::optional<CodeDelivery> nth(std::size_t n) {
        std::unique_lock lock{mutex_};
        changed_.wait_for(lock, std::chrono::seconds{10}, [&] { return sent_.size() > n; });
        if (sent_.size() <= n) { return std::nullopt; }
        return sent_[n];
    }

    [[nodiscard]] std::size_t count() {
        const std::lock_guard lock{mutex_};
        return sent_.size();
    }

private:
    std::mutex                 mutex_;
    std::condition_variable    changed_;
    std::vector<CodeDelivery>  sent_;
};

// Runs one service call to its answer, whichever way it answers.
template <typename Call>
[[nodiscard]] AccountAnswer await(Call call) {
    auto promise = std::make_shared<std::promise<AccountAnswer>>();
    std::future<AccountAnswer> future = promise->get_future();
    std::optional<AccountAnswer> immediate =
        call([promise](AccountAnswer answer) { promise->set_value(std::move(answer)); });
    if (immediate.has_value()) { return std::move(*immediate); }
    if (future.wait_for(std::chrono::seconds{20}) != std::future_status::ready) {
        ADD_FAILURE() << "the flow never answered";
        return AccountAnswer{ErrorCode::Internal, {}, {}, {}};
    }
    return future.get();
}

const acc::Origin kOrigin{anvil::identity::pack_ip("203.0.113.9"), "Mozilla/5.0 (accounts)"};

class Accounts : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        ANVIL_REQUIRE_REDIS();
        ASSERT_TRUE(anvil::testfixture::pools_ready());
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kUsers);
        anvil::testfixture::clear_collection(**client_, kSessions);
        anvil::testfixture::clear_collection(**client_, kVerifications);
    }

    [[nodiscard]] static std::string users_db() {
        return std::string{scratch_names().for_collection(kUsers)};
    }

    [[nodiscard]] static anvil::identity::AuthzService& authz() {
        static anvil::identity::AuthzService service{users_db(), kUsers};
        return service;
    }

    [[nodiscard]] static anvil::identity::SessionService& sessions() {
        static const auto keys =
            std::make_shared<const anvil::auth::TokenKeys>(std::uint8_t{1}, kSigningKey);
        static anvil::identity::SessionService service{users_db(), kSessions, kUsers,
                                                       kSessionPepper, keys, authz(),
                                                       anvil::identity::SessionPolicy{}};
        return service;
    }

    [[nodiscard]] static anvil::identity::VerificationService& verification() {
        static anvil::identity::VerificationService service{
            std::string{scratch_names().for_collection(kVerifications)}, kVerifications,
            kCodePepper, kIndexKey};
        return service;
    }

    [[nodiscard]] static anvil::http::RateLimiter& limiter() {
        static anvil::http::RateLimiter shared;
        return shared;
    }

    [[nodiscard]] acc::AccountServiceDeps deps() const {
        return acc::AccountServiceDeps{users_db(), kUsers, sessions(), authz(), verification(),
                                       limiter()};
    }

    // Client hashing, keyed stage — the default mode.
    [[nodiscard]] std::unique_ptr<acc::AccountService> client_service() {
        return std::make_unique<acc::AccountService>(
            deps(),
            acc::AccountConfig{
                .description = {&kSchema, acc::Hashing::Client, kClientRoutesWithSignIn},
                .credentials = acc::ClientHashing{anvil::auth::PrehashPolicy{
                    .client = kClientParams,
                    .server = anvil::auth::PrehashKeyedDigestStage{.key_id = "k1",
                                                                   .key = key_of(0x01)},
                    .retired_peppers = {},
                    .salt_key = key_of(0x40)}},
                .budgets = wide_budgets(),
                .lock_after = three_strikes(),
                .deliver = outbox_.sink(),
                .code_lifetime = std::chrono::minutes{10}});
    }

    // Server hashing — the opt-out.
    [[nodiscard]] std::unique_ptr<acc::AccountService> server_service() {
        return std::make_unique<acc::AccountService>(
            deps(),
            acc::AccountConfig{
                .description = {&kSchema, acc::Hashing::Server, kServerRoutes},
                .credentials = acc::ServerHashing{kClientParams},
                .budgets = wide_budgets(),
                .lock_after = three_strikes(),
                .deliver = outbox_.sink(),
                .code_lifetime = std::chrono::minutes{10}});
    }

    // What a browser computes: ask the salt route, NFC the password, Argon2id.
    [[nodiscard]] static acc::Secret client_secret(const acc::AccountService& service,
                                                   std::string_view identifier,
                                                   std::string_view password,
                                                   acc::SaltPurpose purpose) {
        const AccountAnswer salt = await([&](acc::Done done) {
            return service.salt(std::string{identifier}, purpose, kOrigin, std::move(done));
        });
        EXPECT_TRUE(salt.salt.has_value());
        const std::optional<std::string> nfc =
            anvil::i18n::normalize(password, anvil::i18n::NormalizeMode::Nfc);
        anvil::auth::PrehashKey k;
        if (!salt.salt.has_value() || !nfc.has_value()) { return k; }
        anvil::crypto::argon2_hash_raw(
            anvil::crypto::Argon2Type::Argon2id, anvil::crypto::kArgon2Version13,
            salt.salt->params, {reinterpret_cast<const std::uint8_t*>(nfc->data()), nfc->size()},
            salt.salt->salt, {}, {}, k.mutable_span());
        return k;
    }

    [[nodiscard]] static acc::RegisterRequest registration(std::string_view local,
                                                           acc::Secret secret) {
        return acc::RegisterRequest{
            .email = std::string{local} + "@example.test",
            .username = std::string{local},
            .phone = {},
            .profile = {{"given_name", "ليلى"}, {"family_name", "Haddad"}},
            .locale = std::nullopt,
            .secret = std::move(secret),
            .origin = kOrigin};
    }

    // Registers, verifies with the delivered code, and returns the email.
    std::string enrolled(const acc::AccountService& service, std::string_view local,
                         std::string_view password) {
        const std::string email = std::string{local} + "@example.test";
        acc::Secret secret =
            service.hashing() == acc::Hashing::Client
                ? client_secret(service, email, password, acc::SaltPurpose::Enroll)
                : acc::Secret{std::string{password}};
        const std::size_t before = outbox_.count();
        const AccountAnswer registered = await([&](acc::Done done) {
            return service.register_account(registration(local, std::move(secret)),
                                            std::move(done));
        });
        EXPECT_EQ(registered.code, ErrorCode::Ok);
        const std::optional<CodeDelivery> code = outbox_.nth(before);
        EXPECT_TRUE(code.has_value());
        if (!code.has_value()) { return email; }
        EXPECT_EQ(code->purpose, CodePurpose::Verify);
        const AccountAnswer verified = await([&](acc::Done done) {
            return service.verify(email, code->code, kOrigin, std::move(done));
        });
        EXPECT_EQ(verified.code, ErrorCode::Ok);
        return email;
    }

    [[nodiscard]] AccountAnswer sign_in(const acc::AccountService& service,
                                        std::string_view identifier, std::string_view password) {
        acc::Secret secret =
            service.hashing() == acc::Hashing::Client
                ? client_secret(service, identifier, password, acc::SaltPurpose::SignIn)
                : acc::Secret{std::string{password}};
        return await([&](acc::Done done) {
            return service.sign_in(std::string{identifier}, std::move(secret), kOrigin,
                                   std::move(done));
        });
    }

    Outbox                                  outbox_;
    std::unique_ptr<mongocxx::pool::entry>  client_;
};

// --- construction ----------------------------------------------------------------------

TEST_F(Accounts, AConfigurationThatCannotBeRightIsRefusedAtConstruction) {
    const auto make = [&](acc::AccountDescription description, acc::CredentialPolicy policy) {
        return acc::AccountService{
            deps(), acc::AccountConfig{.description = description,
                                       .credentials = std::move(policy),
                                       .budgets = wide_budgets(),
                                       .lock_after = three_strikes(),
                                       .deliver = outbox_.sink(),
                                       .code_lifetime = std::chrono::minutes{10}}};
    };
    // The descriptor tells every client to prehash; a server expecting a
    // password would refuse every one of them.
    EXPECT_THROW(make({&kSchema, acc::Hashing::Client, kClientRoutesWithSignIn},
                      acc::ServerHashing{kClientParams}),
                 std::invalid_argument);
    // Client hashing with no salt route: no client could derive anything.
    EXPECT_THROW(make({&kSchema, acc::Hashing::Server, kClientRoutesWithSignIn},
                      acc::ServerHashing{kClientParams}),
                 std::invalid_argument);
    EXPECT_NO_THROW(make({&kSchema, acc::Hashing::Server, kServerRoutes},
                         acc::ServerHashing{kClientParams}));
}

// --- registration and verification -----------------------------------------------------

TEST_F(Accounts, ARegisteredAccountCannotSignInUntilItsContactIsVerified) {
    const auto service = client_service();
    const std::string email = "pending@example.test";
    acc::Secret secret = client_secret(*service, email, "a long passphrase", acc::SaltPurpose::Enroll);
    const AccountAnswer registered = await([&](acc::Done done) {
        return service->register_account(registration("pending", std::move(secret)),
                                         std::move(done));
    });
    ASSERT_EQ(registered.code, ErrorCode::Ok);
    const std::optional<CodeDelivery> code = outbox_.nth(0);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code->purpose, CodePurpose::Verify);
    EXPECT_EQ(code->address, email);
    EXPECT_EQ(code->code.size(), 6U);

    // The RIGHT password, for an account awaiting verification: refused, and
    // refused exactly as a wrong one is. This is the defect the reference login
    // had — it issued a session here.
    const AccountAnswer right_but_pending = sign_in(*service, email, "a long passphrase");
    const AccountAnswer wrong = sign_in(*service, email, "not the passphrase");
    EXPECT_EQ(right_but_pending.code, ErrorCode::Unauthenticated);
    EXPECT_EQ(wrong.code, ErrorCode::Unauthenticated);
    EXPECT_FALSE(right_but_pending.session.has_value());

    const AccountAnswer verified = await([&](acc::Done done) {
        return service->verify(email, code->code, kOrigin, std::move(done));
    });
    ASSERT_EQ(verified.code, ErrorCode::Ok);

    const AccountAnswer signed_in = sign_in(*service, email, "a long passphrase");
    EXPECT_EQ(signed_in.code, ErrorCode::Ok);
    EXPECT_TRUE(signed_in.session.has_value());
    // By username too, and by a differently-cased email: one canonical form.
    EXPECT_EQ(sign_in(*service, "PENDING", "a long passphrase").code, ErrorCode::Ok);
    EXPECT_EQ(sign_in(*service, "Pending@Example.TEST", "a long passphrase").code, ErrorCode::Ok);
}

TEST_F(Accounts, ADuplicateRegistrationAnswersAsANewOneAndTellsTheOwner) {
    const auto service = client_service();
    const std::string email = enrolled(*service, "dup", "a long passphrase");
    const std::size_t before = outbox_.count();

    acc::Secret secret = client_secret(*service, email, "another passphrase",
                                       acc::SaltPurpose::Enroll);
    const AccountAnswer again = await([&](acc::Done done) {
        return service->register_account(registration("dup", std::move(secret)),
                                         std::move(done));
    });
    EXPECT_EQ(again.code, ErrorCode::Ok) << "a taken address answers exactly as a new one";

    const std::optional<CodeDelivery> told = outbox_.nth(before);
    ASSERT_TRUE(told.has_value());
    EXPECT_EQ(told->purpose, CodePurpose::AccountExists);
    EXPECT_TRUE(told->code.empty());
    // And the original password still works: nothing was overwritten.
    EXPECT_EQ(sign_in(*service, email, "a long passphrase").code, ErrorCode::Ok);
}

TEST_F(Accounts, ARepeatedRegistrationOfAPendingAccountSendsTheCodeThatNowWorks) {
    // One live code per address: the repeat replaced the first code, so the
    // owner must be sent the new one, or anybody could void a stranger's
    // pending registration by registering the same address again.
    const auto service = client_service();
    const auto register_once = [&] {
        acc::Secret secret = client_secret(*service, "twice@example.test", "a long passphrase",
                                           acc::SaltPurpose::Enroll);
        return await([&](acc::Done done) {
            return service->register_account(registration("twice", std::move(secret)),
                                             std::move(done));
        });
    };
    ASSERT_EQ(register_once().code, ErrorCode::Ok);
    const std::optional<CodeDelivery> first = outbox_.nth(0);
    ASSERT_EQ(register_once().code, ErrorCode::Ok);
    const std::optional<CodeDelivery> second = outbox_.nth(1);
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(second->purpose, CodePurpose::Verify);

    if (first->code != second->code) {
        EXPECT_EQ(await([&](acc::Done done) {
                      return service->verify("twice@example.test", first->code, kOrigin,
                                             std::move(done));
                  }).code,
                  ErrorCode::Unauthenticated)
            << "the replaced code no longer verifies";
    }
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->verify("twice@example.test", second->code, kOrigin,
                                         std::move(done));
              }).code,
              ErrorCode::Ok);
}

TEST_F(Accounts, AnEmailsSaltIsTheSameBeforeAndAfterItBecomesAnAccount) {
    const auto service = client_service();
    const AccountAnswer before = await([&](acc::Done done) {
        return service->salt("same@example.test", acc::SaltPurpose::SignIn, kOrigin,
                             std::move(done));
    });
    (void)enrolled(*service, "same", "a long passphrase");
    const AccountAnswer after = await([&](acc::Done done) {
        return service->salt("same@example.test", acc::SaltPurpose::SignIn, kOrigin,
                             std::move(done));
    });
    ASSERT_TRUE(before.salt.has_value());
    ASSERT_TRUE(after.salt.has_value());
    EXPECT_EQ(before.salt->salt, after.salt->salt);
    EXPECT_EQ(before.salt->params, after.salt->params);
}

TEST_F(Accounts, AMalformedRegistrationNamesEveryFieldAndNoValue) {
    const auto service = client_service();
    acc::RegisterRequest request{
        .email = "not an email",
        .username = "",
        .phone = "+0123",
        .profile = {{"given_name", ""}, {"nickname", "x"}},
        .locale = std::nullopt,
        .secret = acc::Secret{std::string{"a password where a credential belongs"}},
        .origin = kOrigin};
    const AccountAnswer refused = await([&](acc::Done done) {
        return service->register_account(std::move(request), std::move(done));
    });
    ASSERT_EQ(refused.code, ErrorCode::ValidationFailed);
    std::vector<std::string> named;
    for (const auto& field : refused.fields) { named.emplace_back(field.field); }
    EXPECT_NE(std::find(named.begin(), named.end(), "email"), named.end());
    EXPECT_NE(std::find(named.begin(), named.end(), "username"), named.end());
    EXPECT_NE(std::find(named.begin(), named.end(), "phone"), named.end());
    EXPECT_NE(std::find(named.begin(), named.end(), "given_name"), named.end());
    EXPECT_NE(std::find(named.begin(), named.end(), "credential"), named.end());
    // The unknown profile key is reported with an EMPTY name: it came from the
    // request, and a client never chooses what a response says.
    EXPECT_NE(std::find(named.begin(), named.end(), ""), named.end());
    EXPECT_EQ(std::find(named.begin(), named.end(), "nickname"), named.end());
    EXPECT_EQ(outbox_.count(), 0U);
}

TEST_F(Accounts, AWrongCodeIsRefusedAndTheRightOneStillWorks) {
    const auto service = client_service();
    acc::Secret secret = client_secret(*service, "codes@example.test", "a long passphrase",
                                       acc::SaltPurpose::Enroll);
    ASSERT_EQ(await([&](acc::Done done) {
                  return service->register_account(registration("codes", std::move(secret)),
                                                   std::move(done));
              }).code,
              ErrorCode::Ok);
    const std::optional<CodeDelivery> code = outbox_.nth(0);
    ASSERT_TRUE(code.has_value());
    const std::string wrong = code->code == "000000" ? "111111" : "000000";

    EXPECT_EQ(await([&](acc::Done done) {
                  return service->verify("codes@example.test", wrong, kOrigin, std::move(done));
              }).code,
              ErrorCode::Unauthenticated);
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->verify("nobody@example.test", code->code, kOrigin,
                                         std::move(done));
              }).code,
              ErrorCode::Unauthenticated);
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->verify("codes@example.test", code->code, kOrigin,
                                         std::move(done));
              }).code,
              ErrorCode::Ok);
}

// --- sign-in -------------------------------------------------------------------------------

TEST_F(Accounts, ThreeWrongPasswordsLockTheAccountEvenAgainstTheRightOne) {
    const auto service = client_service();
    const std::string email = enrolled(*service, "locked", "a long passphrase");
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(sign_in(*service, email, "wrong").code, ErrorCode::Unauthenticated);
    }
    // The policy locked it for an hour: the right password is refused too, and
    // indistinguishably.
    EXPECT_EQ(sign_in(*service, email, "a long passphrase").code, ErrorCode::Unauthenticated);
}

TEST_F(Accounts, AnUnknownAccountAndAWrongPasswordAnswerAlike) {
    const auto service = client_service();
    const std::string email = enrolled(*service, "known", "a long passphrase");
    const AccountAnswer wrong = sign_in(*service, email, "wrong");
    const AccountAnswer nobody = sign_in(*service, "nobody@example.test", "wrong");
    EXPECT_EQ(wrong.code, ErrorCode::Unauthenticated);
    EXPECT_EQ(nobody.code, ErrorCode::Unauthenticated);
    EXPECT_EQ(wrong.fields.size(), nobody.fields.size());
}

// --- password reset -------------------------------------------------------------------------

TEST_F(Accounts, AResetReplacesThePasswordAndEndsEverySession) {
    const auto service = client_service();
    const std::string email = enrolled(*service, "reset", "the old passphrase");
    const AccountAnswer before = sign_in(*service, email, "the old passphrase");
    ASSERT_TRUE(before.session.has_value());
    const std::size_t sent = outbox_.count();

    // By username: the code still goes to the contact.
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->reset_request("reset", kOrigin, std::move(done));
              }).code,
              ErrorCode::Ok);
    const std::optional<CodeDelivery> code = outbox_.nth(sent);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code->purpose, CodePurpose::Reset);
    EXPECT_EQ(code->address, email);

    acc::Secret replacement = client_secret(*service, "reset", "the new passphrase",
                                            acc::SaltPurpose::Enroll);
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->reset_confirm("reset", code->code, std::move(replacement),
                                                kOrigin, std::move(done));
              }).code,
              ErrorCode::Ok);

    EXPECT_EQ(sign_in(*service, email, "the old passphrase").code, ErrorCode::Unauthenticated);
    EXPECT_EQ(sign_in(*service, "reset", "the new passphrase").code, ErrorCode::Ok);
    // The session opened before the reset is gone.
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->refresh(before.session->refresh_token, std::move(done));
              }).code,
              ErrorCode::Unauthenticated);
}

TEST_F(Accounts, AResetForNobodyAnswersAlikeAndSendsNothing) {
    const auto service = client_service();
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->reset_request("ghost@example.test", kOrigin, std::move(done));
              }).code,
              ErrorCode::Ok);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    EXPECT_EQ(outbox_.count(), 0U);
}

TEST_F(Accounts, AVerificationCodeCannotAuthoriseAReset) {
    // Both prove the address, but only one was ASKED for.
    const auto service = client_service();
    acc::Secret secret = client_secret(*service, "confused@example.test", "a long passphrase",
                                       acc::SaltPurpose::Enroll);
    ASSERT_EQ(await([&](acc::Done done) {
                  return service->register_account(registration("confused", std::move(secret)),
                                                   std::move(done));
              }).code,
              ErrorCode::Ok);
    const std::optional<CodeDelivery> verify_code = outbox_.nth(0);
    ASSERT_TRUE(verify_code.has_value());

    acc::Secret replacement = client_secret(*service, "confused@example.test", "hijacked",
                                            acc::SaltPurpose::Enroll);
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->reset_confirm("confused@example.test", verify_code->code,
                                                std::move(replacement), kOrigin, std::move(done));
              }).code,
              ErrorCode::Unauthenticated);
}

// --- password change ------------------------------------------------------------------------

TEST_F(Accounts, AChangeNeedsTheCurrentPasswordAndKeepsOnlyThisSession) {
    const auto service = client_service();
    const std::string email = enrolled(*service, "changer", "the old passphrase");
    const AccountAnswer here = sign_in(*service, email, "the old passphrase");
    const AccountAnswer elsewhere = sign_in(*service, email, "the old passphrase");
    ASSERT_TRUE(here.session.has_value());
    ASSERT_TRUE(elsewhere.session.has_value());

    const auto account = anvil::identity::UserRepository{users_db(), kUsers}.find_for_login(
        **client_, email, LoginIdentity::Email);
    ASSERT_TRUE(account.ok() && account.value().has_value());
    const anvil::Uuid user_id = account.value()->id;

    // A wrong current password.
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->change(user_id, here.session->session_id, email,
                                         client_secret(*service, email, "not it",
                                                       acc::SaltPurpose::SignIn),
                                         client_secret(*service, email, "the new passphrase",
                                                       acc::SaltPurpose::Enroll),
                                         std::move(done));
              }).code,
              ErrorCode::Unauthenticated);

    EXPECT_EQ(await([&](acc::Done done) {
                  return service->change(user_id, here.session->session_id, email,
                                         client_secret(*service, email, "the old passphrase",
                                                       acc::SaltPurpose::SignIn),
                                         client_secret(*service, email, "the new passphrase",
                                                       acc::SaltPurpose::Enroll),
                                         std::move(done));
              }).code,
              ErrorCode::Ok);

    EXPECT_EQ(sign_in(*service, email, "the new passphrase").code, ErrorCode::Ok);
    EXPECT_EQ(sign_in(*service, email, "the old passphrase").code, ErrorCode::Unauthenticated);
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->refresh(elsewhere.session->refresh_token, std::move(done));
              }).code,
              ErrorCode::Unauthenticated)
        << "every other session ends";
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->refresh(here.session->refresh_token, std::move(done));
              }).code,
              ErrorCode::Ok)
        << "the session the change was made from survives";
}

// --- server hashing, the opt-out ------------------------------------------------------------

TEST_F(Accounts, ServerHashingRunsTheSameFlowsOnAPassword) {
    const auto service = server_service();
    const std::string email = enrolled(*service, "plain", "a long enough passphrase");
    EXPECT_EQ(sign_in(*service, email, "a long enough passphrase").code, ErrorCode::Ok);
    EXPECT_EQ(sign_in(*service, email, "wrong").code, ErrorCode::Unauthenticated);

    // The server sees the password here, so it is the one that checks it.
    const AccountAnswer weak = await([&](acc::Done done) {
        return service->register_account(registration("weak", acc::Secret{std::string{"short"}}),
                                         std::move(done));
    });
    ASSERT_EQ(weak.code, ErrorCode::ValidationFailed);
    ASSERT_EQ(weak.fields.size(), 1U);
    EXPECT_EQ(weak.fields[0].field, "password");
    // And there is no salt route to ask.
    EXPECT_EQ(await([&](acc::Done done) {
                  return service->salt(email, acc::SaltPurpose::SignIn, kOrigin, std::move(done));
              }).code,
              ErrorCode::NotFound);
}

}  // namespace
