#pragma once

// The per-device queue: the ciphertexts an encrypted send addressed to ONE
// device (docs/22-chat.md §7.6).
//
// A pairwise message (every direct message, and every sender-key distribution)
// is a different ciphertext for every device it is for. Those ciphertexts are
// not the message row's: the row holds the common ciphertext, if any, which
// every member's device reads alike. Each per-device one is a row here, read by
// that device alone and deleted when it says it has them.
//
// --- rows -----------------------------------------------------------------------
//
//   _id   uuid v7, so a device's rows are in send order and "everything up to
//         this one" is a range
//   d     the device it is for
//   u     that device's account
//   c, s  the message it belongs to: the conversation and the seq, so it sits
//         at a position in the log like everything else
//   f     the sender's account
//   fd    the sending device, so the recipient knows which session decrypts it
//   ct    the ciphertext, opaque
//   exp   when the row stops being served and the TTL monitor may remove it
//
// --- the TTL index is allowed here, and is not on messages -------------------
//
// A queue row holds no media reference: an attachment of an encrypted message is
// referenced by the message row, whose sweeper releases it (§4.7). So the TTL
// monitor deleting a row without running any code leaks nothing, and it is the
// garbage collector for a device that never comes back. It is not the access
// control: every read filters `exp` as well (docs/09 §6), because the monitor
// runs about once a minute.
//
// --- what deletes a row ---------------------------------------------------------
//
// The device's acknowledgement (everything up to an id, in one delete_many),
// the message being revoked (§4.5), the device being unlinked, and the TTL. NOT
// delivery: deleting a message once every device has it needs a record per
// device per message of who has it, which is the write per recipient per message
// docs/11 §7 refuses for receipts.

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/collection.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/db/collections.h"

namespace anvil::chat {

namespace device_queue_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kDevice = "d";
inline constexpr std::string_view kUser = "u";
inline constexpr std::string_view kConversation = "c";
inline constexpr std::string_view kSeq = "s";
inline constexpr std::string_view kSender = "f";
inline constexpr std::string_view kSenderDevice = "fd";
inline constexpr std::string_view kCiphertext = "ct";
inline constexpr std::string_view kExpiresAt = "exp";
}  // namespace device_queue_fields

// Days an undelivered per-device ciphertext is kept, unless the deployment
// passes its own (§7.6).
inline constexpr std::uint32_t kDefaultUndeliveredDays = 30;

// The most rows one read answers. A device that was offline for a week drains
// in pages, acknowledging each.
inline constexpr std::int32_t kMaxQueuePage = 100;

// One per-device ciphertext, as stored and as read back.
struct QueuedCiphertext final {
    std::vector<std::uint8_t> ciphertext;
    db::TimeMs                expires_at;
    std::int64_t              seq;
    Uuid                      id;
    Uuid                      device;
    Uuid                      user;
    Uuid                      conversation;
    Uuid                      sender;
    Uuid                      sender_device;
};

class DeviceQueue final {
public:
    DeviceQueue(const db::DatabaseNames& databases, std::string_view collection,
                std::uint32_t undelivered_days = kDefaultUndeliveredDays);

    [[nodiscard]] std::chrono::hours retention() const noexcept { return retention_; }

    // The rows of one send, inside its transaction: they and the message row
    // commit together, or neither does. Each row's id and expiry are the
    // caller's.
    [[nodiscard]] Status enqueue(mongocxx::client& client, mongocxx::client_session& session,
                                 std::span<const QueuedCiphertext> rows) const;

    // `device`'s rows after `after` (or from the start), oldest first, at most
    // `limit`, unexpired. The caller has established that `device` is one of
    // `user`'s current devices; `u` is in the filter as well, so a device id
    // alone never reads another account's rows.
    [[nodiscard]] Result<std::vector<QueuedCiphertext>> read(mongocxx::client& client,
                                                             const Uuid& user, const Uuid& device,
                                                             const std::optional<Uuid>& after,
                                                             std::int32_t limit,
                                                             db::TimeMs now) const;

    // Deletes `device`'s rows up to and including `through`. Answers how many.
    [[nodiscard]] Result<std::int64_t> acknowledge(mongocxx::client& client, const Uuid& user,
                                                   const Uuid& device, const Uuid& through) const;

    // Every row of one message, in the revoke's transaction: delete for
    // everyone removes the per-device ciphertexts with the common one.
    [[nodiscard]] Status discard_message(mongocxx::client& client,
                                         mongocxx::client_session& session,
                                         const Uuid& conversation, std::int64_t seq) const;

    // Every row of an unlinked device. Idempotent, and may run late: a read
    // requires a current device, so the rows of an unlinked one are unreachable
    // the moment the unlink commits.
    [[nodiscard]] Status discard_device(mongocxx::client& client, const Uuid& user,
                                        const Uuid& device) const;

private:
    [[nodiscard]] mongocxx::collection rows(mongocxx::client& client) const;

    std::string        database_;
    std::string_view   collection_;
    std::chrono::hours retention_;
};

}  // namespace anvil::chat
