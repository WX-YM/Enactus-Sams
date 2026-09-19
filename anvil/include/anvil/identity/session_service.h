#pragma once

// Session lifecycle.
//
// The credential that lasts and the credential that authorises are two
// different things, and conflating them produces four separate contradictions
// at once. The split here is the correction:
//
//   REFRESH token   opaque, 32 CSPRNG bytes, looked up in the database. Grants
//                   nothing by itself. Long-lived, rotated on a budget.
//   ACCESS token    the fixed binary layout in anvil/auth/token.h. Carries
//                   permissions, never touches the database, minutes to an hour.
//
// Worst-case stale authority drops from the refresh lifetime to the access
// lifetime, and the perm_epoch channel cuts even that to one cache TTL
// (anvil/identity/authz.h).
//
// Refresh tokens are deliberately NOT JWTs. They are looked up in the database
// anyway — that lookup is what makes revocation work — so a JWT would add
// parsing cost and a second expiry that can disagree with the row's.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/auth/token.h"
#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/sessions.h"
#include "anvil/identity/users.h"

namespace anvil::identity {

// Set on the Failure when an old refresh token is presented after its grace
// window has closed. The session is already revoked and the epoch already
// bumped by the time a caller sees this; its remaining job is the
// high-severity audit row. This is the single highest-signal event the identity
// layer produces — a rotated credential being replayed means it leaked.
inline constexpr std::string_view kRefreshReplayDetected = "refresh_replay";

// Refresh tokens are 32 CSPRNG bytes as unpadded base64url. Published so a
// caller can reject an oversized cookie on length before anything hashes it.
inline constexpr std::size_t kRefreshTokenLength = 43;

struct SessionPolicy final {
    // Staff refresh and absolute expiry are equal on purpose: privileged
    // sessions do not slide. At the interval they re-authenticate, which is
    // where a "sessions last N days" requirement actually belongs.
    std::chrono::seconds staff_refresh{std::chrono::hours{72}};
    std::chrono::seconds staff_absolute{std::chrono::hours{72}};
    std::chrono::seconds client_refresh{std::chrono::hours{24 * 30}};
    // The hard cap. A sliding window with no absolute limit means "requires full
    // re-authentication" is never actually enforced — the window simply moves
    // forward forever.
    std::chrono::seconds client_absolute{std::chrono::hours{24 * 90}};

    std::chrono::seconds staff_access{std::chrono::minutes{15}};
    std::chrono::seconds client_access{std::chrono::hours{1}};

    // One session write per interval per active client. Without it, a session
    // row is rewritten on every refresh, which is write amplification
    // proportional to traffic on a collection whose row count is proportional to
    // users.
    std::chrono::seconds rotation_interval{std::chrono::hours{48}};
    // Two tabs refreshing at the same instant must both succeed. The loser
    // presents a token the winner just rotated away; inside this window that is
    // a race, outside it that is a replay.
    std::chrono::seconds rotation_grace{60};

    // Unbounded session rows are unbounded write amplification and a storage
    // leak an attacker controls. On overflow the least-recently-seen is revoked.
    std::int32_t max_concurrent_sessions{10};
};

struct IssuedSession final {
    std::string  access_token;
    // Empty when the refresh token was NOT rotated, which is the common case:
    // between rotations the client keeps the token it already has and no write
    // occurs at all.
    std::string  refresh_token;
    // The coarse label of the device this session was created on, and whether
    // that device had no other live session at the time.
    //
    // REPORTED rather than acted on. Publishing a security notification is the
    // caller's step, because create() runs inside a login handler's database
    // task and a publish is a second unit of work with its own failure mode — a
    // shed notification must not be able to fail a sign-in.
    std::string  new_device_label;
    Uuid         session_id;
    std::int64_t access_expires_in_seconds;
    std::int64_t refresh_expires_in_seconds;
    bool         from_new_device;
};

// What a "your signed-in devices" screen may disclose. Never the raw user agent
// and never the full address: a feature for killing a session must not become a
// self-service intelligence surface.
struct SessionView final {
    db::TimeMs  last_seen;
    Uuid        session_id;
    std::string coarse_ip;      // /24 for v4, /48 for v6
    std::string device_label;   // derived from the hash, never the string
    bool        is_current;
};

class SessionService final {
public:
    // `pepper` is 32 bytes from configuration, never from the database: stored
    // token digests are then not lookup-able by anyone holding only a database
    // dump. Throws std::invalid_argument on a wrong-sized pepper or null keys —
    // a process that starts with a misconfigured signing story is worse than one
    // that does not start.
    SessionService(std::string database, std::string_view sessions_collection,
                   std::string_view users_collection, std::span<const std::uint8_t> pepper,
                   std::shared_ptr<const auth::TokenKeys> keys, AuthzService& authz,
                   SessionPolicy policy = {});

