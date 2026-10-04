// versioned-write-exempt: the identity document is never read, modified and
// written back. Every write is one conditional update whose filter restates
// the state it depends on (no device yet; the approver linked with the key that
// verified; the id absent; room for one more; the device still idle; the
// pending mark unchanged), and dv moves by $inc inside that same write. db/versioned.h is for a document two
// editors rewrite whole, and nothing here rewrites one.

#include "anvil/chat/devices.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/update.hpp>

#include "anvil/db/repository.h"
#include "anvil/input/fields.h"

namespace anvil::chat {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace idf = identity_fields;
namespace in = device_inputs;

[[nodiscard]] bsoncxx::stdx::string_view key(std::string_view name) noexcept {
    return codec::key_of(name);
}

[[nodiscard]] bsoncxx::types::b_int64 i64(std::int64_t value) noexcept {
    return bsoncxx::types::b_int64{value};
}

// Positional paths for the touch, spelled out so they are .rodata.
inline constexpr std::string_view kSeenPositional = "dev.$.seen";
static_assert(detail::is_path(kSeenPositional, "dev.$", idf::kLastSeen) &&
              detail::is_path("dev.$", idf::kDevices, "$"));

// What a published device is made of, as projection paths. Everything except
// last_seen, the session and the low flag, which are presence and plumbing.
inline constexpr std::array<std::string_view, 12> kPublishedPaths{"dev.i",
                                                                  "dev.s",
                                                                  "dev.ak",
                                                                  "dev.sk",
                                                                  "dev.spk",
                                                                  "dev.sps",
                                                                  "dev.lrk",
                                                                  "dev.lrs",
                                                                  "dev.at",
                                                                  "dev.by",
                                                                  "dev.lsg",
                                                                  "dev.lts"};
static_assert(detail::is_path(kPublishedPaths[0], idf::kDevices, idf::kDeviceId) &&
              detail::is_path(kPublishedPaths[1], idf::kDevices, idf::kSuite) &&
              detail::is_path(kPublishedPaths[2], idf::kDevices, idf::kAgreementKey) &&
              detail::is_path(kPublishedPaths[3], idf::kDevices, idf::kSigningKey) &&
              detail::is_path(kPublishedPaths[4], idf::kDevices, idf::kSignedPrekey) &&
              detail::is_path(kPublishedPaths[5], idf::kDevices, idf::kSignedPrekeySignature) &&
              detail::is_path(kPublishedPaths[6], idf::kDevices, idf::kLastResortKey) &&
              detail::is_path(kPublishedPaths[7], idf::kDevices, idf::kLastResortSignature) &&
              detail::is_path(kPublishedPaths[8], idf::kDevices, idf::kLinkedAt) &&
              detail::is_path(kPublishedPaths[9], idf::kDevices, idf::kApprovedBy) &&
              detail::is_path(kPublishedPaths[10], idf::kDevices, idf::kLinkSignature) &&
              detail::is_path(kPublishedPaths[11], idf::kDevices, idf::kLinkTimestamp));

inline constexpr std::string_view kSigningKeyPath = "dev.sk";
// "The array has a first element", negated in a filter: the account has no
// device at all.
inline constexpr std::string_view kFirstDevicePath = "dev.0";
static_assert(detail::is_path(kFirstDevicePath, idf::kDevices, "0"));
static_assert(detail::is_path(kSigningKeyPath, idf::kDevices, idf::kSigningKey));

template <std::size_t N>
void put(std::array<std::uint8_t, kPrekeyMessageBytes>& out,
         std::size_t& at,
         const std::array<std::uint8_t, N>& bytes) noexcept {
    std::memcpy(out.data() + at, bytes.data(), N);
    at += N;
}

// --- the document a device is ---------------------------------------------------

[[nodiscard]] bsoncxx::document::value device_document(const NewDevice& device,
                                                       const std::optional<LinkProof>& link,
                                                       db::TimeMs now) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, idf::kDeviceId, device.id);
    codec::append_enum(doc, idf::kSuite, device.keys.suite);
    doc.append(kvp(key(idf::kAgreementKey), codec::bytes_bin(device.keys.agreement)));
    doc.append(kvp(key(idf::kSigningKey), codec::bytes_bin(device.keys.signing)));
    doc.append(kvp(key(idf::kSignedPrekey), codec::bytes_bin(device.keys.signed_prekey)));
    doc.append(kvp(key(idf::kSignedPrekeySignature),
                   codec::bytes_bin(device.keys.signed_prekey_signature)));
    doc.append(kvp(key(idf::kLastResortKey), codec::bytes_bin(device.keys.last_resort)));
    doc.append(kvp(key(idf::kLastResortSignature),
                   codec::bytes_bin(device.keys.last_resort_signature)));
    codec::append_time(doc, idf::kLinkedAt, now);
    // Seen at link time, so a device that never makes a second request is
    // still swept after idle_days rather than living forever without the field.
    codec::append_time(doc, idf::kLastSeen, now);
    codec::append_uuid(doc, idf::kSession, device.session);
    // OMITTED on the first device: nothing signed it, and a zeroed signature
    // field would read to a verifying client as a signature that fails.
    if (link.has_value()) {
        codec::append_uuid(doc, idf::kApprovedBy, link->approver);
        doc.append(kvp(key(idf::kLinkSignature), codec::bytes_bin(link->signature)));
        // Checked against the server's clock before this point, so it fits.
        doc.append(
            kvp(key(idf::kLinkTimestamp), i64(static_cast<std::int64_t>(link->timestamp_s))));
    }
    return doc.extract();
}

// The update every device change shares: the version moves, the change is
// marked pending for propagation (§7.4), in the same write as the change.
void append_change(bsoncxx::builder::basic::document& update, db::TimeMs now) {
    update.append(kvp("$inc", make_document(kvp(key(idf::kDeviceSetVersion), i64(1)))));
    // $max, so two changes racing leave the later millisecond, which is what
    // propagation $max-es into every conversation's dsv.
    update.append(
        kvp("$max", make_document(kvp(key(idf::kPendingSince), codec::time_date(now)))));
}

