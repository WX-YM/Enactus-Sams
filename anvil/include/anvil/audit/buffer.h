#pragma once

// What the audit sink holds between flushes, and the policy that decides which
// row goes when it cannot hold any more.
//
// --- why this is a type of its own ------------------------------------------
//
// Batching audit writes removes the write amplification and leaves the shedding
// policy exactly as it was: every row equally droppable, and the row DROPPED the
// one arriving. A flood therefore fills the buffer with thousands of identical
// denials and then discards the permission change written in the middle of it.
// The denial is one of a million rows that say the same thing; the mutation is
// the only copy of what somebody did. They cannot share a shedding policy, and
// the naive one has it backwards.
//
// The policy is the part of the sink that has to be right under load, so it is
// separated from everything that makes load hard to reproduce: there is no mutex
// here, no thread pool, no client and no clock. AuditService owns the lock and
// the flushing; this owns what may be lost and in what order.
//
// --- classify, then coalesce, then bound ------------------------------------
//
//   1. Two classes, from the application's own table (anvil/audit/action.h). A
//      Change is never dropped while a Traffic row is buffered — the buffer
//      evicts oldest-traffic-first to make room, and refuses a Change only when
//      it holds nothing but Changes, which is a database outage rather than a
//      load problem and is counted as its own thing.
//   2. Traffic coalesces. Consecutive traffic rows sharing (actor, coarsened
//      network, code, stealthed) fold into one row carrying a repeat count. A
//      rate per source is what such rows are read back for, and a count IS the
//      rate.
//   3. Only then, a bound. Coalescing removes most of the pressure the bound
//      would otherwise have to be sized for.
//
// --- and then the boundary the policy used to stop at -----------------------
//
// A REFUSED FLUSH IS RE-ADMITTED, not discarded. AuditService::post used to
// drain this buffer, tally what the batch was carrying and — if audit_pool
// refused it — count the loss and drop the batch, so rows the policy above had
// just protected were lost wholesale. The policy governed admission to the
// buffer and stopped at its boundary.
//
// The comment beside that code reasoned that a refusal means the database has
// been unreachable for minutes. A load run against a live, busy mongod refused
// 8,485 batches, which is exactly the case that reasoning excludes.
//
// So readmit() returns the Change rows to the buffer and charges the Traffic
// rows as the drop. The buffer is already bounded, so re-admission cannot grow
// without limit; a change that does not fit either is dropped and counted
// against its own class, which is the honest end of the policy rather than a
// hole in the middle of it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "anvil/audit/action.h"
#include "anvil/audit/record.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"

namespace anvil::audit {

// What makes two consecutive traffic rows the same event repeated.
//
// The network is COARSENED — identity::coarsen_network's rule, /24 and /48 —
// rather than the full address. A key carrying the full address would leave a
// source with a rotating low byte uncoalesceable, which is precisely the shape
// of the traffic this exists for, and the coarse network is already the unit an
// intrusion view groups by.
struct FoldKey final {
    std::optional<Uuid>          actor;
    std::array<std::uint8_t, 16> network;
    std::int32_t                 action;
    ErrorCode                    code;
    bool                         stealthed;

    [[nodiscard]] bool operator==(const FoldKey& other) const noexcept = default;
};

[[nodiscard]] FoldKey fold_key_of(const AuditEntry& entry) noexcept;

class AuditBuffer final {
public:
    // 8,192 rows. At 256 rows per flush that is 32 flushes of headroom — far
    // more than a working pool needs to drain — so reaching it means the
    // database is genuinely gone rather than busy.
    static constexpr std::size_t kDefaultCapacityRows = 8192;

    enum class Admission : std::uint8_t {
        // Took a free slot.
        Buffered,
        // Folded into the open traffic window. Costs no slot at all, which is
        // what keeps a flood off the bound.
        Coalesced,
        // Buffered, and an older traffic row was discarded to make room. Only a
        // Change row is ever admitted this way.
        Evicted,
        // Nothing could be discarded. The row is lost and counted against its
        // own class.
        Refused,
    };

    // `actions` is the application's table, used only to classify. It must
    // outlive the buffer — in practice it is a constexpr array in .rodata.
    AuditBuffer(std::span<const AuditActionSpec> actions, std::size_t capacity_rows);

    // Never throws, never blocks, never grows past the capacity. `at` is the
    // instant the event HAPPENED and is kept as the row's own — a folded window
    // keeps the FIRST of them, because a window stamped with its last row would
    // report a burst as having started at the moment it ended.
    [[nodiscard]] Admission offer(const AuditEntry& entry, db::TimeMs at);

    // Moves everything out, INCLUDING the open fold window, and closes that
    // window. A fold that outlived its flush would be a repeat added to a row
    // already on disk, under an instant already written.
    void drain(std::vector<AuditRow>& out);

    // Returns a refused flush to the buffer. Changes go back; traffic rows are
    // charged as the drop, because they are the compressible class and
    // returning them would displace the changes this exists to protect.
    //
    // Returns how many rows were re-admitted. `batch` is left empty either way.
    std::size_t readmit(std::vector<AuditRow>& batch);

    // Rows held, counting a folded window as the one row it will be written as.
    [[nodiscard]] std::size_t size() const noexcept {
        return changes_.size() + traffic_.size();
    }

    // Per class, because the two mean different things: lost traffic is a load
    // signal and lost changes are an outage. A single counter would make the
    // second invisible behind the first.
    [[nodiscard]] std::uint64_t dropped_traffic() const noexcept { return dropped_traffic_; }
    [[nodiscard]] std::uint64_t dropped_changes() const noexcept { return dropped_changes_; }

    AuditBuffer(const AuditBuffer&) = delete;
    AuditBuffer& operator=(const AuditBuffer&) = delete;

private:
    // Discards the oldest traffic row and charges every request it stood for.
    // False when there is none to discard.
    [[nodiscard]] bool evict_oldest_traffic() noexcept;

    // Declaration order is construction order, largest first.
    std::span<const AuditActionSpec> actions_;
    const std::size_t                capacity_rows_;
    std::uint64_t                    dropped_traffic_ = 0;
    std::uint64_t                    dropped_changes_ = 0;
    // A deque rather than a vector because readmit() puts rows back at the
    // FRONT: a change that has already waited out one failed flush is older than
    // everything else held, and the next flush should carry it first.
    std::deque<AuditRow>             changes_;
    // Evicted from the FRONT and appended at the back, which is what a deque is
    // for: the alternative is erasing the first element of a vector holding
    // eight thousand rows, once per admitted change, under exactly the load this
    // class exists to survive.
    std::deque<AuditRow>             traffic_;
    // The back of traffic_ is open for folding under this key. Absent when the
    // buffer holds no traffic row, or when a flush has just closed the window.
    std::optional<FoldKey>           open_;
};

}  // namespace anvil::audit
