#pragma once

// The account flows, built in: registration, contact verification, sign-in,
// password reset and change, refresh and sign-out (docs/05-auth-sessions.md
// §13).
//
// Every application needs these, and every one of them is a place to leak
// whether an account exists, to let a code be guessed, or to leave a session
// alive that a password change should have ended. So they are anvil's, once,
// and an application supplies only what it alone knows — its account schema
// (schema.h), its rate-limit budgets, its lockout backoff, and how a code
// reaches a person.
//
// --- threads -------------------------------------------------------------------
//
// Every operation is called from wherever the request arrived — a Trantor loop
// thread — and never blocks it: the lookup and every write run on db_pool,
// Argon2 on hash_pool, the keyed prehash stage on cpu_pool (identity/
// prehash_service.h says why). An operation either answers at once — a
// malformed request, a shed pool — and RETURNS that answer without calling
// `done`, or returns nullopt and calls `done` exactly once, later, on a pool
// thread. Never both, and never on the caller's stack.
//
// --- what every failure looks like --------------------------------------------
//
// A sign-in answers Unauthenticated for a wrong secret, a missing account, an
// account awaiting verification, a disabled one and a locked one, after the
// same work. A code check answers Unauthenticated for a wrong code, an expired
// one, an exhausted one and an address with no code. A registration, a resend
// and a reset request answer Accepted whether or not an account exists. The
// reasons are kept apart only server-side, for the audit trail.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "anvil/accounts/identifier.h"
#include "anvil/accounts/schema.h"
#include "anvil/auth/prehash.h"
#include "anvil/core/locale.h"
#include "anvil/core/types.h"
#include "anvil/crypto/argon2.h"
#include "anvil/db/codec.h"
#include "anvil/http/rate_limit.h"
#include "anvil/identity/session_service.h"
#include "anvil/input/fields.h"

namespace anvil::identity {
class AuthzService;
class VerificationService;
class PasswordService;
class PrehashService;
}  // namespace anvil::identity

