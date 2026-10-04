#include "anvil/chat/hub.h"

#include <algorithm>
#include <utility>

namespace anvil::chat {

// --- ChatSocket -------------------------------------------------------------------

bool ChatSocket::push(std::span<const std::uint8_t> frame) noexcept {
    if (frame.empty() || frame.size() > frames::kMaxDownstreamFrameBytes) { return false; }
    const std::size_t record = kSocketRecordHeaderBytes + frame.size();
    bool was_empty = false;
    {
        const std::lock_guard lock{mutex_};
        if (reason_ != CloseReason::None) { return false; }
        // FULL, and therefore refused. Overwriting the oldest frame would keep
        // the socket up while its client silently missed a wake.
        if (ring_.size() - count_ < record) { return false; }
        was_empty = count_ == 0;
        const auto put = [this](std::uint8_t byte) noexcept {
            ring_[head_] = byte;
            head_ = (head_ + 1) % ring_.size();
        };
        put(static_cast<std::uint8_t>(frame.size() >> 8));
        put(static_cast<std::uint8_t>(frame.size() & 0xFF));
        for (const std::uint8_t byte : frame) { put(byte); }
        count_ += record;
    }
    // OUTSIDE the lock: the callback posts onto a loop, and a busy loop must
    // not hold this socket's mutex with a delivery thread waiting on it.
    if (was_empty && notify_) { notify_(id_); }
    return true;
}

std::size_t ChatSocket::drain(std::span<std::uint8_t, kSocketRingBytes> out) noexcept {
    const std::lock_guard lock{mutex_};
    // The tail is derived rather than stored, as SseStream's is.
    const std::size_t tail = (head_ + ring_.size() - count_) % ring_.size();
    for (std::size_t i = 0; i < count_; ++i) { out[i] = ring_[(tail + i) % ring_.size()]; }
    const std::size_t taken = count_;
    count_ = 0;
    return taken;
}

bool ChatSocket::close(CloseReason reason) noexcept {
    {
        const std::lock_guard lock{mutex_};
        if (reason_ != CloseReason::None) { return false; }
        reason_ = reason == CloseReason::None ? CloseReason::Gone : reason;
        // Dropped deliberately: the log is the record, and a ring held for a
        // socket that is gone is memory nobody reads.
        count_ = 0;
    }
    // The writer learns of a close the same way it learns of a frame, so a
    // socket the hub closed is shut by its own loop.
    if (notify_) { notify_(id_); }
    return true;
}

bool ChatSocket::closed() const noexcept {
    const std::lock_guard lock{mutex_};
    return reason_ != CloseReason::None;
}

CloseReason ChatSocket::close_reason() const noexcept {
    const std::lock_guard lock{mutex_};
    return reason_;
}

std::size_t ChatSocket::queued_bytes() const noexcept {
    const std::lock_guard lock{mutex_};
    return count_;
}

// --- ChatHub -----------------------------------------------------------------------

ChatHub::ChatHub(HubLimits limits, HubSubscriptions subscriptions)
    : limits_{limits},
      ceiling_{limits.max_sockets != 0 ? limits.max_sockets
                                       : descriptor_ceiling(kUpgradeShare)},
      subscriptions_{std::move(subscriptions)} {}

Result<SocketPtr> ChatHub::open(const Uuid& user, const Uuid& device,
                                std::function<void(SocketId)> notify) {
    SocketPtr replaced;
    SocketPtr opened;
    {
        const std::lock_guard lock{mutex_};
        std::vector<SocketPtr>& mine = by_user_[user];
        const auto same = std::find_if(mine.begin(), mine.end(), [&device](const SocketPtr& s) {
            return s->device() == device;
        });
        const bool replacing = same != mine.end();
        // A replacement frees the slot it takes, so neither ceiling applies to
        // it; otherwise a client whose socket got stuck could not replace it
        // while the process was full.
        if (!replacing) {
            // RateLimited, not ServiceUnavailable, for SseHub's reason: too
            // many of exactly this kind, and the client should back off.
            if (by_id_.size() >= ceiling_ || mine.size() >= limits_.max_per_account) {
                if (mine.empty()) { by_user_.erase(user); }
                return fail(ErrorCode::RateLimited);
            }
        }
        const SocketId id = next_id_++;
        opened = std::make_shared<ChatSocket>(id, user, device);
        opened->on_ready(std::move(notify));
        if (replacing) {
            replaced = *same;
            by_id_.erase(replaced->id());
            *same = opened;
        } else {
            // The user's first socket here. Under the lock; see the header.
            if (mine.empty() && subscriptions_.subscribe) { subscriptions_.subscribe(user); }
            mine.push_back(opened);
        }
        by_id_.emplace(id, opened);
    }
    if (replaced) { replaced->close(CloseReason::Replaced); }
    return opened;
}

void ChatHub::close(SocketId id, CloseReason reason) noexcept {
    SocketPtr socket;
    {
        const std::lock_guard lock{mutex_};
        const auto found = by_id_.find(id);
        if (found == by_id_.end()) { return; }
        socket = found->second;
        by_id_.erase(found);
        const auto siblings = by_user_.find(socket->user());
        if (siblings != by_user_.end()) {
            std::vector<SocketPtr>& mine = siblings->second;
            std::erase_if(mine, [id](const SocketPtr& other) { return other->id() == id; });
            // A user with no sockets leaves no entry and no subscription.
            if (mine.empty()) {
                by_user_.erase(siblings);
                if (subscriptions_.unsubscribe) {
                    // noexcept, and the subscriber's call throws only
                    // bad_alloc: a missed unsubscribe leaves wakes arriving for
                    // nobody, which the subscriber drops, so it is logged
                    // nowhere and costs a little bandwidth.
                    try {
                        subscriptions_.unsubscribe(socket->user());
                    } catch (...) {
                    }
                }
            }
        }
    }
    // Outside the lock, so the registry's mutex is never held while a
    // socket's is taken.
    if (reason == CloseReason::Overflow) {
        overflowed_.fetch_add(1, std::memory_order_relaxed);
    }
    socket->close(reason);
}

std::vector<SocketPtr> ChatHub::sockets_of(const Uuid& user) const {
    const std::shared_lock lock{mutex_};
    const auto found = by_user_.find(user);
    if (found == by_user_.end()) { return {}; }
    // COPIED under the lock: the pushes happen without it, and a reference
    // into the map would be invalidated by a concurrent close.
    return found->second;
}

std::size_t ChatHub::deliver(const Uuid& user, std::span<const std::uint8_t> frame) {
    std::size_t accepted = 0;
    for (const SocketPtr& socket : sockets_of(user)) {
        if (socket->push(frame)) {
            ++accepted;
            continue;
        }
        // A full ring, or a socket that closed while the lock was not held.
        // Either way it is finished, and closing a closed one is a no-op.
        close(socket->id(), CloseReason::Overflow);
    }
    return accepted;
}

std::size_t ChatHub::resync(const Uuid& user) {
    std::array<std::uint8_t, frames::kSyncBytes> sync{};
    const std::size_t written = frames::encode(frames::Sync{}, sync);
    return deliver(user, std::span<const std::uint8_t>{sync.data(), written});
}

bool ChatHub::has_socket(const Uuid& user) const {
    const std::shared_lock lock{mutex_};
    return by_user_.contains(user);
}

std::size_t ChatHub::open_sockets() const noexcept {
    const std::shared_lock lock{mutex_};
    return by_id_.size();
}

std::uint64_t ChatHub::overflowed() const noexcept {
    return overflowed_.load(std::memory_order_relaxed);
}

}  // namespace anvil::chat
