#pragma once

// The sessions collection.
//
// Two rules govern every method here, and both are corrections to a design that
// looked reasonable and was not:
//
//   * A session is found by its id or by a REFRESH token hash, never by the
//     access token's. Keying on the access token makes rotation impossible — a
//     rotated token has a new hash, and the whole point of the rotation budget
//     is that the row is NOT rewritten on every request, so the session becomes
//     unfindable the first time a token is re-minted.
//   * Every query also filters `expires_at > now`. The TTL index is garbage
//     collection, not access control: the monitor runs roughly every 60 seconds,
//     so an expired session document is still readable and would still
//     authenticate. tools/check-db-discipline.sh fails the build over a query
//     here that omits it.
//
// Storage: the address is 16 packed bytes and the user agent is an 8-byte hash
// rather than the ~200-byte string. At a million live sessions that difference
// is on the order of 200 MB of WiredTiger cache, spent to store a string nothing
// reads back.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"

namespace anvil::identity {

// v4-mapped or native v6, always 16 bytes. Packing rather than storing a string
// also makes the /24 and /48 coarsening a mask instead of a parse.
using PackedIp = std::array<std::uint8_t, 16>;
using UserAgentHash = std::array<std::uint8_t, 8>;

// The field names anvil writes, published for the same reason the user fields
// are: the index catalogue is the application's, and it cannot declare
// `{rt_hash: 1}` unique without knowing what anvil spelled the column.
namespace session_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kUserId = "uid";
inline constexpr std::string_view kRefreshHash = "rt";
inline constexpr std::string_view kPreviousHash = "prt";
inline constexpr std::string_view kPreviousUntil = "pru";
inline constexpr std::string_view kLastSeen = "seen";
// The AUTHORITATIVE expiry, and the field the TTL index is built on. It is
// deliberately not `last_seen`: putting the TTL on the observational timestamp
// expires an active user as soon as the rotation budget stops rewriting it,
// which is up to a whole rotation interval early.
inline constexpr std::string_view kExpiresAt = "expires_at";
inline constexpr std::string_view kAbsoluteExpiry = "abs";
inline constexpr std::string_view kIp = "ip";
inline constexpr std::string_view kUserAgentHash = "ua";
inline constexpr std::string_view kUserType = "ut";
inline constexpr std::string_view kRevoked = "rev";
}  // namespace session_fields

struct SessionRecord final {
    db::TimeMs                last_seen;
    db::TimeMs                expires_at;
    db::TimeMs                abs_expiry;
    std::optional<db::TimeMs> prev_until;
    Uuid                      id;
    Uuid                      user_id;
    PackedIp                  ip;
    UserAgentHash             user_agent_hash;
    UserType                  user_type;
    bool                      revoked;
};

struct NewSession final {
    Uuid              id;
    Uuid              user_id;
    crypto::Digest256 refresh_hash;
    db::TimeMs        now;
    db::TimeMs        expires_at;
    db::TimeMs        abs_expiry;
    PackedIp          ip;
    UserAgentHash     user_agent_hash;
    UserType          user_type;
};

// Which hash matched. The distinction is what makes the two-tab race safe AND
// the replay attack detectable — one mechanism answering both, because they are
// the same observation read at two different times.
enum class RefreshMatch : std::uint8_t {
    Current,        // the ordinary case
    PreviousGrace,  // the previous hash, inside its window: a concurrent tab
    PreviousStale,  // the previous hash, after it closed: a REPLAY
};

struct RefreshLookup final {
    SessionRecord session;
    RefreshMatch  match;
};

// Told which sessions a revocation ended, after the revocation committed: the
// seam through which a session's end reaches state the session made, such as
// the chat device it registered (ChatService::session_ended). Called on the
// revoking thread, which is a db_pool thread, once per revocation with every
// session it ended; `client` is that thread's and must not leave it.
//
// Best effort by construction. The revocation has committed before the hook is
// asked and is not undone by it, and a process killed between the two never
// asks: what a hook ends must also be ended by something durable (a chat
// device whose session is gone can no longer be touched, and the idle sweeper
// ends it). A hook that throws is caught and ignored, because a sign-out must
// not fail over what follows it.
using SessionsRevoked = std::function<void(mongocxx::client& client, const Uuid& user_id,
                                           std::span<const Uuid> session_ids)>;

// Asks `hook`, if set and there is anything to say, swallowing what it throws.
// One function so every revoking service keeps the same promise.
void report_sessions_revoked(const SessionsRevoked& hook, mongocxx::client& client,
                             const Uuid& user_id, std::span<const Uuid> session_ids);

