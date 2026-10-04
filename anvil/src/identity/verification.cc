// versioned-write-exempt: a verification row is a single-document state machine
// — issued, then either consumed or charged an attempt — and every transition is
// one atomic find_one_and_update or upsert whose filter carries the state it
// expects. That is the same interlock update_versioned provides, and it is
// stronger here: a version field would let a caller re-read and retry, which for
// a guess counter is precisely the loop this design refuses.

#include "anvil/identity/verification.h"

#include "anvil/accounts/schema.h"

// The descriptor publishes the code length from accounts/schema.h, which cannot
// include this header's database dependencies. One number, asserted here where
// both are visible, so a client is never told to expect a code of a length
// this process does not mint.
static_assert(anvil::accounts::kAccountCodeDigits == anvil::identity::kVerificationCodeDigits);

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/update.hpp>

#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"

namespace anvil::identity {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = verification_fields;

// A uniformly distributed decimal code, one digit at a time. random_below
// rejects the biased tail rather than taking a modulus of a raw draw: the bias a
// modulus introduces here is small, and a code space of a million with a budget
// of five guesses is exactly the setting where "small" is not an argument.
[[nodiscard]] std::string mint_code() {
    std::string code(kVerificationCodeDigits, '0');
    for (std::size_t i = 0; i < kVerificationCodeDigits; ++i) {
        code[i] = static_cast<char>('0' + crypto::random_below(10));
    }
    return code;
}

}  // namespace

Status VerificationRepository::issue(mongocxx::client& client,
                                     const NewVerification& verification) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document filter;
        codec::append_digest(filter, f::kAddressHash, verification.address_hash);

        // $set every mutable field, so a row left over from a previous attempt is
        // fully replaced rather than partially updated — in particular the
        // attempt counter goes back to zero and `used` back to null, which is
        // what makes a re-issue a fresh start rather than an inherited one.
        //
        // `used` is written as an explicit null rather than omitted: the consume
        // filter matches on null, and a missing field is not the same thing.
        //
        // `_id` is $setOnInsert and NOT $set. It is immutable, so setting it on
        // an upsert that matched an existing row is a server error rather than a
        // no-op — which would mean every re-issue fails and the address stays
        // stuck with its original code until that code expires.
        const bsoncxx::document::value update = make_document(
            kvp("$set",
                make_document(
                    kvp(codec::key_of(f::kCodeHash), codec::digest_bin(verification.code_hash)),
                    kvp(codec::key_of(f::kAttempts), bsoncxx::types::b_int32{0}),
                    kvp(codec::key_of(f::kExpiresAt),
                        codec::time_date(verification.expires_at)),
                    kvp(codec::key_of(f::kUsedAt), bsoncxx::types::b_null{}),
                    kvp(codec::key_of(f::kCreatedAt),
                        codec::time_date(verification.created_at)))),
            kvp("$setOnInsert",
                make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(verification.id)))));

        mongocxx::options::update options{};
        options.upsert(true);

        mongocxx::collection collection = bind(client);
        // ttl-filter-exempt: this is the WRITE that establishes a new lifetime.
        // Filtering the upsert on the old row's expiry would make a re-issue
        // after expiry insert a second row for the same address, which the
        // unique index then rejects — an address that let its code lapse could
        // never request another one.
        collection.update_one(filter.view(), update.view(), options);
        return ok();
    });
}

Result<bool> VerificationRepository::consume(mongocxx::client& client,
                                             const crypto::Digest256& address_hash,
                                             const crypto::Digest256& code_hash,
                                             db::TimeMs now) const {
    return repo::guarded([&]() -> Result<bool> {
        // Address, code, unused, attempts remaining, unexpired — all five in the
        // FILTER. The expiry predicate is explicit and not left to the TTL
        // monitor: it lags about a minute, so an expired code would otherwise
        // still verify for that minute.
        bsoncxx::builder::basic::document filter;
        codec::append_digest(filter, f::kAddressHash, address_hash);
        codec::append_digest(filter, f::kCodeHash, code_hash);
        filter.append(kvp(codec::key_of(f::kUsedAt), bsoncxx::types::b_null{}));
        filter.append(kvp(codec::key_of(f::kAttempts), [](sub_document sub) {
            sub.append(kvp("$lt", bsoncxx::types::b_int32{kMaxVerifyAttempts}));
        }));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        // Marked used AND charged an attempt in one update. Charging on success
        // too keeps the two branches identical in what they write, and the row
        // is spent either way.
        const bsoncxx::document::value update = make_document(
            kvp("$set", make_document(kvp(codec::key_of(f::kUsedAt), codec::time_date(now)))),
            kvp("$inc",
                make_document(kvp(codec::key_of(f::kAttempts), bsoncxx::types::b_int32{1}))));

        mongocxx::collection collection = bind(client);
        const auto before = collection.find_one_and_update(filter.view(), update.view());
        return before.has_value();
    });
}

