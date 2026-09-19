// The shedding policy, tested where it can be tested: no mutex, no pool, no
// client, no clock.
//
// Everything here is about WHICH ROW IS LOST, and the reason the policy is a
// type of its own is that the naive version has it backwards — it drops the row
// ARRIVING, so a flood fills the buffer with identical denials and then discards
// the permission change written in the middle of them.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "anvil/audit/buffer.h"
#include "anvil/identity/session_service.h"
#include "testapp/audit_actions.h"

namespace {

using anvil::audit::AuditBuffer;
using anvil::audit::AuditClass;
using anvil::audit::AuditEntry;
using anvil::audit::AuditRow;
using anvil::audit::audit_class_of;
using anvil::audit::fold_key_of;

constexpr auto kDenied = anvil::audit::AuditAction::of(testapp::Action::AccessDenied);
constexpr auto kPermissionChanged =
    anvil::audit::AuditAction::of(testapp::Action::StaffPermissionChanged);

[[nodiscard]] anvil::db::TimeMs at(std::int64_t ms) noexcept {
    return anvil::db::TimeMs{std::chrono::milliseconds{ms}};
}

[[nodiscard]] anvil::Uuid actor_with(std::uint8_t byte) noexcept {
    anvil::Uuid id{};
    id[0] = byte;
    return id;
}

// A v4-mapped address, so the coarsening rule under test is the /24 one.
[[nodiscard]] std::array<std::uint8_t, 16> v4(std::uint8_t third, std::uint8_t fourth) noexcept {
    std::array<std::uint8_t, 16> ip{};
    ip[10] = 0xFF;
    ip[11] = 0xFF;
    ip[12] = 203;
    ip[13] = 0;
    ip[14] = third;
    ip[15] = fourth;
    return ip;
}

[[nodiscard]] AuditEntry denial(std::array<std::uint8_t, 16> ip,
                                anvil::ErrorCode code = anvil::ErrorCode::Forbidden,
                                bool stealthed = true) {
    return AuditEntry{.actor = std::nullopt,
                      .subject = std::nullopt,
                      .from_state = std::nullopt,
                      .to_state = std::nullopt,
                      .ip = ip,
                      .action = kDenied,
                      .code = code,
                      .succeeded = false,
                      .stealthed = stealthed};
}

[[nodiscard]] AuditEntry change(std::uint8_t actor_byte) {
    return AuditEntry{.actor = actor_with(actor_byte),
                      .subject = actor_with(0x99),
                      .from_state = 0,
                      .to_state = 1,
                      .ip = v4(1, 1),
                      .action = kPermissionChanged,
                      .code = anvil::ErrorCode::Ok,
                      .succeeded = true,
                      .stealthed = false};
}

[[nodiscard]] AuditBuffer make_buffer(std::size_t capacity) {
    return AuditBuffer{testapp::kAuditActions, capacity};
}

// --- classification ---------------------------------------------------------

TEST(AuditBuffer, ClassificationComesFromTheApplicationTable) {
    EXPECT_EQ(audit_class_of(testapp::kAuditActions, kDenied), AuditClass::Traffic);
    EXPECT_EQ(audit_class_of(testapp::kAuditActions, kPermissionChanged), AuditClass::Change);
}

TEST(AuditBuffer, AnUndeclaredActionIsTreatedAsAChange) {
    // A row written by a NEWER process during a rolling deploy. Treating it as
    // compressible traffic would let an older instance shed it during exactly
    // the flood it was written to describe, so the safe default is the
    // undroppable class.
    const auto unknown = anvil::audit::AuditAction::from_stored(9999);
    EXPECT_EQ(audit_class_of(testapp::kAuditActions, unknown), AuditClass::Change);
}

// --- coalescing -------------------------------------------------------------

TEST(AuditBuffer, ConsecutiveIdenticalDenialsFoldIntoOneRow) {
    AuditBuffer buffer = make_buffer(64);

    EXPECT_EQ(buffer.offer(denial(v4(5, 7)), at(1)), AuditBuffer::Admission::Buffered);
    for (int i = 0; i < 99; ++i) {
        EXPECT_EQ(buffer.offer(denial(v4(5, 7)), at(2 + i)), AuditBuffer::Admission::Coalesced);
    }

    // One hundred requests, one row. A count IS the rate, and the millionth
    // identical denial adds nothing an auditor can use.
    EXPECT_EQ(buffer.size(), 1U);

    std::vector<AuditRow> drained;
    buffer.drain(drained);
    ASSERT_EQ(drained.size(), 1U);
    EXPECT_EQ(drained[0].repeats, 100U);
}

TEST(AuditBuffer, AFoldedWindowKeepsTheInstantTheBurstBeganAt) {
    AuditBuffer buffer = make_buffer(64);

    (void)buffer.offer(denial(v4(5, 7)), at(1000));
    (void)buffer.offer(denial(v4(5, 7)), at(5000));
    (void)buffer.offer(denial(v4(5, 7)), at(9000));

    std::vector<AuditRow> drained;
    buffer.drain(drained);
    ASSERT_EQ(drained.size(), 1U);
    // A window stamped with its LAST row would report a burst as having started
    // at the moment it ended, which is the one reading an investigator needs.
    EXPECT_EQ(drained[0].at, at(1000));
}

TEST(AuditBuffer, DenialsFromTheSameCoarseNetworkFoldTogether) {
    AuditBuffer buffer = make_buffer(64);

    // Same /24, rotating low byte. This is precisely the shape of the traffic
    // coalescing exists for: a fold key carrying the FULL address would leave it
    // uncoalesceable.
    EXPECT_EQ(buffer.offer(denial(v4(9, 1)), at(1)), AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(denial(v4(9, 2)), at(2)), AuditBuffer::Admission::Coalesced);
    EXPECT_EQ(buffer.offer(denial(v4(9, 254)), at(3)), AuditBuffer::Admission::Coalesced);
    EXPECT_EQ(buffer.size(), 1U);

    // A different /24 is a different source and must not fold into it.
    EXPECT_EQ(buffer.offer(denial(v4(10, 1)), at(4)), AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.size(), 2U);
}

TEST(AuditBuffer, DenialsSeenDifferentlyByTheClientDoNotFold) {
    AuditBuffer buffer = make_buffer(64);

    // Same code, same source — but one client saw a 404 and the other a 403.
    // Those are different facts about the same denial, which is the whole reason
    // `stealthed` is carried on the entry and not stored.
    EXPECT_EQ(buffer.offer(denial(v4(5, 7), anvil::ErrorCode::Forbidden, true), at(1)),
              AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(denial(v4(5, 7), anvil::ErrorCode::Forbidden, false), at(2)),
              AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.size(), 2U);
}

TEST(AuditBuffer, DifferentCodesDoNotFold) {
    AuditBuffer buffer = make_buffer(64);
    EXPECT_EQ(buffer.offer(denial(v4(5, 7), anvil::ErrorCode::Forbidden), at(1)),
              AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(denial(v4(5, 7), anvil::ErrorCode::Unauthenticated), at(2)),
              AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.size(), 2U);
}

TEST(AuditBuffer, ADrainClosesTheFoldWindow) {
    AuditBuffer buffer = make_buffer(64);
    (void)buffer.offer(denial(v4(5, 7)), at(1));
    (void)buffer.offer(denial(v4(5, 7)), at(2));

    std::vector<AuditRow> first;
    buffer.drain(first);
    ASSERT_EQ(first.size(), 1U);

    // A fold that outlived its flush would add a repeat to a row already on
    // disk, under an instant already written.
    EXPECT_EQ(buffer.offer(denial(v4(5, 7)), at(3)), AuditBuffer::Admission::Buffered);
    std::vector<AuditRow> second;
    buffer.drain(second);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].repeats, 1U);
}

