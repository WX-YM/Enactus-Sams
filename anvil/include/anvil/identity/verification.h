#pragma once

// Contact-address verification: one live row per address, holding a peppered
// digest of a short numeric code and an attempt counter.
//
// Modelled on capability tokens, which is the right precedent: only a digest is
// stored, consumption is one atomic find_one_and_update, and the TTL index is
// backed by an explicit expiry predicate on every read because the monitor lags
// by up to a minute.
//
// --- why the entropy argument has to be made explicitly ---------------------
//
// A six-digit code is about twenty bits. ENGINEERING_RULES.md §5 permits a fast hash for
// token storage because "tokens carry ≥128 bits of CSPRNG entropy", and that
// justification DOES NOT APPLY HERE: a SHA-256 of a six-digit code is
// exhaustible in microseconds by anyone holding both the collection and the
// pepper.
//
// What makes the scheme acceptable is four controls, and all four are
// load-bearing:
//
//   1. Bounded attempts, incremented ATOMICALLY. Never read-compare-write,
//      which N concurrent guesses defeat outright.
//   2. A short lifetime.
//   3. Two rate-limit buckets — one on guesses, one on re-issues — because
//      minting a fresh row would otherwise reset the attempt counter for free.
//      anvil ships the limiter; the buckets are the application's to declare.
//   4. Byte-identical failure for a wrong code, an expired row, exhausted
//      attempts, and no such address.
//
// Remove any one and the scheme fails. They are enumerated here rather than
// left to be inferred from the code, because the code looks correct without the
// third one and the third one is not in this file.
//
// --- why there is no user id ------------------------------------------------
//
// The duplicate-registration branch has to write a byte-identical row WITHOUT
// first reading the user, or the cost of that read is itself an
// account-enumeration oracle — a fresh registration would do one more query than
// a duplicate, measurably. The account is resolved at verify time by the
// normalised address, which that path has to look up anyway.

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/secret.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"

namespace anvil::identity {

namespace verification_fields {
inline constexpr std::string_view kId = "_id";
// The blind index over the address: HMAC, never a plain hash. A plain digest of
// a structured value is exhaustible, and this column is the whole lookup key.
inline constexpr std::string_view kAddressHash = "ah";
inline constexpr std::string_view kCodeHash = "ch";
inline constexpr std::string_view kAttempts = "n";
inline constexpr std::string_view kExpiresAt = "expires_at";
inline constexpr std::string_view kCreatedAt = "created_at";
inline constexpr std::string_view kUsedAt = "used";
}  // namespace verification_fields

// Five wrong guesses and the row is spent. Against a space of a million that is
// 5e-6 per window, and the re-issue limit is what stops the window being reset
// for the price of one request.
inline constexpr std::int32_t kMaxVerifyAttempts = 5;

// Six digits. Long enough that the bounded-attempt budget makes guessing
// hopeless, short enough to be read off a screen and typed — which is the whole
// reason it is not 128 bits of base64url.
inline constexpr std::size_t kVerificationCodeDigits = 6;

struct NewVerification final {
    crypto::Digest256 address_hash;
    crypto::Digest256 code_hash;
    Uuid              id;
    db::TimeMs        expires_at;
    db::TimeMs        created_at;
};

class VerificationRepository final : public repo::RepositoryBase {
public:
    VerificationRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // Replaces whatever was outstanding for this address. An UPSERT on the
    // unique address index rather than a delete-then-insert: one round trip, no
    // window in which an address has no live code, and the attempt counter is
    // reset as part of the same write.
    //
    // Issuing a new code invalidates the previous one immediately, which is the
    // behaviour a re-issue has to have — two live codes would double the guess
    // surface for the price of one request.
    [[nodiscard]] Status issue(mongocxx::client& client,
                               const NewVerification& verification) const;

    // The consume. True when the code was right, and the row is spent in the
    // same operation that says so.
    //
    // The binding — address, code, unused, attempts remaining, not expired — is
    // ENTIRELY in the filter. Nothing is checked against a returned document,
    // because a check-then-act here is exactly what N concurrent guesses beat.
    [[nodiscard]] Result<bool> consume(mongocxx::client& client,
                                       const crypto::Digest256& address_hash,
                                       const crypto::Digest256& code_hash,
                                       db::TimeMs now) const;

    // Costs the address one attempt. Issued on EVERY verification, successful or
    // not: on success it matches nothing, because consume has already marked the
    // row used; on failure it is what makes a wrong guess expensive.
    //
    // Two operations on both branches, deliberately. A success that costs one
    // round trip and a failure that costs two is a difference somebody can
    // measure, and a measurable difference is an oracle.
    [[nodiscard]] Status burn_attempt(mongocxx::client& client,
                                      const crypto::Digest256& address_hash,
                                      db::TimeMs now) const;
};

// Mints codes, holds the pepper and the index key, and keeps the two-operation
// symmetry that makes success and failure indistinguishable by timing.
class VerificationService final {
public:
    // `pepper` hashes the CODE; `index_key` is the HMAC key for the address
    // blind index. Two keys rather than one, for the reason any two uses of one
    // key get separated: compromising the lookup index must not also let
    // somebody confirm a guessed code offline.
    VerificationService(std::string database, std::string_view collection,
                        std::span<const std::uint8_t> pepper,
                        std::span<const std::uint8_t> index_key);

    struct IssuedCode final {
        // Plaintext, returned ONCE, for the caller to put in a message. Nothing
        // stores it.
        std::string code;
        Uuid        id;
    };

    [[nodiscard]] Result<IssuedCode> issue(mongocxx::client& client,
                                           std::string_view normalised_address,
                                           std::chrono::seconds lifetime, db::TimeMs now) const;

    // True when the code was right. Every failure — wrong code, expired row,
    // exhausted attempts, no such address — is the same `false` after the same
    // two operations.
    [[nodiscard]] Result<bool> verify(mongocxx::client& client,
                                      std::string_view normalised_address,
                                      std::string_view code, db::TimeMs now) const;

    [[nodiscard]] crypto::Digest256 address_index(std::string_view normalised) const;

    VerificationService(const VerificationService&) = delete;
    VerificationService& operator=(const VerificationService&) = delete;

private:
    // Declaration order is construction order: codes_ is built from database_.
    const std::string        database_;
    VerificationRepository   codes_;
    crypto::SecretBuffer<32> pepper_;
    crypto::SecretBuffer<32> index_key_;
};

}  // namespace anvil::identity