Status VerificationRepository::burn_attempt(mongocxx::client& client,
                                            const crypto::Digest256& address_hash,
                                            db::TimeMs now) const {
    return repo::guarded([&]() -> Status {
        // Deliberately does NOT filter on the attempt count. A row already at
        // the limit simply matches nothing further, and adding the predicate
        // would make the exhausted case cost a different amount of work than the
        // merely wrong one — which is a difference somebody can measure.
        bsoncxx::builder::basic::document filter;
        codec::append_digest(filter, f::kAddressHash, address_hash);
        filter.append(kvp(codec::key_of(f::kUsedAt), bsoncxx::types::b_null{}));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        const bsoncxx::document::value update = make_document(kvp(
            "$inc", make_document(kvp(codec::key_of(f::kAttempts), bsoncxx::types::b_int32{1}))));

        mongocxx::collection collection = bind(client);
        collection.find_one_and_update(filter.view(), update.view());
        // Matched or not, the answer is the same. Whether an address had a live
        // row is precisely the thing this must not report.
        return ok();
    });
}

VerificationService::VerificationService(std::string database, std::string_view collection,
                                         std::span<const std::uint8_t> pepper,
                                         std::span<const std::uint8_t> index_key)
    : database_{std::move(database)},
      codes_{database_, collection},
      pepper_{},
      index_key_{} {
    if (pepper.size() != pepper_.size() || index_key.size() != index_key_.size()) {
        throw std::invalid_argument{"VerificationService: both keys must be 32 bytes"};
    }
    std::copy(pepper.begin(), pepper.end(), pepper_.data());
    std::copy(index_key.begin(), index_key.end(), index_key_.data());
}

crypto::Digest256 VerificationService::address_index(std::string_view normalised) const {
    // HMAC, not a plain hash. The address space is small and structured enough
    // that a plain digest of it is a lookup table somebody else already has.
    return crypto::blind_index(normalised, index_key_.span());
}

Result<VerificationService::IssuedCode> VerificationService::issue(
    mongocxx::client& client, std::string_view normalised_address,
    std::chrono::seconds lifetime, db::TimeMs now) const {
    if (lifetime.count() <= 0) { return fail(ErrorCode::Internal, "lifetime"); }

    std::string code = mint_code();
    const NewVerification verification{
        .address_hash = address_index(normalised_address),
        .code_hash = crypto::sha256_with_pepper(code, pepper_.span()),
        .id = uuid::generate_v7(),
        .expires_at = now + std::chrono::duration_cast<std::chrono::milliseconds>(lifetime),
        .created_at = now,
    };

    const Status issued = codes_.issue(client, verification);
    if (!issued) { return issued.error(); }
    return IssuedCode{.code = std::move(code), .id = verification.id};
}

Result<bool> VerificationService::verify(mongocxx::client& client,
                                         std::string_view normalised_address,
                                         std::string_view code, db::TimeMs now) const {
    const crypto::Digest256 address = address_index(normalised_address);
    const crypto::Digest256 guess = crypto::sha256_with_pepper(code, pepper_.span());

    const Result<bool> consumed = codes_.consume(client, address, guess, now);
    if (!consumed) { return consumed.error(); }

    // Issued on BOTH branches. On success it matches nothing, because consume
    // has already marked the row used; on failure it is what makes a wrong guess
    // expensive. Two operations either way, so the two branches cannot be told
    // apart by how long they took.
    const Status burned = codes_.burn_attempt(client, address, now);
    if (!burned) { return burned.error(); }

    return consumed.value();
}

}  // namespace anvil::identity
