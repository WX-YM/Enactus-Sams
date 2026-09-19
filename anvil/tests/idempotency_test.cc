// Phase 11 — the idempotency store.
//
// What is asserted here is the set of answers a claim can give, because each one
// has exactly one correct response and getting any of them wrong is a defect
// that never surfaces as an error: a Fresh that should have been a Replay is a
// duplicate write, a Replay that should have been a Mismatch is one request
// answered with another's response, and neither produces a failure anybody sees.
//
// Every case needs a live Redis. There is no in-process mode on purpose — a
// process-local idempotency store answers "fresh" for every retry that reaches a
// different instance, which is not a weaker guarantee but a confidently wrong
// one (anvil/http/idempotency.h).
//
// The one path with no case here is the Redis outage, and it is deliberate
// rather than an omission: RedisClient is a process-wide singleton, so reaching
// that path means taking Redis away from every other case in the binary. What
// the outage must do — refuse, never answer Fresh — is a `catch` with one exit,
// and it is read rather than exercised.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"
#include "anvil/http/idempotency.h"

#include "app_fixture.h"
#include "metrics.h"

namespace anvil::http {
namespace {

constexpr IdempotencyConfig kConfig{
    .retention = std::chrono::seconds{30},
    .in_flight_ttl = std::chrono::seconds{10},
    .max_body_bytes = 64,
};

static_assert(idempotency_config_is_well_formed(kConfig));

// Each of these is a store that does not store, and none of them is visible in a
// review of the numbers themselves.
static_assert(!idempotency_config_is_well_formed(
                  IdempotencyConfig{std::chrono::seconds{0}, std::chrono::seconds{1}, 64}),
              "a zero retention writes a record that has already expired");
static_assert(!idempotency_config_is_well_formed(
                  IdempotencyConfig{std::chrono::seconds{30}, std::chrono::seconds{0}, 64}),
              "a zero in-flight TTL is a marker that blocks nothing");
static_assert(!idempotency_config_is_well_formed(
                  IdempotencyConfig{std::chrono::seconds{10}, std::chrono::seconds{30}, 64}),
              "a marker outliving the retention window locks a client out of an "
              "operation that already succeeded");
static_assert(!idempotency_config_is_well_formed(
                  IdempotencyConfig{std::chrono::seconds{30}, std::chrono::seconds{10},
                                    kMaxRetainedBodyBytes + 1}),
              "anvil's own ceiling on a retained body is not negotiable");

// A distinct caller per case, so two cases in one binary — or two runs against
// one Redis — cannot see each other's records.
[[nodiscard]] std::array<std::uint8_t, 16> some_identity() {
    return uuid::generate_v7();
}

[[nodiscard]] crypto::Digest256 fingerprint_of(std::string_view body) {
    return crypto::sha256(body);
}

}  // namespace

class Idempotency : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_shared<analytics::Registry>(analytics::kInternalMetrics,
                                                          testapp::kMetrics);
        analytics::install_registry(registry_);
    }

    void TearDown() override {
        analytics::uninstall_registry();
        registry_.reset();
    }

    [[nodiscard]] std::uint64_t claims(analytics::IdempotencyOutcome outcome) const {
        return registry_->value_at(analytics::metric_of(analytics::Internal::IdempotencyClaims),
                                   static_cast<std::size_t>(outcome), 0);
    }

    std::shared_ptr<analytics::Registry> registry_;
};

TEST_F(Idempotency, AFirstClaimIsFreshAndASecondIsStillInFlight) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{\"amount\":100}");

    const Result<IdempotencyClaim> first = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().state, IdempotencyState::Fresh);

    // The same request again, before the first has recorded anything. The client
    // must be told to wait rather than admitted to do the work twice.
    const Result<IdempotencyClaim> second = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().state, IdempotencyState::InFlight);

    EXPECT_EQ(claims(analytics::IdempotencyOutcome::Fresh), 1U);
    EXPECT_EQ(claims(analytics::IdempotencyOutcome::InFlight), 1U);
}

