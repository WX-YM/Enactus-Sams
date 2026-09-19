#pragma once

// Permission resolution and the perm_epoch revocation channel.
//
// Putting permissions inside the access token is what makes authorisation cost
// zero database round trips (docs/00-architecture.md §4, steps 4-6). It also
// means a token minted before a permission change keeps the old permissions
// until it expires. With a long-lived privileged token that is unacceptable:
// removing somebody's access would leave them holding it for days.
//
// The fix is a monotonic counter per user. Every access token carries the epoch
// it was minted with; the filter compares that against the authority; a
// mismatch denies immediately and the refresh endpoint re-mints with fresh
// permissions. Revocation latency becomes one cache TTL instead of one token
// lifetime.
//
// Three tiers, and which one answers is the whole performance story:
//
//   local EpochCache   ~10 s TTL. Pure CPU, no allocation. The common case.
//   Redis mirror       1 h TTL. One GET on a local miss.
//   MongoDB users      the authority. Read only when the mirror has expired,
//                      which is at most once per user per hour per instance.
//
// FAIL CLOSED. A Redis error is a DENIAL, never an allow. The alternative —
// treating an unreachable revocation channel as "no revocations" — means an
// outage silently restores every credential the system has ever revoked, and it
// does so without a single error reaching a client.

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/accesscontrol/epoch_resolver.h"
#include "anvil/auth/epoch_cache.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::identity {

// How long the Redis mirror survives without a write. Long, because it is
// refreshed on every miss and its expiry costs exactly one MongoDB read.
inline constexpr std::chrono::seconds kEpochMirrorTtl{3600};

// The Redis key prefix for the mirror. Short because it is sent on every
// authorisation miss, and namespaced because a deployment's Redis is shared
// with the rate limiter and the job queues.
inline constexpr std::string_view kEpochKeyPrefix = "pe:";

// Defined in anvil/accesscontrol so the access filter can name it without
// including an application-layer header (docs/00-architecture.md §2).
using accesscontrol::EpochVerdict;

// A role's permission mask.
//
// anvil ships the RESOLUTION and stores nothing: where role definitions live,
// and how an administrator edits one, is the application's business. What anvil
// needs is the union, computed once at WRITE time and stored — resolving roles
// per request would be a second query and a set union on every protected call,
// which is precisely the cost putting permissions in the token exists to avoid.
class RoleTable final {
public:
    RoleTable() = default;
    explicit RoleTable(std::vector<std::pair<Uuid, PermSet>> roles) noexcept
        : roles_{std::move(roles)} {}

    // Empty when the role is unknown. An unknown role granting NOTHING is the
    // only safe reading: the alternative is a deleted role silently retaining
    // whatever mask it last had, for every account that still names it.
    [[nodiscard]] PermSet mask_for(const Uuid& role_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return roles_.size(); }

private:
    // A linear scan over tens of entries beats a hash map: it is one cache line
    // per four entries and it allocates nothing per lookup.
    std::vector<std::pair<Uuid, PermSet>> roles_;
};

class AuthzService final : public accesscontrol::EpochResolver {
public:
    AuthzService(std::string database, std::string_view users_collection,
                 std::chrono::milliseconds cache_ttl = auth::kDefaultEpochTtl);

    // --- the hot path ------------------------------------------------------
    // Pure CPU, zero allocation, zero I/O. Safe on a Trantor event-loop thread,
    // which is the only reason the filter can answer most requests without
    // leaving it.
    [[nodiscard]] EpochVerdict check_cached(const Uuid& user_id,
                                            std::uint64_t token_epoch) const noexcept override;

    // --- the miss path -----------------------------------------------------
    // BLOCKING; db_pool only. Consults the Redis mirror, falls back to MongoDB
    // when the mirror has expired, and populates both caches on the way back. A
    // Redis error comes back as ServiceUnavailable, which the filter turns into
    // a denial.
    [[nodiscard]] Result<std::uint64_t> resolve(mongocxx::client& client, const Uuid& user_id);

    // The access filter's entry point. Acquires a client on db_pool, calls
    // resolve(), and invokes `done` from that pool thread. Sheds with
    // ServiceUnavailable when the queue is full rather than queueing — which the
    // filter, correctly, treats as a denial rather than as a delay.
    void resolve_async(const Uuid& user_id,
                       std::function<void(Result<std::uint64_t>)> done) override;

    // --- write-time resolution ---------------------------------------------
    // The union of direct grants and role masks, computed HERE, at write time,
    // and stored as `eff`. A read is then one 16-byte load and one AND.
    [[nodiscard]] static PermSet effective_permissions(const PermSet& direct,
                                                       std::span<const Uuid> role_ids,
                                                       const RoleTable& roles) noexcept;

    // Bumps the authority, mirrors it, and drops the local entry so THIS
    // instance honours the change immediately. Other instances honour it within
    // their own cache TTL — that latency is the documented revocation window,
    // not a defect, and it is why the TTL is ten seconds rather than ten
    // minutes.
    [[nodiscard]] Result<std::int64_t> bump_epoch(mongocxx::client& client, const Uuid& user_id);

    [[nodiscard]] auth::EpochCache& cache() noexcept { return cache_; }
    [[nodiscard]] const auth::EpochCache& cache() const noexcept { return cache_; }

    AuthzService(const AuthzService&) = delete;
    AuthzService& operator=(const AuthzService&) = delete;

private:
    // Declaration order is construction order: users_ is built from database_.
    const std::string database_;
    const std::string users_collection_;
    auth::EpochCache  cache_;
};

}  // namespace anvil::identity
