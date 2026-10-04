// Phase 11 — what a refused request tells the client about coming back.
//
// The limiter's counting was already covered by the seam cases in
// seams_phase3_test.cc. What is asserted here is the half that was missing until
// now: the window a verdict reports, and the `Retry-After` derived from it.
//
// The arithmetic cases are the ones worth having. Every one of them is a header
// a client honours INCORRECTLY if the rounding goes the other way — and the
// failure is invisible, because a client that comes back too early is answered
// 429 again and looks exactly like a client that was always going to be refused.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <drogon/HttpResponse.h>
#include <sw/redis++/redis++.h>
#include <trantor/utils/Logger.h>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/random.h"
#include "anvil/http/rate_limit.h"
#include "anvil/http/retry_after.h"
#include "anvil/redis/redis_client.h"

#include "metrics.h"

#include "app_fixture.h"

namespace anvil::http {
namespace {

constexpr RateLimitRule kMinute{"test.minute", std::chrono::seconds{60}, 2};

[[nodiscard]] RateLimitVerdict verdict_with(std::chrono::milliseconds remaining) {
    return RateLimitVerdict{
        .count = 3, .remaining = remaining, .allowed = false, .degraded = false};
}

// A bucket nobody else is using. The key is derived from the rule's bucket and
// the identity, so a fresh identity per test is what keeps two runs against one
// Redis from counting into each other.
//
// DRAWN, not counted. A process-local counter was unique within one process and
// identical across all of them, and `gtest_discover_tests` gives every case its
// own process — which ctest then runs in parallel against one Redis. So the
// three cases here each took `fd00::1`, counted into one key, and the first hit
// of the run reported a count of three. It passed alone and failed in the suite,
// which is the worst shape a test can fail in: the full run is the one nobody
// can reproduce.
[[nodiscard]] std::array<std::uint8_t, 16> unique_address() {
    std::array<std::uint8_t, 16> ip{};
    ip[0] = 0xFD;  // fc00::/7, so this can never be a real client address
    crypto::random_bytes(std::span{ip}.subspan(1));
    return ip;
}

// Installs a registry for the duration of one case and takes it away again. A
// process-wide registry left behind would make one case's counts visible to the
// next, which is how a wiring assertion passes because of somebody else.
//
// It also takes Trantor's log output, because "one line per outage rather than
// one per request" is a property about LINES and there is no other way to state
// it. Capturing them is safe here for the same reason `unique_address` can be
// process-local: gtest_discover_tests gives every case its own process.
class CountedLimiter : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_shared<analytics::Registry>(analytics::kInternalMetrics,
                                                          testapp::kMetrics);
        analytics::install_registry(registry_);
        trantor::Logger::setOutputFunction(
            [this](const char* message, std::uint64_t length) {
                lines_.emplace_back(message, static_cast<std::size_t>(length));
            },
            [] {});
    }

    void TearDown() override {
        trantor::Logger::setOutputFunction(
            [](const char* message, std::uint64_t length) {
                ::fwrite(message, 1, static_cast<std::size_t>(length), stdout);
            },
            [] { ::fflush(stdout); });
        analytics::uninstall_registry();
        registry_.reset();
    }

    [[nodiscard]] std::size_t lines_containing(std::string_view needle) const {
        std::size_t found = 0;
        for (const std::string& line : lines_) {
            if (line.find(needle) != std::string::npos) { ++found; }
        }
        return found;
    }

    // series = source * |decision| + decision, which is the row order
    // kRateLimitLabels declares.
    [[nodiscard]] std::uint64_t decisions(analytics::RateLimitSource source,
                                          analytics::RateLimitDecision decision) const {
        const std::size_t series =
            (static_cast<std::size_t>(source) * analytics::kRateLimitDecisionValues.size()) +
            static_cast<std::size_t>(decision);
        return registry_->value_at(analytics::metric_of(analytics::Internal::RateLimitDecisions),
                                   series, 0);
    }

    std::shared_ptr<analytics::Registry> registry_;
    std::vector<std::string>             lines_;
};

}  // namespace

