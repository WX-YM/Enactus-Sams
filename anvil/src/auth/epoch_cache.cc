#include "anvil/auth/epoch_cache.h"

#include <algorithm>

namespace anvil::auth {
namespace {

// The slot index must depend on ALL 16 bytes. A v7 id carries a 48-bit
// millisecond prefix, so two users created in the same millisecond share their
// first six bytes; indexing on the prefix would collide them into one slot and
// thrash. Mixing both halves with a 64-bit multiplier spreads the low entropy
// across the whole word.
constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ULL;

[[nodiscard]] std::uint64_t load_le64(const Uuid& id, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(id[offset + i]) << (i * 8);
    }
    return value;
}

}  // namespace

EpochCache::EpochCache(std::chrono::milliseconds ttl) noexcept
    : ttl_{ttl}, shards_{}, slots_{}, hits_{0}, misses_{0} {}

std::size_t EpochCache::slot_of(const Uuid& user_id) noexcept {
    std::uint64_t mixed = load_le64(user_id, 0) * kMix;
    mixed ^= load_le64(user_id, 8) * kMix;
    mixed ^= mixed >> 29U;
    mixed *= kMix;
    mixed ^= mixed >> 32U;
    return static_cast<std::size_t>(mixed) & (kSlots - 1);
}

std::optional<std::uint64_t> EpochCache::get(const Uuid& user_id,
                                             Clock::time_point now) const noexcept {
    const std::size_t index = slot_of(user_id);
    const std::lock_guard<std::mutex> lock{shards_[index % kShards]};

    const Slot& slot = slots_[index];
    // Identity is checked before expiry: a slot holding a DIFFERENT user is a
    // miss regardless of how fresh it is, and comparing the id first means a
    // collision never returns another user's epoch even briefly.
    if (!slot.occupied || slot.user_id != user_id || slot.expires_at <= now) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    hits_.fetch_add(1, std::memory_order_relaxed);
    return slot.epoch;
}

void EpochCache::put(const Uuid& user_id, std::uint64_t epoch, Clock::time_point now) noexcept {
    const std::size_t index = slot_of(user_id);
    const std::lock_guard<std::mutex> lock{shards_[index % kShards]};

    Slot& slot = slots_[index];
    slot.epoch = epoch;
    slot.expires_at = now + ttl_;
    slot.user_id = user_id;
    slot.occupied = true;
}

void EpochCache::invalidate(const Uuid& user_id) noexcept {
    const std::size_t index = slot_of(user_id);
    const std::lock_guard<std::mutex> lock{shards_[index % kShards]};

    Slot& slot = slots_[index];
    // Only if the slot still belongs to this user: clearing another user's
    // entry is harmless for correctness but costs them a Redis round trip.
    if (slot.occupied && slot.user_id == user_id) { slot.occupied = false; }
}

void EpochCache::clear() noexcept {
    for (std::size_t shard = 0; shard < kShards; ++shard) {
        const std::lock_guard<std::mutex> lock{shards_[shard]};
        for (std::size_t index = shard; index < kSlots; index += kShards) {
            slots_[index].occupied = false;
        }
    }
}

}  // namespace anvil::auth
