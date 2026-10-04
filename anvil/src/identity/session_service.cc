#include "anvil/identity/session_service.h"

#include <arpa/inet.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/http/client_address.h"

namespace anvil::identity {
namespace {

[[nodiscard]] db::TimeMs plus(db::TimeMs at, std::chrono::seconds seconds) noexcept {
    return at + std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
}

[[nodiscard]] std::int64_t seconds_between(db::TimeMs from, db::TimeMs to) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(to - from).count();
}

// A stable, NON-IDENTIFYING label. The question a sessions listing answers is
// "is that one me?", which a consistent word answers; the user agent string
// answers far more than that and belongs in a log pipeline rather than in a
// self-service API.
constexpr std::array<std::string_view, 16> kDeviceLabels{
    "amber",  "basalt",  "cedar",   "delta", "ember",  "flint",  "garnet", "harbour",
    "indigo", "juniper", "kestrel", "larch", "marble", "nectar", "onyx",   "quartz",
};

constexpr std::array<std::uint8_t, 12> kV4MappedPrefix{0, 0, 0, 0, 0, 0,
                                                       0, 0, 0, 0, 0xFF, 0xFF};

[[nodiscard]] bool is_v4_mapped(const PackedIp& ip) noexcept {
    return std::equal(kV4MappedPrefix.begin(), kV4MappedPrefix.end(), ip.begin());
}

}  // namespace

// v4-mapped addresses carry ::ffff: in the first twelve bytes. Coarsening to
// /24 for those and /48 for native v6 keeps a listing useful — "a different
// network than usual" — without handing out a precise location.
PackedIp coarsen_network(const PackedIp& ip) noexcept {
    const bool v4_mapped = is_v4_mapped(ip);
    PackedIp masked = ip;
    // The last byte of the mapped address for v4, the last ten bytes for native
    // v6. ONE function, so "same source" cannot come to mean two things in two
    // places (anvil/audit/buffer.h folds on the result of this).
    for (std::size_t i = v4_mapped ? 15 : 6; i < masked.size(); ++i) { masked[i] = 0; }
    return masked;
}

std::string coarse_network_of(const PackedIp& ip) {
    const bool v4_mapped = is_v4_mapped(ip);
    const PackedIp masked = coarsen_network(ip);

    std::array<char, INET6_ADDRSTRLEN> text{};
    if (v4_mapped) {
        if (::inet_ntop(AF_INET, masked.data() + 12, text.data(), text.size()) == nullptr) {
            return {};
        }
        return std::string{text.data()} + "/24";
    }
    if (::inet_ntop(AF_INET6, masked.data(), text.data(), text.size()) == nullptr) { return {}; }
    return std::string{text.data()} + "/48";
}

PackedIp pack_ip(std::string_view address) noexcept {
    // ONE parser for the whole system. Two implementations of "these bytes are
    // that address" is how the v4 and v4-mapped forms of one client end up in
    // two different rate-limit buckets, which is a limit that does not limit.
    return http::pack_address(address);
}

UserAgentHash hash_user_agent(std::string_view user_agent) noexcept {
    const XXH64_hash_t hash = XXH3_64bits(user_agent.data(), user_agent.size());
    UserAgentHash out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((hash >> (i * 8)) & 0xFFU);
    }
    return out;
}

std::string device_label_of(const UserAgentHash& hash) {
    // The low nibble of the first byte. The table is a presentation of the hash
    // and not a second fingerprint, so how many bits it consumes is a question
    // about how many words read distinctly, not about collision resistance.
    return std::string{kDeviceLabels[hash[0] & 0x0FU]};
}

