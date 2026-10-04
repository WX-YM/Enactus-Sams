#pragma once

// The permission grid: who may do what, and the three writes that change it.
//
// Every one of them bumps perm_epoch. That is the whole point of the epoch
// channel — a permission change that did not bump it would take effect when the
// last outstanding access token expired, which for a privileged account is
// exactly the window the design exists to close.
//
// --- the invariant a transaction alone does not enforce ---------------------
//
// "The last account of a privileged type cannot be disabled or demoted" is a
// multi-document invariant, and MongoDB gives snapshot isolation rather than
// serialisability. Two transactions demoting two DIFFERENT superadmins each
// count the other as still active, neither writes a document the other wrote,
// and BOTH commit. The population reaches zero with no error anywhere.
//
// Every transaction that could reduce that population therefore $incs one shared
// guard document first. Two of them then collide on it, WiredTiger raises a
// write conflict, the loser is retried by with_transaction, and its retry counts
// against the winner's committed state. The stored number is never read — the
// authoritative count is still a count_documents inside the transaction — so the
// guard cannot drift away from reality the way a denormalised counter would.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/sessions.h"
#include "anvil/identity/users.h"

namespace anvil::identity {

// The `_id` of the guard document. One document, in the application's guard
// collection, carrying a counter nobody reads. It is a MUTEX, not data.
inline constexpr std::string_view kPopulationGuardId = "population";
inline constexpr std::string_view kPopulationGuardField = "held";

// What a permission change did, so the caller can write ONE audit row carrying
// both halves. A row saying only "somebody's permissions changed" cannot answer
// the question an audit log is read to answer.
struct PermissionChange final {
    PermSet      before;
    PermSet      after;
    std::int64_t version;
};

struct UserTypeChange final {
    UserType     before;
    UserType     after;
    std::int64_t version;
};

class StaffService final {
public:
    // `guard_collection` holds the one document every population-reducing
    // transaction collides on. It is upserted rather than seeded by a migration:
    // a document created by a deploy is a document a restored backup can be
    // missing, and two concurrent upserts of the same `_id` collide exactly as
    // two updates of it do — which is the whole point of touching it.
    StaffService(std::string database, std::string_view users_collection,
                 std::string_view sessions_collection, std::string_view guard_collection,
                 AuthzService& authz, SessionsRevoked on_revoked = {});

    // A permission change. Versioned, because two administrators with the grid
    // open on the same person is the normal case for a small team, and a
    // find_one followed by an unconditional write loses one of them in silence.
    //
    // `direct` is what was edited; the stored union is computed from it and the
    // role ids through AuthzService::effective_permissions, so the two can never
    // be written out of step with each other.
    //
    // VersionMismatch when the row moved under the caller. Re-read and retry is
    // the only correct response, and it is the same response for a stale version
    // and a deleted account — distinguishing them on an administrative route is
    // an existence oracle.
    [[nodiscard]] Result<PermissionChange> set_permissions(mongocxx::client& client,
                                                           const Uuid& user_id,
                                                           std::int64_t expected_version,
                                                           const PermSet& direct,
                                                           std::span<const Uuid> role_ids,
                                                           const RoleTable& roles);

    // Promotion and demotion, as ONE versioned write inside a transaction that
    // also proves the privileged population survives it.
    //
    // `protected_type` is the type whose active population may not reach zero.
    // It is a parameter rather than a constant because which type a deployment
    // cannot afford to lose is a deployment's decision — though it is
    // UserType::SuperAdmin in every arrangement anybody should ship.
    //
    // Conflict when this change would remove the last active account of that
    // type. Not Forbidden: nothing about the caller's authority is wrong, and
    // the remedy is to promote somebody else first.
    [[nodiscard]] Result<UserTypeChange> set_user_type(mongocxx::client& client,
                                                       const Uuid& user_id,
                                                       std::int64_t expected_version,
                                                       UserType user_type,
                                                       const PermSet& direct,
                                                       std::span<const Uuid> role_ids,
                                                       const RoleTable& roles,
                                                       UserType protected_type);

    // Enable or disable, returning the status the row held BEFORE the write.
    //
    // Disabling REVOKES every live session and bumps the epoch, in that order.
    // Without the revoke, a disabled account keeps refreshing; without the bump,
    // its outstanding access token keeps working until it expires. Doing only
    // one of the two is the shape of a disable that looks like it worked.
    // The sessions it revoked are told to `on_revoked` after the bump.
    [[nodiscard]] Result<UserStatus> set_status(mongocxx::client& client, const Uuid& user_id,
                                                UserStatus status, UserType protected_type,
                                                db::TimeMs now);

    StaffService(const StaffService&) = delete;
    StaffService& operator=(const StaffService&) = delete;

private:
    // Takes the population guard inside the caller's transaction. It writes a
    // number nobody reads: its only job is to be the one document two concurrent
    // demotions both touch.
    [[nodiscard]] Status guard_population(mongocxx::client& client,
                                          mongocxx::client_session& session) const;

    // Declaration order is construction order: both repositories are built from
    // database_.
    const std::string database_;
    const std::string guard_collection_;
    UserRepository    users_;
    SessionRepository sessions_;
    AuthzService&     authz_;
    // Disabling an account revokes its sessions, and SessionService's hook
    // would never hear of them: the same seam, given here too.
    const SessionsRevoked on_revoked_;
};

}  // namespace anvil::identity
