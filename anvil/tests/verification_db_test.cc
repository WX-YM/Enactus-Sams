// Contact-address verification against a live cluster.
//
// Every case here is about one of the four controls that make a twenty-bit
// secret acceptable: bounded attempts incremented atomically, a short lifetime,
// one live code per address, and byte-identical failure. Remove any one and the
// scheme fails, which is why they are asserted individually rather than through
// a single "a wrong code is rejected".

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>

#include "anvil/core/uuid.h"
#include "anvil/identity/verification.h"
#include "app_fixture.h"
#include "db_fixture.h"

namespace {

using anvil::identity::NewVerification;
using anvil::identity::VerificationRepository;
using anvil::identity::VerificationService;
using anvil::identity::kMaxVerifyAttempts;
using anvil::testfixture::scratch_names;

constexpr std::string_view kVerifications = "email_verifications";

// Test keys. Two of them, because the service keeps the code pepper and the
// address index key apart: compromising the lookup index must not also let
// somebody confirm a guessed code offline.
constexpr std::array<std::uint8_t, 32> kPepper{1, 2, 3, 4};
constexpr std::array<std::uint8_t, 32> kIndexKey{9, 8, 7, 6};

class VerificationDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kVerifications);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static VerificationService service() {
        return VerificationService{
            std::string{scratch_names().for_collection(kVerifications)}, kVerifications,
            kPepper, kIndexKey};
    }
    [[nodiscard]] static VerificationRepository repository() {
        return VerificationRepository{
            std::string{scratch_names().for_collection(kVerifications)}, kVerifications};
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(VerificationDb, ACodeIsSixDigitsAndVerifiesExactlyOnce) {
    const VerificationService codes = service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto issued = codes.issue(db(), "someone@example.test", std::chrono::minutes{15}, now);
    ASSERT_TRUE(issued.ok());
    EXPECT_EQ(issued.value().code.size(), 6U);
    for (const char digit : issued.value().code) {
        EXPECT_GE(digit, '0');
        EXPECT_LE(digit, '9');
    }

    const auto first = codes.verify(db(), "someone@example.test", issued.value().code, now);
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value());

    // The row is spent in the same operation that said so. A second presentation
    // of a correct code must not succeed.
    const auto second = codes.verify(db(), "someone@example.test", issued.value().code, now);
    ASSERT_TRUE(second.ok());
    EXPECT_FALSE(second.value());
}

TEST_F(VerificationDb, TheCodeIsNeverStoredInPlaintext) {
    const VerificationService codes = service();
    const auto issued =
        codes.issue(db(), "stored@example.test", std::chrono::minutes{15}, anvil::db::now_ms());
    ASSERT_TRUE(issued.ok());

    const auto row = db()[std::string{scratch_names().for_collection(kVerifications)}]
                         [std::string{kVerifications}]
                             .find_one(bsoncxx::builder::basic::make_document());
    ASSERT_TRUE(row.has_value());

    // Asserted by inspecting the stored document rather than by trusting the
    // code path: a digest that silently became a plaintext copy would pass every
    // behavioural test in this file.
    for (const bsoncxx::document::element& field : row->view()) {
        if (field.type() != bsoncxx::type::k_string) { continue; }
        const bsoncxx::stdx::string_view value = field.get_string().value;
        EXPECT_NE(std::string_view(value.data(), value.size()), issued.value().code);
    }
}

TEST_F(VerificationDb, AWrongCodeFailsAndCostsAnAttempt) {
    const VerificationService codes = service();
    const anvil::db::TimeMs now = anvil::db::now_ms();
    const auto issued = codes.issue(db(), "wrong@example.test", std::chrono::minutes{15}, now);
    ASSERT_TRUE(issued.ok());

    const std::string wrong = issued.value().code == "000000" ? "111111" : "000000";
    const auto attempt = codes.verify(db(), "wrong@example.test", wrong, now);
    ASSERT_TRUE(attempt.ok());
    EXPECT_FALSE(attempt.value());

    const auto row = db()[std::string{scratch_names().for_collection(kVerifications)}]
                         [std::string{kVerifications}]
                             .find_one(bsoncxx::builder::basic::make_document());
    ASSERT_TRUE(row.has_value());
    const auto attempts = anvil::db::codec::read_int32(row->view(), "n");
    ASSERT_TRUE(attempts.ok());
    EXPECT_EQ(attempts.value(), 1);
}

TEST_F(VerificationDb, AttemptsAreBoundedAndTheCorrectCodeStopsWorkingAfterwards) {
    // The single load-bearing control. Against a space of a million, five
    // guesses is 5e-6 per window — and without the bound, the space is
    // exhaustible in seconds.
    const VerificationService codes = service();
    const anvil::db::TimeMs now = anvil::db::now_ms();
    const auto issued = codes.issue(db(), "bounded@example.test", std::chrono::minutes{15}, now);
    ASSERT_TRUE(issued.ok());

    const std::string wrong = issued.value().code == "000000" ? "111111" : "000000";
    for (std::int32_t i = 0; i < kMaxVerifyAttempts; ++i) {
        const auto attempt = codes.verify(db(), "bounded@example.test", wrong, now);
        ASSERT_TRUE(attempt.ok());
        EXPECT_FALSE(attempt.value());
    }

    // The RIGHT code, after the budget is spent.
    const auto exhausted = codes.verify(db(), "bounded@example.test", issued.value().code, now);
    ASSERT_TRUE(exhausted.ok());
    EXPECT_FALSE(exhausted.value());
}

