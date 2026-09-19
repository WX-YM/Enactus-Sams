#include "anvil/notifications/sse.h"

#include <algorithm>
#include <utility>


namespace anvil::notifications {
namespace {

constexpr std::string_view kStreamField = "stream";

}  // namespace

// --- SseStream ---------------------------------------------------------------

bool SseStream::push(const SseEvent& event) noexcept {
    std::function<void(StreamId)> notify;
    {
        const std::lock_guard<std::mutex> guard{mutex_};
        if (closed_) { return false; }
        // FULL, and therefore refused. Overwriting the oldest slot would keep the
        // connection up while the client silently missed something; refusing
        // hands the caller a condition it already knows how to handle.
        if (count_ >= ring_.size()) { return false; }

        SseEvent& slot = ring_[head_];
        slot = event;
        ++sequence_;
        slot.sequence = sequence_;
        head_ = (head_ + 1) % ring_.size();
        ++count_;
        notify = notify_;
    }
    // OUTSIDE the lock. The callback posts a drain onto an event loop, and a
    // loop that happened to be busy would otherwise hold this stream's mutex for
    // as long as it took — with a delivery worker waiting on it.
    if (notify) { notify(id_); }
    return true;
}

std::size_t SseStream::drain(std::span<SseEvent> out) noexcept {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::size_t taken = std::min(out.size(), count_);
    // The tail is derived rather than stored: with head and count both known,
    // a third index is a third thing that can disagree with the other two.
    const std::size_t tail = (head_ + ring_.size() - count_) % ring_.size();
    for (std::size_t i = 0; i < taken; ++i) { out[i] = ring_[(tail + i) % ring_.size()]; }
    count_ -= taken;
    return taken;
}

void SseStream::close() noexcept {
    const std::lock_guard<std::mutex> guard{mutex_};
    closed_ = true;
    // Dropped deliberately. Whatever was queued is in the inbox, which is the
    // system of record; holding it would keep a ring alive for a connection that
    // is gone.
    count_ = 0;
}

bool SseStream::closed() const noexcept {
    const std::lock_guard<std::mutex> guard{mutex_};
    return closed_;
}

std::size_t SseStream::queued() const noexcept {
    const std::lock_guard<std::mutex> guard{mutex_};
    return count_;
}

// --- the ceiling -------------------------------------------------------------


// --- SseHub ------------------------------------------------------------------

SseHub::SseHub(SseLimits limits) noexcept
    : limits_{limits},
      ceiling_{limits.max_streams != 0 ? limits.max_streams : descriptor_ceiling(kStreamShare)} {}

Result<StreamPtr> SseHub::open(const Uuid& reader) {
    const std::lock_guard<std::shared_mutex> guard{mutex_};

    // RateLimited and not ServiceUnavailable: the request is refused because
    // there are too many of exactly this kind right now, and the client should
    // back off rather than treat the whole service as down.
    if (by_id_.size() >= ceiling_) { return fail(ErrorCode::RateLimited, kStreamField); }

    std::vector<StreamPtr>& mine = by_reader_[reader];
    // Per reader, so a hundred tabs on one account cannot hold the process
    // ceiling open and shut every other reader out.
    if (mine.size() >= limits_.max_per_reader) {
        // Erase an entry this call created, so a refused open does not leave an
        // empty vector behind for every reader who ever hit the limit.
        if (mine.empty()) { by_reader_.erase(reader); }
        return fail(ErrorCode::RateLimited, kStreamField);
    }

    const StreamId id = next_id_;
    ++next_id_;
    StreamPtr stream = std::make_shared<SseStream>(id, reader);
    mine.push_back(stream);
    by_id_.emplace(id, stream);
    return stream;
}

void SseHub::close(StreamId id) noexcept {
    StreamPtr stream;
    {
        const std::lock_guard<std::shared_mutex> guard{mutex_};
        const auto found = by_id_.find(id);
        // Closed twice — once by the client disconnecting and once by the writer
        // noticing — is the normal case rather than an error.
        if (found == by_id_.end()) { return; }
        stream = found->second;
        by_id_.erase(found);

        const auto siblings = by_reader_.find(stream->reader());
        if (siblings != by_reader_.end()) {
            std::vector<StreamPtr>& mine = siblings->second;
            mine.erase(std::remove_if(mine.begin(), mine.end(),
                                      [id](const StreamPtr& other) {
                                          return other->id() == id;
                                      }),
                       mine.end());
            // A reader with no streams leaves no entry. Otherwise the map grows
            // by one node per account that has ever connected and never shrinks.
            if (mine.empty()) { by_reader_.erase(siblings); }
        }
    }
    // Outside the lock: close() takes the stream's own mutex, and taking it while
    // holding the registry's is the ordering that could deadlock against a
    // concurrent push waking a writer that is trying to close.
    stream->close();
}

std::vector<StreamPtr> SseHub::streams_of(const Uuid& reader) const {
    const std::shared_lock<std::shared_mutex> guard{mutex_};
    const auto found = by_reader_.find(reader);
    if (found == by_reader_.end()) { return {}; }
    // COPIED out under the lock. The pushes below happen without it, and a
    // reference into the map would be invalidated by a concurrent close.
    return found->second;
}

std::size_t SseHub::deliver(const Uuid& reader, const SseEvent& event) {
    const std::vector<StreamPtr> streams = streams_of(reader);
    std::size_t accepted = 0;
    for (const StreamPtr& stream : streams) {
        if (stream->push(event)) {
            ++accepted;
            continue;
        }
        // A full ring, or a stream that closed while we were not holding the
        // registry lock. Either way this connection is finished: dropping it is
        // safe because the inbox is the system of record and the client re-reads
        // it on reconnect.
        close(stream->id());
    }
    return accepted;
}

std::size_t SseHub::broadcast_ping(std::int32_t unread) {
    std::vector<StreamPtr> streams;
    {
        const std::shared_lock<std::shared_mutex> guard{mutex_};
        streams.reserve(by_id_.size());
        for (const auto& [id, stream] : by_id_) { streams.push_back(stream); }
    }

    SseEvent ping{};
    ping.type = SseEventKind::Ping;
    ping.unread = unread;

    std::size_t accepted = 0;
    for (const StreamPtr& stream : streams) {
        if (stream->push(ping)) {
            ++accepted;
            continue;
        }
        // A ping is the cheapest possible event, so a ring too full to take one
        // is a connection that has not been drained in a very long time. It is
        // exactly what a keepalive sweep is for.
        close(stream->id());
    }
    return accepted;
}

std::size_t SseHub::open_streams() const noexcept {
    const std::shared_lock<std::shared_mutex> guard{mutex_};
    return by_id_.size();
}

}  // namespace anvil::notifications
