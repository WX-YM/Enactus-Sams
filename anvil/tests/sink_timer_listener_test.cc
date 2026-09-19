// The flush timers, and the `this` they hold.
//
// `AuditService::start()` and `EventSink::start()` each install a repeating
// timer on the event loop, and the lambda captures `this`. Until phase 8's
// defects section, `stop()` set a flag and left the timer installed — so a
// destroyed sink left one firing on freed memory every second for the life of
// the loop. It is the same defect as a pool task capturing `this`, with a
// different owner, and it was safe for the same reason: both sinks are
// `main()`-level objects, which is an ordering rule in an application's main()
// rather than a property of either class.
//
// `stop()` invalidates the timer now, and NOTHING PROVED IT. The row was filed
// open with the reason: every timer here needs a RUNNING event loop, and until
// phase 10 created this binary no test process had one. This is that test.
//
// --- what the assertion actually is -----------------------------------------
//
// A timer left behind fires on a destroyed sink and touches `mutex_` and
// `buffered_`, which under ASan is a heap-use-after-free — on every run, not
// when the timing is unkind, because the sink is on the heap and ASan's
// quarantine does not hand the memory back. The failure is the process dying,
// which is why there is no EXPECT for it.
//
// That makes the rest of each case load-bearing rather than decoration:
//
//   * A PROBE timer, installed on the same loop at the same interval, counts
//     its own ticks. It is what says the loop was firing timers during the
//     window the sink's timer would have fired in — without it, a case that
//     destroys a sink and waits proves only that nothing happened, which is
//     also what a loop with no timers at all looks like.
//   * The sink's OWN timer is observed first, by buffering one row and watching
//     it leave. A `stop()` that removes a timer nobody installed removes
//     nothing, and the case would pass on a `start()` that did nothing at all.
//
// --- the two ERROR lines this file prints are expected ----------------------
//
// The flush each case provokes is posted to its pool, and the task there reaches
// for a `MongoPool` this binary never initialises — so `anvil::guarded` catches
// and logs `task threw in pool audit: MongoPool::init not called`, once per
// case. That is the pool wrapper doing its job, and it is left rather than
// silenced: what these cases observe is that the BUFFER was drained, which
// happens before the post, and initialising a database here to quieten a log
// line would make a `unit` binary need a cluster.
//
// --- and why start() and stop() run on the loop thread ----------------------
//
// Both headers state the contract: Trantor's `invalidateTimer` is synchronous
// only on the loop thread and is QUEUED from anywhere else, which leaves a
// window in which the loop can fire the timer one more time. Drogon's
// termination advice already runs there, which is where `stop()` belongs in an
// application — so that is where these cases call it from, rather than proving
// the contract holds somewhere it does not.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <drogon/HttpAppFramework.h>
#include <trantor/net/EventLoop.h>

#include "anvil/analytics/ingest.h"
#include "anvil/analytics/sessions.h"
#include "anvil/audit/service.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"
#include "anvil/http/client_address.h"

#include "app_fixture.h"
#include "listener_fixture.h"
#include "testapp/audit_actions.h"
#include "testapp/events.h"

namespace anvil {
namespace {

using anvil::testfixture::scratch_names;

constexpr std::string_view kAuditLog = "audit_log";
constexpr analytics::AnalyticsCollections kAnalytics{"analytics_events", "analytics_sessions",
                                                     "analytics_rollups"};

constexpr auto kPermissionChanged =
    audit::AuditAction::of(testapp::Action::StaffPermissionChanged);
constexpr auto kDenied = audit::AuditAction::of(testapp::Action::AccessDenied);

// Both sinks flush at 1 Hz.
static_assert(audit::AuditService::kFlushInterval == analytics::EventSink::kFlushInterval);
constexpr std::chrono::milliseconds kInterval = audit::AuditService::kFlushInterval;

// Runs `work` on the loop thread and waits for it to finish there.
//
// A promise rather than a sleep: `start()` and `stop()` are the operations whose
// THREAD is the contract under test, so "it ran on the loop" has to be a fact of
// the test rather than a hope about scheduling.
void on_loop(std::function<void()> work) {
    std::promise<void> done;
    std::future<void> waited = done.get_future();
    drogon::app().getLoop()->queueInLoop([work = std::move(work), &done] {
        work();
        done.set_value();
    });
    waited.wait();
}

// A repeating timer of its own, at the sinks' interval, counting its ticks into
// storage that outlives it. Captured by value as a shared_ptr, so a probe left
// installed by a failed case is harmless rather than a second use-after-free
// reported in place of the first.
class ProbeTimer final {
public:
    ProbeTimer() : ticks_{std::make_shared<std::atomic<int>>(0)} {
        on_loop([this] {
            id_ = drogon::app().getLoop()->runEvery(
                std::chrono::duration<double>{kInterval}.count(),
                [ticks = ticks_] { ticks->fetch_add(1, std::memory_order_relaxed); });
        });
    }