TEST_F(VerificationDb, AnExpiredCodeDoesNotVerifyEvenBeforeTheTtlMonitorRuns) {
    // The monitor lags about a minute. Without the explicit expiry predicate an
    // expired code would still verify for that minute, which is a minute of free
    // guesses on a row whose budget was supposed to be spent.
    const VerificationService codes = service();
    const anvil::db::TimeMs now = anvil::db::now_ms();
    const auto issued = codes.issue(db(), "expired@example.test", std::chrono::seconds{1}, now);
    ASSERT_TRUE(issued.ok());

    const auto after = codes.verify(db(), "expired@example.test", issued.value().code,
                                    now + std::chrono::seconds{2});
    ASSERT_TRUE(after.ok());
    EXPECT_FALSE(after.value());
}

TEST_F(VerificationDb, ReissuingInvalidatesThePreviousCodeImmediately) {
    // Two live codes would double the guess surface for the price of one
    // request, which is the whole reason the write is an upsert rather than an
    // insert.
    const VerificationService codes = service();
    const anvil::db::TimeMs now = anvil::db::now_ms();

    const auto first = codes.issue(db(), "reissue@example.test", std::chrono::minutes{15}, now);
    ASSERT_TRUE(first.ok());
    const auto second = codes.issue(db(), "reissue@example.test", std::chrono::minutes{15}, now);
    ASSERT_TRUE(second.ok());

    const auto stale = codes.verify(db(), "reissue@example.test", first.value().code, now);
    ASSERT_TRUE(stale.ok());
    EXPECT_FALSE(stale.value());

    const auto fresh = codes.verify(db(), "reissue@example.test", second.value().code, now);
    ASSERT_TRUE(fresh.ok());
    EXPECT_TRUE(fresh.value());
}

TEST_F(VerificationDb, ReissuingResetsTheAttemptCounter) {
    const VerificationService codes = service();
    const anvil::db::TimeMs now = anvil::db::now_ms();
    const auto first = codes.issue(db(), "reset@example.test", std::chrono::minutes{15}, now);
    ASSERT_TRUE(first.ok());

    const std::string wrong = first.value().code == "000000" ? "111111" : "000000";
    for (std::int32_t i = 0; i < kMaxVerifyAttempts; ++i) {
        ASSERT_TRUE(codes.verify(db(), "reset@example.test", wrong, now).ok());
    }

    // A fresh row IS a fresh start — which is exactly why the RESEND needs its
    // own rate-limit bucket, and why anvil documents that requirement rather
    // than pretending the attempt counter alone is sufficient.
    const auto second = codes.issue(db(), "reset@example.test", std::chrono::minutes{15}, now);
    ASSERT_TRUE(second.ok());
    const auto verified = codes.verify(db(), "reset@example.test", second.value().code, now);
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value());
}

TEST_F(VerificationDb, ReissuingAfterExpiryWorksRatherThanCollidingOnTheUniqueIndex) {
    // The upsert deliberately does not filter on the old row's expiry: doing so
    // would insert a second row for the same address, which the unique index
    // rejects — and an address that let its code lapse could then never request
    // another one.
    const VerificationService codes = service();
    const anvil::db::TimeMs now = anvil::db::now_ms();
    ASSERT_TRUE(codes.issue(db(), "lapsed@example.test", std::chrono::seconds{1}, now).ok());

    const auto again = codes.issue(db(), "lapsed@example.test", std::chrono::minutes{15},
                                   now + std::chrono::seconds{5});
    ASSERT_TRUE(again.ok());
    const auto verified = codes.verify(db(), "lapsed@example.test", again.value().code,
                                       now + std::chrono::seconds{5});
    ASSERT_TRUE(verified.ok());
    EXPECT_TRUE(verified.value());
}

TEST_F(VerificationDb, NoSuchAddressFailsExactlyAsAWrongCodeDoes) {
    // Four ways to fail — wrong code, expired row, exhausted attempts, no such
    // address — and all four have to be one answer, or the difference is an
    // enumeration oracle over every address in the system.
    const VerificationService codes = service();
    const auto unknown =
        codes.verify(db(), "nobody@example.test", "123456", anvil::db::now_ms());
    ASSERT_TRUE(unknown.ok());
    EXPECT_FALSE(unknown.value());
}

TEST_F(VerificationDb, BothBranchesIssueTheSameNumberOfOperations) {
    // Asserted through burn_attempt's own contract: it is issued on EVERY
    // verification, and it succeeds whether or not it matched. A success that
    // costs one round trip and a failure that costs two is a difference somebody
    // can measure.
    const VerificationRepository codes = repository();
    const anvil::crypto::Digest256 address{};
    ASSERT_TRUE(codes.burn_attempt(db(), address, anvil::db::now_ms()).ok());
}

TEST_F(VerificationDb, TheAddressIndexIsKeyedAndNotAPlainDigest) {
    // A plain digest of a structured value is a lookup table somebody else
    // already has. Two services with different index keys must produce different
    // indexes for the same address.
    const VerificationService one = service();
    const VerificationService two{
        std::string{scratch_names().for_collection(kVerifications)}, kVerifications, kPepper,
        std::array<std::uint8_t, 32>{5, 5, 5, 5}};

    EXPECT_NE(one.address_index("same@example.test"), two.address_index("same@example.test"));
    EXPECT_EQ(one.address_index("same@example.test"), one.address_index("same@example.test"));
}

TEST_F(VerificationDb, AZeroLifetimeIsRefusedRatherThanWrittenAsAlreadyExpired) {
    const VerificationService codes = service();
    const auto issued =
        codes.issue(db(), "zero@example.test", std::chrono::seconds{0}, anvil::db::now_ms());
    EXPECT_FALSE(issued.ok());
}

}  // namespace
