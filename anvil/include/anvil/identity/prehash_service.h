#pragma once

// The pool-aware wrapper around auth::PrehashHasher — PasswordService's
// counterpart for a deployment in client-prehash mode (docs/05 §12).
//
// The same two rules as PasswordService, with one difference in where the work
// runs:
//
//   * It SHEDS through the return value and never invokes a callback on the
//     calling thread's stack.
//   * The pool is chosen by the SERVER STAGE, because the two stages differ in
//     cost by five orders of magnitude. The Argon2 stage runs on hash_pool,
//     whose size is its memory cap, for every reason PasswordService does. The
//     keyed-digest stage is one HMAC — about a microsecond, no memory to speak
//     of — and running it on hash_pool would put it in a queue sized for 64 MiB
//     jobs, where a burst of plain-mode hashing elsewhere in the process could
//     shed a login that costs nothing. It runs on cpu_pool instead. It is not
//     run inline on the caller's thread, although it could be afforded there,
//     because inline is a callback on the caller's stack and the contract above
//     is worth more than the hop.
//
// The derived key crosses the thread boundary inside a shared_ptr to a
// SecretBuffer: std::function must be copyable and SecretBuffer is move-only,
// and a shared owner is the one way to hand it over without a second plaintext
// copy of the credential. It is zeroed when the last owner — the task — ends.

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "anvil/auth/prehash.h"
#include "anvil/core/result.h"

namespace anvil::identity {

class PrehashService final {
public:
    using EnrollResult = Result<std::string>;
    using VerifyResult = Result<auth::PrehashVerification>;
    using EnrollCallback = std::function<void(EnrollResult)>;
    using VerifyCallback = std::function<void(VerifyResult)>;

    explicit PrehashService(auth::PrehashPolicy policy) : hasher_{std::move(policy)} {}

    // ok()                queued; `on_done` runs on the stage's pool.
    // ServiceUnavailable  that pool's queue is full. `on_done` is NOT invoked.
    [[nodiscard]] Status enroll_async(auth::PrehashKey k, auth::PrehashSaltAnswer client,
                                      EnrollCallback on_done) const;

    // `stored` empty means no account: the dummy record is verified in its place
    // and the outcome is Mismatch, with the same work and the same timing.
    [[nodiscard]] Status verify_async(std::string stored, auth::PrehashKey k,
                                      VerifyCallback on_done) const;

    // Advisory admission control against the pool this deployment's stage runs
    // on, for a caller that wants to shed before its user lookup.
    [[nodiscard]] bool saturated() const;

    // The synchronous half: salt derivation, the salt answer and credential
    // decoding cost microseconds and touch no pool, so a handler calls them
    // directly.
    [[nodiscard]] const auth::PrehashHasher& hasher() const noexcept { return hasher_; }

    PrehashService(const PrehashService&) = delete;
    PrehashService& operator=(const PrehashService&) = delete;

private:
    [[nodiscard]] bool post(std::function<void()> task) const;

    auth::PrehashHasher hasher_;
};

}  // namespace anvil::identity
