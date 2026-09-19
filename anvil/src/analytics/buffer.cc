#include "anvil/analytics/buffer.h"

#include <limits>
#include <utility>

#include "anvil/analytics/counters.h"

namespace anvil::analytics {

EventBuffer::EventBuffer(std::span<const EventSpec> events, std::size_t capacity_rows)
    : events_{events},
      capacity_rows_{capacity_rows},
      dropped_behaviour_{0},
      dropped_conversions_{0},
      conversions_{},
      behaviour_{},
      open_{} {}

bool EventBuffer::evict_oldest_behaviour() noexcept {
    if (behaviour_.empty()) { return false; }
    // Charged for every event it stood for, not for one row. A fold of four
    // thousand page views counted as a single drop understates the loss by
    // exactly the ratio the coalescer achieved — which is how a million
    // discarded rows comes to read as a few thousand in a metric.
    dropped_behaviour_ += behaviour_.front().repeats;
    behaviour_.pop_front();
    // The open window is the BACK of the queue, so evicting from the front
    // closes it only when it was also the last row left.
    if (behaviour_.empty()) { open_.reset(); }
    return true;
}

EventBuffer::Admission EventBuffer::offer(const Event& event, db::TimeMs at) {
    if (event_class_of(events_, event.code) == EventClass::Conversion) {
        if (size() < capacity_rows_) {
            conversions_.push_back(EventRow{at, event, 1});
            return Admission::Buffered;
        }
        // Deliberately BEFORE the refusal: a conversion is never dropped while
        // anything compressible is still held, and only a buffer holding nothing
        // but conversions can refuse one.
        if (evict_oldest_behaviour()) {
            conversions_.push_back(EventRow{at, event, 1});
            return Admission::Evicted;
        }
        ++dropped_conversions_;
        return Admission::Refused;
    }

    const EventFoldKey key = fold_key_of(event);
    if (open_.has_value() && *open_ == key) {
        EventRow& window = behaviour_.back();
        // SATURATING, because an unsigned wrap here would report a refresh storm
        // as a handful of visits — the one reading this collection exists to
        // produce. A count that stopped counting is still true about the
        // direction.
        if (window.repeats < std::numeric_limits<std::uint32_t>::max()) { ++window.repeats; }
        return Admission::Coalesced;
    }

    if (size() >= capacity_rows_) {
        // The ARRIVING behaviour row is the one that goes, not an older one. A
        // behaviour row that displaced a behaviour row would churn the buffer
        // without saving a single row.
        ++dropped_behaviour_;
        return Admission::Refused;
    }
    behaviour_.push_back(EventRow{at, event, 1});
    open_ = key;
    return Admission::Buffered;
}

void EventBuffer::drain(std::vector<EventRow>& out) {
    out.clear();
    out.reserve(size());
    for (EventRow& row : conversions_) { out.push_back(row); }
    for (EventRow& row : behaviour_) { out.push_back(row); }
    conversions_.clear();
    behaviour_.clear();
    // The window goes out WITH the flush. A fold that outlived it would add a
    // repeat to a row already written, under an instant already on disk.
    open_.reset();
}

std::size_t EventBuffer::readmit(std::vector<EventRow>& batch) {
    std::size_t returned = 0;
    // Walked BACKWARDS so push_front restores the batch's own order: these rows
    // are older than anything still held, and the next flush should carry them
    // first.
    for (std::size_t i = batch.size(); i-- > 0;) {
        EventRow& row = batch[i];
        if (event_class_of(events_, row.event.code) != EventClass::Conversion) {
            // The compressible class is charged as the drop. Returning it would
            // displace the conversions this whole mechanism exists to protect,
            // which is the failure the audit sink's version has in a different
            // place.
            dropped_behaviour_ += row.repeats;
            continue;
        }
        if (size() >= capacity_rows_ && !evict_oldest_behaviour()) {
            // The honest end of the policy rather than a hole in the middle of
            // it: the buffer is full of conversions and this one does not fit.
            dropped_conversions_ += row.repeats;
            continue;
        }
        conversions_.push_front(row);
        ++returned;
    }
    batch.clear();
    return returned;
}

}  // namespace anvil::analytics