namespace anvil::accounts {

// --- configuration ----------------------------------------------------------------

// The client hashes (the default): the policy of docs/05 §12.
struct ClientHashing final {
    auth::PrehashPolicy prehash;
};

// The server hashes (the opt-out): plain mode, docs/05 §8.
struct ServerHashing final {
    crypto::Argon2Params params;
};

// Client first, matching Hashing::Client being the default. It must agree with
// AccountDescription::hashing, and the constructor refuses a disagreement: the
// descriptor tells every client which to do, and a server doing the other
// would refuse every one of them.
using CredentialPolicy = std::variant<ClientHashing, ServerHashing>;

// The budgets, from the application's own rate-limit table. Per-address and
// per-identifier both apply to every flow that has both, for docs/05 §4's
// reason: one stops a distributed attack on one account, the other one address
// sweeping many.
struct AccountBudgets final {
    http::RateLimitRule sign_in_ip;       // sign-in, and the salt route
    http::RateLimitRule sign_in_account;
    http::RateLimitRule register_ip;
    http::RateLimitRule code_ip;          // guesses: verify, reset confirm
    http::RateLimitRule code_account;
    http::RateLimitRule issue_ip;         // new codes: resend, reset request
    http::RateLimitRule issue_account;
};

enum class CodePurpose : std::uint8_t {
    // A code proving the contact, after registration or a resend.
    Verify,
    // A code authorising a new password.
    Reset,
    // No code. Somebody registered with a contact that already has an account;
    // its owner should hear about it — "you already have an account" — but the
    // registration answered exactly as a new one did.
    AccountExists,
};

struct CodeDelivery final {
    CodePurpose   purpose;
    LoginIdentity channel;   // Email or Phone
    std::string   address;   // the canonical address
    std::string   code;      // empty for AccountExists
    Locale        locale;
};

// Called on a pool thread AFTER the request has been answered, so how long it
// takes is not in anybody's response time. It should hand the message to a
// queue — anvil/notifications/outbound.h, a job — rather than send it inline,
// and must not throw; an exception is caught and logged.
using DeliverCode = std::function<void(CodeDelivery)>;

// The backoff: how long an account stays locked after `failures` consecutive
// wrong secrets, or nullopt for not at all. Policy is the application's
// (docs/05 §4 "Lockout is a counter, not a policy"); anvil keeps the count.
using LockPolicy =
    std::function<std::optional<db::TimeMs>(std::int32_t failures, db::TimeMs now)>;

struct AccountServiceDeps final {
    std::string                    database;
    std::string_view               users_collection;
    identity::SessionService&      sessions;
    identity::AuthzService&        authz;
    identity::VerificationService& verification;
    http::RateLimiter&             limiter;
};

struct AccountConfig final {
    AccountDescription   description;
    CredentialPolicy     credentials;
    AccountBudgets       budgets;
    LockPolicy           lock_after;
    DeliverCode          deliver;
    std::chrono::seconds code_lifetime{std::chrono::minutes{15}};
};

// --- requests ---------------------------------------------------------------------------

// The secret as it arrived: a password under server hashing, the client's
// derived key under client hashing. Which one is decided by the configuration,
// never by which field a request happened to fill.
using Secret = std::variant<std::string, auth::PrehashKey>;

struct Origin final {
    identity::PackedIp ip;
    std::string        user_agent;
};

struct RegisterRequest final {
    std::string                                      email;
    std::string                                      username;
    std::string                                      phone;
    // Keyed by the schema's own key string; a key the schema does not declare
    // is refused before anything is stored.
    std::vector<std::pair<std::string, std::string>> profile;
    std::optional<Locale>                            locale;
    Secret                                           secret;
    Origin                                           origin;
};

enum class SaltPurpose : std::uint8_t { SignIn, Enroll };

// --- the answer ------------------------------------------------------------------------

struct AccountAnswer final {
    // Ok for a success; the one code a failure maps to otherwise.
    ErrorCode                          code = ErrorCode::Ok;
    // For ValidationFailed only. Field names are schema constants, never keys
    // from the request.
    std::vector<input::FieldError>     fields;
    // A sign-in or a refresh: the cookies to set.
    std::optional<identity::IssuedSession> session;
    // The salt route.
    std::optional<auth::PrehashSaltAnswer> salt;
};

using Done = std::function<void(AccountAnswer)>;

class AccountService final {
public:
    // Throws std::invalid_argument on a description that is not well-formed or
    // that disagrees with `credentials` about who hashes.
    AccountService(AccountServiceDeps deps, AccountConfig config);
    ~AccountService();

    AccountService(const AccountService&) = delete;
    AccountService& operator=(const AccountService&) = delete;

    // Each returns the answer when it is given at once, or nullopt when `done`
    // will be called later. See the header.
    [[nodiscard]] std::optional<AccountAnswer> salt(std::string identifier, SaltPurpose purpose,
                                                    Origin origin, Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> register_account(RegisterRequest request,
                                                                Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> verify(std::string identifier, std::string code,
                                                      Origin origin, Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> resend(std::string identifier, Origin origin,
                                                      Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> sign_in(std::string identifier, Secret secret,
                                                       Origin origin, Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> reset_request(std::string identifier,
                                                             Origin origin, Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> reset_confirm(std::string identifier,
                                                             std::string code, Secret secret,
                                                             Origin origin, Done done) const;
    // `identifier` must name the signed-in account. Under client hashing the
    // client needed it anyway — the current credential is derived under the
    // account's stored salt, which is looked up by identifier — and the new one
    // is enrolled under the salt derived from it.
    [[nodiscard]] std::optional<AccountAnswer> change(const Uuid& user_id, const Uuid& session_id,
                                                      std::string identifier, Secret current,
                                                      Secret replacement, Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> refresh(std::string refresh_token,
                                                       Done done) const;
    [[nodiscard]] std::optional<AccountAnswer> sign_out(const Uuid& user_id,
                                                        const Uuid& session_id,
                                                        Done done) const;

    [[nodiscard]] const AccountDescription& description() const noexcept {
        return config_.description;
    }
    [[nodiscard]] Hashing hashing() const noexcept { return config_.description.hashing; }

private:
    struct Impl;
    AccountConfig         config_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anvil::accounts