// One user and the number of devices they are currently signed in on.
struct SessionCount final {
    Uuid         user_id;
    std::int32_t live;
};

class SessionRepository final : public repo::RepositoryBase {
public:
    SessionRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    [[nodiscard]] Status insert(mongocxx::client& client, const NewSession& session) const;

    // Looks up the current hash first, then the previous-token grace window.
    // nullopt means no live session presented that token, which the caller maps
    // to a real 401 — a client has to be able to tell "sign in again" from
    // "that route is gone".
    [[nodiscard]] Result<std::optional<RefreshLookup>> find_by_refresh_hash(
        mongocxx::client& client, const crypto::Digest256& refresh_hash,
        db::TimeMs now) const;

    [[nodiscard]] Result<std::optional<SessionRecord>> find_by_id(mongocxx::client& client,
                                                                  const Uuid& session_id,
                                                                  db::TimeMs now) const;

    // Rotation, as ONE conditional write. `expected_current_hash` is in the
    // FILTER, which makes this a compare-and-swap: two tabs refreshing at the
    // same instant produce exactly one rotation, and the loser reads the
    // winner's grace window instead of being signed out.
    //
    // `last_seen` is written here and nowhere else. It is the observational
    // timestamp, and folding it into the rotation write is what keeps an active
    // client to one session write per rotation interval rather than one per
    // request.
    //
    // False means the filter matched nothing, which is the losing tab.
    [[nodiscard]] Result<bool> rotate_refresh_hash(
        mongocxx::client& client, const Uuid& session_id,
        const crypto::Digest256& expected_current_hash, const crypto::Digest256& new_hash,
        db::TimeMs prev_until, db::TimeMs new_expires_at, db::TimeMs now) const;

    [[nodiscard]] Status revoke(mongocxx::client& client, const Uuid& session_id,
                                const Uuid& user_id) const;

    // Every unrevoked session of a user, answered BY ID. The ids are what lets
    // something else end with a session — a chat device a session registered
    // is the one anvil has — and a single update_many cannot name what it
    // matched. So the ids are read a page at a time and that page is revoked
    // by id, until a read finds none: a session signed in while this runs is
    // either in a page or created after the last one, never revoked unnamed.
    [[nodiscard]] Result<std::vector<Uuid>> revoke_all(mongocxx::client& client,
                                                       const Uuid& user_id) const;

    // Revokes every live session for a user EXCEPT one — the rotation a password
    // change performs, where signing the person out of the browser they are
    // changing it in is the one outcome nobody wants. By id, as revoke_all.
    [[nodiscard]] Result<std::vector<Uuid>> revoke_all_except(mongocxx::client& client,
                                                              const Uuid& user_id,
                                                              const Uuid& keep_session_id) const;

    // The oldest live sessions beyond `keep`, for the concurrent-session cap.
    // Returns what should be evicted, oldest first, so the caller can revoke and
    // audit them — an eviction nobody can see afterwards is indistinguishable
    // from a session that was never created.
    [[nodiscard]] Result<std::vector<Uuid>> overflow_sessions(mongocxx::client& client,
                                                              const Uuid& user_id,
                                                              db::TimeMs now,
                                                              std::int32_t keep) const;

    // Live sessions, most recently seen first. Bounded by `limit`, because an
    // unbounded result set is an unbounded response (CLAUDE.md §7).
    [[nodiscard]] Result<std::vector<SessionRecord>> list_for_user(mongocxx::client& client,
                                                                   const Uuid& user_id,
                                                                   db::TimeMs now,
                                                                   std::int32_t limit) const;

    // How many devices each of `user_ids` is signed in on, in ONE round trip.
    //
    // An administrative table showing "2 signed in" on every row is the obvious
    // caller, and the obvious implementation — a count per row — makes rendering
    // N rows cost N queries on a screen where a person waits for all of them. A
    // `$in` plus a `$group` is one.
    //
    // Users with no live session are ABSENT from the result rather than present
    // with zero: the aggregation has nothing to group for them, and inventing a
    // row here would mean the caller could not tell "none" from "not asked".
    [[nodiscard]] Result<std::vector<SessionCount>> count_live_for_users(
        mongocxx::client& client, std::span<const Uuid> user_ids, db::TimeMs now) const;

private:
    // revoke_all and revoke_all_except, which differ only in the one session
    // they keep.
    [[nodiscard]] Result<std::vector<Uuid>> revoke_live(mongocxx::client& client,
                                                        const Uuid& user_id,
                                                        std::optional<Uuid> keep) const;
};

}  // namespace anvil::identity
