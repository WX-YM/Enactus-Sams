// Phase 8 — who a visitor is, and whether this one is kept.
//
// Both halves are privacy properties before they are measurement properties. The
// packed address never reaches a row, because an IPv4 address is a 32-bit input
// space and an unkeyed digest of one is reversible by anybody holding a database
// dump; and a session is kept WHOLE or dropped whole, because a half-observed
// funnel reports a drop-off that is an artefact of the sampler with no way to
// tell it from a real one afterwards.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include "anvil/analytics/event.h"
#include "anvil/analytics/sessions.h"
#include "anvil/crypto/random.h"

namespace anvil::analytics {
namespace {

[[nodiscard]] VisitorId visitor(std::uint8_t byte) noexcept {
    VisitorId id{};
    id[0] = byte;
    return id;
}

TEST(Sampling, ADenominatorOfOneKeepsEverything) {
    // Sampling is a pressure valve, not a policy.
    for (std::uint8_t i = 0; i < 50; ++i) {
        EXPECT_TRUE(session_is_sampled_in(visitor(i), 1));
        EXPECT_TRUE(session_is_sampled_in(visitor(i), 0));
    }
}

TEST(Sampling, TheSameSessionSamplesTheSameWayEveryTime) {
    // A session is kept WHOLE or dropped WHOLE. Per-event sampling keeps a
    // random half of every session, and a half-observed funnel reports a
    // drop-off that is an artefact of the sampler with no way to tell it from a
    // real one afterwards.
    const VisitorId session = visitor(0x5A);
    const bool first = session_is_sampled_in(session, 4);
    for (int i = 0; i < 1000; ++i) { EXPECT_EQ(session_is_sampled_in(session, 4), first); }
}

TEST(Sampling, ADenominatorActuallyThins) {
    // Not an exact ratio — that would be an assertion about xxh3's distribution
    // rather than about this code — but a denominator that kept everything or
    // nothing would be a sampler that does not sample.
    int kept = 0;
    for (int i = 0; i < 256; ++i) {
        VisitorId session{};
        session[0] = static_cast<std::uint8_t>(i);
        session[1] = static_cast<std::uint8_t>(i * 7);
        if (session_is_sampled_in(session, 4)) { ++kept; }
    }
    EXPECT_GT(kept, 0);
    EXPECT_LT(kept, 256);
}

// --- sessionisation ---------------------------------------------------------

class VisitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        crypto::Key256 pepper;
        crypto::random_bytes(pepper.mutable_span());
        install_visitor_pepper(std::move(pepper));
    }
};

TEST_F(VisitorTest, TheSameAddressAndDayProduceTheSameId) {
    const http::PackedAddress address = http::pack_address("203.0.113.4");
    EXPECT_EQ(visitor_id(address, 100), visitor_id(address, 100));
    // It rotates daily, which bounds what the collection can be used to
    // reconstruct to a day.
    EXPECT_NE(visitor_id(address, 100), visitor_id(address, 101));
}

TEST_F(VisitorTest, TheAddressIsNotRecoverableFromTheId) {
    // Not a proof — it cannot be — but the property that would be violated by
    // the naive implementation: the id must not be a truncation or a plain
    // digest of the sixteen bytes, either of which is enumerable over a 32-bit
    // input space by anybody holding a dump.
    const http::PackedAddress address = http::pack_address("203.0.113.4");
    const VisitorId id = visitor_id(address, 100);
    EXPECT_FALSE(std::equal(address.begin(), address.end(), id.begin()));
    EXPECT_FALSE(is_anonymous_visitor(id));
}

TEST_F(VisitorTest, IPv6IsCoarsenedToSixtyFourAndIPv4IsNot) {
    // Privacy addressing rotates the v6 interface identifier, which mints a new
    // visitor for the same person several times a day and inflates exactly the
    // number this exists to produce.
    const http::PackedAddress first = http::pack_address("2001:db8:1:2::1");
    const http::PackedAddress second = http::pack_address("2001:db8:1:2:aaaa:bbbb:cccc:dddd");
    EXPECT_EQ(visitor_id(first, 7), visitor_id(second, 7));

    // A different /64 is a different visitor: /64 is the smallest block a site
    // is assigned, so it is the coarsest cut that does not merge households.
    const http::PackedAddress elsewhere = http::pack_address("2001:db8:1:3::1");
    EXPECT_NE(visitor_id(first, 7), visitor_id(elsewhere, 7));

    // v4 is NOT coarsened. Coarsening it merges everyone behind one NAT into a
    // single visitor and deflates the count instead.
    EXPECT_NE(visitor_id(http::pack_address("203.0.113.4"), 7),
              visitor_id(http::pack_address("203.0.113.5"), 7));
}

TEST(VisitorWithoutPepper, RecordsNothingRatherThanSomethingReversible) {
    // There is no fallback to an unkeyed digest. A deployment that has not set
    // ANALYTICS_VISITOR_PEPPER records nothing, because an unkeyed digest of a
    // 32-bit address space is reversible from a database dump — a configuration
    // mistake that would otherwise look exactly like success.
    clear_visitor_pepper();
    ASSERT_FALSE(visitor_pepper_installed());
    EXPECT_TRUE(is_anonymous_visitor(visitor_id(http::pack_address("203.0.113.4"), 1)));

    // And the pepper goes back, because this binary's other cases install one in
    // their own fixture and a global left cleared would make the ORDER of these
    // tests load-bearing.
    crypto::Key256 pepper;
    crypto::random_bytes(pepper.mutable_span());
    install_visitor_pepper(std::move(pepper));
}

TEST(DayNumbers, AreFlooredSoAPreEpochInstantDoesNotLandEarly) {
    EXPECT_EQ(day_of(db::TimeMs{std::chrono::milliseconds{0}}), 0);
    EXPECT_EQ(day_of(db::TimeMs{std::chrono::milliseconds{86'399'999}}), 0);
    EXPECT_EQ(day_of(db::TimeMs{std::chrono::milliseconds{86'400'000}}), 1);
    EXPECT_EQ(day_of(db::TimeMs{std::chrono::milliseconds{-1}}), -1);
}

}  // namespace
}  // namespace anvil::analytics