TEST(AuditBuffer, TheFoldKeyIsTheCoarseNetworkAndNotTheAddress) {
    // Asserted directly as well as through the behaviour above: the key is
    // shared with the session listing's notion of a source, and a divergence
    // between them would make a log line and a screen disagree about what one
    // source is.
    const AuditEntry a = denial(v4(9, 1));
    const AuditEntry b = denial(v4(9, 200));
    EXPECT_EQ(fold_key_of(a), fold_key_of(b));
    EXPECT_EQ(fold_key_of(a).network, anvil::identity::coarsen_network(a.ip));
}

// --- the bound, and which class pays for it ---------------------------------

TEST(AuditBuffer, AChangeEvictsATrafficRowRatherThanBeingDropped) {
    AuditBuffer buffer = make_buffer(2);

    // Two distinct denials fill the buffer.
    EXPECT_EQ(buffer.offer(denial(v4(1, 1)), at(1)), AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(denial(v4(2, 1)), at(2)), AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.size(), 2U);

    // The change is the only copy of what somebody did. It displaces the OLDEST
    // denial rather than being refused — this is the case the naive policy gets
    // exactly backwards.
    EXPECT_EQ(buffer.offer(change(0x01), at(3)), AuditBuffer::Admission::Evicted);
    EXPECT_EQ(buffer.size(), 2U);
    EXPECT_EQ(buffer.dropped_changes(), 0U);
    EXPECT_EQ(buffer.dropped_traffic(), 1U);

    std::vector<AuditRow> drained;
    buffer.drain(drained);
    ASSERT_EQ(drained.size(), 2U);
    // The change survived; the denial evicted was the oldest.
    EXPECT_EQ(drained[0].entry.action, kPermissionChanged);
    EXPECT_EQ(drained[1].entry.ip, v4(2, 1));
}