SessionService::SessionService(std::string database, std::string_view sessions_collection,
                               std::string_view users_collection,
                               std::span<const std::uint8_t> pepper,
                               std::shared_ptr<const auth::TokenKeys> keys, AuthzService& authz,
                               SessionPolicy policy, SessionsRevoked on_revoked)
    : database_{std::move(database)},
      sessions_{database_, sessions_collection},
      users_{database_, users_collection},
      keys_{std::move(keys)},
      authz_{authz},
      policy_{policy},
      on_revoked_{std::move(on_revoked)},
      pepper_{} {
    if (!keys_) { throw std::invalid_argument{"SessionService: token keys must not be null"}; }
    if (pepper.size() != pepper_.size()) {
        throw std::invalid_argument{"SessionService: pepper must be 32 bytes"};
    }
    std::copy(pepper.begin(), pepper.end(), pepper_.data());
}

crypto::Digest256 SessionService::hash_refresh_token(std::string_view token) const {
    return crypto::sha256_with_pepper(token, pepper_.span());
}

std::chrono::seconds SessionService::access_ttl(UserType type) const noexcept {
    return is_staff(type) ? policy_.staff_access : policy_.client_access;
}

std::string SessionService::mint_access_token(const Uuid& user_id, const Uuid& session_id,
                                              const PermSet& permissions,
                                              std::uint64_t perm_epoch, UserType type,
                                              Locale locale, db::TimeMs now) const {
    const db::TimeMs expires = plus(now, access_ttl(type));
    const auth::AccessClaims claims{
        .user_id = user_id,
        .session_id = session_id,
        .permissions = permissions,
        .perm_epoch = perm_epoch,
        .expires_at = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(expires.time_since_epoch())
                .count()),
        .user_type = type,
        .locale = locale,
        .reserved = {},
    };
    return auth::encode(claims, *keys_);
}

Result<IssuedSession> SessionService::create(mongocxx::client& client,
                                             const UserAuthRecord& user, const PackedIp& ip,
                                             std::string_view user_agent,
                                             db::TimeMs now) const {
    // The cap is enforced BEFORE the insert, so an eviction happens in the
    // request that caused it rather than at some later sweep. Asking for one
    // more than the cap is what makes "already at the limit" answerable from a
    // single query.
    const Result<std::vector<SessionRecord>> live =
        sessions_.list_for_user(client, user.id, now, policy_.max_concurrent_sessions + 1);
    if (!live) { return live.error(); }
    if (static_cast<std::int32_t>(live.value().size()) >= policy_.max_concurrent_sessions) {
        // list_for_user sorts by last_seen descending, so the least recently
        // seen is last.
        const Uuid oldest = live.value().back().id;
        const Status evicted = sessions_.revoke(client, oldest, user.id);
        if (!evicted) { return evicted.error(); }
        report_sessions_revoked(on_revoked_, client, user.id, std::span{&oldest, 1});
    }

    const bool staff = is_staff(user.user_type);
    const Uuid session_id = uuid::generate_v7();
    const UserAgentHash device = hash_user_agent(user_agent);
    // Answered from the rows already read for the cap, so a new-device
    // notification costs no extra query. "New" therefore means "no OTHER live
    // session on this device", which is the honest reading: a session that
    // expired or was revoked is one this account can no longer observe, so a
    // sign-in following it deserves the alert.
    const bool new_device = std::none_of(live.value().begin(), live.value().end(),
                                         [&device](const SessionRecord& record) {
                                             return record.user_agent_hash == device;
                                         });
    std::string refresh_token = crypto::random_token();

    const NewSession session{
        .id = session_id,
        .user_id = user.id,
        .refresh_hash = hash_refresh_token(refresh_token),
        .now = now,
        .expires_at = plus(now, staff ? policy_.staff_refresh : policy_.client_refresh),
        .abs_expiry = plus(now, staff ? policy_.staff_absolute : policy_.client_absolute),
        .ip = ip,
        .user_agent_hash = device,
        .user_type = user.user_type,
    };

    const Status inserted = sessions_.insert(client, session);
    if (!inserted) { return inserted.error(); }

    return IssuedSession{
        .access_token = mint_access_token(user.id, session_id, user.effective_permissions,
                                          static_cast<std::uint64_t>(user.perm_epoch),
                                          user.user_type, user.locale, now),
        .refresh_token = std::move(refresh_token),
        // Rendered only when it will be used. The label is a lookup and a
        // string, and every sign-in from a known device pays for neither.
        .new_device_label = new_device ? device_label_of(device) : std::string{},
        .session_id = session_id,
        .access_expires_in_seconds = access_ttl(user.user_type).count(),
        .refresh_expires_in_seconds = seconds_between(now, session.expires_at),
        .from_new_device = new_device,
    };
}

