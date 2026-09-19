// What an open connection costs, and when it is asked again whether it may stay.
//
// No socket in any of it, which is the point: the application owns the socket
// and anvil owns the budget and the schedule, so every property here is
// falsifiable without a network — the same line `sse.h` draws.

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>

#include "anvil/accesscontrol/decision.h"
#include "anvil/core/descriptor_budget.h"
#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"
#include "anvil/http/upgrade.h"

namespace anvil {
namespace {

constexpr std::int64_t kOpened = 1'900'000'000;

// A resolver that is never consulted on the one route class below that does not
// consult one. It exists because a null reference is undefined behaviour even
// where nothing dereferences it, and ASan+UBSan is the configuration this suite
// runs in.
class UnusedEpochs final : public accesscontrol::EpochResolver {
public:
    [[nodiscard]] accesscontrol::EpochVerdict check_cached(const Uuid&,
                                                           std::uint64_t) const noexcept override {
        ADD_FAILURE() << "a public connection must consult no epoch authority";
        return accesscontrol::EpochVerdict::Mismatch;
    }
    void resolve_async(const Uuid&, std::function<void(Result<std::uint64_t>)> done) override {
        ADD_FAILURE() << "a public connection must not resolve an epoch";
        done(fail(ErrorCode::ServiceUnavailable));
    }
};

[[nodiscard]] UserContext holder() noexcept {
    UserContext ctx{};
    ctx.perm_epoch = 5;
    ctx.user_type = UserType::Staff;
    ctx.locale = *Locale::from_tag("en");
    return ctx;
}

[[nodiscard]] http::UpgradedConnection opened_at(std::int64_t at) noexcept {
    return http::UpgradedConnection{holder(), static_cast<std::uint32_t>(at + 3600), at};
}

TEST(FrameBudget, ABurstInsideTheWindowIsAcceptedAndThenTheConnectionCloses) {
    http::UpgradedConnection connection = opened_at(kOpened);

    for (std::size_t i = 0; i < http::kMaxFramesPerWindow; ++i) {
        EXPECT_EQ(connection.admit_frame(16, kOpened), http::FrameVerdict::Accept)
            << "frame " << i;
    }
    // Closed, not answered. There is nobody to answer on a socket the client is
    // still flooding, and a 429 would be one more thing to write.
    EXPECT_EQ(connection.admit_frame(16, kOpened), http::FrameVerdict::Close);
    EXPECT_EQ(connection.frames_in_window(), http::kMaxFramesPerWindow);
}

TEST(FrameBudget, TheWindowRestartsRatherThanSliding) {
    // A sliding window needs the timestamp of every frame in it, which is the
    // per-connection memory this budget exists to bound. The cost is stated
    // where the rule is: a client can spend two windows across a boundary, which
    // is twice the budget over twice the period and is not the flood the rule is
    // about.
    http::UpgradedConnection connection = opened_at(kOpened);
    for (std::size_t i = 0; i < http::kMaxFramesPerWindow; ++i) {
        ASSERT_EQ(connection.admit_frame(16, kOpened), http::FrameVerdict::Accept);
    }
    ASSERT_EQ(connection.admit_frame(16, kOpened), http::FrameVerdict::Close);

    const std::int64_t next_window = kOpened + http::kFrameWindow.count();
    EXPECT_EQ(connection.admit_frame(16, next_window), http::FrameVerdict::Accept);
    EXPECT_EQ(connection.frames_in_window(), 1U);
}

TEST(FrameBudget, AnOversizedFrameClosesAndSpendsNoBudget) {
    // Refused on a comparison against a length the framework has already
    // reported, before anything reads the payload — so a client cannot exhaust
    // the frame budget with frames that were never going to be handled.
    http::UpgradedConnection connection = opened_at(kOpened);
    EXPECT_EQ(connection.admit_frame(http::kMaxFrameBytes + 1, kOpened),
              http::FrameVerdict::Close);
    EXPECT_EQ(connection.frames_in_window(), 0U);

    // And the cap itself is inclusive, or the constant does not mean what it
    // says.
    EXPECT_EQ(connection.admit_frame(http::kMaxFrameBytes, kOpened),
              http::FrameVerdict::Accept);
}

TEST(FrameBudget, AClockThatWentBackwardsDoesNotGrantAnUnboundedWindow) {
    // `now_unix` is the local clock, so this is a step from ntp rather than
    // anything a client can cause. A budget that depends on the clock being
    // monotonic is a budget with a hole in it, and the hole is unbounded.
    http::UpgradedConnection connection = opened_at(kOpened);
    for (std::size_t i = 0; i < http::kMaxFramesPerWindow; ++i) {
        ASSERT_EQ(connection.admit_frame(16, kOpened), http::FrameVerdict::Accept);
    }

    const std::int64_t backwards = kOpened - 600;
    EXPECT_EQ(connection.admit_frame(16, backwards), http::FrameVerdict::Accept);
    for (std::size_t i = 1; i < http::kMaxFramesPerWindow; ++i) {
        ASSERT_EQ(connection.admit_frame(16, backwards), http::FrameVerdict::Accept) << i;
    }
    EXPECT_EQ(connection.admit_frame(16, backwards), http::FrameVerdict::Close)
        << "the window restarted, which is right, but it must still be a window";
}

TEST(RecheckSchedule, TheFirstCheckIsOnePeriodAfterTheHandshake) {
    // Not immediately: the handshake ran the filter's own check, and the cache
    // cannot have changed its answer yet.
    const http::UpgradedConnection connection = opened_at(kOpened);
    EXPECT_FALSE(connection.due_for_recheck(kOpened));
    EXPECT_FALSE(connection.due_for_recheck(kOpened + http::kRecheckPeriod.count() - 1));
    EXPECT_TRUE(connection.due_for_recheck(kOpened + http::kRecheckPeriod.count()));
}

TEST(RecheckSchedule, ThePeriodIsTheCachesOwnAndNotANumberOfItsOwn) {
    // Re-checking faster than the epoch cache can change its answer is work
    // whose result is already determined; slower would make a connection a way
    // to outlive a revocation every request-path caller honours.
    EXPECT_EQ(http::kRecheckPeriod,
              std::chrono::duration_cast<std::chrono::seconds>(auth::kDefaultEpochTtl));
}

TEST(RecheckSchedule, ASweepThatFellBehindDoesNotRunABurstOfCatchUpChecks) {
    // Scheduled from NOW rather than by adding a period to the last due time. A
    // process that was stalled for an hour would otherwise owe three hundred and
    // sixty checks it can learn nothing from — every one of them reads the same
    // cache entry and gets the same answer.
    http::UpgradedConnection connection = opened_at(kOpened);
    const std::int64_t late = kOpened + (http::kRecheckPeriod.count() * 100);
    ASSERT_TRUE(connection.due_for_recheck(late));

    connection.note_recheck(late);
    EXPECT_FALSE(connection.due_for_recheck(late));
    EXPECT_FALSE(connection.due_for_recheck(late + http::kRecheckPeriod.count() - 1));
    EXPECT_TRUE(connection.due_for_recheck(late + http::kRecheckPeriod.count()));
}

TEST(RecheckSchedule, TheConnectionCarriesWhatTheRecheckNeeds) {
    // The two values `still_authorized` takes that a connection has to have kept
    // from its handshake, and the reason the expiry is one of them: it is
    // deliberately not in `UserContext`.
    const http::UpgradedConnection connection = opened_at(kOpened);
    EXPECT_EQ(connection.context().perm_epoch, 5U);
    EXPECT_EQ(connection.expires_at_unix(), static_cast<std::uint32_t>(kOpened + 3600));

    const accesscontrol::RoutePolicy policy{PermSet{}, "/ws/feed",
                                            accesscontrol::RouteAccess::Public};
    // Public consults no epoch, so this needs no resolver behaviour to answer —
    // what it asserts is that the two values fit the function they are kept for.
    UnusedEpochs epochs;
    EXPECT_EQ(accesscontrol::still_authorized(connection.context(),
                                              connection.expires_at_unix(), policy, epochs,
                                              kOpened),
              accesscontrol::ConnectionVerdict::Keep);
}

TEST(UpgradeCeiling, TheShareIsCarvedFromOneBudgetAndNotDerivedTwice) {
    // Streams and upgrades are separate registries drawing on one descriptor
    // budget. The assertion that they fit is in the header that holds both
    // claims; what this pins is that an upgrade ceiling is a real number and
    // strictly smaller than the streams one, which is the split anvil chose.
    const std::size_t upgrades = descriptor_ceiling(kUpgradeShare);
    const std::size_t streams = descriptor_ceiling(kStreamShare);
    EXPECT_LE(upgrades, streams);
    static_assert(shares_fit(kStreamShare, kUpgradeShare));

    // A share with no denominator is a configuration error, not an unbounded
    // ceiling.
    EXPECT_EQ(descriptor_ceiling(DescriptorShare{1, 0}), 0U);
}

}  // namespace
}  // namespace anvil
