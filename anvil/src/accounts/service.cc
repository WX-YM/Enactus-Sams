#include "anvil/accounts/service.h"

#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

#include <trantor/utils/Logger.h>

#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/password_service.h"
#include "anvil/identity/prehash_service.h"
#include "anvil/identity/users.h"
#include "anvil/identity/verification.h"

namespace anvil::accounts {
namespace {

using identity::UserAuthRecord;
using anvil::UserStatus;

[[nodiscard]] AccountAnswer answer(ErrorCode code) {
    AccountAnswer out;
    out.code = code;
    return out;
}

[[nodiscard]] AccountAnswer invalid(std::string_view field, input::Reason reason) {
    AccountAnswer out = answer(ErrorCode::ValidationFailed);
    out.fields.push_back(input::FieldError{field, reason});
    return out;
}

// The key a code is stored under: which flow it belongs to, which kind of
// identifier, and the identifier. The purpose is part of the key so that a code
// issued to verify an address cannot authorise a new password for it, or the
// other way round — both prove possession of the address, but only one was
// ASKED for, and a code that does something its recipient did not request is a
// confused deputy waiting for a phishing email.
[[nodiscard]] std::string code_key(CodePurpose purpose, LoginIdentity kind,
                                   std::string_view canonical) {
    std::string key;
    key.reserve(canonical.size() + 4);
    key.push_back(purpose == CodePurpose::Reset ? 'r' : 'v');
    key.push_back(':');
    key.push_back(static_cast<char>('0' + static_cast<int>(kind)));
    key.push_back(':');
    key.append(canonical);
    return key;
}

// Runs `work` on db_pool with a pooled connection. False when the pool shed it.
template <typename Work>
[[nodiscard]] bool on_db(Work work) {
    return Pools::db().try_post(anvil::guarded("db", [work = std::move(work)]() mutable {
        auto entry = db::MongoPool::instance().acquire();
        work(*entry);
    }));
}

[[nodiscard]] bool limited(http::RateLimiter& limiter, const identity::PackedIp& ip,
                           const http::RateLimitRule& by_ip, std::string_view canonical,
                           const http::RateLimitRule& by_account) {
    // Both checks always run, so a request refused by the first costs what one
    // refused by the second does.
    const bool ip_ok = limiter.check_ip(ip, by_ip).allowed;
    const bool account_ok = limiter.check_account(canonical, by_account).allowed;
    return !(ip_ok && account_ok);
}

[[nodiscard]] auth::PrehashKey take_key(Secret& secret) {
    return std::move(std::get<auth::PrehashKey>(secret));
}

}  // namespace

// --- the credential machinery, one per mode ---------------------------------------------

struct Verified final {
    crypto::VerifyOutcome outcome;
    std::string           upgraded;
};

struct AccountService::Impl final {
    AccountServiceDeps                        deps;
    identity::UserRepository                  users;
    std::unique_ptr<identity::PasswordService> server;
    std::unique_ptr<identity::PrehashService>  client;

    Impl(AccountServiceDeps d, CredentialPolicy& credentials)
        : deps{std::move(d)}, users{deps.database, deps.users_collection} {
        if (auto* hashing = std::get_if<ServerHashing>(&credentials)) {
            server = std::make_unique<identity::PasswordService>(hashing->params);
        } else {
            client = std::make_unique<identity::PrehashService>(
                std::move(std::get<ClientHashing>(credentials).prehash));
        }
    }

    // Verifies `secret` against `stored` — empty for no account, which burns the
    // same work against a dummy — and calls `then` on a pool thread. False when
    // the pool shed it, and `then` is not called.
    [[nodiscard]] bool verify(std::string stored, std::shared_ptr<Secret> secret,
                              std::function<void(Result<Verified>)> then) const {
        if (server) {
            const Status queued = server->verify_async(
                std::move(stored), std::get<std::string>(*secret),
                [then](identity::PasswordService::VerifyResult result) {
                    if (!result) {
                        then(result.error());
                        return;
                    }
                    then(Verified{result.value().outcome, std::move(result.value().upgraded_hash)});
                });
            return queued.ok();
        }
        const Status queued = client->verify_async(
            std::move(stored), take_key(*secret),
            [then](identity::PrehashService::VerifyResult result) {
                if (!result) {
                    then(result.error());
                    return;
                }
                then(Verified{result.value().outcome,
                              std::move(result.value().upgraded_record)});
            });
        return queued.ok();
    }

