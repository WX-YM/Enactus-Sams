#include "anvil/audit/buffer.h"

#include <limits>
#include <utility>

#include "anvil/analytics/counters.h"
#include "anvil/identity/session_service.h"

namespace anvil::audit {

FoldKey fold_key_of(const AuditEntry& entry) noexcept {
    return FoldKey{
        .actor = entry.actor,
        // The COARSE network, not the address. identity::coarsen_network is the
        // one place "same source" is decided in this system, shared with the
        // session listing so a log line and a screen cannot come to disagree
        // about what a source is.
        .network = identity::coarsen_network(entry.ip),
        .action = entry.action.stored(),
        .code = entry.code,
        .stealthed = entry.stealthed,
    };
}

AuditBuffer::AuditBuffer(std::span<const AuditActionSpec> actions, std::size_t capacity_rows)
    : actions_{actions},
      capacity_rows_{capacity_rows},
      dropped_traffic_{0},
      dropped_changes_{0},
      changes_{},
      traffic_{},
      open_{} {}

bool AuditBuffer::evict_oldest_traffic() noexcept {
    if (traffic_.empty()) { return false; }
    // Charged for every request it stood for, not for one row. A fold of four
    // thousand denials counted as a single drop would understate the loss by
    // exactly the ratio the coalescer achieved — which is how a million
    // discarded rows comes to read as a few thousand in a metric.
    dropped_traffic_ += traffic_.front().repeats;
    // Reported as a METRIC and not as a line, because this fires once per
    // admitted change under exactly the flood that makes it interesting
    // (docs/00-architecture.md §9). The stage matters: a full buffer is the sink
    // outrunning its pool, which is a different failure from a refused flush.
    analytics::count_many(analytics::Internal::AuditRowsDropped,
                          analytics::AuditDropClass::Traffic,
                          analytics::AuditDropStage::Buffer, traffic_.front().repeats);
    traffic_.pop_front();
    // The open window is the BACK of the queue, so evicting from the front
    // closes it only when it was also the last row left.
    if (traffic_.empty()) { open_.reset(); }
    return true;
}

AuditBuffer::Admission AuditBuffer::offer(const AuditEntry& entry, db::TimeMs at) {
    if (audit_class_of(actions_, entry.action) == AuditClass::Change) {
        if (size() < capacity_rows_) {
            changes_.push_back(AuditRow{entry, at});
            return Admission::Buffered;
        }
        // Deliberately BEFORE the refusal: a change is never dropped while
        // anything compressible is still held, and only a buffer holding nothing
        // but changes can refuse one. That is a database outage rather than a
        // load problem, which is why the two counters are separate.
        if (evict_oldest_traffic()) {
            changes_.push_back(AuditRow{entry, at});
            return Admission::Evicted;
        }
        ++dropped_changes_;
        analytics::count(analytics::Internal::AuditRowsDropped,
                         analytics::AuditDropClass::Change,
                         analytics::AuditDropStage::Buffer);
        return Admission::Refused;
    }

    const FoldKey key = fold_key_of(entry);
    if (open_.has_value() && *open_ == key) {
        AuditRow& window = traffic_.back();
        // SATURATING, because an unsigned wrap here would report a flood as a
        // handful of requests — the one reading an auditor has to be able to
        // trust. One flush window cannot reach this, and a count that stopped
        // counting is still true about the direction.
        if (window.repeats < std::numeric_limits<std::uint32_t>::max()) { ++window.repeats; }
        return Admission::Coalesced;
    }

    if (size() >= capacity_rows_) {
        // The ARRIVING traffic row is the one that goes, not an older one. The
        // earliest evidence of a flood is worth more than its newest identical
        // copy, and a traffic row that displaced a traffic row would churn the
        // buffer without saving a single row.
        ++dropped_traffic_;
        analytics::count(analytics::Internal::AuditRowsDropped,
                         analytics::AuditDropClass::Traffic,
                         analytics::AuditDropStage::Buffer);
        return Admission::Refused;
    }
    traffic_.push_back(AuditRow{entry, at});
    open_ = key;
    return Admission::Buffered;
}

std::size_t AuditBuffer::readmit(std::vector<AuditRow>& batch) {
    std::size_t returned = 0;
    // Walked BACKWARDS so push_front restores the batch's own order: these rows
    // are older than anything still held, and the next flush should carry them
    // first.
    for (std::size_t i = batch.size(); i-- > 0;) {
        AuditRow& row = batch[i];
        if (audit_class_of(actions_, row.entry.action) != AuditClass::Change) {
            // The compressible class is charged as the drop. Returning it would
            // displace the changes this whole mechanism exists to protect, and
            // a rate per source survives losing a window of it in a way that
            // "somebody changed a permission" does not.
            dropped_traffic_ += row.repeats;
            analytics::count_many(analytics::Internal::AuditRowsDropped,
                                  analytics::AuditDropClass::Traffic,
                                  analytics::AuditDropStage::Flush, row.repeats);
            continue;
        }
        if (size() >= capacity_rows_ && !evict_oldest_traffic()) {
            // The honest end of the policy rather than a hole in the middle of
            // it: the buffer is full of changes and this one does not fit.
            dropped_changes_ += row.repeats;
            analytics::count_many(analytics::Internal::AuditRowsDropped,
                                  analytics::AuditDropClass::Change,
                                  analytics::AuditDropStage::Flush, row.repeats);
            continue;
        }
        changes_.push_front(std::move(row));
        ++returned;
    }
    batch.clear();
    return returned;
}

void AuditBuffer::drain(std::vector<AuditRow>& out) {
    out.clear();
    out.reserve(size());
    for (AuditRow& row : changes_) { out.push_back(std::move(row)); }
    for (AuditRow& row : traffic_) { out.push_back(std::move(row)); }
    changes_.clear();
    traffic_.clear();
    // The window goes out WITH the flush. A fold that outlived it would add a
    // repeat to a row already written, under an instant already on disk.
    open_.reset();
}

}  // namespace anvil::audit
