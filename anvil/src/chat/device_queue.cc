// versioned-write-exempt: a queue row is inserted once and deleted once, by the
// device it is for, by a revoke, by an unlink or by the TTL monitor. Nothing
// reads one and writes it back.

#include "anvil/chat/device_queue.h"

#include <algorithm>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>

#include "anvil/db/repository.h"

namespace anvil::chat {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

namespace codec = db::codec;
namespace qf = device_queue_fields;

[[nodiscard]] bsoncxx::stdx::string_view key(std::string_view name) noexcept {
    return codec::key_of(name);
}

[[nodiscard]] bsoncxx::types::b_binary blob(std::span<const std::uint8_t> bytes) noexcept {
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                    static_cast<std::uint32_t>(bytes.size()), bytes.data()};
}

// The rows of one device of one account: the id alone would do, as a device id
// names one device anywhere, and the account is there so that a device id
// presented by the wrong account matches nothing even if that ever stopped
// being true.
[[nodiscard]] bsoncxx::builder::basic::document device_filter(const Uuid& user,
                                                              const Uuid& device) {
    bsoncxx::builder::basic::document filter;
    codec::append_uuid(filter, qf::kDevice, device);
    codec::append_uuid(filter, qf::kUser, user);
    return filter;
}

[[nodiscard]] Result<QueuedCiphertext> decode(const bsoncxx::document::view& doc) {
    QueuedCiphertext out{};
    for (const auto& [field, target] :
         {std::pair{qf::kId, &out.id}, std::pair{qf::kDevice, &out.device},
          std::pair{qf::kUser, &out.user}, std::pair{qf::kConversation, &out.conversation},
          std::pair{qf::kSender, &out.sender}, std::pair{qf::kSenderDevice, &out.sender_device}}) {
        const Result<Uuid> id = codec::read_uuid(doc, field);
        if (!id) { return id.error(); }
        *target = id.value();
    }
    const Result<std::int64_t> seq = codec::read_int64(doc, qf::kSeq);
    if (!seq) { return seq.error(); }
    out.seq = seq.value();
    const Result<db::TimeMs> expires = codec::read_time(doc, qf::kExpiresAt);
    if (!expires) { return expires.error(); }
    out.expires_at = expires.value();
    auto element = doc.find(key(qf::kCiphertext));
    if (element == doc.end() || element->type() != bsoncxx::type::k_binary) {
        return fail(ErrorCode::Internal, qf::kCiphertext);
    }
    const bsoncxx::types::b_binary bin = element->get_binary();
    out.ciphertext.assign(bin.bytes, bin.bytes + bin.size);
    return out;
}

}  // namespace

DeviceQueue::DeviceQueue(const db::DatabaseNames& databases, std::string_view collection,
                         std::uint32_t undelivered_days)
    : database_{databases.for_collection(collection)},
      collection_{collection},
      retention_{std::chrono::hours{24} * std::max<std::uint32_t>(undelivered_days, 1)} {}

mongocxx::collection DeviceQueue::rows(mongocxx::client& client) const {
    return client[database_][std::string{collection_}];
}

Status DeviceQueue::enqueue(mongocxx::client& client, mongocxx::client_session& session,
                            std::span<const QueuedCiphertext> queued) const {
    if (queued.empty()) { return ok(); }
    return repo::guarded_in_transaction([&]() -> Status {
        std::vector<bsoncxx::document::value> docs;
        docs.reserve(queued.size());
        for (const QueuedCiphertext& row : queued) {
            bsoncxx::builder::basic::document doc;
            codec::append_uuid(doc, qf::kId, row.id);
            codec::append_uuid(doc, qf::kDevice, row.device);
            codec::append_uuid(doc, qf::kUser, row.user);
            codec::append_uuid(doc, qf::kConversation, row.conversation);
            doc.append(kvp(key(qf::kSeq), bsoncxx::types::b_int64{row.seq}));
            codec::append_uuid(doc, qf::kSender, row.sender);
            codec::append_uuid(doc, qf::kSenderDevice, row.sender_device);
            doc.append(kvp(key(qf::kCiphertext), blob(row.ciphertext)));
            codec::append_time(doc, qf::kExpiresAt, row.expires_at);
            docs.push_back(doc.extract());
        }
        rows(client).insert_many(session, docs);
        return ok();
    });
}

Result<std::vector<QueuedCiphertext>> DeviceQueue::read(mongocxx::client& client,
                                                        const Uuid& user, const Uuid& device,
                                                        const std::optional<Uuid>& after,
                                                        std::int32_t limit,
                                                        db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::vector<QueuedCiphertext>> {
        const std::int32_t bounded = std::clamp(limit, 1, kMaxQueuePage);
        bsoncxx::builder::basic::document filter = device_filter(user, device);
        if (after.has_value()) {
            filter.append(kvp(key(qf::kId), make_document(kvp("$gt", codec::uuid_bin(*after)))));
        }
        // The TTL monitor is a garbage collector, not an access control.
        filter.append(kvp(key(qf::kExpiresAt), make_document(kvp("$gt", codec::time_date(now)))));
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(qf::kDevice), 1), kvp(key(qf::kId), 1)));
        options.limit(bounded);
        std::vector<QueuedCiphertext> out;
        out.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view doc : rows(client).find(filter.view(), options)) {
            Result<QueuedCiphertext> row = decode(doc);
            if (!row) { return row.error(); }
            out.push_back(std::move(row).value());
        }
        return out;
    });
}

Result<std::int64_t> DeviceQueue::acknowledge(mongocxx::client& client, const Uuid& user,
                                              const Uuid& device, const Uuid& through) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter = device_filter(user, device);
        filter.append(kvp(key(qf::kId), make_document(kvp("$lte", codec::uuid_bin(through)))));
        const auto result = rows(client).delete_many(filter.view());
        return result.has_value() ? static_cast<std::int64_t>(result->deleted_count())
                                  : std::int64_t{0};
    });
}

Status DeviceQueue::discard_message(mongocxx::client& client, mongocxx::client_session& session,
                                    const Uuid& conversation, std::int64_t seq) const {
    return repo::guarded_in_transaction([&]() -> Status {
        rows(client).delete_many(
            session, make_document(kvp(key(qf::kConversation), codec::uuid_bin(conversation)),
                                   kvp(key(qf::kSeq), bsoncxx::types::b_int64{seq}))
                         .view());
        return ok();
    });
}

Status DeviceQueue::discard_device(mongocxx::client& client, const Uuid& user,
                                   const Uuid& device) const {
    return repo::guarded([&]() -> Status {
        rows(client).delete_many(device_filter(user, device).view());
        return ok();
    });
}

}  // namespace anvil::chat
