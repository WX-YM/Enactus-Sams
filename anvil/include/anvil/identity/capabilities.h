#pragma once

// Short, scoped grants redeemed by a LATER, SEPARATE request: an upload slot, a
// destructive-action confirmation, a preview credential for content that is not
// published yet.
//
// Consumption is ONE find_one_and_update. Check-then-act here is a double-spend
// bug — two concurrent confirmed deletes, or one irreversible operation
// performed twice — and no amount of care at the call site closes the window
// between the check and the act, because the window is the design.
//
// The binding — user, scope, subject — lives in the FILTER rather than in a
// check against the returned document. Two consequences, both deliberate:
//
//   * A token issued to user X and presented by user Y matches nothing, so it is
//     rejected AND not consumed. A confused deputy cannot burn somebody else's
//     grant.
//   * A token for subject A presented against subject B likewise matches
//     nothing.
//
// Only a digest is stored. A raw token in a collection is a credential anybody
// with a database dump can replay, and the digest is peppered so that dump is
// not enough on its own (ENGINEERING_RULES.md §5).

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"
#include "anvil/identity/capability_spec.h"

namespace anvil::identity {

namespace capability_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kHash = "h";
inline constexpr std::string_view kUserId = "uid";
inline constexpr std::string_view kSubject = "sub";
inline constexpr std::string_view kScope = "sc";
inline constexpr std::string_view kExpiresAt = "expires_at";
// When it was redeemed, or null. A CONSUMED token is kept until its TTL rather
// than deleted, because "this was already used" and "this never existed" are
// different facts and only the first one tells an investigator that a replay
// happened.
inline constexpr std::string_view kUsedAt = "used";
}  // namespace capability_fields

struct NewCapability final {
    crypto::Digest256   hash;
    Uuid                id;
    Uuid                user_id;
    // What the grant is ABOUT, when the operation has an object: the draft being
    // previewed, the record being deleted. Absent for a scope that authorises an
    // operation with no object, such as opening an upload slot.
    std::optional<Uuid> subject;
    db::TimeMs          expires_at;
    CapabilityScope     scope;
};

// What a successful redemption proves. Returned so a caller can act on the
// binding rather than re-deriving it from whatever it happened to have in hand.
struct CapabilityGrant final {
    Uuid                id;
    Uuid                user_id;
    std::optional<Uuid> subject;
    CapabilityScope     scope;
};

class CapabilityRepository final : public repo::RepositoryBase {
public:
    CapabilityRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    [[nodiscard]] Status insert(mongocxx::client& client,
                                const NewCapability& capability) const;

    // Atomic and single-use. nullopt means "no live, unused, correctly-bound
    // token with that hash" and carries no further detail: on a stealth route a
    // caller turns it into a 404 plus an audit row, and the four ways it can
    // fail must be indistinguishable to whoever presented it.
    [[nodiscard]] Result<std::optional<CapabilityGrant>> consume(
        mongocxx::client& client, const crypto::Digest256& hash, CapabilityScope expected,
        const Uuid& user_id, const std::optional<Uuid>& subject, db::TimeMs now) const;

    // Non-consuming, and NOT bound to a user. For a credential presented where
    // no session exists, which is the whole reason a non-single-use scope exists
    // at all: a preview arrives on an origin the host-only session cookie can
    // never reach, so there is no user id to filter on and demanding one would
    // reject every request.
    //
    // The binding it DOES enforce is the one that matters there — scope and
    // subject, both in the filter — so a token for draft A presented against
    // draft B matches nothing. `used` staying in the filter means a token
    // consumed elsewhere cannot be replayed as a read credential.
    [[nodiscard]] Result<std::optional<CapabilityGrant>> verify(
        mongocxx::client& client, const crypto::Digest256& hash, CapabilityScope expected,
        const std::optional<Uuid>& subject, db::TimeMs now) const;
};

// Mints and redeems. Holds the scope table and the pepper, so no caller has to
// hold either — and a caller that held the pepper would be one refactor away
// from logging it.
class CapabilityService final {
public:
    // Throws std::invalid_argument on a wrong-sized pepper or a malformed scope
    // table. Both are boot failures: a process that starts with a scope table it
    // cannot use issues tokens that authorise nothing, which surfaces as a
    // feature that silently never works.
    CapabilityService(std::string database, std::string_view collection,
                      std::span<const std::uint8_t> pepper,
                      std::span<const CapabilityScopeSpec> scopes);

    // The token in plaintext, returned ONCE. Nothing stores it and nothing can
    // recover it: the row holds only SHA-256(token ‖ pepper).
    struct IssuedCapability final {
        std::string token;
        Uuid        id;
    };

    [[nodiscard]] Result<IssuedCapability> issue(mongocxx::client& client,
                                                 CapabilityScope scope, const Uuid& user_id,
                                                 const std::optional<Uuid>& subject,
                                                 std::chrono::seconds lifetime,
                                                 db::TimeMs now) const;

    // Redeems according to the scope's own rule: a single-use scope is consumed,
    // and one that is not is verified. Which it is comes from the TABLE rather
    // than from the caller, because a caller that chose would be a caller that
    // could choose wrong — and choosing "verify" for a single-use scope is
    // exactly the double-spend this design exists to prevent.
    [[nodiscard]] Result<std::optional<CapabilityGrant>> redeem(
        mongocxx::client& client, std::string_view token, CapabilityScope expected,
        const std::optional<Uuid>& user_id, const std::optional<Uuid>& subject,
        db::TimeMs now) const;

    [[nodiscard]] std::span<const CapabilityScopeSpec> scopes() const noexcept {
        return scopes_;
    }
    [[nodiscard]] crypto::Digest256 hash_token(std::string_view token) const;

    CapabilityService(const CapabilityService&) = delete;
    CapabilityService& operator=(const CapabilityService&) = delete;

private:
    // Declaration order is construction order: tokens_ is built from database_.
    const std::string                   database_;
    CapabilityRepository                tokens_;
    std::span<const CapabilityScopeSpec> scopes_;
    crypto::SecretBuffer<32>            pepper_;
};

}  // namespace anvil::identity
