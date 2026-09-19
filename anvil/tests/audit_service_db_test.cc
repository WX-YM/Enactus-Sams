// AuditService against a live cluster — the properties that belong to the
// SERVICE rather than to a row, and that nothing asserted before this file.
//
// Every case here is about the pool being the thing under pressure, because
// that is where this sink has been wrong twice. Both defects are invisible to a
// green suite: no response changes, no exception escapes, and the only evidence
// is a row that is not on disk.
//
// It needs audit_pool's ONE worker and its queue of four (tests/app_fixture.h),
// and it takes them through tests/pool_gate.h: a blocker occupies the worker so
// the queue behind it is observable rather than raced against.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <bsoncxx/builder/basic/document.hpp>

#include "anvil/audit/service.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/types.h"
#include "anvil/core/uuid.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "pool_gate.h"
#include "testapp/audit_actions.h"

namespace anvil::audit {
namespace {

using anvil::testfixture::scratch_names;

constexpr std::string_view kAuditLog = "audit_log";

constexpr auto kDenied = AuditAction::of(testapp::Action::AccessDenied);
constexpr auto kPermissionChanged = AuditAction::of(testapp::Action::StaffPermissionChanged);

class AuditServiceDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        if (!anvil::testfixture::pools_ready()) { GTEST_SKIP() << "thread pools unavailable"; }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kAuditLog);
        gate_ = std::make_unique<anvil::testfixture::PoolGate>(Pools::audit());
    }

    void block_pool() { gate_->block(); }
    void fill_queue() { gate_->fill_queue(); }
    void drain_pool() { gate_->drain(); }

    [[nodiscard]] std::int64_t rows_on_disk() {
        return (**client_)[std::string{scratch_names().for_collection(kAuditLog)}]
                          [std::string{kAuditLog}]
                              .count_documents(bsoncxx::builder::basic::make_document());
    }

    [[nodiscard]] static std::unique_ptr<AuditService> service() {
        return std::make_unique<AuditService>(
            std::string{scratch_names().for_collection(kAuditLog)}, kAuditLog,
            testapp::kAuditActions, kDenied);
    }

    std::unique_ptr<mongocxx::pool::entry>        client_;
    std::unique_ptr<anvil::testfixture::PoolGate> gate_;
};

// A change, with its own actor, so no two of them share a fold key and a batch
// of N rows is N rows.
[[nodiscard]] AuditEntry a_change() {
    return AuditEntry{.actor = uuid::generate_v4(),
                      .subject = uuid::generate_v4(),
                      .from_state = 0,
                      .to_state = 1,
                      .ip = {},
                      .action = kPermissionChanged,
                      .code = ErrorCode::Ok,
                      .succeeded = true,
                      .stealthed = false};
}

// A denial, with its own source network, so no two of them fold into one row.
[[nodiscard]] AuditEntry a_denial(std::uint8_t network) {
    std::array<std::uint8_t, 16> ip{};
    ip[10] = 0xFF;
    ip[11] = 0xFF;
    ip[12] = 203;
    ip[13] = 0;
    ip[14] = network;
    ip[15] = 7;
    return AuditEntry{.actor = std::nullopt,
                      .subject = std::nullopt,
                      .from_state = std::nullopt,
                      .to_state = std::nullopt,
                      .ip = ip,
                      .action = kDenied,
                      .code = ErrorCode::Forbidden,
                      .succeeded = false,
                      .stealthed = true};
}

// --- the task must not outlive its captures ---------------------------------

TEST_F(AuditServiceDb, APostedBatchOutlivesTheServiceThatPostedIt) {
    // post() handed audit_pool a task capturing `this`, and a task the pool has
    // accepted runs after post() returns. It survived in production only because
    // an AuditService is a main()-level object that outlives Pools::shutdown() —
    // an ordering rule in an application's main(), not a property of this class.
    // The load gate found the identical shape in EventSink::post on its first
    // run, as a stack-use-after-return.
    //
    // So: the service is on the heap, and it is destroyed while its batch is
    // still queued. Under ASan a captured `this` is a heap-use-after-free here
    // rather than a test that passes because the timing was kind.
    block_pool();

    std::unique_ptr<AuditService> sink = service();
    for (std::size_t i = 0; i < AuditService::kBatchRows; ++i) { sink->write_async(a_change()); }
    ASSERT_EQ(sink->buffered(), 0U) << "the batch never reached the pool";
    ASSERT_EQ(sink->dropped_changes(), 0U);

    sink.reset();
    drain_pool();

    EXPECT_EQ(rows_on_disk(), static_cast<std::int64_t>(AuditService::kBatchRows))
        << "the batch a destroyed service posted is still the batch that must land";
}

// --- a refused flush is not a lost batch ------------------------------------