TEST_F(Idempotency, ARecordedResponseIsReplayedByteForByte) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{\"amount\":100}");
    constexpr std::string_view kResponse = R"({"id":"7f3a","state":"placed"})";

    const Result<IdempotencyClaim> first = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(store.record(first.value(), 201, kResponse).ok());

    const Result<IdempotencyClaim> repeat = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(repeat.ok());
    EXPECT_EQ(repeat.value().state, IdempotencyState::Replay);
    EXPECT_EQ(repeat.value().status, 201);
    // The same bytes, not an equivalent response: a client comparing an id it
    // already rendered against a freshly-generated one would see two orders.
    EXPECT_EQ(repeat.value().body, kResponse);
    EXPECT_EQ(claims(analytics::IdempotencyOutcome::Replay), 1U);
}

TEST_F(Idempotency, AFailureIsReplayedToo) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{}");

    const Result<IdempotencyClaim> first = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(first.ok());
    // Recording the FAILURE is the correct call when the handler failed with side
    // effects: the retry learns what the first attempt learnt, and the work is
    // not repeated. release() is for the other case, and only for it.
    ASSERT_TRUE(store.record(first.value(), 409, R"({"error":{"code":"CONFLICT"}})").ok());

    const Result<IdempotencyClaim> repeat = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(repeat.ok());
    EXPECT_EQ(repeat.value().state, IdempotencyState::Replay);
    EXPECT_EQ(repeat.value().status, 409);
}

TEST_F(Idempotency, TheSameKeyForADifferentRequestIsAMismatchAndNeverAReplay) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();

    const Result<IdempotencyClaim> first =
        store.claim("orders.create", who, "k-1", fingerprint_of("{\"amount\":100}"));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(store.record(first.value(), 201, "{}").ok());

    // A client that reused one key for two different orders. Replaying would
    // answer the second order with the first one's response, which is worse than
    // either performing it or refusing it.
    const Result<IdempotencyClaim> other =
        store.claim("orders.create", who, "k-1", fingerprint_of("{\"amount\":900}"));
    ASSERT_TRUE(other.ok());
    EXPECT_EQ(other.value().state, IdempotencyState::Mismatch);
    EXPECT_TRUE(other.value().body.empty());
    EXPECT_EQ(claims(analytics::IdempotencyOutcome::Mismatch), 1U);
}

TEST_F(Idempotency, AMismatchIsReportedEvenWhileTheFirstAttemptIsStillRunning) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();

    ASSERT_TRUE(store.claim("orders.create", who, "k-1", fingerprint_of("a")).ok());

    // Reporting this as InFlight would tell the client to come back and try the
    // same thing again, which can only ever fail the same way until the marker
    // expires. The fingerprint therefore decides before the state does.
    const Result<IdempotencyClaim> other =
        store.claim("orders.create", who, "k-1", fingerprint_of("b"));
    ASSERT_TRUE(other.ok());
    EXPECT_EQ(other.value().state, IdempotencyState::Mismatch);
}

TEST_F(Idempotency, AnotherCallersIdenticalKeyIsAFreshRequest) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const crypto::Digest256 body = fingerprint_of("{}");

    const Result<IdempotencyClaim> mine = store.claim("orders.create", some_identity(), "1", body);
    ASSERT_TRUE(mine.ok());
    ASSERT_TRUE(store.record(mine.value(), 201, "mine").ok());

    // "1" is the key every client picks first. If the identity were not part of
    // the key, this caller would be handed somebody else's response — which is
    // the worst failure this class has.
    const Result<IdempotencyClaim> theirs =
        store.claim("orders.create", some_identity(), "1", body);
    ASSERT_TRUE(theirs.ok());
    EXPECT_EQ(theirs.value().state, IdempotencyState::Fresh);
}

TEST_F(Idempotency, AnotherRoutesIdenticalKeyIsAFreshRequest) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{}");

    const Result<IdempotencyClaim> created = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(store.record(created.value(), 201, "created").ok());

    // One client reusing a key across a create and a delete would otherwise be
    // answered the create's 201 to its delete.
    const Result<IdempotencyClaim> deleted = store.claim("orders.delete", who, "k-1", body);
    ASSERT_TRUE(deleted.ok());
    EXPECT_EQ(deleted.value().state, IdempotencyState::Fresh);
}