// Remembered so a link message replayed after its device was unlinked is
// refused. The slice keeps the last kMaxDevicesCeiling: replacing a whole
// account's worth of devices inside the replay window is the only way to push
// an id out, and it needs an approving signature per device.
void append_retire(bsoncxx::builder::basic::document& update, const Uuid& device) {
    update.append(kvp(
        "$push",
        make_document(kvp(
            key(idf::kRetired),
            make_document(kvp("$each", make_array(codec::uuid_bin(device))),
                          kvp("$slice", -static_cast<std::int32_t>(kMaxDevicesCeiling)))))));
}

[[nodiscard]] bsoncxx::document::value not_in(const Uuid& id) {
    return make_document(kvp("$ne", codec::uuid_bin(id)));
}

[[nodiscard]] bsoncxx::document::value absent() {
    return make_document(kvp("$exists", bsoncxx::types::b_bool{false}));
}

// --- decoding -----------------------------------------------------------------------

template <std::size_t N>
[[nodiscard]] Status read_key(const bsoncxx::document::view& doc,
                              std::string_view field,
                              std::array<std::uint8_t, N>& out) {
    return codec::read_bytes(doc, field, out);
}

[[nodiscard]] Result<PublishedDevice> decode_published(const bsoncxx::document::view& doc) {
    PublishedDevice out{};
    const Result<Uuid> id = codec::read_uuid(doc, idf::kDeviceId);
    if (!id) {
        return id.error();
    }
    out.id = id.value();
    const Result<Suite> suite = codec::read_enum(doc, idf::kSuite, kMaxSuite);
    if (!suite) {
        return suite.error();
    }
    // Reserved is never written; reading it back is corruption.
    if (suite.value() == Suite::Reserved) {
        return fail(ErrorCode::Internal, idf::kSuite);
    }
    out.keys.suite = suite.value();
    for (const Status read :
         {read_key(doc, idf::kAgreementKey, out.keys.agreement),
          read_key(doc, idf::kSigningKey, out.keys.signing),
          read_key(doc, idf::kSignedPrekey, out.keys.signed_prekey),
          read_key(doc, idf::kSignedPrekeySignature, out.keys.signed_prekey_signature),
          read_key(doc, idf::kLastResortKey, out.keys.last_resort),
          read_key(doc, idf::kLastResortSignature, out.keys.last_resort_signature)}) {
        if (!read) {
            return read.error();
        }
    }
    const Result<db::TimeMs> at = codec::read_time(doc, idf::kLinkedAt);
    if (!at) {
        return at.error();
    }
    out.linked_at = at.value();
    const bool has_approver = doc.find(key(idf::kApprovedBy)) != doc.end();
    const bool has_signature = doc.find(key(idf::kLinkSignature)) != doc.end();
    const bool has_timestamp = doc.find(key(idf::kLinkTimestamp)) != doc.end();
    // All three or none: a half proof is a chain a client cannot verify, and
    // serving it would look like a server that dropped a signature.
    if (has_approver != has_signature || has_approver != has_timestamp) {
        return fail(ErrorCode::Internal, idf::kApprovedBy);
    }
    if (has_approver) {
        LinkProof link{};
        const Result<Uuid> approver = codec::read_uuid(doc, idf::kApprovedBy);
        if (!approver) {
            return approver.error();
        }
        link.approver = approver.value();
        if (const Status sig = read_key(doc, idf::kLinkSignature, link.signature); !sig) {
            return sig.error();
        }
        const Result<std::int64_t> timestamp = codec::read_int64(doc, idf::kLinkTimestamp);
        if (!timestamp) {
            return timestamp.error();
        }
        if (timestamp.value() < 0) {
            return fail(ErrorCode::Internal, idf::kLinkTimestamp);
        }
        link.timestamp_s = static_cast<std::uint64_t>(timestamp.value());
        out.link = link;
    }
    return out;
}

[[nodiscard]] Result<DeviceRecord> decode_device(const bsoncxx::document::view& doc) {
    Result<PublishedDevice> published = decode_published(doc);
    if (!published) {
        return published.error();
    }
    DeviceRecord out{.published = std::move(published).value(),
                     .last_seen = {},
                     .session = {},
                     .low = false};
    const Result<db::TimeMs> seen = codec::read_time(doc, idf::kLastSeen);
    if (!seen) {
        return seen.error();
    }
    out.last_seen = seen.value();
    const Result<Uuid> session = codec::read_uuid(doc, idf::kSession);
    if (!session) {
        return session.error();
    }
    out.session = session.value();
    if (doc.find(key(idf::kLow)) != doc.end()) {
        const Result<bool> low = codec::read_bool(doc, idf::kLow);
        if (!low) {
            return low.error();
        }
        out.low = low.value();
    }
    return out;
}

// Each element of `dev`, bounded by the ceiling: a document holding more is
// corruption or a hostile writer, and either way not something to walk.
template <typename Fn>
[[nodiscard]] Status for_each_device(const bsoncxx::document::view& doc, Fn&& fn) {
    auto element = doc.find(key(idf::kDevices));
    if (element == doc.end()) {
        return ok();
    }
    if (element->type() != bsoncxx::type::k_array) {
        return fail(ErrorCode::Internal, idf::kDevices);
    }
    std::size_t seen = 0;
    for (const bsoncxx::array::element& entry : element->get_array().value) {
        if (entry.type() != bsoncxx::type::k_document || ++seen > kMaxDevicesCeiling) {
            return fail(ErrorCode::Internal, idf::kDevices);
        }
        if (const Status visited = fn(entry.get_document().value); !visited) {
            return visited;
        }
    }
    return ok();
}