Result<IssuedSession> SessionService::refresh(mongocxx::client& client,
                                              std::string_view refresh_token, db::TimeMs now) {
    // Length before hashing, so an oversized cookie costs a compare rather than
    // a SHA-256 over whatever an attacker sent.
    if (refresh_token.size() != kRefreshTokenLength) {
        return fail(ErrorCode::Unauthenticated, "refresh");
    }

    const crypto::Digest256 presented = hash_refresh_token(refresh_token);
    const Result<std::optional<RefreshLookup>> found =
        sessions_.find_by_refresh_hash(client, presented, now);
    if (!found) { return found.error(); }
    if (!found.value().has_value()) { return fail(ErrorCode::Unauthenticated, "refresh"); }

    const SessionRecord& session = found.value()->session;

    if (found.value()->match == RefreshMatch::PreviousStale) {
        // An old refresh token presented after its grace window closed means a
        // rotated credential was replayed, which means it leaked. The whole
        // session goes, and the epoch bump kills its outstanding access token
        // too rather than waiting out the access lifetime.
        const Status revoked = sessions_.revoke(client, session.id, session.user_id);
        if (!revoked) { return revoked.error(); }
        const Result<std::int64_t> bumped = authz_.bump_epoch(client, session.user_id);
        if (!bumped) { return bumped.error(); }
        report_sessions_revoked(on_revoked_, client, session.user_id, std::span{&session.id, 1});
        return fail(ErrorCode::Unauthenticated, kRefreshReplayDetected);
    }

    // The one read that makes a permission change take effect on a live session.
    // Everything else on the request path is stateless by design; this is where
    // fresh authority enters the system.
    const Result<std::optional<UserPermRecord>> user =
        users_.find_permissions(client, session.user_id);
    if (!user) { return user.error(); }
    if (!user.value().has_value() || user.value()->status != UserStatus::Active) {
        const Status revoked = sessions_.revoke(client, session.id, session.user_id);
        if (!revoked) { return revoked.error(); }
        report_sessions_revoked(on_revoked_, client, session.user_id, std::span{&session.id, 1});
        return fail(ErrorCode::Unauthenticated, "status");
    }

    std::string rotated_token;
    db::TimeMs refresh_expiry = session.expires_at;

    // Rotate at most once per interval, and NEVER on the grace path: the other
    // tab has just rotated, and rotating again would invalidate the token it is
    // about to store.
    const bool due = plus(session.last_seen, policy_.rotation_interval) <= now;
    if (due && found.value()->match == RefreshMatch::Current) {
        // The FRESHLY READ type, never the one frozen on the session document at
        // sign-in. That field is a snapshot of what the account was when it
        // signed in, and a promotion changes the account without touching it —
        // so reading it here would let a session that began as a client keep
        // sliding on the client window after its owner became staff, which is
        // the one lifetime the policy is emphatic about.
        const db::TimeMs extended =
            is_staff(user.value()->user_type)
                ? session.expires_at
                : std::min(plus(now, policy_.client_refresh), session.abs_expiry);

        std::string candidate = crypto::random_token();
        const Result<bool> swapped = sessions_.rotate_refresh_hash(
            client, session.id, presented, hash_refresh_token(candidate),
            plus(now, policy_.rotation_grace), extended, now);
        if (!swapped) { return swapped.error(); }
        if (swapped.value()) {
            rotated_token = std::move(candidate);
            refresh_expiry = extended;
        }
        // Losing the compare-and-swap means a concurrent tab rotated first. That
        // is NOT an error: the caller keeps the token it already holds and the
        // other tab's rotation stands.
    }

    return IssuedSession{
        .access_token = mint_access_token(session.user_id, session.id,
                                          user.value()->effective_permissions,
                                          static_cast<std::uint64_t>(user.value()->perm_epoch),
                                          user.value()->user_type, user.value()->locale, now),
        .refresh_token = std::move(rotated_token),
        // A refresh is not a sign-in. The device was recognised when the session
        // was created, and re-alerting on every token rotation would be a stream
        // of alerts about nothing — which is how a security notification stops
        // being read.
        .new_device_label = {},
        .session_id = session.id,
        .access_expires_in_seconds = access_ttl(user.value()->user_type).count(),
        .refresh_expires_in_seconds = seconds_between(now, refresh_expiry),
        .from_new_device = false,
    };
}

