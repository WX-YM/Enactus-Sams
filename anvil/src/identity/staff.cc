// versioned-write-exempt: the one unversioned write here is the population
// guard, and it is not data. It $incs a counter nobody reads, in a document that
// exists only so two concurrent demotions collide on it and one of them is
// retried against the other's committed state. There is nothing to lose-update:
// the authoritative count is still a count_documents inside the same
// transaction, which is what keeps the guard from drifting away from reality the
// way a denormalised counter would. Every write here that IS a read-modify-write
// goes through UserRepository, which goes through db/versioned.h.

#include "anvil/identity/staff.h"

#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/update.hpp>

#include "anvil/db/repository.h"

namespace anvil::identity {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;

// Carries a Failure out of a with_transaction callback.
//
// It must be an exception rather than a captured flag, because the helper
// commits whatever the callback leaves behind: returning normally after
// discovering a violated invariant would COMMIT the writes made before it.
// Throwing rolls them back, which is the only correct exit.
struct AbortTransaction final {
    Failure failure;
};

}  // namespace

StaffService::StaffService(std::string database, std::string_view users_collection,
                           std::string_view sessions_collection,
                           std::string_view guard_collection, AuthzService& authz)
    : database_{std::move(database)},
      guard_collection_{guard_collection},
      users_{database_, users_collection},
      sessions_{database_, sessions_collection},
      authz_{authz} {}

Status StaffService::guard_population(mongocxx::client& client,
                                      mongocxx::client_session& session) const {
    return repo::guarded_in_transaction([&]() -> Status {
        mongocxx::collection guard = client[database_][guard_collection_];

        mongocxx::options::update options{};
        // Upserted rather than seeded by a migration: a document created by a
        // deploy is a document a restored backup can be missing, and two
        // concurrent upserts of the same `_id` collide exactly as two updates of
        // it do — which is the whole point of touching it.
        options.upsert(true);

        guard.update_one(
            session,
            make_document(kvp("_id", bsoncxx::types::b_string{
                                          codec::key_of(kPopulationGuardId)}))
                .view(),
            make_document(kvp("$inc", [](sub_document sub) {
                sub.append(kvp(codec::key_of(kPopulationGuardField),
                               bsoncxx::types::b_int64{1}));
            })).view(),
            options);
        return ok();
    });
}

Result<PermissionChange> StaffService::set_permissions(mongocxx::client& client,
                                                       const Uuid& user_id,
                                                       std::int64_t expected_version,
                                                       const PermSet& direct,
                                                       std::span<const Uuid> role_ids,
                                                       const RoleTable& roles) {
    // Read for the BEFORE half of the audit row. It is not a check-then-act: the
    // write below carries the expected version, so a row that moved between this
    // read and that write fails rather than overwriting.
    const Result<std::optional<AccountRecord>> before = users_.find_account(client, user_id);
    if (!before) { return before.error(); }
    if (!before.value().has_value()) { return fail(ErrorCode::NotFound, "id"); }

    const PermSet effective = AuthzService::effective_permissions(direct, role_ids, roles);
    const Result<std::int64_t> version =
        users_.set_permissions(client, user_id, expected_version, direct, effective);
    if (!version) { return version.error(); }

    // AFTER the write, and only on success. Bumping first would revoke every
    // session of an account whose permissions then failed to change.
    const Result<std::int64_t> epoch = authz_.bump_epoch(client, user_id);
    if (!epoch) { return epoch.error(); }

    return PermissionChange{.before = before.value()->direct_permissions,
                            .after = direct,
                            .version = version.value()};
}