[[nodiscard]] Result<std::int64_t> read_version(const bsoncxx::document::view& doc) {
    return codec::read_int64(doc, idf::kDeviceSetVersion);
}

[[nodiscard]] std::int64_t unix_seconds(db::TimeMs at) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count();
}

}  // namespace

// --- messages and validation -------------------------------------------------------

PrekeyMessage prekey_message(std::string_view domain,
                             const Uuid& device,
                             const crypto::X25519PublicKey& key) noexcept {
    PrekeyMessage out{};
    // Both prekey domains are 14 bytes; anything else is a caller's bug, and a
    // truncated domain would still never collide with a real one.
    const std::size_t domain_bytes = std::min<std::size_t>(domain.size(), 14);
    for (std::size_t i = 0; i < domain_bytes; ++i) {
        out[i] = static_cast<std::uint8_t>(domain[i]);
    }
    std::size_t at = 14;
    put(out, at, device);
    put(out, at, key);
    return out;
}

LinkMessage link_message(const Uuid& account,
                         const Uuid& device,
                         const crypto::X25519PublicKey& agreement,
                         const crypto::Ed25519PublicKey& signing,
                         std::uint64_t timestamp_s) noexcept {
    LinkMessage out{};
    std::size_t at = 0;
    const auto append = [&out, &at](const std::uint8_t* bytes, std::size_t count) noexcept {
        std::memcpy(out.data() + at, bytes, count);
        at += count;
    };
    for (const char c : kLinkDomain) {
        out[at++] = static_cast<std::uint8_t>(c);
    }
    append(account.data(), account.size());
    append(device.data(), device.size());
    append(agreement.data(), agreement.size());
    append(signing.data(), signing.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        out[at++] = static_cast<std::uint8_t>(timestamp_s >> shift);
    }
    return out;
}

Status validate_device(const Uuid& device, const DeviceKeys& keys) noexcept {
    if (is_nil(device)) {
        return fail(ErrorCode::ValidationFailed, in::kDeviceId);
    }
    if (keys.suite == Suite::Reserved || keys.suite > kMaxSuite) {
        return fail(ErrorCode::ValidationFailed, in::kSuite);
    }
    if (!crypto::x25519_public_key_is_valid(keys.agreement)) {
        return fail(ErrorCode::ValidationFailed, in::kAgreementKey);
    }
    if (!crypto::ed25519_public_key_is_valid(keys.signing)) {
        return fail(ErrorCode::ValidationFailed, in::kSigningKey);
    }
    if (!crypto::x25519_public_key_is_valid(keys.signed_prekey)) {
        return fail(ErrorCode::ValidationFailed, in::kSignedPrekey);
    }
    if (!crypto::ed25519_verify(keys.signing,
                                prekey_message(kSignedPrekeyDomain, device, keys.signed_prekey),
                                keys.signed_prekey_signature)) {
        return fail(ErrorCode::ValidationFailed, in::kSignedPrekeySignature);
    }
    if (!crypto::x25519_public_key_is_valid(keys.last_resort)) {
        return fail(ErrorCode::ValidationFailed, in::kLastResortKey);
    }
    if (!crypto::ed25519_verify(keys.signing,
                                prekey_message(kLastResortDomain, device, keys.last_resort),
                                keys.last_resort_signature)) {
        return fail(ErrorCode::ValidationFailed, in::kLastResortSignature);
    }
    return ok();
}

// --- the directory ------------------------------------------------------------------

DeviceDirectory::DeviceDirectory(const db::DatabaseNames& databases,
                                 const DeviceCollections& collections,
                                 const DeviceConfig& config)
    : database_{databases.for_collection(collections.identities)},
      links_database_{collections.links.empty()
                          ? std::string{}
                          : std::string{databases.for_collection(collections.links)}},
      links_{collections.links},
      full_path_{std::string{idf::kDevices} + "." +
                 std::to_string(
                     std::clamp<std::uint32_t>(config.max_devices, 1, kMaxDevicesCeiling) - 1)},
      collection_{collections.identities},
      config_{config} {}

mongocxx::collection DeviceDirectory::identities(mongocxx::client& client) const {
    return client[database_][std::string{collection_}];
}

mongocxx::collection DeviceDirectory::links(mongocxx::client& client) const {
    return client[links_database_][std::string{links_}];
}

namespace {

namespace lf = link_fields;

[[nodiscard]] bsoncxx::document::value live_request(const crypto::Digest256& digest,
                                                    const Uuid& user, db::TimeMs now) {
    // The expiry in the filter: the TTL monitor is a collector, not a gate.
    return make_document(kvp(key(lf::kId), codec::digest_bin(digest)),
                         kvp(key(lf::kUser), codec::uuid_bin(user)),
                         kvp(key(lf::kExpiresAt), make_document(kvp("$gt", codec::time_date(now)))));
}

[[nodiscard]] Result<LinkRequest> decode_link_request(const bsoncxx::document::view& doc) {
    LinkRequest out{};
    const Result<Uuid> user = codec::read_uuid(doc, lf::kUser);
    if (!user) { return user.error(); }
    out.user = user.value();
    const Result<Uuid> session = codec::read_uuid(doc, lf::kSession);
    if (!session) { return session.error(); }
    out.session = session.value();
    const Result<Uuid> device = codec::read_uuid(doc, lf::kDevice);
    if (!device) { return device.error(); }
    out.device = device.value();
    if (const Status read = read_key(doc, lf::kAgreementKey, out.agreement); !read) {
        return read.error();
    }
    if (const Status read = read_key(doc, lf::kSigningKey, out.signing); !read) {
        return read.error();
    }
    const Result<db::TimeMs> expires = codec::read_time(doc, lf::kExpiresAt);
    if (!expires) { return expires.error(); }
    out.expires_at = expires.value();
    if (doc.find(key(lf::kApprover)) != doc.end()) {
        LinkProof proof{};
        const Result<Uuid> approver = codec::read_uuid(doc, lf::kApprover);
        if (!approver) { return approver.error(); }
        proof.approver = approver.value();
        const Result<std::int64_t> at = codec::read_int64(doc, lf::kTimestamp);
        if (!at) { return at.error(); }
        if (at.value() < 0) { return fail(ErrorCode::Internal, lf::kTimestamp); }
        proof.timestamp_s = static_cast<std::uint64_t>(at.value());
        if (const Status read = read_key(doc, lf::kSignature, proof.signature); !read) {
            return read.error();
        }
        out.approval = proof;
    }
    return out;
}

}  // namespace

