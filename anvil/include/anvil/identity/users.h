#pragma once

// The users collection: the schema anvil owns, and the repository over it.
//
// Every read here is a PROJECTION, and which one is chosen is a security
// decision rather than an optimisation. `AuthRecord` carries the stored
// password hash and answers exactly one question — "may this credential in" —
// so it is never read for anything else and never crosses a response boundary.
// `PermRecord` is nine bytes of the same document and answers the refresh
// path's question. A screen that renders a person must not be answered by the
// query that answers a login, because the moment it is, the hash is one field
// addition away from a response body.
//
// Returning the whole document to read one boolean also costs network, BSON
// decode CPU and heap on every login (CLAUDE.md §7), but that is the smaller of
// the two reasons.
//
// Which fields anvil owns, and why an application may keep its own in the same
// document, is in anvil/identity/user_fields.h.

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"
#include "anvil/identity/login_identity.h"

namespace anvil::identity {


// Everything the login path needs, and nothing else.
//
// Ordered largest-alignment-first so the struct carries no interior padding
// (CLAUDE.md §2.3).
struct UserAuthRecord final {
    std::string               password_hash;
    std::optional<db::TimeMs> lock_until;
    Uuid                      id;
    PermSet                   effective_permissions;
    std::int64_t              perm_epoch;
    std::int32_t              failure_count;
    UserType                  user_type;
    UserStatus                status;
    Locale                    locale;
};

// The refresh path's re-read: the ONE read that makes a permission change take
// effect on a live session without waiting out a token lifetime.
struct UserPermRecord final {
    PermSet      effective_permissions;
    std::int64_t perm_epoch;
    UserType     user_type;
    UserStatus   status;
    Locale       locale;
};

// One account as an administrative screen sees it. Deliberately NOT
// UserAuthRecord with fields added — see the header comment.
//
// `version` is here because a permission edit is a read-modify-write and
// therefore optimistic (CLAUDE.md §6), and this row is where the client learns
// the version to send back.
struct AccountRecord final {
    db::TimeMs   created_at;
    std::string  username;   // the display spelling, never the lookup key
    std::string  email;      // likewise
    Uuid         id;
    PermSet      direct_permissions;
    PermSet      effective_permissions;
    std::int64_t version;
    UserType     user_type;
    UserStatus   status;
    Locale       locale;
};

// Where an account listing resumed from. A PAIR, because the listing is ordered
// by (user_type, _id) — that order is what lets the index walk serve the sort
// instead of the server sorting the page in memory, and a cursor on `_id` alone
// could not express a position inside it.
struct AccountCursor final {
    Uuid     id;
    UserType user_type;
};

// How an account listing is narrowed. Every member is optional and they
// compose; nothing set is "everybody, by type then id".
//
// `email_normalised` is an EQUALITY and `username_prefix` is a prefix, and the
// asymmetry is deliberate: a substring search across every address in the
// system is a harvesting tool with a staff session in front of it, so an
// address must be known in full before it matches. A username is a public
// handle and a prefix over it rides the unique index as a range.
// Every member carries a default, so a caller may name only what it is
// narrowing by. Without them `AccountQuery{.limit = 20}` leaves `limit`'s
// preceding members value-initialised but the struct's own `limit` is the only
// field the caller thought about — and a partially-initialised aggregate is a
// warning at every call site and an indeterminate bound at one of them.
struct AccountQuery final {
    std::optional<AccountCursor> after = std::nullopt;
    std::string_view             email_normalised = {};
    std::string_view             username_prefix = {};   // already normalised
    std::optional<UserStatus>    status = std::nullopt;
    std::optional<UserType>      user_type = std::nullopt;
    // Bounded by the repository at one when a caller leaves it unset, because a
    // result set with no bound is a response with no bound (CLAUDE.md §7).
    std::int32_t                 limit = 0;
};

// A new account.
//
// `user_type` is NOT a member. Self-service registration creates one kind of
// account and staff creation is a different authority behind a different
// permission; making the field unrepresentable here is cheaper than remembering
// to ignore it at every call site.
struct NewUser final {
    // Allocated by the CALLER, not here. A duplicate-identity registration must
    // answer byte-identically to a fresh one, and it cannot do that if the id
    // only comes into existence on the success path.
    Uuid             id;
    std::string_view email_normalised;
    std::string_view email_display;
    std::string_view username_normalised;
    std::string_view username_display;
    std::string_view password_hash;
    // E.164 and nothing else, already validated by the caller. Empty means the
    // account has none, and it is written ABSENT — see fields::kPhone for what
    // a partial unique index does with an empty string. The email and the
    // username are written absent when empty too, for the same reason: an
    // account schema may make either optional (anvil/accounts/schema.h), and
    // then the application declares that index partial as it does the phone's.
    std::string_view phone_e164;
    Locale           locale;
    // A field rather than a constant because the two creation paths disagree
    // legitimately: registration writes PendingVerification, and an operator
    // creating an account for somebody writes Active.
    UserStatus       status;
    // The application's profile, key and already-validated value, written under
    // fields::kProfile. Empty writes no subdocument at all. The keys come from
    // the application's account schema and never from a request.
    std::span<const std::pair<std::string_view, std::string_view>> profile{};
};

// What a password flow needs about an account it already knows by id: the
// stored hash and the version it is written back under, the status, and the
// identifiers an enrolment salt may be derived from. One projection, so a
// password change is one read.
struct CredentialRecord final {
    UserAuthRecord auth;
    std::string    email_normalised;     // empty when the account has none
    std::string    username_normalised;  // likewise
    std::string    phone_e164;           // likewise
    std::int64_t   version;
};

// One account's id and the handle to show for it.
//
// The USERNAME rather than a display name, for the reason an action name is
// never translated: an investigator lining a screen up against a server log
// needs the same word on both, and a display name is optional, localised and
// changeable.
struct AccountName final {
    std::string name;
    Uuid        id;
};

class UserRepository final : public repo::RepositoryBase {
public:
    UserRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // nullopt means no such account. The caller MUST still burn the Argon2 time
    // against a dummy hash — see identity/password_service.h. Without that, a
    // missing account answers in microseconds and an existing one in ~100 ms,
    // which is a trivially exploitable enumeration oracle.
    [[nodiscard]] Result<std::optional<UserAuthRecord>> find_for_login(
        mongocxx::client& client, std::string_view normalised, LoginIdentity kind) const;