TEST_F(AuditServiceDb, ARefusedFlushReturnsItsChangesToTheBuffer) {
    // The database is answering — this fixture just wrote to it — and the pool
    // still refuses, which is the case the old comment beside this code reasoned
    // could not happen and a load run produced 8,485 times.
    block_pool();
    fill_queue();

    std::unique_ptr<AuditService> sink = service();
    for (std::size_t i = 0; i < AuditService::kBatchRows; ++i) { sink->write_async(a_change()); }

    EXPECT_EQ(sink->refused_flushes(), 1U) << "the flush was not refused, so this proves nothing";
    // The whole batch used to be counted here and thrown away.
    EXPECT_EQ(sink->dropped_changes(), 0U);
    EXPECT_EQ(sink->buffered(), AuditService::kBatchRows);

    // And they are still real rows: the destructor's synchronous final flush is
    // what puts the re-admitted batch on disk.
    sink.reset();
    EXPECT_EQ(rows_on_disk(), static_cast<std::int64_t>(AuditService::kBatchRows));

    drain_pool();
}

// --- and a refused flush is not attempted again per row ---------------------

TEST_F(AuditServiceDb, ASaturatedPoolCostsOneRefusalAndNotOnePerRow) {
    // The storm re-admission opened. A refused flush puts its CHANGES back, and
    // write_async posts a flush the moment the buffer crosses kBatchRows — so
    // with the queue still full, every subsequent row drained 256 rows, copied
    // them into a task, was refused, and put them back, with a LOG_WARN each
    // time. Roughly 50 KB of memory traffic, three allocations and one log line
    // per row, in the state where the process is already behind.
    //
    // 512 rows is the shape that shows it: the first 256 fill the buffer and are
    // refused once, and the second 256 each arrive at a buffer that is ALREADY
    // over the batch size, which is the condition that used to repeat the whole
    // flush. One refusal is the fix; 257 is the defect.
    block_pool();
    fill_queue();

    std::unique_ptr<AuditService> sink = service();
    for (std::size_t i = 0; i < AuditService::kBatchRows * 2; ++i) {
        sink->write_async(a_change());
    }

    EXPECT_EQ(sink->refused_flushes(), 1U)
        << "one refusal is what tells the sink the pool is full; the rest are the storm";
    EXPECT_EQ(sink->deferred_flushes(), AuditService::kBatchRows)
        << "every row past the first batch must be buffered, not re-flushed";
    // Nothing was lost by waiting. The rows are in a bounded buffer that sheds
    // by class if it fills, which is where the loss belongs.
    EXPECT_EQ(sink->buffered(), AuditService::kBatchRows * 2);
    EXPECT_EQ(sink->dropped_changes(), 0U);
    EXPECT_EQ(sink->dropped_traffic(), 0U);

    sink.reset();
    drain_pool();
    EXPECT_EQ(rows_on_disk(), static_cast<std::int64_t>(AuditService::kBatchRows * 2));
}

TEST_F(AuditServiceDb, TheNextRowAfterThePoolDrainsFlushesAgain) {
    // The other half: a latch that never comes down is a sink that stops writing
    // for the life of the process. `saturated()` is advisory and it is used as
    // exactly that — not to decide the flush, but to decide whether asking again
    // can produce a different answer.
    block_pool();
    fill_queue();

    std::unique_ptr<AuditService> sink = service();
    for (std::size_t i = 0; i < AuditService::kBatchRows; ++i) { sink->write_async(a_change()); }
    ASSERT_EQ(sink->refused_flushes(), 1U);
    ASSERT_EQ(sink->buffered(), AuditService::kBatchRows);

    drain_pool();

    // One more row, and the buffer is already over the batch size — so this is
    // the attempt the latch would have suppressed.
    sink->write_async(a_change());
    EXPECT_EQ(sink->buffered(), 0U) << "the latch never came down";
    EXPECT_EQ(sink->refused_flushes(), 1U);

    sink.reset();
    drain_pool();
    EXPECT_EQ(rows_on_disk(), static_cast<std::int64_t>(AuditService::kBatchRows + 1));
}

TEST_F(AuditServiceDb, ARefusedFlushChargesTheTrafficItDoesNotReturn) {
    block_pool();
    fill_queue();

    std::unique_ptr<AuditService> sink = service();
    for (std::size_t i = 0; i < AuditService::kBatchRows; ++i) {
        sink->write_async(a_denial(static_cast<std::uint8_t>(i)));
    }

    ASSERT_EQ(sink->refused_flushes(), 1U);
    // Traffic is the compressible class and returning it would displace the
    // changes re-admission exists to protect, so it is charged as the drop —
    // per request it stood for, not per row.
    EXPECT_EQ(sink->dropped_traffic(), AuditService::kBatchRows);
    EXPECT_EQ(sink->dropped_changes(), 0U);
    EXPECT_EQ(sink->buffered(), 0U);

    sink.reset();
    drain_pool();
}

}  // namespace
}  // namespace anvil::audit