Status DeviceDirectory::put_link_request(mongocxx::client& client,
                                         const crypto::Digest256& digest,
                                         const LinkRequest& request) const {
    return repo::guarded([&]() -> Status {
        // One pending request per session: the one it left before is replaced.
        links(client).delete_many(
            make_document(kvp(key(lf::kUser), codec::uuid_bin(request.user)),
                          kvp(key(lf::kSession), codec::uuid_bin(request.session)))
                .view());
        bsoncxx::builder::basic::document doc;
        codec::append_digest(doc, lf::kId, digest);
        codec::append_uuid(doc, lf::kUser, request.user);
        codec::append_uuid(doc, lf::kSession, request.session);
        codec::append_uuid(doc, lf::kDevice, request.device);
        doc.append(kvp(key(lf::kAgreementKey), codec::bytes_bin(request.agreement)));
        doc.append(kvp(key(lf::kSigningKey), codec::bytes_bin(request.signing)));
        codec::append_time(doc, lf::kExpiresAt, request.expires_at);
        links(client).insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<LinkRequest>> DeviceDirectory::find_link_request(
    mongocxx::client& client, const crypto::Digest256& digest, const Uuid& user,
    db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<LinkRequest>> {
        const auto found = links(client).find_one(live_request(digest, user, now).view());
        if (!found) { return std::optional<LinkRequest>{}; }
        Result<LinkRequest> decoded = decode_link_request(found->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<LinkRequest>{decoded.value()};
    });
}

Result<bool> DeviceDirectory::approve_link_request(mongocxx::client& client,
                                                   const crypto::Digest256& digest,
                                                   const Uuid& user, const LinkProof& proof,
                                                   db::TimeMs now) const {
    return repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document filter;
        filter.append(bsoncxx::builder::concatenate(live_request(digest, user, now).view()));
        // Approved once: a second approval, even a valid one, changes nothing.
        filter.append(kvp(key(lf::kApprover), absent()));
        bsoncxx::builder::basic::document set;
        codec::append_uuid(set, lf::kApprover, proof.approver);
        set.append(kvp(key(lf::kTimestamp), i64(static_cast<std::int64_t>(proof.timestamp_s))));
        set.append(kvp(key(lf::kSignature), codec::bytes_bin(proof.signature)));
        const auto result = links(client).update_one(
            filter.view(), make_document(kvp("$set", set.extract())).view());
        return result.has_value() && result->matched_count() == 1;
    });
}

Result<std::optional<LinkRequest>> DeviceDirectory::collect_link_approval(
    mongocxx::client& client, const crypto::Digest256& digest, const Uuid& user,
    const Uuid& session, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<LinkRequest>> {
        bsoncxx::builder::basic::document filter;
        filter.append(bsoncxx::builder::concatenate(live_request(digest, user, now).view()));
        codec::append_uuid(filter, lf::kSession, session);
        filter.append(kvp(key(lf::kApprover), make_document(kvp("$exists", bsoncxx::types::b_bool{true}))));
        // Read once: the approval is removed in the operation that hands it over.
        const auto taken = links(client).find_one_and_delete(filter.view());
        if (!taken) { return std::optional<LinkRequest>{}; }
        Result<LinkRequest> decoded = decode_link_request(taken->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<LinkRequest>{decoded.value()};
    });
}

Status DeviceDirectory::register_first_device(mongocxx::client& client,
                                              const Uuid& user,
                                              const NewDevice& device,
                                              db::TimeMs authenticated_at,
                                              db::TimeMs now) const {
    // A minute of tolerance into the future, for the clock of whichever process
    // recorded the authentication; none at all past the five minutes.
    const auto age = now - authenticated_at;
    // CapabilityRequired, never Unauthenticated: the session is valid, and a
    // client reads a 401 as an expired access token, refreshes, is refused
    // again and signs its person out of every tab. What is missing is a second,
    // deliberate act — a password or a passkey — which is what 428 says.
    if (age > kFreshAuthentication || age < -std::chrono::minutes{1}) {
        return fail(ErrorCode::CapabilityRequired, in::kAuthenticatedAt);
    }
    if (const Status valid = validate_device(device.id, device.keys); !valid) {
        return valid;
    }

    const Status written = repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, idf::kId, user);
        // "No device at all", as the server evaluates it inside the write.
        filter.append(kvp(key(kFirstDevicePath), absent()));
        filter.append(kvp(key(idf::kRetired), not_in(device.id)));
        bsoncxx::builder::basic::document update;
        update.append(kvp("$push",
                          make_document(kvp(key(idf::kDevices),
                                            device_document(device, std::nullopt, now)))));
        append_change(update, now);
        mongocxx::options::update options{};
        // The upsert is what makes the first-ever registration and a racing
        // second one a single decision: when the filter does not match because
        // the account already has a device, the server tries to INSERT a second
        // document with the same _id and refuses it.
        options.upsert(true);
        identities(client).update_one(filter.view(), update.view(), options);
        return ok();
    });
    if (!written && written.error().code == ErrorCode::Conflict) {
        return fail(ErrorCode::Conflict, in::kDevices);
    }
    return written;
}