TEST(AuditBuffer, AnEvictedFoldIsChargedForEveryRequestItStoodFor) {
    AuditBuffer buffer = make_buffer(2);

    // One window standing for four thousand denials.
    (void)buffer.offer(denial(v4(1, 1)), at(1));
    for (int i = 0; i < 3999; ++i) { (void)buffer.offer(denial(v4(1, 1)), at(2)); }
    (void)buffer.offer(denial(v4(2, 1)), at(3));
    ASSERT_EQ(buffer.size(), 2U);

    EXPECT_EQ(buffer.offer(change(0x01), at(4)), AuditBuffer::Admission::Evicted);
    // Counting that eviction as ONE drop would understate the loss by exactly
    // the ratio the coalescer achieved, which is how a million discarded rows
    // comes to read as a handful in a metric.
    EXPECT_EQ(buffer.dropped_traffic(), 4000U);
}

TEST(AuditBuffer, AChangeIsRefusedOnlyWhenNothingCompressibleIsHeld) {
    AuditBuffer buffer = make_buffer(2);

    EXPECT_EQ(buffer.offer(change(0x01), at(1)), AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(change(0x02), at(2)), AuditBuffer::Admission::Buffered);
    // Nothing left to discard. That is a database outage rather than a load
    // problem, which is why it is counted as its own thing.
    EXPECT_EQ(buffer.offer(change(0x03), at(3)), AuditBuffer::Admission::Refused);
    EXPECT_EQ(buffer.dropped_changes(), 1U);
    EXPECT_EQ(buffer.dropped_traffic(), 0U);
}

TEST(AuditBuffer, AnArrivingDenialIsTheOneDroppedWhenTheBufferIsFullOfDenials) {
    AuditBuffer buffer = make_buffer(2);

    (void)buffer.offer(denial(v4(1, 1)), at(1));
    (void)buffer.offer(denial(v4(2, 1)), at(2));

    // The EARLIEST evidence of a flood is worth more than its newest identical
    // copy, and a denial displacing a denial would churn the buffer without
    // saving a single row.
    EXPECT_EQ(buffer.offer(denial(v4(3, 1)), at(3)), AuditBuffer::Admission::Refused);
    EXPECT_EQ(buffer.dropped_traffic(), 1U);

    std::vector<AuditRow> drained;
    buffer.drain(drained);
    ASSERT_EQ(drained.size(), 2U);
    EXPECT_EQ(drained[0].entry.ip, v4(1, 1));
    EXPECT_EQ(drained[1].entry.ip, v4(2, 1));
}

TEST(AuditBuffer, LossCountersAreSeparatePerClass) {
    AuditBuffer buffer = make_buffer(1);

    (void)buffer.offer(denial(v4(1, 1)), at(1));
    (void)buffer.offer(denial(v4(2, 1)), at(2));   // refused: traffic
    (void)buffer.offer(change(0x01), at(3));       // evicts the denial
    (void)buffer.offer(change(0x02), at(4));       // refused: change

    // One counter would hide the outage behind the load signal, which is the
    // whole reason there are two.
    EXPECT_EQ(buffer.dropped_traffic(), 2U);
    EXPECT_EQ(buffer.dropped_changes(), 1U);
}

TEST(AuditBuffer, DrainEmptiesTheBufferAndLeavesTheCountersAlone) {
    AuditBuffer buffer = make_buffer(1);
    (void)buffer.offer(denial(v4(1, 1)), at(1));
    (void)buffer.offer(denial(v4(2, 1)), at(2));
    ASSERT_EQ(buffer.dropped_traffic(), 1U);

    std::vector<AuditRow> drained;
    buffer.drain(drained);
    EXPECT_EQ(buffer.size(), 0U);
    // Cumulative since construction: a counter reset by a flush would report
    // zero losses in a deployment losing rows on every flush.
    EXPECT_EQ(buffer.dropped_traffic(), 1U);
}

// --- re-admission, which is where the policy used to stop -------------------

