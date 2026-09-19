#include "anvil/identity/password_service.h"

#include <openssl/crypto.h>

#include <exception>
#include <utility>

#include "anvil/core/thread_pools.h"

namespace anvil::identity {
namespace {

constexpr std::string_view kPoolName = "hash";

// The plaintext must not outlive the task. A std::string's destructor frees the
// buffer without touching its contents, so the password stays legible in freed
// heap until the allocator hands that block to something else — which may be a
// response body. OPENSSL_cleanse survives the optimiser's dead-store removal in
// a way that a plain memset does not (ENGINEERING_RULES.md §5).
void burn(std::string& secret) noexcept {
    if (!secret.empty()) { OPENSSL_cleanse(secret.data(), secret.size()); }
}

}  // namespace

bool PasswordService::saturated() noexcept { return Pools::hash().saturated(); }

Status PasswordService::hash_async(std::string password, HashCallback on_done) const {
    // Captured BY VALUE, never by reference: the caller's frame is gone the
    // moment try_post returns (ENGINEERING_RULES.md §3.3).
    auto task = [this, password = std::move(password), on_done = std::move(on_done)]() mutable {
        HashResult result = fail(ErrorCode::Internal);
        try {
            result = hasher_.hash(password);
        } catch (const std::exception&) {
            // The reason is logged by the pool's guard. It never reaches the
            // client: a hashing failure is an Internal, and Internal carries
            // nothing beyond a request id (docs/00-architecture.md §8).
            result = fail(ErrorCode::Internal);
        }
        burn(password);
        on_done(std::move(result));
    };

    if (!Pools::hash().try_post(anvil::guarded(kPoolName, std::move(task)))) {
        return fail(ErrorCode::ServiceUnavailable);
    }
    return ok();
}

Status PasswordService::verify_async(std::string stored, std::string password,
                                     VerifyCallback on_done) const {
    auto task = [this, stored = std::move(stored), password = std::move(password),
                 on_done = std::move(on_done)]() mutable {
        VerifyResult result = fail(ErrorCode::Internal);
        try {
            if (stored.empty()) {
                // No such account. Burn the same time and the same memory, then
                // report a mismatch — identical work, identical answer and
                // identical timing to a wrong password against a real account.
                hasher_.consume_dummy_time();
                result = VerifyReport{{}, auth::VerifyOutcome::Mismatch};
            } else {
                const auth::VerifyOutcome outcome = hasher_.verify(stored, password);
                std::string upgraded;
                if (outcome == auth::VerifyOutcome::Match && hasher_.needs_rehash(stored)) {
                    // One extra Argon2 call, once per user, on the login that
                    // discovers the stale parameters. Doing it HERE is what lets
                    // the plaintext stay inside this task rather than travelling
                    // to a continuation that would have to keep it alive.
                    upgraded = hasher_.hash(password);
                }
                result = VerifyReport{std::move(upgraded), outcome};
            }
        } catch (const std::exception&) {
            result = fail(ErrorCode::Internal);
        }
        burn(password);
        on_done(std::move(result));
    };

    if (!Pools::hash().try_post(anvil::guarded(kPoolName, std::move(task)))) {
        return fail(ErrorCode::ServiceUnavailable);
    }
    return ok();
}

}  // namespace anvil::identity