Status DeviceDirectory::link_device(mongocxx::client& client,
                                    const Uuid& user,
                                    const Uuid& approver,
                                    const NewDevice& device,
                                    std::uint64_t timestamp_s,
                                    const crypto::Ed25519Signature& link_signature,
                                    db::TimeMs now) const {
    // The window bounds how long a captured link is worth anything. Compared in
    // signed arithmetic only after the value is known to fit.
    if (timestamp_s > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(ErrorCode::ValidationFailed, in::kTimestamp);
    }
    const std::int64_t skew_s = unix_seconds(now) - static_cast<std::int64_t>(timestamp_s);
    if (skew_s > kLinkSkew.count() || skew_s < -kLinkSkew.count()) {
        return fail(ErrorCode::ValidationFailed, in::kTimestamp);
    }
    if (const Status valid = validate_device(device.id, device.keys); !valid) {
        return valid;
    }

    // The approver's stored key, so the signature can be checked BEFORE any
    // write. The read is not the authority: everything it establishes is
    // restated in the write's filter below.
    struct Seen final {
        crypto::Ed25519PublicKey approver_key;
        std::uint32_t devices;
        bool approver_found;
        bool id_taken;
    };

    const Result<std::optional<Seen>> read =
        repo::guarded([&]() -> Result<std::optional<Seen>> {
            mongocxx::options::find options{};
            options.projection(make_document(kvp(key(idf::kDeviceIdPath), 1),
                                             kvp(key(kSigningKeyPath), 1),
                                             kvp(key(idf::kRetired), 1)));
            const auto found = identities(client).find_one(
                make_document(kvp(key(idf::kId), codec::uuid_bin(user))).view(), options);
            if (!found) {
                return std::optional<Seen>{};
            }
            Seen seen{
                .approver_key = {}, .devices = 0, .approver_found = false, .id_taken = false};
            const Status walked =
                for_each_device(found->view(), [&](const bsoncxx::document::view& row) {
                    const Result<Uuid> id = codec::read_uuid(row, idf::kDeviceId);
                    if (!id) {
                        return Status{id.error()};
                    }
                    ++seen.devices;
                    seen.id_taken = seen.id_taken || id.value() == device.id;
                    if (id.value() == approver) {
                        if (const Status key_read =
                                read_key(row, idf::kSigningKey, seen.approver_key);
                            !key_read) {
                            return key_read;
                        }
                        seen.approver_found = true;
                    }
                    return ok();
                });
            if (!walked) {
                return walked.error();
            }
            if (auto retired = found->view().find(key(idf::kRetired));
                retired != found->view().end() && retired->type() == bsoncxx::type::k_array) {
                for (const bsoncxx::array::element& entry : retired->get_array().value) {
                    if (entry.type() != bsoncxx::type::k_binary) {
                        continue;
                    }
                    const auto bin = entry.get_binary();
                    seen.id_taken = seen.id_taken ||
                                    (bin.size == device.id.size() &&
                                     std::memcmp(bin.bytes, device.id.data(), bin.size) == 0);
                }
            }
            return std::optional<Seen>{seen};
        });
    if (!read) {
        return read.error();
    }
    if (!read.value().has_value() || !read.value()->approver_found) {
        return fail(ErrorCode::Forbidden, in::kApprover);
    }
    const Seen& seen = *read.value();
    if (!crypto::ed25519_verify(
            seen.approver_key,
            link_message(
                user, device.id, device.keys.agreement, device.keys.signing, timestamp_s),
            link_signature)) {
        return fail(ErrorCode::Forbidden, in::kLinkSignature);
    }
    if (seen.id_taken) {
        return fail(ErrorCode::Conflict, in::kDeviceId);
    }
    if (seen.devices >= std::clamp<std::uint32_t>(config_.max_devices, 1, kMaxDevicesCeiling)) {
        return fail(ErrorCode::Conflict, in::kDevices);
    }

    const LinkProof proof{
        .signature = link_signature, .timestamp_s = timestamp_s, .approver = approver};
    const Result<bool> written = repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, idf::kId, user);
        // The approver still linked, with the very key the signature verified
        // under: unlinking it between the read and here admits nobody.
        filter.append(kvp(
            key(idf::kDevices),
            make_document(kvp("$elemMatch",
                              make_document(kvp(key(idf::kDeviceId), codec::uuid_bin(approver)),
                                            kvp(key(idf::kSigningKey),
                                                codec::bytes_bin(seen.approver_key)))))));
        filter.append(kvp(key(idf::kDeviceIdPath), not_in(device.id)));
        filter.append(kvp(key(idf::kRetired), not_in(device.id)));
        // Room for one more, decided by the server with the write.
        filter.append(kvp(key(full_path_), absent()));
        bsoncxx::builder::basic::document update;
        update.append(
            kvp("$push",
                make_document(kvp(key(idf::kDevices), device_document(device, proof, now)))));
        append_change(update, now);
        const auto result = identities(client).update_one(filter.view(), update.view());
        return result.has_value() && result->matched_count() == 1;
    });
    if (!written) {
        // A duplicate key here is the unique index on the device id: the id is
        // linked to some other account.
        if (written.error().code == ErrorCode::Conflict) {
            return fail(ErrorCode::Conflict, in::kDeviceId);
        }
        return written.error();
    }
    // The set changed between the read and the write: a concurrent link took
    // the last slot, or the approver was unlinked. Either way, not admitted.
    if (!written.value()) {
        return fail(ErrorCode::Conflict, in::kDevices);
    }
    return ok();
}

