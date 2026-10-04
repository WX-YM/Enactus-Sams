// versioned-write-exempt: nothing here is read, modified and written back. A
// one-time key leaves by find_one_and_delete; the per-device count moves by a
// conditional $inc whose filter is the bound itself, inside the transaction
// that inserts the rows it counts; the low flag is a blind $set or $unset whose
// last writer is meant to win.

#include "anvil/chat/prekeys.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_delete.hpp>
#include <mongocxx/options/update.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/core/uuid.h"
#include "anvil/db/repository.h"

namespace anvil::chat {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

namespace codec = db::codec;
namespace idf = identity_fields;
namespace pf = prekey_fields;
namespace in = prekey_inputs;

[[nodiscard]] bsoncxx::stdx::string_view key(std::string_view name) noexcept {
    return codec::key_of(name);
}

// Positional paths into the identity document's device element.
inline constexpr std::string_view kCountPositional = "dev.$.pk";
inline constexpr std::string_view kLowPositional = "dev.$.low";
static_assert(detail::is_path(kCountPositional, "dev.$", pf::kStoredCount) &&
              detail::is_path(kLowPositional, "dev.$", idf::kLow) &&
              detail::is_path("dev.$", idf::kDevices, "$"));

// Thrown out of the transaction body so the driver aborts rather than commits;
// returning normally from a callback whose write the server refused makes it
// retry the commit of an aborted transaction for two minutes.
struct Abort final {
    Failure failure;
};

[[nodiscard]] bsoncxx::document::value device_rows(const Uuid& user, const Uuid& device) {
    return make_document(kvp(key(pf::kDevice), codec::uuid_bin(device)),
                         kvp(key(pf::kUser), codec::uuid_bin(user)));
}

// "This device of this account", as an $elemMatch on the identity document.
[[nodiscard]] bsoncxx::document::value current_device(const Uuid& user, const Uuid& device) {
    return make_document(kvp(key(idf::kId), codec::uuid_bin(user)),
                         kvp(key(idf::kDevices),
                             make_document(kvp("$elemMatch",
                                               make_document(kvp(key(idf::kDeviceId),
                                                                 codec::uuid_bin(device)))))));
}

}  // namespace

PrekeyDirectory::PrekeyDirectory(const db::DatabaseNames& databases,
                                 const DeviceCollections& collections,
                                 const DeviceConfig& config)
    : devices_{databases, collections, config},
      identities_database_{databases.for_collection(collections.identities)},
      prekeys_database_{databases.for_collection(collections.prekeys)},
      identities_{collections.identities},
      prekeys_{collections.prekeys} {}

mongocxx::collection PrekeyDirectory::identities(mongocxx::client& client) const {
    return client[identities_database_][std::string{identities_}];
}

mongocxx::collection PrekeyDirectory::prekeys(mongocxx::client& client) const {
    return client[prekeys_database_][std::string{prekeys_}];
}

Status PrekeyDirectory::upload(mongocxx::client& client,
                               const Uuid& user,
                               const Uuid& device,
                               std::span<const OneTimePrekey> keys) const {
    if (keys.empty() || keys.size() > kMaxPrekeyBatch) {
        return fail(ErrorCode::ValidationFailed, in::kPrekeys);
    }
    std::array<std::uint32_t, kMaxPrekeyBatch> ids{};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!crypto::x25519_public_key_is_valid(keys[i].key)) {
            return fail(ErrorCode::ValidationFailed, in::kPrekey);
        }
        ids[i] = keys[i].id;
    }
    // A key id twice in one batch would be refused by the unique index halfway
    // through the insert; refusing it here names the cause.
    const auto used = ids.begin() + static_cast<std::ptrdiff_t>(keys.size());
    std::sort(ids.begin(), used);
    if (std::adjacent_find(ids.begin(), used) != used) {
        return fail(ErrorCode::ValidationFailed, in::kKeyId);
    }

    const auto count = static_cast<std::int64_t>(keys.size());
    std::vector<bsoncxx::document::value> rows;
    rows.reserve(keys.size());
    for (const OneTimePrekey& one : keys) {
        bsoncxx::builder::basic::document row;
        codec::append_uuid(row, pf::kId, uuid::generate_v4());
        codec::append_uuid(row, pf::kDevice, device);
        codec::append_uuid(row, pf::kUser, user);
        row.append(kvp(key(pf::kKey), codec::bytes_bin(one.key)));
        row.append(kvp(key(pf::kKeyId), bsoncxx::types::b_int64{std::int64_t{one.id}}));
        rows.push_back(row.extract());
    }

    const Status written = repo::guarded([&]() -> Status {
        try {
            auto session = client.start_session();
            repo::in_transaction(session, [&](mongocxx::client_session* txn) {
                // The reservation: the device is current, and the batch fits.
                // A missing count is a device that never uploaded, which $not
                // matches and $inc creates.
                bsoncxx::builder::basic::document filter;
                codec::append_uuid(filter, idf::kId, user);
                filter.append(kvp(
                    key(idf::kDevices),
                    make_document(kvp(
                        "$elemMatch",
                        make_document(
                            kvp(key(idf::kDeviceId), codec::uuid_bin(device)),
                            kvp(key(pf::kStoredCount),
                                make_document(kvp("$not",
                                                  make_document(kvp("$gt",
                                                                    bsoncxx::types::b_int64{
                                                                        kMaxPrekeysPerDevice -
                                                                        count}))))))))));
                const auto reserved = identities(client).update_one(
                    *txn,
                    filter.view(),
                    make_document(kvp("$inc",
                                      make_document(kvp(key(kCountPositional),
                                                        bsoncxx::types::b_int64{count}))),
                                  kvp("$unset", make_document(kvp(key(kLowPositional), ""))))
                        .view());
                if (!reserved.has_value() || reserved->matched_count() != 1) {
                    // Why it did not match, for the caller: a device that is not
                    // this account's is refused differently from a full one.
                    mongocxx::options::find only_id{};
                    only_id.projection(make_document(kvp(key(idf::kId), 1)));
                    const auto exists = identities(client).find_one(
                        *txn, current_device(user, device).view(), only_id);
                    throw Abort{exists ? fail(ErrorCode::Conflict, in::kPrekeys)
                                       : fail(ErrorCode::Forbidden, in::kDevice)};
                }
                // A key id the device already holds collides on {d, kid} and
                // aborts the transaction, reservation included.
                prekeys(client).insert_many(*txn, rows);
            });
        } catch (const Abort& aborted) {
            return aborted.failure;
        }
        return ok();
    });
    // A duplicate key out of the insert has no field of its own.
    if (!written && written.error().code == ErrorCode::Conflict &&
        written.error().field.empty()) {
        return fail(ErrorCode::Conflict, in::kKeyId);
    }
    return written;
}

