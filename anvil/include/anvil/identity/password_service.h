#pragma once

// The pool-aware wrapper around auth::PasswordHasher.
//
// The primitive knows how to hash. This knows the two things that make hashing
// safe to expose to the internet:
//
//   1. It NEVER runs on a Trantor event-loop thread. One Argon2id call blocks
//      for ~100 ms; on a loop thread that is every connection that loop owns
//      stalled for a tenth of a second (CLAUDE.md §4).
//   2. It SHEDS. hash_pool's size IS the memory cap — size × the per-hash memory
//      budget is the worst-case RSS one attacker can pin by opening concurrent
//      logins — and its queue is bounded. When the queue is full the answer is
//      503, returned BEFORE any database access, rather than a queued request
//      that will still be holding 64 MiB when it finally runs.
//
// Shedding is reported as a RETURN VALUE rather than through the callback, so
// the callback is never invoked re-entrantly from the calling thread. A caller
// that sheds simply returns; it never has to reason about whether its own
// continuation already ran on the stack beneath it.

#include <functional>
#include <string>
#include <string_view>

#include "anvil/auth/password.h"
#include "anvil/core/result.h"

namespace anvil::identity {

class PasswordService final {
public:
    // A verify may also produce an upgraded hash, and it has to happen inside
    // the SAME task: rehashing needs the plaintext, and the plaintext is zeroed
    // the moment the task ends. Returning it from a later callback would mean
    // either keeping the password alive across a thread hop or not rehashing at
    // all.
    struct VerifyReport final {
        // Non-empty only when the stored parameters were below policy AND the
        // password verified. The caller writes it back CONDITIONALLY on the old
        // hash, so a concurrent password change is never overwritten.
        std::string         upgraded_hash;
        auth::VerifyOutcome outcome;
    };

    using HashResult = Result<std::string>;
    using VerifyResult = Result<VerifyReport>;
    using HashCallback = std::function<void(HashResult)>;
    using VerifyCallback = std::function<void(VerifyResult)>;

    explicit PasswordService(auth::Argon2Params params) : hasher_{params} {}

    // ok()                the work is queued; `on_done` runs on hash_pool.
    // ServiceUnavailable  the queue is full. `on_done` is NOT invoked.
    [[nodiscard]] Status hash_async(std::string password, HashCallback on_done) const;

    // `stored` empty means the account does not exist. The DUMMY hash is
    // verified in its place so the elapsed time and the memory traffic match a
    // real verify, and the outcome is Mismatch. Skipping the work would make a
    // missing account answer in microseconds and an existing one in ~100 ms,
    // which is an account-enumeration oracle anybody with a stopwatch can read.
    [[nodiscard]] Status verify_async(std::string stored, std::string password,
                                      VerifyCallback on_done) const;

    // Advisory admission control, for a caller that wants to shed BEFORE it
    // touches the database. try_post is still the authority; this only lets the
    // login path refuse without first doing a user lookup it is about to throw
    // away.
    [[nodiscard]] static bool saturated() noexcept;

    [[nodiscard]] bool needs_rehash(std::string_view encoded) const noexcept {
        return hasher_.needs_rehash(encoded);
    }
    [[nodiscard]] const auth::Argon2Params& params() const noexcept { return hasher_.params(); }

    PasswordService(const PasswordService&) = delete;
    PasswordService& operator=(const PasswordService&) = delete;

private:
    auth::PasswordHasher hasher_;
};

}  // namespace anvil::identity
