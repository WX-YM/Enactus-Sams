// versioned-write-exempt: a capability row is written once and redeemed once.
// The redemption is a find_one_and_update whose filter carries the whole binding
// AND the unused state, which is a stronger interlock than a version field: a
// second redemption matches nothing rather than losing a race with the first.

#include "anvil/identity/capabilities.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>

#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"

namespace anvil::identity {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = capability_fields;

// Handed to options.projection() BY VALUE. A .view() into this temporary would
// leave the options holding a pointer into a document that died at the end of
// the statement — see the note on account_projection_document in
// src/identity/users.cc.
[[nodiscard]] bsoncxx::document::value grant_projection() {
    return make_document(kvp(codec::key_of(f::kId), 1), kvp(codec::key_of(f::kUserId), 1),
                         kvp(codec::key_of(f::kScope), 1), kvp(codec::key_of(f::kSubject), 1));
}

[[nodiscard]] Result<CapabilityGrant> decode_grant(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<Uuid> owner = codec::read_uuid(doc, f::kUserId);
    if (!owner) { return owner.error(); }
    const Result<std::optional<Uuid>> bound = codec::read_optional_uuid(doc, f::kSubject);
    if (!bound) { return bound.error(); }
    const Result<std::int32_t> scope = codec::read_int32(doc, f::kScope);
    if (!scope) { return scope.error(); }

    return CapabilityGrant{.id = id.value(),
                           .user_id = owner.value(),
                           .subject = bound.value(),
                           .scope = CapabilityScope::from_stored(scope.value())};
}

}  // namespace

Status CapabilityRepository::insert(mongocxx::client& client,
                                    const NewCapability& capability) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document doc;
        codec::append_uuid(doc, f::kId, capability.id);
        codec::append_digest(doc, f::kHash, capability.hash);
        codec::append_uuid(doc, f::kUserId, capability.user_id);
        // Written explicitly as null when absent rather than omitted: the
        // subject is part of every consume filter, and a filter comparing
        // against null must match a stored null rather than a missing field —
        // those are the same to MongoDB today and are not a thing to depend on
        // silently across a schema change.
        codec::append_optional_uuid(doc, f::kSubject, capability.subject);
        doc.append(kvp(codec::key_of(f::kScope),
                       bsoncxx::types::b_int32{capability.scope.stored()}));
        codec::append_time(doc, f::kExpiresAt, capability.expires_at);
        doc.append(kvp(codec::key_of(f::kUsedAt), bsoncxx::types::b_null{}));

        mongocxx::collection tokens = bind(client);
        tokens.insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<CapabilityGrant>> CapabilityRepository::consume(
    mongocxx::client& client, const crypto::Digest256& hash, CapabilityScope expected,
    const Uuid& user_id, const std::optional<Uuid>& subject, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<CapabilityGrant>> {
        bsoncxx::builder::basic::document filter;
        codec::append_digest(filter, f::kHash, hash);
        filter.append(kvp(codec::key_of(f::kUsedAt), bsoncxx::types::b_null{}));
        // The expiry predicate is MANDATORY and separate from the TTL index: the
        // monitor runs roughly every 60 seconds, so an expired token is still
        // readable and would otherwise still redeem.
        repo::append_not_expired(filter, f::kExpiresAt, now);
        codec::append_uuid(filter, f::kUserId, user_id);
        filter.append(kvp(codec::key_of(f::kScope), bsoncxx::types::b_int32{expected.stored()}));
        codec::append_optional_uuid(filter, f::kSubject, subject);

        mongocxx::options::find_one_and_update options{};
        // The document BEFORE the update: `used` is the only field the update
        // touches, and the caller has no use for its new value.
        options.return_document(mongocxx::options::return_document::k_before);
        options.projection(grant_projection());

        mongocxx::collection tokens = bind(client);
        const auto consumed = tokens.find_one_and_update(
            filter.view(), make_document(kvp("$set", [now](sub_document sub) {
                codec::append_time(sub, f::kUsedAt, now);
            })).view(),
            options);
        if (!consumed) { return std::optional<CapabilityGrant>{}; }

        const Result<CapabilityGrant> grant = decode_grant(consumed->view());
        if (!grant) { return grant.error(); }
        return std::optional<CapabilityGrant>{grant.value()};
    });
}