    // Enrols `secret` as a new stored record. Under client hashing the salt is
    // DERIVED here, from the identifier the enrolling request names — never taken
    // from the body (docs/05 §12).
    [[nodiscard]] bool enroll(std::shared_ptr<Secret> secret, LoginIdentity kind,
                              const std::string& canonical,
                              std::function<void(Result<std::string>)> then) const {
        if (server) {
            return server->hash_async(std::get<std::string>(*secret),
                                      [then](identity::PasswordService::HashResult result) {
                                          then(std::move(result));
                                      })
                .ok();
        }
        const auth::PrehashHasher& hasher = client->hasher();
        auth::PrehashSaltAnswer salt{
            hasher.derive_salt(static_cast<std::uint8_t>(kind), canonical),
            hasher.policy().client};
        return client
            ->enroll_async(take_key(*secret), salt,
                           [then](identity::PrehashService::EnrollResult result) {
                               then(std::move(result));
                           })
            .ok();
    }
};

// --- construction --------------------------------------------------------------------------

AccountService::AccountService(AccountServiceDeps deps, AccountConfig config)
    : config_{std::move(config)} {
    if (!account_description_is_well_formed(config_.description)) {
        throw std::invalid_argument{"the account description is not well-formed"};
    }
    const bool client_policy = std::holds_alternative<ClientHashing>(config_.credentials);
    if (client_policy != (config_.description.hashing == Hashing::Client)) {
        // The descriptor tells every client which to do. A server doing the
        // other refuses every one of them, and says so only in its logs.
        throw std::invalid_argument{"the credential policy disagrees with the description"};
    }
    if (!config_.lock_after || !config_.deliver) {
        throw std::invalid_argument{"an account service needs a lock policy and a delivery"};
    }
    impl_ = std::make_unique<Impl>(std::move(deps), config_.credentials);
}

AccountService::~AccountService() = default;

namespace {

// Hands a code to the application after the request has been answered.
void hand_over(const DeliverCode& deliver, CodeDelivery delivery) {
    try {
        deliver(std::move(delivery));
    } catch (const std::exception& failure) {
        LOG_ERROR << "account code delivery threw: " << failure.what();
    } catch (...) {
        LOG_ERROR << "account code delivery threw";
    }
}

[[nodiscard]] bool secret_matches(Hashing hashing, const Secret& secret) noexcept {
    return hashing == Hashing::Client ? std::holds_alternative<auth::PrehashKey>(secret)
                                      : std::holds_alternative<std::string>(secret);
}

[[nodiscard]] std::string_view secret_field(Hashing hashing) noexcept {
    return hashing == Hashing::Client ? "credential" : "password";
}

}  // namespace

// --- the salt route ---------------------------------------------------------------------

std::optional<AccountAnswer> AccountService::salt(std::string identifier, SaltPurpose purpose,
                                                  Origin origin, Done done) const {
    if (!impl_->client) { return answer(ErrorCode::NotFound); }

    const LoginIdentity kind = kind_by_shape(identifier);
    std::string canonical;
    const input::Reason reason = canonicalise(kind, identifier, canonical);
    if (reason != input::Reason::Ok) { return invalid("identifier", reason); }

    const auth::PrehashHasher& hasher = impl_->client->hasher();
    if (purpose == SaltPurpose::Enroll) {
        // A pure function of the identifier: no lookup, so nothing in it can
        // depend on whether an account exists.
        AccountAnswer out;
        out.salt = auth::PrehashSaltAnswer{
            hasher.derive_salt(static_cast<std::uint8_t>(kind), canonical), hasher.policy().client};
        return out;
    }

    const Impl* impl = impl_.get();
    const AccountBudgets budgets = config_.budgets;
    const bool queued = on_db([impl, budgets, kind, canonical, origin, done](
                                  mongocxx::client& db) {
        // The sign-in budget, per address only: the salt route is the first half
        // of a sign-in, and per-identifier counting here would let anybody spend
        // somebody else's sign-in budget by asking for their salt.
        if (!impl->deps.limiter.check_ip(origin.ip, budgets.sign_in_ip).allowed) {
            done(answer(ErrorCode::RateLimited));
            return;
        }
        const auto found = impl->users.find_for_login(db, canonical, kind);
        if (!found) {
            done(answer(found.error().code));
            return;
        }
        std::optional<std::string_view> stored;
        if (found.value().has_value()) { stored = found.value()->password_hash; }
        AccountAnswer out;
        out.salt = impl->client->hasher().answer_for(stored, static_cast<std::uint8_t>(kind),
                                                     canonical);
        done(std::move(out));
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

// --- registration -----------------------------------------------------------------------

std::optional<AccountAnswer> AccountService::register_account(RegisterRequest request,
                                                              Done done) const {
    const AccountSchema& schema = *config_.description.schema;
    const Hashing hashing = config_.description.hashing;
    AccountAnswer refused = answer(ErrorCode::ValidationFailed);

    // --- identifiers, canonicalised, each against the schema ------------------------
    struct Canonical final {
        std::string email;
        std::string username;
        std::string phone;
    } canonical;
    const std::array<std::pair<LoginIdentity, const std::string*>, 3> given{{
        {LoginIdentity::Email, &request.email},
        {LoginIdentity::Username, &request.username},
        {LoginIdentity::Phone, &request.phone},
    }};
    for (const auto& [kind, raw] : given) {
        const IdentifierSpec* spec = find_identifier(schema, kind);
        std::string& out = kind == LoginIdentity::Email      ? canonical.email
                           : kind == LoginIdentity::Username ? canonical.username
                                                             : canonical.phone;
        if (spec == nullptr) {
            if (!raw->empty()) {
                refused.fields.push_back({identifier_name(kind), input::Reason::NotAllowed});
            }
            continue;
        }
        if (raw->empty()) {
            if (spec->required) {
                refused.fields.push_back({identifier_name(kind), input::Reason::Required});
            }
            continue;
        }
        const input::Reason reason = canonicalise(kind, *raw, out);
        if (reason != input::Reason::Ok) {
            refused.fields.push_back({identifier_name(kind), reason});
        }
    }

    // --- the profile ------------------------------------------------------------------
    std::vector<std::pair<std::string_view, std::string_view>> profile;
    profile.reserve(schema.profile.size());
    for (const auto& [key, value] : request.profile) {
        bool known = false;
        for (const ProfileFieldSpec& spec : schema.profile) { known = known || spec.key == key; }
        // Reported with an EMPTY field name: the key came from the request, and
        // a client must never choose what appears in a response (input/fields.h).
        if (!known) { refused.fields.push_back({std::string_view{}, input::Reason::NotAllowed}); }
    }
    for (const ProfileFieldSpec& spec : schema.profile) {
        const std::string* value = nullptr;
        for (const auto& [key, given_value] : request.profile) {
            if (key == spec.key) { value = &given_value; }
        }
        if (value == nullptr || value->empty()) {
            if (spec.required) { refused.fields.push_back({spec.key, input::Reason::Required}); }
            continue;
        }
        const input::Reason reason = input::check_text(*value, spec.rules);
        if (reason != input::Reason::Ok) {
            refused.fields.push_back({spec.key, reason});
            continue;
        }
        profile.emplace_back(spec.key, *value);
    }

    // --- the secret ------------------------------------------------------------------
    if (!secret_matches(hashing, request.secret)) {
        refused.fields.push_back({secret_field(hashing), input::Reason::Required});
    } else if (hashing == Hashing::Server) {
        // Only the server-hashed password can be checked here. A prehashed one
        // arrives as 32 opaque bytes: its length and whether it is on a breach
        // list were the client's to check, and a client that skipped them has
        // weakened only its own account (docs/05 §13).
        const std::string& password = std::get<std::string>(request.secret);
        input::Reason reason = input::check_password(password);
        if (reason == input::Reason::Ok && input::is_breached_password(password)) {
            reason = input::Reason::Breached;
        }
        if (reason != input::Reason::Ok) { refused.fields.push_back({"password", reason}); }
    }

    if (!refused.fields.empty()) { return refused; }

    // --- enrol, then insert --------------------------------------------------------------
    const LoginIdentity contact = schema.contact;
    const std::string contact_canonical =
        contact == LoginIdentity::Email ? canonical.email : canonical.phone;
    const Locale locale = request.locale.value_or(Locale{});

    // Everything the later hops need, owned in one place so each hop captures a
    // pointer rather than copying a registration into every lambda.
    struct State final {
        RegisterRequest                                              request;
        Canonical                                                    canonical;
        std::vector<std::pair<std::string, std::string>>             profile;
        std::string                                                  contact_canonical;
        Locale                                                       locale;
        std::shared_ptr<Secret>                                      secret;
    };
    auto state = std::make_shared<State>();
    state->canonical = std::move(canonical);
    for (const auto& [key, value] : profile) { state->profile.emplace_back(key, value); }
    state->contact_canonical = contact_canonical;
    state->locale = locale;
    state->secret = std::make_shared<Secret>(std::move(request.secret));
    state->request = std::move(request);

    const Impl* impl = impl_.get();
    const AccountConfig* config = &config_;
    const bool queued = on_db([impl, config, state, contact, done](mongocxx::client&) {
        if (!impl->deps.limiter.check_ip(state->request.origin.ip, config->budgets.register_ip)
                 .allowed) {
            done(answer(ErrorCode::RateLimited));
            return;
        }

        // Enrolment runs BEFORE the insert, for a new address and a taken one
        // alike: skipping it for a duplicate would answer a registration of a
        // taken address in microseconds and a new one in the time of a hash.
        const bool enrolling = impl->enroll(
            state->secret, contact, state->contact_canonical,
            [impl, config, state, contact, done](Result<std::string> record) {
                if (!record) {
                    done(answer(record.error().code));
                    return;
                }
                const bool inserted = on_db([impl, config, state, contact, done,
                                             hash = std::move(record.value())](
                                                mongocxx::client& db) {
                    std::vector<std::pair<std::string_view, std::string_view>> stored_profile;
                    for (const auto& [key, value] : state->profile) {
                        stored_profile.emplace_back(key, value);
                    }
                    const bool verify_first =
                        config->description.schema->activation == Activation::AfterVerification;
                    const Status stored = impl->users.insert(
                        db, identity::NewUser{
                                .id = anvil::uuid::generate_v7(),
                                .email_normalised = state->canonical.email,
                                .email_display = state->request.email,
                                .username_normalised = state->canonical.username,
                                .username_display = state->request.username,
                                .password_hash = hash,
                                .phone_e164 = state->canonical.phone,
                                .locale = state->locale,
                                .status = verify_first ? UserStatus::PendingVerification
                                                       : UserStatus::Active,
                                .profile = stored_profile});
                    const bool duplicate = !stored && stored.error().code == ErrorCode::Conflict;
                    if (!stored && !duplicate) {
                        done(answer(stored.error().code));
                        return;
                    }

                    // A code is issued for a new address AND a taken one — the
                    // row is what makes both paths the same work (identity/
                    // verification.h). Only the new one is sent it.
                    std::optional<identity::VerificationService::IssuedCode> code;
                    if (verify_first) {
                        auto issued = impl->deps.verification.issue(
                            db, code_key(CodePurpose::Verify, contact, state->contact_canonical),
                            config->code_lifetime, db::now_ms());
                        if (issued) { code = std::move(issued.value()); }
                    }

                    done(answer(ErrorCode::Ok));

                    if (duplicate) {
                        // The issue above REPLACED any code this address held — one
                        // live row per address (identity/verification.h). For an
                        // account still awaiting verification that code was the only
                        // one its owner had, so the new one is sent in its place,
                        // exactly as a resend would; otherwise a second registration
                        // by anybody would silently void a stranger's pending code.
                        // An active account's owner is told somebody tried instead.
                        // Found AFTER the answer: a read only the duplicate path
                        // makes must not be in its response time.
                        const auto existing = impl->users.find_by_identity(
                            db, state->contact_canonical, contact);
                        const bool pending =
                            existing && existing.value().has_value() &&
                            existing.value()->status == UserStatus::PendingVerification;
                        if (pending && code.has_value()) {
                            hand_over(config->deliver,
                                      CodeDelivery{CodePurpose::Verify, contact,
                                                   state->contact_canonical, std::move(code->code),
                                                   existing.value()->locale});
                        } else {
                            hand_over(config->deliver,
                                      CodeDelivery{CodePurpose::AccountExists, contact,
                                                   state->contact_canonical, {}, state->locale});
                        }
                    } else if (code.has_value()) {
                        hand_over(config->deliver,
                                  CodeDelivery{CodePurpose::Verify, contact,
                                               state->contact_canonical, std::move(code->code),
                                               state->locale});
                    }
                });
                if (!inserted) { done(answer(ErrorCode::ServiceUnavailable)); }
            });
        if (!enrolling) { done(answer(ErrorCode::ServiceUnavailable)); }
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

// --- contact verification ---------------------------------------------------------------

std::optional<AccountAnswer> AccountService::verify(std::string identifier, std::string code,
                                                    Origin origin, Done done) const {
    const LoginIdentity contact = config_.description.schema->contact;
    std::string canonical;
    if (canonicalise(contact, identifier, canonical) != input::Reason::Ok) {
        return answer(ErrorCode::Unauthenticated);
    }

    const Impl* impl = impl_.get();
    const AccountBudgets budgets = config_.budgets;
    const bool queued = on_db([impl, budgets, contact, canonical = std::move(canonical),
                               code = std::move(code), origin, done](mongocxx::client& db) {
        if (limited(impl->deps.limiter, origin.ip, budgets.code_ip, canonical,
                    budgets.code_account)) {
            done(answer(ErrorCode::RateLimited));
            return;
        }
        const db::TimeMs now = db::now_ms();
        const auto verified = impl->deps.verification.verify(
            db, code_key(CodePurpose::Verify, contact, canonical), code, now);
        if (!verified || !verified.value()) {
            done(answer(ErrorCode::Unauthenticated));
            return;
        }
        const auto account = impl->users.find_by_identity(db, canonical, contact);
        if (!account || !account.value().has_value()) {
            done(answer(ErrorCode::Unauthenticated));
            return;
        }
        auto session = db.start_session();
        const auto activated = impl->users.activate(db, session, account.value()->id, now);
        // False means the account was not pending: verifying twice is not an
        // error (identity/users.h), and an account disabled since the code was
        // sent stays disabled because the status is in activate's filter.
        done(answer(activated ? ErrorCode::Ok : activated.error().code));
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

std::optional<AccountAnswer> AccountService::resend(std::string identifier, Origin origin,
                                                    Done done) const {
    const LoginIdentity contact = config_.description.schema->contact;
    std::string canonical;
    const input::Reason reason = canonicalise(contact, identifier, canonical);
    if (reason != input::Reason::Ok) { return invalid("identifier", reason); }

    const Impl* impl = impl_.get();
    const AccountConfig* config = &config_;
    const bool queued = on_db([impl, config, contact, canonical = std::move(canonical), origin,
                               done](mongocxx::client& db) {
        if (limited(impl->deps.limiter, origin.ip, config->budgets.issue_ip, canonical,
                    config->budgets.issue_account)) {
            done(answer(ErrorCode::RateLimited));
            return;
        }
        const auto account = impl->users.find_by_identity(db, canonical, contact);
        // Issued whether or not there is an account, so both answers are the
        // same work. Only a pending account is sent it.
        auto issued = impl->deps.verification.issue(
            db, code_key(CodePurpose::Verify, contact, canonical), config->code_lifetime,
            db::now_ms());
        done(answer(ErrorCode::Ok));

        if (issued && account && account.value().has_value() &&
            account.value()->status == UserStatus::PendingVerification) {
            hand_over(config->deliver,
                      CodeDelivery{CodePurpose::Verify, contact, canonical,
                                   std::move(issued.value().code), account.value()->locale});
        }
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

// --- sign-in ------------------------------------------------------------------------------

std::optional<AccountAnswer> AccountService::sign_in(std::string identifier, Secret secret,
                                                     Origin origin, Done done) const {
    const LoginIdentity kind = kind_by_shape(identifier);
    const IdentifierSpec* spec = find_identifier(*config_.description.schema, kind);
    std::string canonical;
    // A kind the schema does not let anybody sign in with, an identifier that
    // cannot be one, a secret of the wrong mode: one answer, and none of them
    // says anything about an account.
    if (spec == nullptr || !spec->sign_in ||
        canonicalise(kind, identifier, canonical) != input::Reason::Ok ||
        !secret_matches(config_.description.hashing, secret)) {
        return answer(ErrorCode::Unauthenticated);
    }

    const Impl* impl = impl_.get();
    const AccountConfig* config = &config_;
    auto shared_secret = std::make_shared<Secret>(std::move(secret));
    const bool queued = on_db([impl, config, kind, canonical = std::move(canonical), origin,
                               shared_secret, done](mongocxx::client& db) {
        if (limited(impl->deps.limiter, origin.ip, config->budgets.sign_in_ip, canonical,
                    config->budgets.sign_in_account)) {
            done(answer(ErrorCode::RateLimited));
            return;
        }
        const auto found = impl->users.find_for_login(db, canonical, kind);
        if (!found) {
            done(answer(found.error().code));
            return;
        }
        auto record = std::make_shared<std::optional<UserAuthRecord>>(found.value());
        std::string stored = record->has_value() ? (*record)->password_hash : std::string{};

        const bool verifying = impl->verify(
            std::move(stored), shared_secret,
            [impl, config, record, origin, done](Result<Verified> verified) {
                if (!verified) {
                    done(answer(verified.error().code));
                    return;
                }
                if (!record->has_value()) {
                    done(answer(ErrorCode::Unauthenticated));
                    return;
                }
                const UserAuthRecord& account = **record;
                const db::TimeMs now = db::now_ms();
                const bool match = verified.value().outcome == crypto::VerifyOutcome::Match;
                const bool locked = account.lock_until.has_value() && *account.lock_until > now;
                const bool active = account.status == UserStatus::Active;

                if (!match) {
                    // Counted, and the lock the application's policy computes
                    // from the count is stored. Answered either way: a shed
                    // count is a lost increment, not a reason to say more.
                    const bool counted = on_db([impl, config, record, now, done](
                                                   mongocxx::client& db2) {
                        const UserAuthRecord& who = **record;
                        (void)impl->users.record_login_failure(
                            db2, who.id, config->lock_after(who.failure_count + 1, now));
                        done(answer(ErrorCode::Unauthenticated));
                    });
                    if (!counted) { done(answer(ErrorCode::Unauthenticated)); }
                    return;
                }
                // The right secret for an account that may not sign in — pending
                // verification, disabled, locked — answers exactly as the wrong
                // one does (docs/05 §4). This is the check the reference login
                // was missing.
                if (!active || locked) {
                    done(answer(ErrorCode::Unauthenticated));
                    return;
                }

                const bool opened = on_db([impl, record, origin, now, done,
                                           upgraded = std::move(verified.value().upgraded)](
                                              mongocxx::client& db2) {
                    const UserAuthRecord& who = **record;
                    if (who.failure_count > 0) { (void)impl->users.clear_login_failures(db2, who.id); }
                    if (!upgraded.empty()) {
                        // Conditional on the record just verified, so a password
                        // change that raced this sign-in is never overwritten.
                        (void)impl->users.replace_password_hash(db2, who.id, who.password_hash,
                                                                upgraded);
                    }
                    auto issued = impl->deps.sessions.create(db2, who, origin.ip,
                                                             origin.user_agent, now);
                    if (!issued) {
                        done(answer(issued.error().code));
                        return;
                    }
                    AccountAnswer out;
                    out.session = std::move(issued.value());
                    done(std::move(out));
                });
                if (!opened) { done(answer(ErrorCode::ServiceUnavailable)); }
            });
        if (!verifying) { done(answer(ErrorCode::ServiceUnavailable)); }
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

// --- password reset -------------------------------------------------------------------------

std::optional<AccountAnswer> AccountService::reset_request(std::string identifier, Origin origin,
                                                           Done done) const {
    const LoginIdentity kind = kind_by_shape(identifier);
    const IdentifierSpec* spec = find_identifier(*config_.description.schema, kind);
    if (spec == nullptr || !spec->sign_in) {
        return invalid("identifier", input::Reason::NotAllowed);
    }
    std::string canonical;
    const input::Reason reason = canonicalise(kind, identifier, canonical);
    if (reason != input::Reason::Ok) { return invalid("identifier", reason); }

    const Impl* impl = impl_.get();
    const AccountConfig* config = &config_;
    const bool queued = on_db([impl, config, kind, canonical = std::move(canonical), origin,
                               done](mongocxx::client& db) {
        if (limited(impl->deps.limiter, origin.ip, config->budgets.issue_ip, canonical,
                    config->budgets.issue_account)) {
            done(answer(ErrorCode::RateLimited));
            return;
        }
        const auto found = impl->users.find_for_login(db, canonical, kind);
        // Issued whether or not there is an account: the row is the same work,
        // and it is keyed by what the person will type at the confirm step.
        auto issued = impl->deps.verification.issue(
            db, code_key(CodePurpose::Reset, kind, canonical), config->code_lifetime,
            db::now_ms());
        done(answer(ErrorCode::Ok));

        if (!issued || !found || !found.value().has_value() ||
            found.value()->status != UserStatus::Active) {
            return;
        }
        // AFTER the answer: finding where to send it is a second read that only
        // an existing account costs, and it must not be in the response time.
        const auto account = impl->users.find_credential(db, found.value()->id);
        if (!account || !account.value().has_value()) { return; }
        const LoginIdentity contact = config->description.schema->contact;
        const std::string& address = contact == LoginIdentity::Email
                                         ? account.value()->email_normalised
                                         : account.value()->phone_e164;
        if (address.empty()) { return; }
        hand_over(config->deliver,
                  CodeDelivery{CodePurpose::Reset, contact, address,
                               std::move(issued.value().code), found.value()->locale});
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

std::optional<AccountAnswer> AccountService::reset_confirm(std::string identifier,
                                                           std::string code, Secret secret,
                                                           Origin origin, Done done) const {
    const LoginIdentity kind = kind_by_shape(identifier);
    const IdentifierSpec* spec = find_identifier(*config_.description.schema, kind);
    std::string canonical;
    if (spec == nullptr || !spec->sign_in ||
        canonicalise(kind, identifier, canonical) != input::Reason::Ok ||
        !secret_matches(config_.description.hashing, secret)) {
        return answer(ErrorCode::Unauthenticated);
    }
    if (config_.description.hashing == Hashing::Server) {
        const std::string& password = std::get<std::string>(secret);
        input::Reason reason = input::check_password(password);
        if (reason == input::Reason::Ok && input::is_breached_password(password)) {
            reason = input::Reason::Breached;
        }
        if (reason != input::Reason::Ok) { return invalid("password", reason); }
    }

    const Impl* impl = impl_.get();
    const AccountConfig* config = &config_;
    auto shared_secret = std::make_shared<Secret>(std::move(secret));
    const bool queued = on_db([impl, config, kind, canonical = std::move(canonical),
                               code = std::move(code), origin, shared_secret,
                               done](mongocxx::client& db) {
        if (limited(impl->deps.limiter, origin.ip, config->budgets.code_ip, canonical,
                    config->budgets.code_account)) {
            done(answer(ErrorCode::RateLimited));
            return;
        }
        // The code first: it is the cheap check, and it is what makes the
        // expensive one reachable at all.
        const auto verified = impl->deps.verification.verify(
            db, code_key(CodePurpose::Reset, kind, canonical), code, db::now_ms());
        if (!verified || !verified.value()) {
            done(answer(ErrorCode::Unauthenticated));
            return;
        }
        const auto found = impl->users.find_for_login(db, canonical, kind);
        if (!found || !found.value().has_value() ||
            found.value()->status != UserStatus::Active) {
            done(answer(ErrorCode::Unauthenticated));
            return;
        }
        const Uuid user_id = found.value()->id;

        const bool enrolling = impl->enroll(
            shared_secret, kind, canonical,
            [impl, user_id, done](Result<std::string> record) {
                if (!record) {
                    done(answer(record.error().code));
                    return;
                }
                const bool written = on_db([impl, user_id, done,
                                            hash = std::move(record.value())](
                                               mongocxx::client& db2) {
                    const auto current = impl->users.find_credential(db2, user_id);
                    if (!current || !current.value().has_value()) {
                        done(answer(ErrorCode::Unauthenticated));
                        return;
                    }
                    const auto set = impl->users.set_password_hash(db2, user_id,
                                                                   current.value()->version, hash);
                    if (!set) {
                        done(answer(set.error().code));
                        return;
                    }
                    // Whoever knew the old password is signed out everywhere, and
                    // the epoch ends every access token already issued rather than
                    // waiting out its lifetime.
                    (void)impl->users.clear_login_failures(db2, user_id);
                    (void)impl->deps.sessions.revoke_all(db2, user_id);
                    (void)impl->deps.authz.bump_epoch(db2, user_id);
                    done(answer(ErrorCode::Ok));
                });
                if (!written) { done(answer(ErrorCode::ServiceUnavailable)); }
            });
        if (!enrolling) { done(answer(ErrorCode::ServiceUnavailable)); }
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

// --- password change -------------------------------------------------------------------------

std::optional<AccountAnswer> AccountService::change(const Uuid& user_id, const Uuid& session_id,
                                                    std::string identifier, Secret current,
                                                    Secret replacement, Done done) const {
    const Hashing hashing = config_.description.hashing;
    const LoginIdentity kind = kind_by_shape(identifier);
    const IdentifierSpec* spec = find_identifier(*config_.description.schema, kind);
    std::string canonical;
    if (spec == nullptr || !spec->sign_in ||
        canonicalise(kind, identifier, canonical) != input::Reason::Ok ||
        !secret_matches(hashing, current) || !secret_matches(hashing, replacement)) {
        return answer(ErrorCode::Unauthenticated);
    }
    if (hashing == Hashing::Server) {
        const std::string& password = std::get<std::string>(replacement);
        input::Reason reason = input::check_password(password);
        if (reason == input::Reason::Ok && input::is_breached_password(password)) {
            reason = input::Reason::Breached;
        }
        if (reason != input::Reason::Ok) { return invalid("new_password", reason); }
    }

    const Impl* impl = impl_.get();
    auto old_secret = std::make_shared<Secret>(std::move(current));
    auto new_secret = std::make_shared<Secret>(std::move(replacement));
    const bool queued = on_db([impl, user_id, session_id, kind, canonical = std::move(canonical),
                               old_secret, new_secret, done](mongocxx::client& db) {
        const auto named = impl->users.find_for_login(db, canonical, kind);
        const auto account = impl->users.find_credential(db, user_id);
        if (!named || !account) {
            done(answer(ErrorCode::Internal));
            return;
        }
        // The identifier must be this account's. Otherwise the stored record is
        // withheld and the dummy is verified in its place, so a signed-in caller
        // cannot use this route to time whether an identifier belongs to anyone.
        const bool ours = named.value().has_value() && account.value().has_value() &&
                          named.value()->id == user_id;
        std::string stored = ours ? account.value()->auth.password_hash : std::string{};
        const std::int64_t version = ours ? account.value()->version : 0;

        const bool verifying = impl->verify(
            std::move(stored), old_secret,
            [impl, user_id, session_id, kind, canonical, new_secret, version, ours,
             done](Result<Verified> verified) {
                if (!verified) {
                    done(answer(verified.error().code));
                    return;
                }
                if (!ours || verified.value().outcome != crypto::VerifyOutcome::Match) {
                    done(answer(ErrorCode::Unauthenticated));
                    return;
                }
                const bool enrolling = impl->enroll(
                    new_secret, kind, canonical,
                    [impl, user_id, session_id, version, done](Result<std::string> record) {
                        if (!record) {
                            done(answer(record.error().code));
                            return;
                        }
                        const bool written = on_db([impl, user_id, session_id, version, done,
                                                    hash = std::move(record.value())](
                                                       mongocxx::client& db2) {
                            // Under the version that was verified: a reset or a
                            // second change that landed meanwhile wins, and this
                            // one is refused rather than overwriting it.
                            const auto set =
                                impl->users.set_password_hash(db2, user_id, version, hash);
                            if (!set) {
                                done(answer(set.error().code));
                                return;
                            }
                            // Every OTHER session ends: signing somebody out of the
                            // tab they are changing their password in is the one
                            // outcome nobody wants. The epoch bump ends the other
                            // sessions' access tokens now; this one refreshes into
                            // a new token on its next request.
                            (void)impl->deps.sessions.revoke_others(db2, user_id, session_id);
                            (void)impl->deps.authz.bump_epoch(db2, user_id);
                            done(answer(ErrorCode::Ok));
                        });
                        if (!written) { done(answer(ErrorCode::ServiceUnavailable)); }
                    });
                if (!enrolling) { done(answer(ErrorCode::ServiceUnavailable)); }
            });
        if (!verifying) { done(answer(ErrorCode::ServiceUnavailable)); }
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

// --- refresh and sign-out -----------------------------------------------------------------

std::optional<AccountAnswer> AccountService::refresh(std::string refresh_token,
                                                     Done done) const {
    if (refresh_token.empty()) { return answer(ErrorCode::Unauthenticated); }
    const Impl* impl = impl_.get();
    const bool queued = on_db([impl, token = std::move(refresh_token), done](mongocxx::client& db) {
        auto issued = impl->deps.sessions.refresh(db, token, db::now_ms());
        if (!issued) {
            done(answer(issued.error().code));
            return;
        }
        AccountAnswer out;
        out.session = std::move(issued.value());
        done(std::move(out));
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

std::optional<AccountAnswer> AccountService::sign_out(const Uuid& user_id, const Uuid& session_id,
                                                      Done done) const {
    const Impl* impl = impl_.get();
    const bool queued = on_db([impl, user_id, session_id, done](mongocxx::client& db) {
        // Not reported: revoking a session that is already gone is the ordinary
        // shape of a retried sign-out.
        (void)impl->deps.sessions.revoke(db, session_id, user_id);
        done(answer(ErrorCode::Ok));
    });
    if (!queued) { return answer(ErrorCode::ServiceUnavailable); }
    return std::nullopt;
}

}  // namespace anvil::accounts
