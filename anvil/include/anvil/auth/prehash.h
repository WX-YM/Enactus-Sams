#pragma once

// Client-side password prehashing ("server relief").
//
// The browser runs the expensive Argon2id over the password and sends its
// OUTPUT, `k`. The server never sees the password and never stores `k`: it
// stores a SERVER STAGE over `k` — a keyed digest, or a second Argon2id —
// and verifies a later `k` against that.
//
// What each half buys, and what it does not:
//
//   * The expensive work moves off the server. With the keyed-digest stage a
//     login costs one HMAC (~1 µs) instead of ~100 ms of CPU and 64 MiB of RSS on
//     hash_pool, so the pool's memory cap stops being reachable from the login
//     route at all.
//   * The plaintext never reaches this process, a TLS-terminating proxy, a
//     request log or a crash dump. What does arrive is bound to one account's
//     salt, so it is worthless against any other site the password was reused on.
//   * Offline resistance after a database leak is UNCHANGED from plain mode: the
//     client salt and parameters are in the record, so every guess still costs a
//     full Argon2id at the client-stage parameters, plus the stage.
//   * The server stage is what stops the database being a table of working
//     credentials. `k` IS the credential now; storing it verbatim would let
//     anybody holding a dump sign in as everyone in it. The stage is therefore
//     mandatory — there is no "store k" option to choose.
//
// The client is not a security boundary, and nothing here depends on it being
// one. A client that skips the prehash and posts garbage gets Mismatch. The
// controls are the stage, the rate limits and the timing equaliser, all of which
// run here.
//
// Plain mode (auth/password.h) is unchanged and remains fully supported. A
// deployment chooses one mode; docs/05-auth-sessions.md §12 says how to move an
// existing plain-mode deployment across without asking anybody to reset a
// password.
//
// Wire contract, shared byte-for-byte with hammer (docs/05 §12):
//
//   k          = Argon2id(UTF-8(NFC(password)), salt16, m, t, p, T = 32, v = 0x13)
//   credential = base64url(k), unpadded — 43 characters
//   salt route = {"algorithm":"argon2id","version":19,"salt":"<b64url>",
//                 "memory_kib":M,"iterations":T,"parallelism":P,"hash_bytes":32}
//
// Every call that performs Argon2 BLOCKS and must run on hash_pool, never on a
// Trantor event-loop thread (CLAUDE.md §4). identity/prehash_service.h is the
// pool-aware wrapper; this header enforces nothing about threads.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/crypto/argon2.h"
#include "anvil/crypto/secret.h"

namespace anvil::auth {

// Fixed by version 1 of the record and the wire. A different length is a
// different version, not a parameter: the stored record, the salt answer and the
// client all have to agree on it, and the only way three parties stay agreed is
// for none of them to be told it.
inline constexpr std::size_t kPrehashSaltBytes = 16;
inline constexpr std::size_t kPrehashKeyBytes = 32;
inline constexpr std::size_t kPrehashCredentialChars = 43;

// A key id names a pepper inside a stored record, so it is bounded and drawn
// from a charset that can never contain the record's own `$` delimiter.
inline constexpr std::size_t kPrehashMaxKeyIdChars = 16;

using PrehashSalt = std::array<std::uint8_t, kPrehashSaltBytes>;

// `k`. A SecretBuffer, because it is the credential: whoever holds it can sign
// in, so it is zeroed on every path out of scope exactly as a password is.
using PrehashKey = crypto::SecretBuffer<kPrehashKeyBytes>;

// What the salt route answers, and what the client hashes with.
struct PrehashSaltAnswer final {
    PrehashSalt          salt;
    crypto::Argon2Params params;
};

// --- server stages -----------------------------------------------------------

// HMAC-SHA256(pepper, "anvil.prehash.stage.v1" ‖ 0x00 ‖ k).
//
// The pepper lives in configuration and never in the database, so a dump
// without the configuration cannot even START an offline guess — and with it,
// every guess still costs the full client-stage Argon2id.
struct PrehashKeyedDigestStage final {
    std::string    key_id;
    crypto::Key256 key;
};

// A full Argon2id over the raw bytes of `k`, with a fresh 16-byte salt.
//
// The existing server-side hash, kept as a stage for a deployment that wants
// defence in depth against a client stage that later proves too weak. It
// costs what plain mode costs, so it gives back the server-relief half of this
// module; that is a trade a deployment makes knowingly, never by default.
struct PrehashArgon2Stage final {
    crypto::Argon2Params params;
};

using PrehashServerStage = std::variant<PrehashKeyedDigestStage, PrehashArgon2Stage>;

struct PrehashRetiredPepper final {
    std::string    key_id;
    crypto::Key256 key;
};

struct PrehashPolicy final {
    // What a NEW enrolment and a missing account's salt answer use. An existing
    // record keeps the parameters it was enrolled with: raising these cannot
    // upgrade it, because the server never holds the password. It is upgraded at
    // the next password change (docs/05 §12).
    crypto::Argon2Params client;

    PrehashServerStage server;

    // Peppers of earlier rotations. A record under one still verifies, and
    // `needs_rehash` reports it so the login that proves it moves it onto the
    // current stage. Held by the POLICY rather than by the keyed stage because a
    // deployment that moves from the keyed stage to the Argon2 stage still has
    // every existing record under a pepper, and dropping the peppers with the
    // stage kind would lock every one of those accounts out.
    std::vector<PrehashRetiredPepper> retired_peppers;