Result<std::vector<ClaimedBundle>> PrekeyDirectory::claim(mongocxx::client& client,
                                                          const Uuid& user) const {
    // The CURRENT devices, from the identity document: an unlinked device's
    // rows are never looked at, whatever is left of them.
    const std::array<Uuid, 1> account{user};
    const Result<std::vector<AccountDevices>> listed = devices_.devices_of(client, account, 1);
    if (!listed) {
        return listed.error();
    }
    std::vector<ClaimedBundle> out;
    if (listed.value().empty()) {
        return out;
    }
    const std::vector<PublishedDevice>& devices = listed.value()[0].devices;
    out.reserve(devices.size());

    for (const PublishedDevice& device : devices) {
        Result<std::optional<OneTimePrekey>> taken =
            repo::guarded([&]() -> Result<std::optional<OneTimePrekey>> {
                mongocxx::options::find_one_and_delete options{};
                options.projection(
                    make_document(kvp(key(pf::kKey), 1), kvp(key(pf::kKeyId), 1)));
                // The whole guarantee: the server chooses the row and removes
                // it in one operation, so no other claim can be given it.
                const auto row = prekeys(client).find_one_and_delete(
                    device_rows(user, device.id).view(), options);
                if (!row) {
                    return std::optional<OneTimePrekey>{};
                }
                OneTimePrekey one{};
                if (const Status read = codec::read_bytes(row->view(), pf::kKey, one.key);
                    !read) {
                    return read.error();
                }
                const Result<std::int64_t> id = codec::read_int64(row->view(), pf::kKeyId);
                if (!id) {
                    return id.error();
                }
                if (id.value() < 0 || id.value() > std::int64_t{UINT32_MAX}) {
                    return fail(ErrorCode::Internal, pf::kKeyId);
                }
                one.id = static_cast<std::uint32_t>(id.value());
                return std::optional<OneTimePrekey>{one};
            });
        if (!taken) {
            return taken.error();
        }
        out.push_back(ClaimedBundle{device.keys, taken.value(), device.id});
    }

    // The bookkeeping, in one write: a count back for each key taken, and for
    // each device found empty the low flag and a count reset to what the pool
    // provably holds. Answered regardless of its outcome: the keys above are
    // already deleted, and failing the claim now would spend them on nobody.
    const Status booked = repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document inc;
        bsoncxx::builder::basic::document set;
        bsoncxx::builder::basic::array filters;
        bool incs = false;
        bool sets = false;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const std::string name = "d" + std::to_string(i);
            const std::string element = std::string{idf::kDevices} + ".$[" + name + "].";
            filters.append(make_document(
                kvp(name + "." + std::string{idf::kDeviceId}, codec::uuid_bin(out[i].device))));
            if (out[i].one_time.has_value()) {
                inc.append(
                    kvp(element + std::string{pf::kStoredCount}, bsoncxx::types::b_int64{-1}));
                incs = true;
            } else {
                set.append(kvp(element + std::string{idf::kLow}, bsoncxx::types::b_bool{true}));
                set.append(
                    kvp(element + std::string{pf::kStoredCount}, bsoncxx::types::b_int64{0}));
                sets = true;
            }
        }
        bsoncxx::builder::basic::document update;
        if (incs) {
            update.append(kvp("$inc", inc.extract()));
        }
        if (sets) {
            update.append(kvp("$set", set.extract()));
        }
        mongocxx::options::update options{};
        options.array_filters(filters.extract());
        identities(client).update_one(
            make_document(kvp(key(idf::kId), codec::uuid_bin(user))).view(),
            update.view(),
            options);
        return ok();
    });
    if (!booked) {
        LOG_WARN << "prekey claim bookkeeping not written; the count may read high until the "
                    "device's pool next runs empty";
    }
    return out;
}

Result<std::int64_t> PrekeyDirectory::remaining(mongocxx::client& client,
                                                const Uuid& user,
                                                const Uuid& device) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        mongocxx::options::count options{};
        options.limit(kMaxPrekeysPerDevice);
        return prekeys(client).count_documents(device_rows(user, device).view(), options);
    });
}

Status PrekeyDirectory::discard(mongocxx::client& client,
                                const Uuid& user,
                                const Uuid& device) const {
    return repo::guarded([&]() -> Status {
        prekeys(client).delete_many(device_rows(user, device).view());
        return ok();
    });
}

}  // namespace anvil::chat
