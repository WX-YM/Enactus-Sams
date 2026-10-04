#pragma once

// Password hashing.
//
// Argon2id, always. bcrypt is banned outright, and not on general principle:
// it truncates at 72 BYTES, and Arabic is 2 bytes per character in UTF-8, so a
// 40-character Arabic passphrase is silently cut to 36. The user's deliberately
// long password is weaker than they believe, with no error and no way to detect
// it. A bilingual system cannot use bcrypt without pre-hashing,
// and Argon2id removes the question.
//
// Two cost controls that are security requirements, not tuning knobs:
//
//   * Every call here BLOCKS for ~100 ms and reserves 64 MiB. It must run on
//     hash_pool and never on a Trantor event-loop thread.
//   * hash_pool's size IS the memory cap: size x 64 MiB is the worst-case RSS
//     one attacker can pin by opening concurrent logins. When the
//     queue is full the caller sheds with 503 rather than queueing.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "anvil/crypto/argon2.h"

namespace anvil::auth {

// Argon2Params and VerifyOutcome moved to anvil/crypto/argon2.h, which is the
// one reusable Argon2 component both this primitive and auth::prehash's server
// stage build on (docs/05-auth-sessions.md, client prehash). Aliased rather
// than redeclared so every existing `auth::Argon2Params` and `auth::VerifyOutcome`
// call site keeps compiling unchanged.
using Argon2Params = crypto::Argon2Params;
using VerifyOutcome = crypto::VerifyOutcome;

// Defaults from docs/05-auth-sessions.md §6: m = 64 MiB, t = 3, p = 1.
inline constexpr Argon2Params kDefaultArgon2Params{
    .memory_kib = 65536,
    .iterations = 3,
    .parallelism = 1,
};

// A hard ceiling applied BEFORE the password reaches Argon2. Field validation
// already caps a password at 128 code points; this is defence in depth, because
// an unbounded password is a memory amplification vector regardless of what
// validation ran — and this is the layer that is reached even when none did.
inline constexpr std::size_t kMaxPasswordBytes = 1024;

class PasswordHasher final {
public:
    explicit PasswordHasher(Argon2Params params);

    // Both take the RAW password and normalise internally. Normalisation must
    // be identical at signup and login or the same passphrase typed on two
    // keyboards produces two different hashes and the user simply cannot log
    // in — so it is not left to the caller to remember (docs/03-i18n-utf8.md §6).
    //
    // NFC only: never trimmed, never case-folded. A password is bytes the user
    // chose, and leading whitespace is part of it.
    [[nodiscard]] std::string hash(std::string_view password) const;
    [[nodiscard]] VerifyOutcome verify(std::string_view encoded,
                                       std::string_view password) const;

    // Burns the same time and memory as a real verify, for the case where no
    // account matched. Skipping the hash on an unknown user makes a missing
    // account return in microseconds and an existing one in ~100 ms — a
    // trivially exploitable enumeration oracle.
    void consume_dummy_time() const noexcept;

    // True when a stored hash was produced with parameters below current
    // policy, so a successful login can trigger a background rehash.
    [[nodiscard]] bool needs_rehash(std::string_view encoded) const noexcept;

    [[nodiscard]] const Argon2Params& params() const noexcept { return params_; }

private:
    // Declared before params_ so it is constructed first: the constructor
    // computes it using params_, so params_ must be initialised before it.
    Argon2Params params_;
    std::string  dummy_encoded_;
};

// Parsed cost parameters from an encoded hash, or nullopt if unparseable.
// Exposed for testing and for the rehash policy check.
[[nodiscard]] std::optional<Argon2Params> parse_encoded_params(
    std::string_view encoded) noexcept;

}  // namespace anvil::auth