    ~ProbeTimer() {
        on_loop([this] { drogon::app().getLoop()->invalidateTimer(id_); });
    }

    ProbeTimer(const ProbeTimer&) = delete;
    ProbeTimer& operator=(const ProbeTimer&) = delete;

    [[nodiscard]] int ticks() const noexcept { return ticks_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<int>> ticks_;
    trantor::TimerId                  id_ = 0;
};

// Polls `ready` until it holds or the deadline passes. Returns whether it held.
[[nodiscard]] bool within(std::chrono::milliseconds budget,
                          const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return ready();
}

[[nodiscard]] audit::AuditEntry a_change() {
    return audit::AuditEntry{.actor = uuid::generate_v4(),
                             .subject = uuid::generate_v4(),
                             .from_state = 0,
                             .to_state = 1,
                             .ip = {},
                             .action = kPermissionChanged,
                             .code = ErrorCode::Ok,
                             .succeeded = true,
                             .stealthed = false};
}

class SinkTimers : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pools_ready()) { GTEST_SKIP() << "thread pools unavailable"; }
        // Forces the one listener in this binary to boot, which is what gives
        // every case here a running event loop to hang a timer on. Nothing here
        // sends a request; the loop is the whole reason to be in this process.
        (void)anvil::testfixture::listener_port();
    }
};

// --- the audit sink ---------------------------------------------------------

TEST_F(SinkTimers, AnAuditSinkDestroyedAfterStopLeavesNoTimerBehind) {
    ProbeTimer probe;

    auto sink = std::make_unique<audit::AuditService>(
        std::string{scratch_names().for_collection(kAuditLog)}, kAuditLog,
        testapp::kAuditActions, kDenied);
    on_loop([&sink] { sink->start(); });

    // One row, far below kBatchRows, so nothing but the TIMER can move it. Its
    // flush is posted to audit_pool, where the task fails to reach a database
    // this binary never initialised — and that is fine: what is observed is that
    // the buffer was drained, which happens before the post.
    sink->write_async(a_change());
    ASSERT_EQ(sink->buffered(), 1U);
    ASSERT_TRUE(within(kInterval * 5, [&sink] { return sink->buffered() == 0; }))
        << "the flush timer never fired, so stopping it would prove nothing";

    const int ticks_before = probe.ticks();
    on_loop([&sink] { sink->stop(); });
    sink.reset();

    // Two full intervals in which a timer that is still installed WILL fire. The
    // probe is what turns "nothing happened" into "timers were firing and this
    // one was not among them".
    ASSERT_TRUE(within(kInterval * 6, [&probe, ticks_before] {
        return probe.ticks() >= ticks_before + 2;
    })) << "the loop stopped firing timers, so this case waited for nothing";

    SUCCEED() << "a timer left installed would have been a heap-use-after-free under ASan";
}

// --- the analytics sink, which has the same shape ---------------------------

TEST_F(SinkTimers, AnEventSinkDestroyedAfterStopLeavesNoTimerBehind) {
    crypto::Key256 pepper;
    crypto::random_bytes(pepper.mutable_span());
    analytics::install_visitor_pepper(std::move(pepper));

    ProbeTimer probe;

    auto sink = std::make_unique<analytics::EventSink>(
        scratch_names(), kAnalytics, testapp::kEvents, analytics::IngestConfig{});
    on_loop([&sink] { sink->start(); });

    analytics::DimensionValues dimensions = analytics::no_dimensions();
    dimensions[0] = static_cast<std::uint8_t>(testapp::Surface::Web);
    const analytics::Offer offered{static_cast<analytics::EventCode>(
                                       testapp::Event::SignupCompleted),
                                   dimensions, http::pack_address("203.0.113.9"), std::nullopt,
                                   true};
    ASSERT_EQ(sink->offer(offered), analytics::EventSink::Outcome::Recorded);
    ASSERT_EQ(sink->buffered(), 1U);
    ASSERT_TRUE(within(kInterval * 5, [&sink] { return sink->buffered() == 0; }))
        << "the flush timer never fired, so stopping it would prove nothing";

    const int ticks_before = probe.ticks();
    on_loop([&sink] { sink->stop(); });
    sink.reset();

    ASSERT_TRUE(within(kInterval * 6, [&probe, ticks_before] {
        return probe.ticks() >= ticks_before + 2;
    })) << "the loop stopped firing timers, so this case waited for nothing";

    SUCCEED() << "a timer left installed would have been a heap-use-after-free under ASan";
}

}  // namespace
}  // namespace anvil