Result<UserTypeChange> StaffService::set_user_type(mongocxx::client& client, const Uuid& user_id,
                                                   std::int64_t expected_version,
                                                   UserType user_type, const PermSet& direct,
                                                   std::span<const Uuid> role_ids,
                                                   const RoleTable& roles,
                                                   UserType protected_type) {
    const Result<std::optional<AccountRecord>> before = users_.find_account(client, user_id);
    if (!before) { return before.error(); }
    if (!before.value().has_value()) { return fail(ErrorCode::NotFound, "id"); }

    const AccountRecord& account = *before.value();
    // Only a change that takes an ACTIVE account OUT of the protected type can
    // reduce that population. Guarding every write would serialise the whole
    // permission grid on one document for no reason.
    const bool reduces_population = account.user_type == protected_type &&
                                    account.status == UserStatus::Active &&
                                    user_type != protected_type;

    const PermSet effective = AuthzService::effective_permissions(direct, role_ids, roles);

    Result<std::int64_t> version = fail(ErrorCode::Internal, "type");
    try {
        auto session = client.start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            if (reduces_population) {
                const Status guarded = guard_population(client, *txn);
                if (!guarded) { throw AbortTransaction{guarded.error()}; }

                // The count is taken INSIDE the transaction that performs the
                // demotion. A count taken outside it is a count that was true a
                // moment ago, which is the whole failure this guards against.
                const Result<std::int64_t> remaining =
                    users_.count_active_of_type(client, *txn, protected_type);
                if (!remaining) { throw AbortTransaction{remaining.error()}; }
                if (remaining.value() <= 1) {
                    throw AbortTransaction{Failure{ErrorCode::Conflict, "population"}};
                }
            }

            // The type and the mask move TOGETHER: an account holding permission
            // bits it is no longer the kind of account to hold is a row that
            // authorises something nobody decided.
            const Result<std::int64_t> written = users_.set_user_type(
                client, *txn, user_id, expected_version, user_type, direct, effective);
            if (!written) { throw AbortTransaction{written.error()}; }
            version = written.value();
        });
    } catch (const AbortTransaction& aborted) {
        return aborted.failure;
    } catch (const mongocxx::operation_exception& error) {
        return repo::translate(error);
    } catch (const mongocxx::exception& error) {
        return repo::translate(error);
    }

    if (!version) { return version.error(); }

    const Result<std::int64_t> epoch = authz_.bump_epoch(client, user_id);
    if (!epoch) { return epoch.error(); }

    return UserTypeChange{
        .before = account.user_type, .after = user_type, .version = version.value()};
}

Result<UserStatus> StaffService::set_status(mongocxx::client& client, const Uuid& user_id,
                                            UserStatus status, UserType protected_type,
                                            db::TimeMs now) {
    const Result<std::optional<AccountRecord>> before = users_.find_account(client, user_id);
    if (!before) { return before.error(); }
    if (!before.value().has_value()) { return fail(ErrorCode::NotFound, "id"); }

    const AccountRecord& account = *before.value();
    const bool reduces_population = account.user_type == protected_type &&
                                    account.status == UserStatus::Active &&
                                    status != UserStatus::Active;

    Result<UserStatus> previous = fail(ErrorCode::Internal, "status");
    try {
        auto session = client.start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            if (reduces_population) {
                const Status guarded = guard_population(client, *txn);
                if (!guarded) { throw AbortTransaction{guarded.error()}; }

                const Result<std::int64_t> remaining =
                    users_.count_active_of_type(client, *txn, protected_type);
                if (!remaining) { throw AbortTransaction{remaining.error()}; }
                if (remaining.value() <= 1) {
                    throw AbortTransaction{Failure{ErrorCode::Conflict, "population"}};
                }
            }

            const Result<std::optional<UserStatus>> written =
                users_.set_status(client, *txn, user_id, status, now);
            if (!written) { throw AbortTransaction{written.error()}; }
            if (!written.value().has_value()) {
                throw AbortTransaction{Failure{ErrorCode::NotFound, "id"}};
            }
            previous = *written.value();
        });
    } catch (const AbortTransaction& aborted) {
        return aborted.failure;
    } catch (const mongocxx::operation_exception& error) {
        return repo::translate(error);
    } catch (const mongocxx::exception& error) {
        return repo::translate(error);
    }

    if (!previous) { return previous.error(); }

    if (status != UserStatus::Active) {
        // Revoke, THEN bump. The revoke stops the refresh path from minting a
        // fresh access token; the bump kills the one already outstanding. Either
        // alone is a disable that looks like it worked and did not — and doing
        // them in the other order leaves a window in which a refresh mints a
        // token carrying the new epoch.
        const Result<std::int64_t> revoked = sessions_.revoke_all(client, user_id);
        if (!revoked) { return revoked.error(); }
    }

    const Result<std::int64_t> epoch = authz_.bump_epoch(client, user_id);
    if (!epoch) { return epoch.error(); }

    // Permissions are deliberately UNTOUCHED. Turning an account back on
    // restores exactly what it had, which is what an enable/disable control
    // promises — clearing the grid on disable would make re-enabling a
    // reconstruction job.
    return previous.value();
}

}  // namespace anvil::identity