    // Creates a session and mints both tokens. Enforces the concurrent-session
    // cap FIRST, so an eviction is visible in the same request that caused it
    // rather than at some later sweep nobody is watching.
    [[nodiscard]] Result<IssuedSession> create(mongocxx::client& client,
                                               const UserAuthRecord& user, const PackedIp& ip,
                                               std::string_view user_agent,
                                               db::TimeMs now) const;

    // The one read that makes a permission change take effect on a live session.
    // Non-const: a detected replay revokes and bumps, which mutates the epoch
    // cache.
    [[nodiscard]] Result<IssuedSession> refresh(mongocxx::client& client,
                                                std::string_view refresh_token, db::TimeMs now);

    // Revoking BUMPS the epoch. Without that, the revoked session's outstanding
    // access token keeps working until it expires — which is the whole access
    // lifetime, and is exactly the window a sign-out is supposed to close.
    [[nodiscard]] Status revoke(mongocxx::client& client, const Uuid& session_id,
                                const Uuid& user_id);
    [[nodiscard]] Result<std::int64_t> revoke_all(mongocxx::client& client, const Uuid& user_id);

    // Every session but the caller's own — what a password change performs.
    // Signing somebody out of the browser they are changing their password in is
    // the one outcome nobody wants from a password change.
    [[nodiscard]] Result<std::int64_t> revoke_others(mongocxx::client& client,
                                                     const Uuid& user_id,
                                                     const Uuid& keep_session_id);

    [[nodiscard]] Result<std::vector<SessionView>> list(mongocxx::client& client,
                                                        const Uuid& user_id,
                                                        const Uuid& current_session_id,
                                                        db::TimeMs now) const;

    [[nodiscard]] const SessionPolicy& policy() const noexcept { return policy_; }
    [[nodiscard]] const SessionRepository& sessions() const noexcept { return sessions_; }

    // SHA-256(token ‖ pepper). Exposed so a controller can clear the cookie for
    // a token it never needs to hold in any other form.
    [[nodiscard]] crypto::Digest256 hash_refresh_token(std::string_view token) const;

    SessionService(const SessionService&) = delete;
    SessionService& operator=(const SessionService&) = delete;

private:
    [[nodiscard]] std::chrono::seconds access_ttl(UserType type) const noexcept;
    [[nodiscard]] std::string mint_access_token(const Uuid& user_id, const Uuid& session_id,
                                                const PermSet& permissions,
                                                std::uint64_t perm_epoch, UserType type,
                                                Locale locale, db::TimeMs now) const;

    // Declaration order is construction order: both repositories are built from
    // database_, and keys_ is read by mint_access_token.
    const std::string                      database_;
    SessionRepository                      sessions_;
    UserRepository                         users_;
    std::shared_ptr<const auth::TokenKeys> keys_;
    AuthzService&                          authz_;
    const SessionPolicy                    policy_;
    crypto::SecretBuffer<32>               pepper_;
};

// True for every account type that is not an ordinary client. The session
// policy branches on it, and so does the access lifetime: a privileged session
// is worth less time.
[[nodiscard]] constexpr bool is_staff(UserType type) noexcept {
    return type != UserType::Client;
}

// Packs a textual address into 16 bytes; v4 becomes v4-mapped v6. Returns the
// all-zero address for anything unparseable rather than throwing — a malformed
// peer address must not fail a login, and a zero address is visibly wrong in an
// audit row rather than quietly plausible.
[[nodiscard]] PackedIp pack_ip(std::string_view address) noexcept;

// 8 bytes of xxh3 over the user agent. The full string is ~200 bytes per session
// document; at a million live sessions that is ~200 MB of WiredTiger cache, and
// nothing in this layer has a use for the string that the hash does not serve.
[[nodiscard]] UserAgentHash hash_user_agent(std::string_view user_agent) noexcept;

// The coarse device name a user-agent hash renders as: one word from a fixed
// table, so two sessions from the same browser read as the same device and no
// two screens spell it differently.
//
// A word rather than a parsed product name because the question a sessions
// listing answers is "is that one me?", which a consistent label answers — and
// echoing the raw user agent back is a stored-XSS vector in whatever renders it
// and tells an attacker exactly what is being fingerprinted.
[[nodiscard]] std::string device_label_of(const UserAgentHash& hash);

// The rendered source of a stored address: `/24` for v4, `/48` for native v6,
// empty for an address that will not format.
//
// Coarsened because no surface that shows it has any use for a precise address.
// "A different network than usual" is what a sessions listing means, and "one
// source is denied ten thousand times" is what an audit screen means; the host
// bits answer neither question and are a location if they leak.
[[nodiscard]] std::string coarse_network_of(const PackedIp& ip);

// The same rule before it is rendered: the address with its host bits cleared.
//
// The audit sink needs to know whether two denials came from ONE source, which
// is a comparison rather than a rendering — and doing it by formatting both
// addresses and comparing strings would put two allocations and an inet_ntop on
// the path taken once per denied request under flood. Exposed so that "same
// source" means exactly one thing in this system.
[[nodiscard]] PackedIp coarsen_network(const PackedIp& ip) noexcept;

}  // namespace anvil::identity