TEST_F(Idempotency, AReleasedClaimIsFreshAgain) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{}");

    const Result<IdempotencyClaim> first = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(first.ok());
    // The handler failed before writing anything, so the client may retry at
    // once rather than waiting out the marker.
    store.release(first.value());

    const Result<IdempotencyClaim> retry = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value().state, IdempotencyState::Fresh);
}

TEST_F(Idempotency, ReleaseNeverDeletesACompletedRecord) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{}");

    const Result<IdempotencyClaim> first = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(store.record(first.value(), 201, "done").ok());

    // A `catch` that runs on a path which already succeeded. Deleting here would
    // turn the store into a no-op for exactly the request that needed it.
    store.release(first.value());

    const Result<IdempotencyClaim> repeat = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(repeat.ok());
    EXPECT_EQ(repeat.value().state, IdempotencyState::Replay);
    EXPECT_EQ(repeat.value().body, "done");
}

TEST_F(Idempotency, AnOversizeResponseIsCompletedWithoutABody) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{}");

    const Result<IdempotencyClaim> first = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(store.record(first.value(), 201, std::string(kConfig.max_body_bytes + 1, 'x'))
                    .ok());

    // Dropped rather than truncated: a truncated JSON document is a parse error
    // the client cannot tell from a corrupt response. The work still happened
    // exactly once, which is the property — the body is what is gone.
    const Result<IdempotencyClaim> repeat = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(repeat.ok());
    EXPECT_EQ(repeat.value().state, IdempotencyState::Completed);
    EXPECT_TRUE(repeat.value().body.empty());
    EXPECT_EQ(repeat.value().status, 201);
    EXPECT_EQ(claims(analytics::IdempotencyOutcome::Completed), 1U);
}

TEST_F(Idempotency, ASupersededAttemptCannotOverwriteTheRecordThatReplacedIt) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{}");

    // A first attempt claims, then stalls past its own deadline.
    const Result<IdempotencyClaim> stalled = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(stalled.ok());
    // Its marker is gone; a second attempt takes the key and finishes.
    store.release(stalled.value());
    const Result<IdempotencyClaim> second = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().state, IdempotencyState::Fresh);
    ASSERT_TRUE(store.record(second.value(), 201, "second").ok());

    // The first attempt now wakes up and tries to record. Without the fence it
    // would overwrite a newer attempt's record, and the client would be answered
    // the response to a request that has since been superseded.
    const Status late = store.record(stalled.value(), 500, "first");
    EXPECT_FALSE(late.ok());
    EXPECT_EQ(late.code(), ErrorCode::Conflict);

    const Result<IdempotencyClaim> repeat = store.claim("orders.create", who, "k-1", body);
    ASSERT_TRUE(repeat.ok());
    EXPECT_EQ(repeat.value().body, "second");
}

TEST_F(Idempotency, AnEmptyIdentityIsRefusedRatherThanTreatedAsAnonymous) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    // An empty identity merges every anonymous caller into one namespace, where
    // the first client to use the key "1" is replayed to every other client that
    // picks it. It is what a caller reaches by passing a span it forgot to fill.
    const Result<IdempotencyClaim> claimed =
        store.claim("orders.create", {}, "k-1", fingerprint_of("{}"));
    EXPECT_FALSE(claimed.ok());
    EXPECT_EQ(claimed.code(), ErrorCode::Internal);
}

TEST_F(Idempotency, AKeyOutsideTheBoundIsARefusalAndNotAHash) {
    ANVIL_REQUIRE_REDIS();

    IdempotencyStore store{kConfig};
    const std::array<std::uint8_t, 16> who = some_identity();
    const crypto::Digest256 body = fingerprint_of("{}");

    EXPECT_EQ(store.claim("orders.create", who, "", body).code(), ErrorCode::ValidationFailed);
    // Without a bound, one request makes the server hash whatever it sends.
    const std::string huge(kMaxIdempotencyKeyBytes + 1, 'k');
    EXPECT_EQ(store.claim("orders.create", who, huge, body).code(),
              ErrorCode::ValidationFailed);
    // The bound itself is accepted, so the refusal is a bound and not an
    // off-by-one that quietly narrows it.
    EXPECT_TRUE(
        store.claim("orders.create", who, std::string(kMaxIdempotencyKeyBytes, 'k'), body).ok());
}

}  // namespace anvil::http
