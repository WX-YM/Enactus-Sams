// Phase 8 — the second shedding policy.
//
// Everything here is about WHICH ROW IS LOST. It restates the audit sink's
// assertions against the second sink deliberately: the two have the same shape
// and different stakes, and a shared type would be one refactor away from
// letting an analytics flood evict an audit row (anvil/analytics/buffer.h).
//
// The last three cases are the part the audit sink did not have when this was
// written. A refused flush there drained the buffer, counted the loss and
// dropped the batch — so rows the classify-and-shed policy had just protected
// were lost wholesale. A load run measured 8,485 such refusals against a live,
// busy mongod, which is the case the comment beside that code reasoned could
// not happen. Both sinks re-admit now, and tests/audit_buffer_test.cc holds the
// same three cases for the first one.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "anvil/analytics/buffer.h"
#include "anvil/analytics/event.h"

#include "events.h"

namespace anvil::analytics {
namespace {

[[nodiscard]] VisitorId visitor(std::uint8_t byte) noexcept {
    VisitorId id{};
    id[0] = byte;
    return id;
}

[[nodiscard]] Event page_view(std::uint8_t who, testapp::Surface surface) noexcept {
    DimensionValues dimensions = no_dimensions();
    dimensions[0] = static_cast<std::uint8_t>(surface);
    dimensions[1] = static_cast<std::uint8_t>(testapp::Referrer::Direct);
    return Event{static_cast<EventCode>(testapp::Event::PageViewed), dimensions, visitor(who),
                 std::nullopt};
}

[[nodiscard]] Event signup(std::uint8_t who) noexcept {
    DimensionValues dimensions = no_dimensions();
    dimensions[0] = static_cast<std::uint8_t>(testapp::Surface::Web);
    return Event{static_cast<EventCode>(testapp::Event::SignupCompleted), dimensions,
                 visitor(who), std::nullopt};
}

[[nodiscard]] db::TimeMs at(std::int64_t ms) noexcept {
    return db::TimeMs{std::chrono::milliseconds{ms}};
}

TEST(EventBufferPolicy, ConsecutiveIdenticalBehaviourRowsFoldIntoOne) {
    EventBuffer buffer{testapp::kEvents, 8};
    EXPECT_EQ(buffer.offer(page_view(1, testapp::Surface::Web), at(1)),
              EventBuffer::Admission::Buffered);
    for (int i = 0; i < 99; ++i) {
        EXPECT_EQ(buffer.offer(page_view(1, testapp::Surface::Web), at(2 + i)),
                  EventBuffer::Admission::Coalesced);
    }
    // A refresh storm is one row, and a count IS the rate.
    EXPECT_EQ(buffer.size(), 1U);

    std::vector<EventRow> drained;
    buffer.drain(drained);
    ASSERT_EQ(drained.size(), 1U);
    EXPECT_EQ(drained[0].repeats, 100U);
    // The FIRST instant, not the last: a window stamped with its last row would
    // report a burst as having started at the moment it ended.
    EXPECT_EQ(drained[0].at, at(1));
}

TEST(EventBufferPolicy, ADifferentSessionOrDimensionDoesNotFold) {
    EventBuffer buffer{testapp::kEvents, 8};
    EXPECT_EQ(buffer.offer(page_view(1, testapp::Surface::Web), at(1)),
              EventBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(page_view(2, testapp::Surface::Web), at(2)),
              EventBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(page_view(2, testapp::Surface::Ios), at(3)),
              EventBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.size(), 3U);
}

TEST(EventBufferPolicy, AConversionIsNeverDroppedWhileBehaviourIsBuffered) {
    EventBuffer buffer{testapp::kEvents, 2};
    EXPECT_EQ(buffer.offer(page_view(1, testapp::Surface::Web), at(1)),
              EventBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(page_view(2, testapp::Surface::Web), at(2)),
              EventBuffer::Admission::Buffered);

    // Full. The arriving BEHAVIOUR row is the one that goes.
    EXPECT_EQ(buffer.offer(page_view(3, testapp::Surface::Web), at(3)),
              EventBuffer::Admission::Refused);
    EXPECT_EQ(buffer.dropped_behaviour(), 1U);
    EXPECT_EQ(buffer.dropped_conversions(), 0U);

    // A conversion evicts instead. Get this backwards and a flood of page views
    // evicts the one signup the whole funnel is about.
    EXPECT_EQ(buffer.offer(signup(4), at(4)), EventBuffer::Admission::Evicted);
    EXPECT_EQ(buffer.dropped_behaviour(), 2U);
    EXPECT_EQ(buffer.dropped_conversions(), 0U);
}

TEST(EventBufferPolicy, AConversionIsRefusedOnlyWhenNothingElseIsHeld) {
    EventBuffer buffer{testapp::kEvents, 2};
    EXPECT_EQ(buffer.offer(signup(1), at(1)), EventBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(signup(2), at(2)), EventBuffer::Admission::Buffered);
    // Nothing compressible left. This is the state worth alerting on, and it is
    // counted as its own thing rather than folded into a drop total.
    EXPECT_EQ(buffer.offer(signup(3), at(3)), EventBuffer::Admission::Refused);
    EXPECT_EQ(buffer.dropped_conversions(), 1U);
    EXPECT_EQ(buffer.dropped_behaviour(), 0U);
}

TEST(EventBufferPolicy, AnEvictedFoldIsChargedForEveryEventItStoodFor) {
    EventBuffer buffer{testapp::kEvents, 2};
    for (int i = 0; i < 50; ++i) {
        static_cast<void>(buffer.offer(page_view(1, testapp::Surface::Web), at(i)));
    }
    static_cast<void>(buffer.offer(page_view(2, testapp::Surface::Web), at(100)));
    ASSERT_EQ(buffer.size(), 2U);

    EXPECT_EQ(buffer.offer(signup(3), at(101)), EventBuffer::Admission::Evicted);
    // Fifty, not one. Counting a folded window as a single drop understates the
    // loss by exactly the ratio the coalescer achieved.
    EXPECT_EQ(buffer.dropped_behaviour(), 50U);
}

// --- re-admission, which is the correction the audit sink's shape needed -----

TEST(EventBufferReadmission, ARefusedFlushReturnsItsConversionsAndChargesTheRest) {
    EventBuffer buffer{testapp::kEvents, 8};
    static_cast<void>(buffer.offer(signup(1), at(1)));
    static_cast<void>(buffer.offer(page_view(2, testapp::Surface::Web), at(2)));
    static_cast<void>(buffer.offer(signup(3), at(3)));

    std::vector<EventRow> batch;
    buffer.drain(batch);
    ASSERT_EQ(batch.size(), 3U);
    ASSERT_EQ(buffer.size(), 0U);

    // The pool refused it. AuditService used to drop the whole batch here, which
    // lost rows the classify-and-shed policy had just protected; a load run
    // measured 8,485 such refusals against a live, busy mongod. Both sinks now
    // hand the batch back to their buffer.
    EXPECT_EQ(buffer.readmit(batch), 2U);
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(buffer.size(), 2U);
    EXPECT_EQ(buffer.dropped_behaviour(), 1U);
    EXPECT_EQ(buffer.dropped_conversions(), 0U);

    std::vector<EventRow> again;
    buffer.drain(again);
    ASSERT_EQ(again.size(), 2U);
    // Both conversions came back, with their own instants intact.
    EXPECT_EQ(again[0].at, at(1));
    EXPECT_EQ(again[1].at, at(3));
}

TEST(EventBufferReadmission, ConversionsThatDoNotFitAreDroppedAndCounted) {
    EventBuffer buffer{testapp::kEvents, 2};
    std::vector<EventRow> batch;
    batch.push_back(EventRow{at(1), signup(1), 1});
    batch.push_back(EventRow{at(2), signup(2), 1});
    batch.push_back(EventRow{at(3), signup(3), 1});

    // The buffer is already full of conversions, so the third does not fit. The
    // honest end of the policy rather than a hole in the middle of it.
    static_cast<void>(buffer.offer(signup(4), at(4)));
    static_cast<void>(buffer.offer(signup(5), at(5)));
    EXPECT_EQ(buffer.readmit(batch), 0U);
    EXPECT_EQ(buffer.dropped_conversions(), 3U);
    EXPECT_EQ(buffer.size(), 2U);
}

TEST(EventBufferReadmission, ReadmissionCannotGrowTheBufferPastItsBound) {
    // The whole reason re-admission is safe: the buffer is already bounded, so
    // returning a batch to it cannot grow without limit.
    EventBuffer buffer{testapp::kEvents, 4};
    std::vector<EventRow> batch;
    for (std::uint8_t i = 0; i < 200; ++i) { batch.push_back(EventRow{at(i), signup(i), 1}); }
    static_cast<void>(buffer.readmit(batch));
    EXPECT_LE(buffer.size(), 4U);
}

}  // namespace
}  // namespace anvil::analytics
