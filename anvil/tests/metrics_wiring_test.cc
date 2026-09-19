// Phase 8 — the counters, wired to the mechanisms they name.
//
// A counter that exists and is never incremented reports zero, and zero is what
// a healthy deployment reports too. The two are indistinguishable from outside,
// which is exactly the failure docs/00 §9 gives as the reason two of these
// metrics exist at all — so the wiring is asserted rather than assumed.
//
// These are the sites a unit test can reach without a live cluster or a real
// listener. The rest are covered where their own subsystem is: the pool wait
// histogram by anvil_db_tests, the orphan sweep by the media suite.

#include <gtest/gtest.h>

#include <array>
#include <memory>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/gauges.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/analytics/snapshot.h"
#include "anvil/audit/buffer.h"
#include "anvil/core/thread_pools.h"

#include "app_fixture.h"
#include "metrics.h"
#include "testapp/audit_actions.h"

namespace anvil::analytics {
namespace {

using anvil::audit::AuditBuffer;
using anvil::audit::AuditEntry;

constexpr auto kDenied = anvil::audit::AuditAction::of(testapp::Action::AccessDenied);
constexpr auto kPermissionChanged =
    anvil::audit::AuditAction::of(testapp::Action::StaffPermissionChanged);

// `network` distinguishes one entry from another, and it is the COARSE network
// that does — fold_key_of uses identity::coarsen_network's /24, so two addresses
// differing only in their last byte are one event repeated rather than two.
// Varying the third octet is what makes these rows distinct at all.
[[nodiscard]] AuditEntry entry(anvil::audit::AuditAction action, std::uint8_t network) {
    std::array<std::uint8_t, 16> ip{};
    ip[10] = 0xFF;
    ip[11] = 0xFF;
    ip[12] = 198;
    ip[14] = network;
    ip[15] = 7;
    return AuditEntry{.actor = std::nullopt,
                      .subject = std::nullopt,
                      .from_state = std::nullopt,
                      .to_state = std::nullopt,
                      .ip = ip,
                      .action = action,
                      .code = anvil::ErrorCode::Forbidden,
                      .succeeded = false,
                      .stealthed = true};
}

[[nodiscard]] db::TimeMs at(std::int64_t ms) noexcept {
    return db::TimeMs{std::chrono::milliseconds{ms}};
}

// Installs a registry for the duration of one test and takes it away again. A
// process-wide registry left installed would make one test's counts visible to
// the next, which is how a wiring assertion passes because of somebody else.
class WiredMetrics : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_shared<Registry>(kInternalMetrics, testapp::kMetrics);
        install_registry(registry_);
    }

    void TearDown() override {
        uninstall_registry();
        registry_.reset();
    }

    [[nodiscard]] std::uint64_t dropped(AuditDropClass cls, AuditDropStage stage) const {
        // series = class * |stage| + stage.
        const std::size_t series =
            (static_cast<std::size_t>(cls) * kAuditStageValues.size()) +
            static_cast<std::size_t>(stage);
        return registry_->value_at(metric_of(Internal::AuditRowsDropped), series, 0);
    }

    std::shared_ptr<Registry> registry_;
};

TEST_F(WiredMetrics, ADroppedAuditRowIsCountedByClassAndStage) {
    // Capacity two, so the third row has to displace something and the fourth
    // has nothing left to displace.
    AuditBuffer buffer{testapp::kAuditActions, 2};

    EXPECT_EQ(buffer.offer(entry(kDenied, 1), at(1)), AuditBuffer::Admission::Buffered);
    EXPECT_EQ(buffer.offer(entry(kDenied, 2), at(2)), AuditBuffer::Admission::Buffered);
    // A traffic row arriving at a full buffer is the one that goes: the earliest
    // evidence of a flood is worth more than its newest identical copy.
    EXPECT_EQ(buffer.offer(entry(kDenied, 3), at(3)), AuditBuffer::Admission::Refused);
    EXPECT_EQ(dropped(AuditDropClass::Traffic, AuditDropStage::Buffer), 1U);
    EXPECT_EQ(dropped(AuditDropClass::Change, AuditDropStage::Buffer), 0U);

    // A change is never dropped while anything compressible is held, so this one
    // evicts a traffic row rather than being refused.
    EXPECT_EQ(buffer.offer(entry(kPermissionChanged, 4), at(4)),
              AuditBuffer::Admission::Evicted);
    EXPECT_EQ(dropped(AuditDropClass::Traffic, AuditDropStage::Buffer), 2U);

    // The second change displaces the last compressible row, which leaves the
    // buffer holding nothing but changes.
    EXPECT_EQ(buffer.offer(entry(kPermissionChanged, 5), at(5)),
              AuditBuffer::Admission::Evicted);
    EXPECT_EQ(dropped(AuditDropClass::Traffic, AuditDropStage::Buffer), 3U);

    // Only now can a change be refused, and that is a database outage rather
    // than a load problem — which is the whole reason the two classes are
    // separate series and not one total.
    EXPECT_EQ(buffer.offer(entry(kPermissionChanged, 6), at(6)),
              AuditBuffer::Admission::Refused);
    EXPECT_EQ(dropped(AuditDropClass::Change, AuditDropStage::Buffer), 1U);
    EXPECT_EQ(dropped(AuditDropClass::Change, AuditDropStage::Flush), 0U);
}

TEST_F(WiredMetrics, ACoalescedWindowIsChargedForEveryRequestItStoodFor) {
    AuditBuffer buffer{testapp::kAuditActions, 2};
    // Same fold key three times: one row, three repeats.
    for (int i = 0; i < 3; ++i) { (void)buffer.offer(entry(kDenied, 7), at(i)); }
    (void)buffer.offer(entry(kDenied, 8), at(10));
    EXPECT_EQ(buffer.size(), 2U);

    // Evicting that window costs three rows, not one. Counting it as one would
    // understate the loss by exactly the ratio the coalescer achieved, which is
    // how a million discarded rows comes to read as a few thousand.
    EXPECT_EQ(buffer.offer(entry(kPermissionChanged, 9), at(11)),
              AuditBuffer::Admission::Evicted);
    EXPECT_EQ(dropped(AuditDropClass::Traffic, AuditDropStage::Buffer), 3U);
}

TEST_F(WiredMetrics, ThePoolQueueDepthGaugeIsSampledAtCollection) {
    if (!anvil::testfixture::pools_ready()) { GTEST_SKIP() << "thread pools unavailable"; }

    // Installed by Pools::init, so a deployment cannot have bounded queues and
    // no view of how full they are.
    Snapshot snapshot{*registry_};
    snapshot.collect();

    const MetricId depth = metric_of(Internal::PoolQueueDepth);
    EXPECT_EQ(registry_->value_at(depth, static_cast<std::size_t>(PoolLabel::Db), 0),
              Pools::db().queue_depth());
    EXPECT_EQ(registry_->value_at(depth, static_cast<std::size_t>(PoolLabel::Hash), 0),
              Pools::hash().queue_depth());
}

TEST(UninstalledRegistry, AnIncrementBeforeInstallIsANoOp) {
    // A library whose counter call kills an application that has not called
    // install_registry is a library with a worse failure mode than the missing
    // number it was reporting.
    uninstall_registry();
    ASSERT_EQ(registry(), nullptr);
    count(Internal::AuthzCacheHits, AuthzTier::Local);
    sample(Internal::PoolQueueDepth, PoolLabel::Db, 4);
    observe(Internal::MongoPoolWaitMicroseconds, 12);
    SUCCEED();
}

}  // namespace
}  // namespace anvil::analytics