    [[nodiscard]] Result<std::optional<UserPermRecord>> find_permissions(
        mongocxx::client& client, const Uuid& user_id) const;

    // By id, for a flow that has already established whose account it is — a
    // password change behind the access filter, a reset whose code verified.
    // Never on a path an unauthenticated caller can aim at an arbitrary id.
    [[nodiscard]] Result<std::optional<CredentialRecord>> find_credential(
        mongocxx::client& client, const Uuid& user_id) const;

    // Relies on the unique indexes for collision detection rather than a prior
    // find_one: check-then-insert is a race, and the extra read is wasted work
    // on the happy path. A duplicate comes back as ErrorCode::Conflict — which
    // an UNAUTHENTICATED caller must never be shown, because "that address is
    // taken" is account enumeration in one sentence.
    [[nodiscard]] Status insert(mongocxx::client& client, const NewUser& user) const;

    [[nodiscard]] Result<std::optional<AccountRecord>> find_account(mongocxx::client& client,
                                                                    const Uuid& user_id) const;

    // An account by its normalised email or its normalised username, whichever
    // an operator typed. Both are unique indexes, so this is one equality either
    // way and never a scan.
    [[nodiscard]] Result<std::optional<AccountRecord>> find_by_identity(
        mongocxx::client& client, std::string_view normalised, LoginIdentity kind) const;

    [[nodiscard]] Result<std::vector<AccountRecord>> list_accounts(
        mongocxx::client& client, const AccountQuery& query) const;

    // One username per id that names an account — the "who" and "on what"
    // columns of an audit screen. Without it both render a raw UUID, which is
    // the shape of an answer and not one.
    //
    // BATCHED: one `$in` per page, never a point read per row. An empty `ids`
    // issues no query at all, which is the common case — the rows an audit
    // collection is busiest with are denials carrying neither field.
    //
    // An id naming no account is simply ABSENT from the result rather than an
    // error. A log outlives the accounts it names.
    [[nodiscard]] Result<std::vector<AccountName>> names_of(mongocxx::client& client,
                                                            std::span<const Uuid> ids) const;

    // A permission change, versioned. `direct` and `effective` move together or
    // the stored union stops meaning anything.
    //
    // VersionMismatch when the row moved under the caller: two administrators
    // with the grid open on the same person is the normal case for a small team,
    // and a find_one followed by an unconditional write loses one of them in
    // silence (CLAUDE.md §6).
    [[nodiscard]] Result<std::int64_t> set_permissions(mongocxx::client& client,
                                                       const Uuid& user_id,
                                                       std::int64_t expected_version,
                                                       const PermSet& direct,
                                                       const PermSet& effective) const;