Result<std::optional<CapabilityGrant>> CapabilityRepository::verify(
    mongocxx::client& client, const crypto::Digest256& hash, CapabilityScope expected,
    const std::optional<Uuid>& subject, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<CapabilityGrant>> {
        bsoncxx::builder::basic::document filter;
        codec::append_digest(filter, f::kHash, hash);
        // Kept even though nothing here consumes: a token burned elsewhere must
        // not be replayable as a read credential.
        filter.append(kvp(codec::key_of(f::kUsedAt), bsoncxx::types::b_null{}));
        repo::append_not_expired(filter, f::kExpiresAt, now);
        filter.append(kvp(codec::key_of(f::kScope), bsoncxx::types::b_int32{expected.stored()}));
        codec::append_optional_uuid(filter, f::kSubject, subject);

        mongocxx::options::find options{};
        options.projection(grant_projection());

        mongocxx::collection tokens = bind(client);
        const auto found = tokens.find_one(filter.view(), options);
        if (!found) { return std::optional<CapabilityGrant>{}; }

        const Result<CapabilityGrant> grant = decode_grant(found->view());
        if (!grant) { return grant.error(); }
        return std::optional<CapabilityGrant>{grant.value()};
    });
}

CapabilityService::CapabilityService(std::string database, std::string_view collection,
                                     std::span<const std::uint8_t> pepper,
                                     std::span<const CapabilityScopeSpec> scopes)
    : database_{std::move(database)},
      tokens_{database_, collection},
      scopes_{scopes},
      pepper_{} {
    if (pepper.size() != pepper_.size()) {
        throw std::invalid_argument{"CapabilityService: pepper must be 32 bytes"};
    }
    if (scopes_.empty()) {
        throw std::invalid_argument{"CapabilityService: the scope table must not be empty"};
    }
    if (!capability_table_is_well_formed(scopes_)) {
        throw std::invalid_argument{
            "CapabilityService: duplicate scope value or name, empty name, or a "
            "non-positive value"};
    }
    std::copy(pepper.begin(), pepper.end(), pepper_.data());
}

crypto::Digest256 CapabilityService::hash_token(std::string_view token) const {
    // A fast hash is CORRECT here, not a shortcut: the token carries 256 bits of
    // CSPRNG entropy, so there is nothing to brute-force, and a slow KDF on a
    // lookup path is a denial-of-service vector rather than a defence
    // (CLAUDE.md §5).
    return crypto::sha256_with_pepper(token, pepper_.span());
}

Result<CapabilityService::IssuedCapability> CapabilityService::issue(
    mongocxx::client& client, CapabilityScope scope, const Uuid& user_id,
    const std::optional<Uuid>& subject, std::chrono::seconds lifetime, db::TimeMs now) const {
    // An undeclared scope mints NOTHING. It is a programming error rather than a
    // request error, and the reference application's static_assert catches it at
    // compile time — but a token that authorises a scope no consume site knows
    // how to check is worse than no token at all.
    if (capability_spec_of(scopes_, scope) == nullptr) {
        return fail(ErrorCode::Internal, "scope");
    }
    if (lifetime.count() <= 0) { return fail(ErrorCode::Internal, "lifetime"); }

    std::string token = crypto::random_token();
    const Uuid id = uuid::generate_v7();
    const NewCapability capability{
        .hash = hash_token(token),
        .id = id,
        .user_id = user_id,
        .subject = subject,
        .expires_at = now + std::chrono::duration_cast<std::chrono::milliseconds>(lifetime),
        .scope = scope,
    };

    const Status inserted = tokens_.insert(client, capability);
    if (!inserted) { return inserted.error(); }
    return IssuedCapability{.token = std::move(token), .id = id};
}

Result<std::optional<CapabilityGrant>> CapabilityService::redeem(
    mongocxx::client& client, std::string_view token, CapabilityScope expected,
    const std::optional<Uuid>& user_id, const std::optional<Uuid>& subject,
    db::TimeMs now) const {
    const CapabilityScopeSpec* spec = capability_spec_of(scopes_, expected);
    // A scope this build does not declare cannot be redeemed. During a rolling
    // deploy that means a token minted by a newer process is refused by an older
    // one, which is the correct direction to fail: the alternative is an older
    // process acting on a grant whose meaning it does not know.
    if (spec == nullptr) { return fail(ErrorCode::CapabilityInvalid, "scope"); }

    const crypto::Digest256 hash = hash_token(token);

    if (!spec->single_use) {
        // A read credential, and deliberately not bound to a user: it is
        // presented where no session exists. Its binding is scope and subject.
        return tokens_.verify(client, hash, expected, subject, now);
    }

    // A single-use scope demands a user. Redeeming one without an actor would
    // drop the confused-deputy check that makes the whole binding worth having.
    if (!user_id.has_value()) { return fail(ErrorCode::CapabilityRequired, "user"); }
    return tokens_.consume(client, hash, expected, *user_id, subject, now);
}

}  // namespace anvil::identity