Result<bool> DeviceDirectory::unlink_device(mongocxx::client& client,
                                            const Uuid& user,
                                            const Uuid& device,
                                            db::TimeMs now) const {
    return repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, idf::kId, user);
        // Matching on the device makes a repeat a no-op: dv does not move and
        // nothing is marked pending for a change that did not happen.
        codec::append_uuid(filter, idf::kDeviceIdPath, device);
        bsoncxx::builder::basic::document update;
        update.append(kvp("$pull",
                          make_document(kvp(key(idf::kDevices),
                                            make_document(kvp(key(idf::kDeviceId),
                                                              codec::uuid_bin(device)))))));
        append_change(update, now);
        append_retire(update, device);
        const auto result = identities(client).update_one(filter.view(), update.view());
        return result.has_value() && result->matched_count() == 1;
    });
}

Result<std::optional<Uuid>> DeviceDirectory::unlink_session(mongocxx::client& client,
                                                            const Uuid& user,
                                                            const Uuid& session,
                                                            db::TimeMs now) const {
    // Which device the session registered, then an unlink of exactly that one.
    // Not a check-then-act: unlink_device is conditional on the device and
    // idempotent, so a device unlinked in between is simply not unlinked twice.
    const Result<std::optional<Uuid>> found =
        repo::guarded([&]() -> Result<std::optional<Uuid>> {
            bsoncxx::builder::basic::document filter;
            codec::append_uuid(filter, idf::kId, user);
            codec::append_uuid(filter, idf::kSessionPath, session);
            mongocxx::options::find options{};
            options.projection(
                make_document(kvp(key(idf::kDeviceIdPath), 1), kvp(key(idf::kSessionPath), 1)));
            const auto doc = identities(client).find_one(filter.view(), options);
            if (!doc) {
                return std::optional<Uuid>{};
            }
            std::optional<Uuid> device;
            const Status walked =
                for_each_device(doc->view(), [&](const bsoncxx::document::view& row) {
                    const Result<Uuid> sid = codec::read_uuid(row, idf::kSession);
                    if (!sid) {
                        return Status{sid.error()};
                    }
                    if (sid.value() != session) {
                        return ok();
                    }
                    const Result<Uuid> id = codec::read_uuid(row, idf::kDeviceId);
                    if (!id) {
                        return Status{id.error()};
                    }
                    device = id.value();
                    return ok();
                });
            if (!walked) {
                return walked.error();
            }
            return device;
        });
    if (!found) {
        return found.error();
    }
    if (!found.value().has_value()) {
        return std::optional<Uuid>{};
    }
    const Result<bool> unlinked = unlink_device(client, user, *found.value(), now);
    if (!unlinked) {
        return unlinked.error();
    }
    return unlinked.value() ? found.value() : std::optional<Uuid>{};
}

Result<std::vector<AccountDevices>> DeviceDirectory::devices_of(mongocxx::client& client,
                                                                std::span<const Uuid> users,
                                                                std::int32_t limit) const {
    if (limit <= 0 || users.size() > static_cast<std::size_t>(limit)) {
        return fail(ErrorCode::ValidationFailed, in::kUsers);
    }
    return repo::guarded([&]() -> Result<std::vector<AccountDevices>> {
        std::vector<AccountDevices> out;
        if (users.empty()) {
            return out;
        }
        out.reserve(users.size());
        bsoncxx::builder::basic::array ids;
        for (const Uuid& user : users) {
            ids.append(codec::uuid_bin(user));
        }
        bsoncxx::builder::basic::document projection;
        projection.append(kvp(key(idf::kDeviceSetVersion), 1));
        for (const std::string_view path : kPublishedPaths) {
            projection.append(kvp(key(path), 1));
        }
        mongocxx::options::find options{};
        options.projection(projection.extract());
        options.limit(static_cast<std::int64_t>(users.size()));
        for (const bsoncxx::document::view doc : identities(client).find(
                 make_document(kvp(key(idf::kId), make_document(kvp("$in", ids.extract()))))
                     .view(),
                 options)) {
            AccountDevices account{};
            const Result<Uuid> user = codec::read_uuid(doc, idf::kId);
            if (!user) {
                return user.error();
            }
            account.user = user.value();
            const Result<std::int64_t> version = read_version(doc);
            if (!version) {
                return version.error();
            }
            account.device_set_version = version.value();
            const Status walked = for_each_device(doc, [&](const bsoncxx::document::view& row) {
                Result<PublishedDevice> device = decode_published(row);
                if (!device) {
                    return Status{device.error()};
                }
                account.devices.push_back(std::move(device).value());
                return ok();
            });
            if (!walked) {
                return walked.error();
            }
            out.push_back(std::move(account));
        }
        return out;
    });
}

Result<std::vector<DeviceOwner>> DeviceDirectory::device_owners(mongocxx::client& client,
                                                               std::span<const Uuid> users,
                                                               std::int32_t limit) const {
    if (limit <= 0 || users.size() > static_cast<std::size_t>(limit)) {
        return fail(ErrorCode::ValidationFailed, in::kUsers);
    }
    return repo::guarded([&]() -> Result<std::vector<DeviceOwner>> {
        std::vector<DeviceOwner> out;
        if (users.empty()) {
            return out;
        }
        bsoncxx::builder::basic::array ids;
        for (const Uuid& user : users) {
            ids.append(codec::uuid_bin(user));
        }
        mongocxx::options::find options{};
        options.projection(make_document(kvp(key(idf::kDeviceIdPath), 1)));
        options.limit(static_cast<std::int64_t>(users.size()));
        for (const bsoncxx::document::view doc : identities(client).find(
                 make_document(kvp(key(idf::kId), make_document(kvp("$in", ids.extract()))))
                     .view(),
                 options)) {
            const Result<Uuid> user = codec::read_uuid(doc, idf::kId);
            if (!user) {
                return user.error();
            }
            const Status walked = for_each_device(doc, [&](const bsoncxx::document::view& row) {
                const Result<Uuid> id = codec::read_uuid(row, idf::kDeviceId);
                if (!id) {
                    return Status{id.error()};
                }
                out.push_back(DeviceOwner{.device = id.value(), .user = user.value()});
                return ok();
            });
            if (!walked) {
                return walked.error();
            }
        }
        return out;
    });
}