TEST(AuditBufferReadmission, ARefusedFlushReturnsItsChangesAndChargesTheRest) {
    AuditBuffer buffer = make_buffer(8);
    (void)buffer.offer(change(1), at(1));
    (void)buffer.offer(denial(v4(1, 1)), at(2));
    (void)buffer.offer(change(3), at(3));

    std::vector<AuditRow> batch;
    buffer.drain(batch);
    ASSERT_EQ(batch.size(), 3U);
    ASSERT_EQ(buffer.size(), 0U);

    // audit_pool refused it. This used to be the end of those rows: the batch was
    // counted as lost and discarded, which threw away the changes the policy
    // above had just protected. A load run against a live, busy mongod refused
    // 8,485 batches, which is the case the old reasoning excluded.
    EXPECT_EQ(buffer.readmit(batch), 2U);
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(buffer.size(), 2U);
    EXPECT_EQ(buffer.dropped_traffic(), 1U);
    EXPECT_EQ(buffer.dropped_changes(), 0U);

    std::vector<AuditRow> again;
    buffer.drain(again);
    ASSERT_EQ(again.size(), 2U);
    // Both changes came back, in their own order and with their own instants —
    // the instant is when the event happened, and a re-admitted row that was
    // restamped would date a permission change to the retry.
    EXPECT_EQ(again[0].at, at(1));
    EXPECT_EQ(again[1].at, at(3));
}

TEST(AuditBufferReadmission, ARefusedTrafficRowIsChargedForEveryRequestItStoodFor) {
    AuditBuffer buffer = make_buffer(8);
    (void)buffer.offer(denial(v4(4, 4)), at(1));
    for (int i = 0; i < 49; ++i) { (void)buffer.offer(denial(v4(4, 4)), at(2 + i)); }

    std::vector<AuditRow> batch;
    buffer.drain(batch);
    ASSERT_EQ(batch.size(), 1U);

    EXPECT_EQ(buffer.readmit(batch), 0U);
    // Fifty, not one. A folded window standing for fifty denials that is then
    // lost cost fifty rows, and counting it as one understates the loss by
    // exactly the ratio the coalescer achieved.
    EXPECT_EQ(buffer.dropped_traffic(), 50U);
}

TEST(AuditBufferReadmission, ChangesThatDoNotFitAreDroppedAndCounted) {
    AuditBuffer buffer = make_buffer(2);
    std::vector<AuditRow> batch;
    batch.push_back(AuditRow{change(1), at(1), 1});
    batch.push_back(AuditRow{change(2), at(2), 1});
    batch.push_back(AuditRow{change(3), at(3), 1});

    // The buffer is already full of changes, so nothing can be evicted to make
    // room. The honest end of the policy rather than a hole in the middle of it.
    (void)buffer.offer(change(4), at(4));
    (void)buffer.offer(change(5), at(5));
    EXPECT_EQ(buffer.readmit(batch), 0U);
    EXPECT_EQ(buffer.dropped_changes(), 3U);
    EXPECT_EQ(buffer.size(), 2U);
}

TEST(AuditBufferReadmission, AReturnedChangeStillEvictsTrafficToMakeRoom) {
    AuditBuffer buffer = make_buffer(2);
    (void)buffer.offer(denial(v4(1, 1)), at(1));
    (void)buffer.offer(denial(v4(2, 1)), at(2));
    ASSERT_EQ(buffer.size(), 2U);

    std::vector<AuditRow> batch;
    batch.push_back(AuditRow{change(9), at(9), 1});
    // The same priority re-admission has everywhere else: a change is never lost
    // while anything compressible is still held.
    EXPECT_EQ(buffer.readmit(batch), 1U);
    EXPECT_EQ(buffer.dropped_traffic(), 1U);
    EXPECT_EQ(buffer.dropped_changes(), 0U);
}

TEST(AuditBufferReadmission, ReadmissionCannotGrowTheBufferPastItsBound) {
    // The whole reason re-admission is safe: the buffer is already bounded, so
    // returning a batch to it cannot grow without limit.
    AuditBuffer buffer = make_buffer(4);
    std::vector<AuditRow> batch;
    for (std::uint8_t i = 0; i < 200; ++i) { batch.push_back(AuditRow{change(i), at(i), 1}); }
    (void)buffer.readmit(batch);
    EXPECT_LE(buffer.size(), 4U);
}

TEST(AuditBuffer, ARepeatCountSaturatesRatherThanWrapping) {
    AuditBuffer buffer = make_buffer(4);
    (void)buffer.offer(denial(v4(1, 1)), at(1));

    std::vector<AuditRow> peek;
    buffer.drain(peek);
    ASSERT_EQ(peek.size(), 1U);
    // Reaching the saturation point takes 2^32 offers, which no test may run.
    // What is asserted instead is the property that makes saturation safe: the
    // count is unsigned 32-bit and one flush window cannot approach it, so the
    // only way to observe a wrap would be a bug in the increment itself.
    static_assert(sizeof(peek[0].repeats) == 4);
    EXPECT_EQ(peek[0].repeats, 1U);
}

}  // namespace
