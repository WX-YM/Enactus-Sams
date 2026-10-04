#include "anvil/identity/prehash_service.h"

#include <exception>
#include <utility>
#include <variant>

#include "anvil/core/thread_pools.h"

namespace anvil::identity {
namespace {

[[nodiscard]] bool keyed(const auth::PrehashPolicy& policy) noexcept {
    return std::holds_alternative<auth::PrehashKeyedDigestStage>(policy.server);
}

}  // namespace

bool PrehashService::saturated() const {
    return keyed(hasher_.policy()) ? Pools::cpu().saturated() : Pools::hash().saturated();
}

bool PrehashService::post(std::function<void()> task) const {
    if (keyed(hasher_.policy())) {
        return Pools::cpu().try_post(anvil::guarded("cpu", std::move(task)));
    }
    return Pools::hash().try_post(anvil::guarded("hash", std::move(task)));
}

Status PrehashService::enroll_async(auth::PrehashKey k, auth::PrehashSaltAnswer client,
                                    EnrollCallback on_done) const {
    auto key = std::make_shared<auth::PrehashKey>(std::move(k));
    auto task = [this, key = std::move(key), client, on_done = std::move(on_done)]() {
        EnrollResult result = fail(ErrorCode::Internal);
        try {
            result = hasher_.enroll(*key, client);
        } catch (const std::exception&) {
            // Logged by the pool's guard; the client sees an Internal and a
            // request id, never the reason (docs/00-architecture.md §8).
            result = fail(ErrorCode::Internal);
        }
        on_done(std::move(result));
    };
    if (!post(std::move(task))) { return fail(ErrorCode::ServiceUnavailable); }
    return ok();
}

Status PrehashService::verify_async(std::string stored, auth::PrehashKey k,
                                    VerifyCallback on_done) const {
    auto key = std::make_shared<auth::PrehashKey>(std::move(k));
    auto task = [this, stored = std::move(stored), key = std::move(key),
                 on_done = std::move(on_done)]() {
        VerifyResult result = fail(ErrorCode::Internal);
        try {
            if (stored.empty()) {
                hasher_.consume_dummy_time();
                result = auth::PrehashVerification{{}, crypto::VerifyOutcome::Mismatch};
            } else {
                result = hasher_.verify(stored, *key);
            }
        } catch (const std::exception&) {
            result = fail(ErrorCode::Internal);
        }
        on_done(std::move(result));
    };
    if (!post(std::move(task))) { return fail(ErrorCode::ServiceUnavailable); }
    return ok();
}

}  // namespace anvil::identity