Result<std::optional<IdentityRecord>> DeviceDirectory::identity(mongocxx::client& client,
                                                                const Uuid& user) const {
    return repo::guarded([&]() -> Result<std::optional<IdentityRecord>> {
        mongocxx::options::find options{};
        options.projection(make_document(kvp(key(idf::kRetired), 0)));
        const auto found = identities(client).find_one(
            make_document(kvp(key(idf::kId), codec::uuid_bin(user))).view(), options);
        if (!found) {
            return std::optional<IdentityRecord>{};
        }
        const bsoncxx::document::view doc = found->view();
        IdentityRecord out{};
        out.user = user;
        const Result<std::int64_t> version = read_version(doc);
        if (!version) {
            return version.error();
        }
        out.device_set_version = version.value();
        const Result<std::optional<db::TimeMs>> pending =
            codec::read_optional_time(doc, idf::kPendingSince);
        if (!pending) {
            return pending.error();
        }
        out.pending_since = pending.value();
        const Status walked = for_each_device(doc, [&](const bsoncxx::document::view& row) {
            Result<DeviceRecord> device = decode_device(row);
            if (!device) {
                return Status{device.error()};
            }
            out.devices.push_back(std::move(device).value());
            return ok();
        });
        if (!walked) {
            return walked.error();
        }
        return std::optional<IdentityRecord>{std::move(out)};
    });
}

Status DeviceDirectory::rotate_prekeys(mongocxx::client& client,
                                       const Uuid& user,
                                       const Uuid& device,
                                       const std::optional<SignedPrekey>& signed_prekey,
                                       const std::optional<SignedPrekey>& last_resort) const {
    if (!signed_prekey.has_value() && !last_resort.has_value()) {
        return Failure{ErrorCode::ValidationFailed, in::kSignedPrekey,
                       static_cast<std::uint16_t>(input::Reason::Required)};
    }
    if (signed_prekey.has_value() && !crypto::x25519_public_key_is_valid(signed_prekey->key)) {
        return fail(ErrorCode::ValidationFailed, in::kSignedPrekey);
    }
    if (last_resort.has_value() && !crypto::x25519_public_key_is_valid(last_resort->key)) {
        return fail(ErrorCode::ValidationFailed, in::kLastResortKey);
    }
    // The signing key the device was admitted with: read, verified against,
    // and restated in the write's filter.
    const Result<std::optional<crypto::Ed25519PublicKey>> stored =
        repo::guarded([&]() -> Result<std::optional<crypto::Ed25519PublicKey>> {
            mongocxx::options::find options{};
            options.projection(make_document(kvp(key(idf::kDeviceIdPath), 1),
                                             kvp(key(kSigningKeyPath), 1)));
            const auto found = identities(client).find_one(
                make_document(kvp(key(idf::kId), codec::uuid_bin(user))).view(), options);
            if (!found) { return std::optional<crypto::Ed25519PublicKey>{}; }
            std::optional<crypto::Ed25519PublicKey> out;
            const Status walked =
                for_each_device(found->view(), [&](const bsoncxx::document::view& row) {
                    const Result<Uuid> id = codec::read_uuid(row, idf::kDeviceId);
                    if (!id) { return Status{id.error()}; }
                    if (id.value() != device) { return ok(); }
                    crypto::Ed25519PublicKey signing{};
                    if (const Status read = read_key(row, idf::kSigningKey, signing); !read) {
                        return read;
                    }
                    out = signing;
                    return ok();
                });
            if (!walked) { return walked.error(); }
            return out;
        });
    if (!stored) { return stored.error(); }
    if (!stored.value().has_value()) { return fail(ErrorCode::Forbidden, in::kDeviceId); }
    const crypto::Ed25519PublicKey& signing = *stored.value();
    if (signed_prekey.has_value() &&
        !crypto::ed25519_verify(signing,
                                prekey_message(kSignedPrekeyDomain, device, signed_prekey->key),
                                signed_prekey->signature)) {
        return fail(ErrorCode::ValidationFailed, in::kSignedPrekeySignature);
    }
    if (last_resort.has_value() &&
        !crypto::ed25519_verify(signing,
                                prekey_message(kLastResortDomain, device, last_resort->key),
                                last_resort->signature)) {
        return fail(ErrorCode::ValidationFailed, in::kLastResortSignature);
    }
    const Result<bool> written = repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, idf::kId, user);
        filter.append(kvp(key(idf::kDevices),
                          make_document(kvp("$elemMatch",
                                            make_document(kvp(key(idf::kDeviceId),
                                                              codec::uuid_bin(device)),
                                                          kvp(key(idf::kSigningKey),
                                                              codec::bytes_bin(signing)))))));
        bsoncxx::builder::basic::document set;
        const auto positional = [](std::string_view field) {
            return std::string{idf::kDevices} + ".$." + std::string{field};
        };
        if (signed_prekey.has_value()) {
            set.append(kvp(positional(idf::kSignedPrekey), codec::bytes_bin(signed_prekey->key)));
            set.append(kvp(positional(idf::kSignedPrekeySignature),
                           codec::bytes_bin(signed_prekey->signature)));
        }
        if (last_resort.has_value()) {
            set.append(kvp(positional(idf::kLastResortKey), codec::bytes_bin(last_resort->key)));
            set.append(kvp(positional(idf::kLastResortSignature),
                           codec::bytes_bin(last_resort->signature)));
        }
        const auto result = identities(client).update_one(
            filter.view(), make_document(kvp("$set", set.extract())).view());
        return result.has_value() && result->matched_count() == 1;
    });
    if (!written) { return written.error(); }
    if (!written.value()) { return fail(ErrorCode::Forbidden, in::kDeviceId); }
    return ok();
}

