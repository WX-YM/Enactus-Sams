#pragma once

// The local half of the revocation channel (docs/04-access-control.md §5).
//
// Access tokens carry the perm_epoch they were minted with. The filter compares
// that against the authoritative epoch, so a permission change takes effect in
// seconds rather than at token expiry. Reading the authority on
// every request would put a Redis round trip on the hottest path in the system,
// so the authority is cached locally with a short TTL: the common case is a
// load and a compare, and only a cold or recently-changed user costs a GET.
//
// Three properties are load-bearing:
//
//   * FIXED CAPACITY. This is a cache on a security path, fed by a user id an
//     attacker chooses. An unbounded map is a memory-exhaustion vector reachable
//     with forged tokens — and the entries would be for users that do not
//     exist. Direct-mapped over a fixed slot array means the memory cost is
//     decided at compile time and a flood evicts rather than grows.
//   * SHORT TTL. The TTL is the revocation latency: a permission removed now is
//     honoured everywhere within it. ~10 s (docs/05-auth-sessions.md §1).
//   * FAIL CLOSED. A miss is not an allow. It means "ask the authority", and if
//     the authority cannot be reached the request is denied: a revocation
//     channel nobody can read is not permission to skip revocation.
//
// Direct-mapped rather than LRU on purpose: an LRU needs a list splice per hit,
// which is two writes to shared memory on every authorized request. A
// direct-mapped slot is one compare and, on a hit, no write at all.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "anvil/core/types.h"

namespace anvil::auth {

// Revocation latency for a user whose epoch is already cached. Matches the
// ~10 s the design budgets for a permission change to propagate.
inline constexpr std::chrono::milliseconds kDefaultEpochTtl{10'000};

class EpochCache final {
public:
    using Clock = std::chrono::steady_clock;

    // Power of two, so slot selection is a mask rather than a division. 1024
    // slots x 40 bytes is 40 KiB — small enough to be per-process and large
    // enough that a real staff population never collides meaningfully.
    static constexpr std::size_t kSlots = 1024;
    // Enough shards that concurrent lookups for different users rarely contend,
    // few enough that the mutex array stays in cache.
    static constexpr std::size_t kShards = 64;

    static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");
    static_assert(kSlots % kShards == 0);

    explicit EpochCache(std::chrono::milliseconds ttl = kDefaultEpochTtl) noexcept;

    // nullopt on a miss, on an expired entry, or when another user occupies the
    // slot. Never allocates.
    [[nodiscard]] std::optional<std::uint64_t> get(const Uuid& user_id,
                                                   Clock::time_point now) const noexcept;

    // Overwrites whatever the slot held. Eviction is the point: the alternative
    // is growth an attacker controls.
    void put(const Uuid& user_id, std::uint64_t epoch, Clock::time_point now) noexcept;

    // Drops the entry for one user, so a bump made by THIS instance takes
    // effect immediately rather than at the TTL. Other instances still wait for
    // their own TTL — that is the documented revocation latency, not a bug.
    void invalidate(const Uuid& user_id) noexcept;

    // Empties every slot. For tests and for a configuration reload.
    void clear() noexcept;

    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return kSlots; }
    [[nodiscard]] std::chrono::milliseconds ttl() const noexcept { return ttl_; }

    // Hit and miss counts, for the authz-cache-hit-rate metric
    // (docs/00-architecture.md §9). Relaxed: a metric that is off by one under
    // contention is fine, and making it exact would add a fence to every hit.
    [[nodiscard]] std::uint64_t hits() const noexcept {
        return hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t misses() const noexcept {
        return misses_.load(std::memory_order_relaxed);
    }

    EpochCache(const EpochCache&) = delete;
    EpochCache& operator=(const EpochCache&) = delete;

private:
    // 40 bytes, ordered largest-alignment-first so there is no interior padding
    // (ENGINEERING_RULES.md §2.3).
    struct Slot final {
        std::uint64_t     epoch;
        Clock::time_point expires_at;
        Uuid              user_id;
        bool              occupied;
    };

    [[nodiscard]] static std::size_t slot_of(const Uuid& user_id) noexcept;

    // Declaration order is construction order and the members below are read by
    // every lookup, so the immutable ones come first.
    const std::chrono::milliseconds ttl_;
    mutable std::array<std::mutex, kShards> shards_;
    std::array<Slot, kSlots>                slots_;
    mutable std::atomic<std::uint64_t>      hits_;
    mutable std::atomic<std::uint64_t>      misses_;
};

}  // namespace anvil::auth
