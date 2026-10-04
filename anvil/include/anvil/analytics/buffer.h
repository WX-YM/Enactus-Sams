#pragma once

// What the event sink holds between flushes, and the policy that decides which
// row goes when it cannot hold any more.
//
// --- why this is a SECOND TYPE and not a shared one -------------------------
//
// It is anvil/audit/buffer.h's policy a second time, deliberately. The two sinks
// have the same SHAPE and different STAKES: an analytics flood must never be
// able to evict an audit row, and a shared buffer is one refactor away from
// letting it. Sharing the type would save a hundred lines and put the forensic
// record behind the same bound as a page-view counter.
//
// --- classify, then coalesce, then bound ------------------------------------
//
//   1. Two classes, from the application's own table (anvil/analytics/event_spec.h).
//      A Conversion is never dropped while a Behaviour row is buffered — the
//      buffer evicts oldest-behaviour-first to make room, and refuses a
//      Conversion only when it holds nothing else. That is counted as its own
//      thing rather than folded into a drop total, because it is the state worth
//      alerting on.
//   2. Behaviour coalesces. Consecutive rows sharing (session, code, dimensions)
//      fold into one row carrying a repeat count. A refresh storm is one row,
//      and a count IS the rate.
//   3. Only then, a bound. Coalescing removes most of the pressure the bound
//      would otherwise have to be sized for.
//
// --- the correction that came from the audit sink's shape -------------------
//
// A REFUSED FLUSH IS RE-ADMITTED, not discarded. AuditService::post used to
// drain its buffer, tally what it was carrying, and — if the pool refused —
// count the loss and drop the batch, so rows the classify-and-shed policy had
// just protected were lost wholesale. The comment beside that code reasoned that
// a refusal means the database has been unreachable for minutes; a load run
// measured 8,485 refusals against a live, busy mongod, which is exactly the case
// that reasoning excluded. This shipped with the correction rather than the
// defect, and the audit sink has since been corrected to match.
//
// So readmit() returns the Conversion rows to the buffer and charges the
// Behaviour rows as the drop. The buffer is already bounded, so re-admission
// cannot grow without limit; if the conversions do not fit either they are
// dropped and counted against their own class, which is the honest end of the
// policy rather than a hole in the middle of it.
//
// Like the audit buffer, this owns WHAT MAY BE LOST and nothing else: no mutex,
// no thread pool, no client and no clock. EventSink owns those.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "anvil/analytics/event.h"
#include "anvil/analytics/event_spec.h"
#include "anvil/db/codec.h"

namespace anvil::analytics {

class EventBuffer final {
public:
    // 8,192 rows, which at 72 bytes each is a little over half a megabyte. At
    // 512 rows per flush that is sixteen flushes of headroom in front of a sink
    // that flushes every second.
    static constexpr std::size_t kDefaultCapacityRows = 8192;

    enum class Admission : std::uint8_t {
        // Took a free slot.
        Buffered,
        // Folded into the open behaviour window. Costs no slot at all, which is
        // what keeps a refresh storm off the bound.
        Coalesced,
        // Buffered, and an older behaviour row was discarded to make room. Only
        // a Conversion is ever admitted this way.
        Evicted,
        // Nothing could be discarded. The row is lost and counted against its
        // own class.
        Refused,
    };

    // `events` is the application's table, used only to classify. It must
    // outlive the buffer — in practice it is a constexpr array in .rodata.
    EventBuffer(std::span<const EventSpec> events, std::size_t capacity_rows);

    // Never throws, never blocks, never grows past the capacity. `at` is the
    // instant the event HAPPENED; a folded window keeps the FIRST of them,
    // because a window stamped with its last row would report a burst as having
    // started at the moment it ended.
    [[nodiscard]] Admission offer(const Event& event, db::TimeMs at);

    // Moves everything out, INCLUDING the open fold window, and closes that
    // window. A fold that outlived its flush would be a repeat added to a row
    // already on disk, under an instant already written.
    void drain(std::vector<EventRow>& out);

    // Returns a refused flush to the buffer. Conversions go back; behaviour rows
    // are charged as the drop, because they are the compressible class and
    // returning them would displace the conversions this exists to protect.
    //
    // Returns how many rows were re-admitted. `batch` is left empty either way.
    std::size_t readmit(std::vector<EventRow>& batch);

    // Rows held, counting a folded window as the one row it will be written as.
    [[nodiscard]] std::size_t size() const noexcept {
        return conversions_.size() + behaviour_.size();
    }

    // Per class, because the two mean different things. Lost behaviour is a load
    // signal — compressible rows the coalescer could not fold fast enough. A
    // lost conversion means the buffer held nothing compressible at all, which
    // is the state worth paging somebody about, and a single counter would hide
    // it behind the first.
    [[nodiscard]] std::uint64_t dropped_behaviour() const noexcept {
        return dropped_behaviour_;
    }
    [[nodiscard]] std::uint64_t dropped_conversions() const noexcept {
        return dropped_conversions_;
    }

    EventBuffer(const EventBuffer&) = delete;
    EventBuffer& operator=(const EventBuffer&) = delete;

private:
    // Discards the oldest behaviour row and charges every event it stood for.
    // False when there is none to discard.
    [[nodiscard]] bool evict_oldest_behaviour() noexcept;

    // Declaration order is construction order, largest first.
    std::span<const EventSpec> events_;
    const std::size_t          capacity_rows_;
    std::uint64_t              dropped_behaviour_ = 0;
    std::uint64_t              dropped_conversions_ = 0;
    // A deque rather than a vector because readmit() puts rows back at the
    // FRONT: a conversion that has already waited out one failed flush is older
    // than everything else held, and the next flush should carry it first.
    std::deque<EventRow>       conversions_;
    // Evicted from the FRONT and appended at the back, which is what a deque is
    // for: the alternative is erasing the first element of a vector holding
    // eight thousand rows, once per admitted conversion, under exactly the load
    // this class exists to survive.
    std::deque<EventRow>       behaviour_;
    // The back of behaviour_ is open for folding under this key. Absent when the
    // buffer holds no behaviour row, or when a flush has just closed the window.
    std::optional<EventFoldKey> open_;
};

}  // namespace anvil::analytics