TEST(RetryAfter, RoundsUpSoTheClientDoesNotReturnBeforeTheWindowCloses) {
    // 1400 ms is two seconds, not one. Truncating sends the client back 400 ms
    // early, which answers 429 again — a header meant to remove one refusal
    // would have added one.
    EXPECT_EQ(retry_after_seconds(verdict_with(std::chrono::milliseconds{1400}), kMinute), 2U);
    EXPECT_EQ(retry_after_seconds(verdict_with(std::chrono::milliseconds{1000}), kMinute), 1U);
    EXPECT_EQ(retry_after_seconds(verdict_with(std::chrono::milliseconds{1001}), kMinute), 2U);
}

TEST(RetryAfter, NeverAnswersZero) {
    // `Retry-After: 0` means retry immediately, which is precisely the stampede
    // the header exists to prevent — and it is what a closed or nearly-closed
    // window rounds to under any honest arithmetic.
    EXPECT_EQ(retry_after_seconds(verdict_with(std::chrono::milliseconds{40}), kMinute), 1U);
    EXPECT_EQ(retry_after_seconds(verdict_with(std::chrono::milliseconds::zero()), kMinute),
              1U);
    EXPECT_EQ(retry_after_seconds(verdict_with(std::chrono::milliseconds{-5}), kMinute), 1U);
}

TEST(RetryAfter, ClampsToTheRulesOwnWindow) {
    // A key that outlived the rule that made it — a deploy shortening a window
    // while keys are open. Telling a client to sleep for the old window is a
    // self-inflicted outage on a rule that no longer asks for it.
    EXPECT_EQ(retry_after_seconds(verdict_with(std::chrono::hours{1}), kMinute), 60U);
}

TEST(RetryAfter, WritesTheHeaderAndNothingElse) {
    const drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k429TooManyRequests);
    apply_retry_after(*response, 7);

    EXPECT_EQ(response->getHeader("Retry-After"), "7");
    // The status is the caller's: a shed is 503 and a refusal is 429, and a
    // function that set one would be wrong for the other half of its callers.
    EXPECT_EQ(response->statusCode(), drogon::k429TooManyRequests);
}

TEST(RetryAfter, TheShedConstantIsWhatTheDocumentsPromise) {
    // docs/00-architecture.md §3's pool table says `503, Retry-After: 1`, and it
    // said so for eleven phases while nothing emitted it.
    EXPECT_EQ(kShedRetryAfterSeconds, 1U);
}

TEST(LocalBuckets, ReportTheWindowTheyOpened) {
    LocalBuckets buckets;
    const auto now = LocalBuckets::Clock::now();

    // The first hit opens the window, so the whole of it is left.
    const BucketHit first = buckets.hit("a", kMinute, now);
    EXPECT_EQ(first.count, 1U);
    EXPECT_EQ(first.remaining, kMinute.window);

    // A hit ten seconds in reports fifty, against the same steady_clock the slot
    // was stamped from rather than against a wall clock an NTP step can move.
    const BucketHit later = buckets.hit("a", kMinute, now + std::chrono::seconds{10});
    EXPECT_EQ(later.count, 2U);
    EXPECT_EQ(later.remaining, std::chrono::seconds{50});

    // Past the end, the window restarts rather than reporting a negative one.
    const BucketHit restarted = buckets.hit("a", kMinute, now + std::chrono::seconds{61});
    EXPECT_EQ(restarted.count, 1U);
    EXPECT_EQ(restarted.remaining, kMinute.window);
}

TEST(RateLimiter, ReportsAWindowRedisOwnsRatherThanOneThisProcessComputed) {
    ANVIL_REQUIRE_REDIS();

    RateLimiter limiter;
    const std::array<std::uint8_t, 16> ip = unique_address();

    const RateLimitVerdict first = limiter.check_ip(ip, kMinute);
    ASSERT_FALSE(first.degraded) << "this case is about the Redis path";
    EXPECT_EQ(first.count, 1U);
    EXPECT_TRUE(first.allowed);
    // The key was created by this call, so what is left is the window minus the
    // round trip — never more than the window and never zero.
    EXPECT_GT(first.remaining, std::chrono::milliseconds::zero());
    EXPECT_LE(first.remaining, kMinute.window);

    // Every later hit reads the SAME window rather than opening a new one, which
    // is what makes the number mean anything to a client that keeps asking.
    const RateLimitVerdict second = limiter.check_ip(ip, kMinute);
    EXPECT_EQ(second.count, 2U);
    EXPECT_LE(second.remaining, first.remaining);
    EXPECT_GT(second.remaining, std::chrono::milliseconds::zero());
}