Result<bool> DeviceDirectory::touch(mongocxx::client& client,
                                    const Uuid& user,
                                    const Uuid& device,
                                    db::TimeMs now) const {
    return repo::guarded([&]() -> Result<bool> {
        const db::TimeMs stale_before = now - config_.touch_interval;
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, idf::kId, user);
        // The coalescing is in the FILTER: a recent last_seen matches nothing,
        // so the common case is a read of one index entry and no write at all.
        filter.append(
            kvp(key(idf::kDevices),
                make_document(
                    kvp("$elemMatch",
                        make_document(
                            kvp(key(idf::kDeviceId), codec::uuid_bin(device)),
                            kvp(key(idf::kLastSeen),
                                make_document(kvp("$lt", codec::time_date(stale_before)))))))));
        const auto result = identities(client).update_one(
            filter.view(),
            make_document(
                kvp("$set", make_document(kvp(key(kSeenPositional), codec::time_date(now)))))
                .view());
        return result.has_value() && result->modified_count() == 1;
    });
}

Result<std::vector<UnlinkedDevice>> DeviceDirectory::unlink_idle(mongocxx::client& client,
                                                                 db::TimeMs now,
                                                                 std::uint32_t idle_days,
                                                                 std::int32_t batch) const {
    const db::TimeMs cutoff =
        now - std::chrono::hours{24} * std::max<std::uint32_t>(idle_days, 1);
    const std::int32_t bounded = batch > 0 ? batch : 1;
    // The scan collects first and writes after, so no update moves a document
    // under the cursor that found it.
    const Result<std::vector<UnlinkedDevice>> idle =
        repo::guarded([&]() -> Result<std::vector<UnlinkedDevice>> {
            std::vector<UnlinkedDevice> out;
            mongocxx::options::find options{};
            options.projection(make_document(kvp(key(idf::kDeviceIdPath), 1),
                                             kvp(key(idf::kLastSeenPath), 1)));
            options.limit(bounded);
            for (const bsoncxx::document::view doc : identities(client).find(
                     make_document(kvp(key(idf::kLastSeenPath),
                                       make_document(kvp("$lt", codec::time_date(cutoff)))))
                         .view(),
                     options)) {
                const Result<Uuid> user = codec::read_uuid(doc, idf::kId);
                if (!user) {
                    return user.error();
                }
                const Status walked =
                    for_each_device(doc, [&](const bsoncxx::document::view& row) {
                        const Result<db::TimeMs> seen = codec::read_time(row, idf::kLastSeen);
                        if (!seen) {
                            return Status{seen.error()};
                        }
                        if (seen.value() >= cutoff) {
                            return ok();
                        }
                        const Result<Uuid> id = codec::read_uuid(row, idf::kDeviceId);
                        if (!id) {
                            return Status{id.error()};
                        }
                        out.push_back(UnlinkedDevice{user.value(), id.value()});
                        return ok();
                    });
                if (!walked) {
                    return walked.error();
                }
            }
            return out;
        });
    if (!idle) {
        return idle.error();
    }

    std::vector<UnlinkedDevice> unlinked;
    unlinked.reserve(idle.value().size());
    for (const UnlinkedDevice& candidate : idle.value()) {
        const Result<bool> removed = repo::guarded([&]() -> Result<bool> {
            const auto still_idle = [&]() {
                return make_document(
                    kvp(key(idf::kDeviceId), codec::uuid_bin(candidate.device)),
                    kvp(key(idf::kLastSeen),
                        make_document(kvp("$lt", codec::time_date(cutoff)))));
            };
            bsoncxx::builder::basic::document filter;
            codec::append_uuid(filter, idf::kId, candidate.user);
            // Idleness restated: a device that made a request after the scan
            // has a fresh last_seen and is not matched.
            filter.append(
                kvp(key(idf::kDevices), make_document(kvp("$elemMatch", still_idle()))));
            bsoncxx::builder::basic::document update;
            update.append(kvp("$pull", make_document(kvp(key(idf::kDevices), still_idle()))));
            append_change(update, now);
            append_retire(update, candidate.device);
            const auto result = identities(client).update_one(filter.view(), update.view());
            return result.has_value() && result->matched_count() == 1;
        });
        if (!removed) {
            return removed.error();
        }
        if (removed.value()) {
            unlinked.push_back(candidate);
        }
    }
    return unlinked;
}

Result<std::vector<PendingChange>> DeviceDirectory::pending_changes(mongocxx::client& client,
                                                                    db::TimeMs before,
                                                                    std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<PendingChange>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;
        mongocxx::options::find options{};
        options.projection(make_document(kvp(key(idf::kPendingSince), 1)));
        options.sort(make_document(kvp(key(idf::kPendingSince), 1)));
        options.limit(bounded);
        std::vector<PendingChange> out;
        out.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view doc : identities(client).find(
                 make_document(kvp(key(idf::kPendingSince),
                                   make_document(kvp("$lt", codec::time_date(before)))))
                     .view(),
                 options)) {
            const Result<Uuid> user = codec::read_uuid(doc, idf::kId);
            if (!user) {
                return user.error();
            }
            const Result<db::TimeMs> since = codec::read_time(doc, idf::kPendingSince);
            if (!since) {
                return since.error();
            }
            out.push_back(PendingChange{.since = since.value(), .user = user.value()});
        }
        return out;
    });
}

Result<bool> DeviceDirectory::clear_pending(mongocxx::client& client,
                                            const Uuid& user,
                                            db::TimeMs since) const {
    return repo::guarded([&]() -> Result<bool> {
        const auto result = identities(client).update_one(
            make_document(kvp(key(idf::kId), codec::uuid_bin(user)),
                          kvp(key(idf::kPendingSince), codec::time_date(since)))
                .view(),
            make_document(kvp("$unset", make_document(kvp(key(idf::kPendingSince), ""))))
                .view());
        return result.has_value() && result->modified_count() == 1;
    });
}

}  // namespace anvil::chat