Status SessionService::revoke(mongocxx::client& client, const Uuid& session_id,
                              const Uuid& user_id) {
    const Status revoked = sessions_.revoke(client, session_id, user_id);
    if (!revoked) { return revoked.error(); }
    const Result<std::int64_t> bumped = authz_.bump_epoch(client, user_id);
    if (!bumped) { return bumped.error(); }
    // After the bump: the access token is what stops the session being used,
    // and nothing the hook does should stand between a sign-out and that.
    report_sessions_revoked(on_revoked_, client, user_id, std::span{&session_id, 1});
    return ok();
}

Result<std::int64_t> SessionService::revoke_all(mongocxx::client& client, const Uuid& user_id) {
    const Result<std::vector<Uuid>> revoked = sessions_.revoke_all(client, user_id);
    if (!revoked) { return revoked.error(); }
    // ONE bump for the whole set: the epoch is per user, not per session, so N
    // increments would be N writes to say the same thing once.
    const Result<std::int64_t> bumped = authz_.bump_epoch(client, user_id);
    if (!bumped) { return bumped.error(); }
    report_sessions_revoked(on_revoked_, client, user_id, revoked.value());
    return static_cast<std::int64_t>(revoked.value().size());
}

Result<std::int64_t> SessionService::revoke_others(mongocxx::client& client, const Uuid& user_id,
                                                   const Uuid& keep_session_id) {
    const Result<std::vector<Uuid>> revoked =
        sessions_.revoke_all_except(client, user_id, keep_session_id);
    if (!revoked) { return revoked.error(); }
    // The epoch bump invalidates the SURVIVING session's access token too, which
    // is correct and is why the caller must re-mint: the alternative is
    // distinguishing sessions inside the epoch, and an epoch that means
    // different things for different sessions is not one counter any more.
    const Result<std::int64_t> bumped = authz_.bump_epoch(client, user_id);
    if (!bumped) { return bumped.error(); }
    report_sessions_revoked(on_revoked_, client, user_id, revoked.value());
    return static_cast<std::int64_t>(revoked.value().size());
}

Result<std::vector<SessionView>> SessionService::list(mongocxx::client& client,
                                                      const Uuid& user_id,
                                                      const Uuid& current_session_id,
                                                      db::TimeMs now) const {
    const Result<std::vector<SessionRecord>> rows =
        sessions_.list_for_user(client, user_id, now, policy_.max_concurrent_sessions);
    if (!rows) { return rows.error(); }

    std::vector<SessionView> views;
    views.reserve(rows.value().size());
    for (const SessionRecord& record : rows.value()) {
        views.push_back(SessionView{
            .last_seen = record.last_seen,
            .session_id = record.id,
            .coarse_ip = coarse_network_of(record.ip),
            .device_label = device_label_of(record.user_agent_hash),
            .is_current = record.id == current_session_id,
        });
    }
    return views;
}

}  // namespace anvil::identity
