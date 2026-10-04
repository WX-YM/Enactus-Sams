#include "anvil/identity/authz.h"

#include <array>
#include <charconv>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <trantor/utils/Logger.h>

#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/identity/users.h"
#include "anvil/analytics/counters.h"
#include "anvil/redis/redis_client.h"

namespace anvil::identity {
namespace {

// The prefix plus 22 characters of base64url. Built on the STACK: the miss path
// is rare but it is still a security path, and a heap allocation per denied
// request is a lever an attacker can pull by sending tokens with epochs that
// will never be cached.
constexpr std::size_t kEpochKeyBytes = 3 + 22;

static_assert(kEpochKeyPrefix.size() == 3, "the key buffer is sized from the prefix");

[[nodiscard]] std::array<char, kEpochKeyBytes> epoch_key(const Uuid& user_id) noexcept {
    std::array<char, kEpochKeyBytes> key{};
    for (std::size_t i = 0; i < kEpochKeyPrefix.size(); ++i) { key[i] = kEpochKeyPrefix[i]; }
    const std::array<char, 22> encoded = uuid::to_base64url(user_id);
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        key[kEpochKeyPrefix.size() + i] = encoded[i];
    }
    return key;
}

// from_chars: no allocation, no locale, and a stored value that is not a plain
// integer reads as CORRUPTION rather than as zero. Zero would be a valid-looking
// epoch that no token can ever match, so silently accepting it would lock every
// user out instead of failing loudly — which is the same outcome, reached in a
// way nobody can diagnose.
[[nodiscard]] std::optional<std::uint64_t> parse_epoch(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) { return std::nullopt; }
    return value;
}

}  // namespace

PermSet RoleTable::mask_for(const Uuid& role_id) const noexcept {
    for (const auto& [id, mask] : roles_) {
        if (id == role_id) { return mask; }
    }
    return PermSet{};
}

AuthzService::AuthzService(std::string database, std::string_view users_collection,
                           std::chrono::milliseconds cache_ttl)
    : database_{std::move(database)},
      users_collection_{users_collection},
      cache_{cache_ttl} {}

EpochVerdict AuthzService::check_cached(const Uuid& user_id,
                                        std::uint64_t token_epoch) const noexcept {
    const std::optional<std::uint64_t> cached =
        cache_.get(user_id, auth::EpochCache::Clock::now());
    if (!cached.has_value()) { return EpochVerdict::Unknown; }
    // Counted here rather than logged, for the reason docs/00 §9 gives: this
    // runs once per protected request, and a line per occurrence is itself the
    // outage under the load that makes it interesting. The hit RATE is this
    // series over the sum of the three, so the denominator is inside the same
    // family rather than in a second metric that can disagree with it.
    analytics::count(analytics::Internal::AuthzCacheHits, analytics::AuthzTier::Local);
    return *cached == token_epoch ? EpochVerdict::Match : EpochVerdict::Mismatch;
}