TEST(RateLimiter, ARefusedHitCarriesAWindowAndThereforeAHeader) {
    ANVIL_REQUIRE_REDIS();

    RateLimiter limiter;
    const std::array<std::uint8_t, 16> ip = unique_address();

    (void)limiter.check_ip(ip, kMinute);
    (void)limiter.check_ip(ip, kMinute);
    const RateLimitVerdict refused = limiter.check_ip(ip, kMinute);

    ASSERT_FALSE(refused.degraded);
    EXPECT_FALSE(refused.allowed);
    // The point of the whole change: before it, this was the moment a 429 went
    // out with nothing on it but a status.
    const std::uint32_t seconds = retry_after_seconds(refused, kMinute);
    EXPECT_GE(seconds, 1U);
    EXPECT_LE(seconds, 60U);
}

TEST(RateLimiter, AWeightedCheckSpendsItsWeightAndRefusesPastTheRule) {
    ANVIL_REQUIRE_REDIS();
    RateLimiter limiter;
    // A byte budget: a thousand bytes a minute.
    constexpr RateLimitRule kBytes{"weighted-test", std::chrono::minutes{1}, 1000};
    const std::string who = "weighted-" + std::to_string(unique_address()[15]) +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const RateLimitVerdict first = limiter.check_account_weighted(who, kBytes, 600);
    ASSERT_FALSE(first.degraded);
    EXPECT_TRUE(first.allowed);
    EXPECT_EQ(first.count, 600U);
    const RateLimitVerdict over = limiter.check_account_weighted(who, kBytes, 600);
    EXPECT_FALSE(over.allowed);
    EXPECT_EQ(over.count, 1200U) << "a refused weight is spent, or it could be retried free";
    // An unweighted check on the same bucket counts one.
    EXPECT_EQ(limiter.check_account(who, kBytes).count, 1201U);
}

TEST(LocalBuckets, AWeightedHitCountsItsWeightAndSaturates) {
    LocalBuckets buckets;
    const auto now = LocalBuckets::Clock::now();
    EXPECT_EQ(buckets.hit("w", kMinute, now, 40).count, 40U);
    EXPECT_EQ(buckets.hit("w", kMinute, now, 2).count, 42U);
    EXPECT_EQ(buckets.hit("w", kMinute, now, std::uint64_t{1} << 40U).count, UINT32_MAX);
    EXPECT_EQ(buckets.hit("w", kMinute, now, 0).count, UINT32_MAX);
}

TEST(RateLimiter, RepairsAKeyThatLostItsExpiryRatherThanLockingItForever) {
    ANVIL_REQUIRE_REDIS();

    RateLimiter limiter;
    const std::array<std::uint8_t, 16> ip = unique_address();
    (void)limiter.check_ip(ip, kMinute);

    // PERSIST is the state the old script could not recover from: a key at a
    // count above one can never be at one again, so the `hits == 1` branch that
    // used to set the expiry would never run and the bucket would refuse that
    // identity permanently. Reached here deliberately, because in production it
    // is reached by a failure nobody watches for.
    std::string key = "rl:ip:";
    key += kMinute.bucket;
    key.push_back(':');
    key += crypto::base64url_encode(ip);
    sw::redis::Redis& redis = redis::RedisClient::instance();
    redis.persist(key);
    ASSERT_EQ(redis.ttl(key), -1) << "the key must actually have lost its expiry";

    const RateLimitVerdict repaired = limiter.check_ip(ip, kMinute);
    ASSERT_FALSE(repaired.degraded);
    EXPECT_GT(repaired.remaining, std::chrono::milliseconds::zero());
    EXPECT_GT(redis.ttl(key), 0) << "the window was not restored";
}

// --- the degrade path is a number, not a log line ---------------------------