    // Keys the salt a missing account is answered with, so that the answer is
    // stable per identifier and indistinguishable from a real account's.
    crypto::Key256 salt_key;
};

struct PrehashVerification final {
    // Non-empty only when the credential matched AND the record's server stage
    // is behind policy. The caller writes it back CONDITIONALLY on the record it
    // verified, exactly as with PasswordService, so a concurrent password change
    // is never overwritten by a rehash of the credential it replaced.
    std::string           upgraded_record;
    crypto::VerifyOutcome outcome;
};

// --- the hasher --------------------------------------------------------------

class PrehashHasher final {
public:
    // Throws std::invalid_argument on a policy that cannot be right: client
    // parameters below the floor plain mode enforces, a key id outside
    // [a-z0-9]{1,16}, or a pepper id that repeats another. Configuration is
    // checked once, at boot, not on the first login that trips over it.
    explicit PrehashHasher(PrehashPolicy policy);

    // HMAC-SHA256(salt_key, "anvil.prehash.salt.v1" ‖ 0x00 ‖ kind ‖ identifier),
    // first 16 bytes. `kind` separates identifier spaces that could otherwise
    // collide — an email and a username can be the same string. The reference
    // application passes identity::LoginIdentity's value; the byte is the
    // caller's, so this module does not depend on the identity layer.
    //
    // `canonical_identifier` must be the form the login LOOKUP uses. Salting one
    // spelling and looking up another is an account whose salt answer depends on
    // how its owner typed their address.
    [[nodiscard]] PrehashSalt derive_salt(std::uint8_t kind,
                                          std::string_view canonical_identifier) const;

    // The salt route's answer. `stored` is the account's record, or nullopt when
    // there is no account; either way the caller has already done the one
    // indexed lookup, so both branches cost the same round trips. A record that
    // is present and not a prehash record answers as a missing account would:
    // the login that follows fails identically, and the answer discloses nothing
    // a missing account's would not.
    [[nodiscard]] PrehashSaltAnswer answer_for(std::optional<std::string_view> stored,
                                               std::uint8_t kind,
                                               std::string_view canonical_identifier) const;

    // The ordinary enrolment: the client sent `k`, computed against `client` —
    // which the caller derives itself (derive_salt + policy().client) and never
    // takes from the request body.
    [[nodiscard]] std::string enroll(const PrehashKey& k, const PrehashSaltAnswer& client) const;

    // Enrolment when the server holds the plaintext: an operator-created
    // account, a seeded one, a test. NFC exactly as auth::PasswordHasher does,
    // then the client stage at policy().client, then enroll(). Throws
    // std::invalid_argument over kMaxPasswordBytes, as PasswordHasher does.
    [[nodiscard]] std::string enroll_plaintext(std::string_view password,
                                               const PrehashSalt& salt) const;

    // Malformed covers an unparseable record, an unwrapped plain-mode
    // `$argon2id$` record (accepting one would make the stored value itself a
    // working credential), and a pepper id this process no longer holds. Every
    // caller maps it to the same answer as Mismatch; it exists so the server can
    // alert on a corrupt row.
    [[nodiscard]] PrehashVerification verify(std::string_view stored, const PrehashKey& k) const;

    // The same work as a verify against a record of the current stage, for a
    // login that matched no account. Without it a missing account answers in the
    // time of a parse and a real one in the time of the stage.
    void consume_dummy_time() const noexcept;

    // True when the record's SERVER stage is behind policy: another stage kind,
    // a retired pepper, or Argon2 stage parameters below policy. Unparseable
    // counts. Client-stage parameters are never considered, because nothing
    // that happens at login can change them.
    [[nodiscard]] bool needs_rehash(std::string_view stored) const noexcept;

    // Rewrites a plain-mode record as a prehash record, OFFLINE: no password and
    // nobody signing in. PasswordHasher's raw output over UTF-8(NFC(password))
    // with a 32-byte tag IS `k` for that record's own salt and parameters, so
    // the wrapped record accepts exactly the credential a client computes from
    // the same password. ValidationFailed for anything that is not such a record
    // — another algorithm, version, salt or tag length — because guessing would
    // lock its owner out silently.
    [[nodiscard]] Result<std::string> wrap_legacy(std::string_view legacy) const;

    [[nodiscard]] const PrehashPolicy& policy() const noexcept { return policy_; }

private:
    PrehashPolicy policy_;
    PrehashKey    dummy_key_;
    std::string   dummy_record_;
};

// --- the wire ------------------------------------------------------------------

// Exactly kPrehashCredentialChars of unpadded base64url, decoded; nullopt for
// anything else. Length is checked before a byte is decoded, so an oversized
// field costs a comparison.
[[nodiscard]] std::optional<PrehashKey> decode_prehash_credential(std::string_view wire) noexcept;

// Appends the salt answer's JSON object to `body`. The one place its shape is
// written, so the key order in the contract is the key order on the wire.
void append_prehash_salt_answer(std::string& body, const PrehashSaltAnswer& answer);

}  // namespace anvil::auth