Result<std::uint64_t> AuthzService::resolve(mongocxx::client& client, const Uuid& user_id) {
    const std::array<char, kEpochKeyBytes> key = epoch_key(user_id);
    const sw::redis::StringView key_view{key.data(), key.size()};

    std::optional<std::uint64_t> authority;
    try {
        sw::redis::Redis& redis = redis::RedisClient::instance();
        if (const sw::redis::OptionalString mirrored = redis.get(key_view)) {
            authority = parse_epoch(*mirrored);
            if (!authority.has_value()) { return fail(ErrorCode::Internal, "perm_epoch"); }
            analytics::count(analytics::Internal::AuthzCacheHits,
                             analytics::AuthzTier::Mirror);
        }
    } catch (const std::exception& e) {
        // Fail CLOSED. An unreachable revocation channel must not read as "no
        // revocations": that turns a Redis outage into a silent restoration of
        // every credential this system has ever revoked, with no error reaching
        // anybody who could notice.
        LOG_ERROR << "perm_epoch mirror unavailable, denying: " << e.what();
        return fail(ErrorCode::ServiceUnavailable, "perm_epoch");
    }

    if (!authority.has_value()) {
        // The mirror has expired. MongoDB is the authority; this costs one
        // indexed read per user per mirror TTL, and it is what stops an expired
        // cache key from signing a whole population out.
        analytics::count(analytics::Internal::AuthzCacheHits,
                         analytics::AuthzTier::Authority);
        const UserRepository users{database_, users_collection_};
        const Result<std::optional<UserPermRecord>> stored =
            users.find_permissions(client, user_id);
        if (!stored) { return stored.error(); }
        if (!stored.value().has_value()) {
            // The token names a user that no longer exists. Deny, and do NOT
            // cache: caching an absence would make a deleted-then-recreated id
            // read as denied for the whole cache TTL.
            return fail(ErrorCode::Unauthenticated, "user");
        }
        if (stored.value()->status != UserStatus::Active) {
            return fail(ErrorCode::Unauthenticated, "status");
        }
        authority = static_cast<std::uint64_t>(stored.value()->perm_epoch);

        try {
            sw::redis::Redis& redis = redis::RedisClient::instance();
            redis.set(key_view, std::to_string(*authority), kEpochMirrorTtl);
        } catch (const std::exception& e) {
            // Repopulating the mirror is an OPTIMISATION. Failing it means the
            // next request pays for another MongoDB read, which is correct but
            // slower — not a reason to deny a request whose authority was just
            // read successfully.
            LOG_WARN << "could not repopulate perm_epoch mirror: " << e.what();
        }
    }

    cache_.put(user_id, *authority, auth::EpochCache::Clock::now());
    return *authority;
}

void AuthzService::resolve_async(const Uuid& user_id,
                                 std::function<void(Result<std::uint64_t>)> done) {
    // Captured BY VALUE. This is called from an event-loop thread and that frame
    // is gone the instant try_post returns (CLAUDE.md §3.3).
    auto task = [this, user_id, done]() mutable {
        auto client = db::MongoPool::instance().acquire();
        done(resolve(*client, user_id));
    };

    if (!Pools::db().try_post(anvil::guarded("db", std::move(task)))) {
        // A full queue is SHED, never queued. The filter turns this into a
        // denial, which is the correct reading: the revocation channel could not
        // be consulted, so nothing is granted.
        done(fail(ErrorCode::ServiceUnavailable, "perm_epoch"));
    }
}

PermSet AuthzService::effective_permissions(const PermSet& direct,
                                            std::span<const Uuid> role_ids,
                                            const RoleTable& roles) noexcept {
    PermSet effective = direct;
    for (const Uuid& role_id : role_ids) { effective |= roles.mask_for(role_id); }
    // SuperAdmin is deliberately NOT synthesised here as an all-ones mask. It is
    // an explicit user-type check at the decision site, so that "holds every
    // permission" and "is a superadmin" stay distinguishable in an audit row and
    // no bit-fiddling accident can manufacture the second from the first
    // (anvil/accesscontrol/decision.h).
    return effective;
}

Result<std::int64_t> AuthzService::bump_epoch(mongocxx::client& client, const Uuid& user_id) {
    const UserRepository users{database_, users_collection_};
    const Result<std::int64_t> bumped = users.bump_perm_epoch(client, user_id);
    if (!bumped) { return bumped.error(); }

    const std::array<char, kEpochKeyBytes> key = epoch_key(user_id);
    const sw::redis::StringView key_view{key.data(), key.size()};
    try {
        redis::RedisClient::instance().set(key_view, std::to_string(bumped.value()),
                                           kEpochMirrorTtl);
    } catch (const std::exception& set_failed) {
        // The mirror now holds the OLD epoch, which is exactly the value the
        // revoked token carries — leaving it there would keep that token working
        // for the mirror's full hour. Deleting the key is the safe failure: the
        // next resolve() misses and re-reads the authority.
        LOG_ERROR << "could not mirror bumped perm_epoch: " << set_failed.what();
        try {
            redis::RedisClient::instance().del(key_view);
        } catch (const std::exception& del_failed) {
            LOG_FATAL << "perm_epoch mirror is stale and could not be cleared for "
                      << uuid::to_string(user_id) << ": " << del_failed.what();
        }
    }

    cache_.invalidate(user_id);
    return bumped.value();
}

}  // namespace anvil::identity