TEST_F(CountedLimiter, TheLocalFallbackIsCountedRatherThanLoggedPerRequest) {
    // No ANVIL_REQUIRE_REDIS here, and that is the whole mechanism of the case:
    // gtest_discover_tests gives every case its own process, and this one never
    // initialises RedisClient — so `RedisClient::instance()` throws, `check`
    // catches, and the local bucket answers. The degrade path is reached
    // deterministically rather than by unplugging a server.
    //
    // RedisClient::init is a process-wide singleton and cannot be undone, so a
    // process in which some EARLIER case initialised it cannot reach this path
    // at all. That is not the configuration the gate runs — ctest drives one
    // case per process — but running the binary by hand is, and a case that
    // failed there would be read as a real defect by whoever did it. Guarded
    // rather than left to the ordering, and guarded on the fact rather than on
    // a list of the cases that would do it.
    if (redis::RedisClient::healthy()) {
        GTEST_SKIP() << "RedisClient is already initialised in this process, so the local "
                        "fallback is unreachable — run this case in its own process, which "
                        "is what ctest does";
    }
    //
    // What this pins is the gap the phase-11 row named: `degraded` said in its
    // own comment that it was surfaced so it would appear in metrics, and no
    // metric consumed it. A counter at zero and a counter that does not exist
    // read identically from outside, which is the same argument docs/00 §9 makes
    // for anvil_audit_rows_dropped existing at all.
    RateLimiter limiter;
    const std::array<std::uint8_t, 16> ip = unique_address();

    // kMinute allows two, so the third is refused — by a bucket only this
    // process can see, which is the distinction the `source` label carries.
    const RateLimitVerdict first = limiter.check_ip(ip, kMinute);
    ASSERT_TRUE(first.degraded) << "Redis answered, so this proves nothing";
    EXPECT_TRUE(first.allowed);
    (void)limiter.check_ip(ip, kMinute);
    const RateLimitVerdict refused = limiter.check_ip(ip, kMinute);
    EXPECT_TRUE(refused.degraded);
    EXPECT_FALSE(refused.allowed);

    using analytics::RateLimitDecision;
    using analytics::RateLimitSource;
    EXPECT_EQ(decisions(RateLimitSource::Local, RateLimitDecision::Allowed), 2U);
    EXPECT_EQ(decisions(RateLimitSource::Local, RateLimitDecision::Refused), 1U);
    // The whole point of the source label: an operator reading `shared` alone
    // would see a deployment that refused nothing and allowed nothing.
    EXPECT_EQ(decisions(RateLimitSource::Shared, RateLimitDecision::Allowed), 0U);
    EXPECT_EQ(decisions(RateLimitSource::Shared, RateLimitDecision::Refused), 0U);

    // THREE requests, ONE line. This is the half the counter cannot state and
    // the half the row was actually about: the previous shape wrote a LOG_WARN
    // on every single request for as long as Redis was unreachable, which is
    // what docs/00-architecture.md §9 forbids by name — under the load that
    // makes such a line fire, the line is itself the outage. Nothing here
    // refuses the request earlier, so every request reaches that path, which is
    // what distinguishes this from the idempotency store's `record`/`release`.
    EXPECT_EQ(lines_containing("degraded to local buckets"), 1U);
    EXPECT_EQ(lines_containing("recovered"), 0U);
}

TEST_F(CountedLimiter, TheSharedCounterIsCountedUnderItsOwnSource) {
    ANVIL_REQUIRE_REDIS();

    RateLimiter limiter;
    const std::array<std::uint8_t, 16> ip = unique_address();

    const RateLimitVerdict first = limiter.check_ip(ip, kMinute);
    ASSERT_FALSE(first.degraded) << "this case is about the Redis path";
    (void)limiter.check_ip(ip, kMinute);
    const RateLimitVerdict refused = limiter.check_ip(ip, kMinute);
    ASSERT_FALSE(refused.allowed);

    using analytics::RateLimitDecision;
    using analytics::RateLimitSource;
    EXPECT_EQ(decisions(RateLimitSource::Shared, RateLimitDecision::Allowed), 2U);
    EXPECT_EQ(decisions(RateLimitSource::Shared, RateLimitDecision::Refused), 1U);
    EXPECT_EQ(decisions(RateLimitSource::Local, RateLimitDecision::Allowed), 0U);
    EXPECT_EQ(decisions(RateLimitSource::Local, RateLimitDecision::Refused), 0U);
}

TEST_F(CountedLimiter, ACheckWithNoRegistryInstalledStillAnswers) {
    // The counter is instrumentation and must never be a dependency. Every
    // increment goes through analytics::count, whose null check is the one place
    // this is decided — but the limiter is on the request path, so the case is
    // worth stating here rather than trusting a helper two headers away.
    analytics::uninstall_registry();

    RateLimiter limiter;
    const RateLimitVerdict verdict = limiter.check_ip(unique_address(), kMinute);
    EXPECT_TRUE(verdict.allowed);
    EXPECT_EQ(verdict.count, 1U);
}

}  // namespace anvil::http