    // Promotion and demotion, as ONE versioned write: the type and the mask are
    // a single fact. An account holding permission bits it is no longer the kind
    // of account to hold is a row that authorises something nobody decided.
    //
    // Takes the caller's session because demoting the last privileged account
    // has to commit together with the count that proved another one remains.
    [[nodiscard]] Result<std::int64_t> set_user_type(mongocxx::client& client,
                                                     mongocxx::client_session& session,
                                                     const Uuid& user_id,
                                                     std::int64_t expected_version,
                                                     UserType user_type,
                                                     const PermSet& direct,
                                                     const PermSet& effective) const;

    // Enable or disable, returning the status the row held BEFORE the write.
    //
    // find_one_and_update rather than a read followed by a write, and the
    // pre-image is why: an audit row records what the account changed FROM, and
    // reading that separately would record a value that was true a moment
    // earlier. Unversioned on purpose — it sets an absolute value the caller
    // chose rather than deriving a new one from the old, so there is no update
    // to lose.
    //
    // nullopt means no such account.
    [[nodiscard]] Result<std::optional<UserStatus>> set_status(mongocxx::client& client,
                                                               mongocxx::client_session& session,
                                                               const Uuid& user_id,
                                                               UserStatus status,
                                                               db::TimeMs now) const;

    // How many accounts of `user_type` can still sign in. Read inside the
    // caller's transaction, after the population guard has been taken.
    [[nodiscard]] Result<std::int64_t> count_active_of_type(mongocxx::client& client,
                                                            mongocxx::client_session& session,
                                                            UserType user_type) const;

    // PendingVerification -> Active, and nothing else. The current status is in
    // the FILTER, so an account disabled since the code was issued cannot be
    // quietly re-enabled by somebody who still holds it. False means the row was
    // not pending, which a caller treats as success: verifying twice is not an
    // error.
    [[nodiscard]] Result<bool> activate(mongocxx::client& client,
                                        mongocxx::client_session& session, const Uuid& user_id,
                                        db::TimeMs now) const;

    // One step of the unverified-account sweep: deletes a single account still
    // awaiting verification and created before `before`, returning its id so the
    // caller can audit the deletion. nullopt when there is nothing left.
    //
    // find_one_and_delete rather than delete_many, for two reasons that both
    // matter: an account verified between the find and the delete is never
    // taken, because the status is in the filter; and the caller bounds the
    // batch rather than holding a pool thread for as long as the backlog is.
    //
    // A TTL index would perform the same deletion and record nothing, which is
    // most of why this is a job.
    [[nodiscard]] Result<std::optional<Uuid>> delete_one_stale_pending(mongocxx::client& client,
                                                                       db::TimeMs before) const;

    // Login failure accounting. `lock_until` is computed by the CALLER from the
    // resulting count, so the backoff policy lives in the application and this
    // stays a write. Returns the new count.
    [[nodiscard]] Result<std::int32_t> record_login_failure(
        mongocxx::client& client, const Uuid& user_id,
        const std::optional<db::TimeMs>& lock_until) const;

    [[nodiscard]] Status clear_login_failures(mongocxx::client& client,
                                              const Uuid& user_id) const;

    // Returns the NEW epoch. One atomic $inc, so N concurrent permission changes
    // produce N distinct epochs and none is lost.
    [[nodiscard]] Result<std::int64_t> bump_perm_epoch(mongocxx::client& client,
                                                       const Uuid& user_id) const;

    // Background rehash after a successful authentication against below-policy
    // stored parameters. Conditional on the hash that was verified still being
    // the stored one, so a password change racing the rehash cannot resurrect
    // the credential it replaced.
    [[nodiscard]] Status replace_password_hash(mongocxx::client& client, const Uuid& user_id,
                                               std::string_view expected_hash,
                                               std::string_view new_hash) const;

    // Sets the stored hash unconditionally and bumps nothing. The CALLER revokes
    // sessions and bumps the epoch; folding those in here would make a password
    // change a three-collection operation inside a repository, which is a
    // service's job (docs/00-architecture.md §2).
    [[nodiscard]] Result<std::int64_t> set_password_hash(mongocxx::client& client,
                                                         const Uuid& user_id,
                                                         std::int64_t expected_version,
                                                         std::string_view new_hash) const;
};

}  // namespace anvil::identity
