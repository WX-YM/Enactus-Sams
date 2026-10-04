#pragma once

// The key directory: one-time prekeys, handed out at most once each
// (docs/22-chat.md §7.5).
//
// A sender opening a session with a device needs one of that device's one-time
// prekeys, and the forward secrecy of the session's first message rests on
// nobody else ever having been given the same one. So a claim is ONE
// find_one_and_delete per device: the server picks the key and removes it in
// the same operation, and two concurrent claims cannot both be answered with
// it. A read followed by a delete would hand the key to both.
//
// When a device has none left, the claim answers its LAST-RESORT key (signed,
// reusable, published with the device in anvil/chat/devices.h) and flags the
// device `low`, so it uploads more on its next connection. Its upload clears
// the flag.
//
// --- rows --------------------------------------------------------------------------
//
//   _id   uuid v4 (the row's own; nothing addresses it)
//   d     device id
//   u     account id
//   k     X25519 public key, 32 bytes
//   kid   the CLIENT's key id, unique per device ({d, kid} unique), which a
//         sender echoes so the device can find the private half
//
// --- the per-device bound, and how exact it is ------------------------------------
//
// At most kMaxPrekeysPerDevice rows per device. The count is a counter on the
// device's element of the identity document (`pk`), not a count of rows,
// because a count followed by an insert lets every concurrent uploader pass the
// same count. An upload reserves its batch with one conditional $inc whose
// filter is "pk + n <= 1000", in the same transaction as the insert, so the
// reservation and the rows commit together or not at all; a claim takes one
// back with $inc -1. The bound is therefore exact, with one deliberate
// exception: a claim that finds a device EMPTY resets `pk` to 0, which corrects
// any upward drift a crash between a claim's delete and its decrement left
// behind (an upload refused at a count the pool does not hold would leave the
// device on its last-resort key for good). An upload committing in the instant
// between that claim's empty read and its reset is the one way past the bound,
// and it is past it by that one batch.
//
// --- unlinked devices ----------------------------------------------------------------
//
// A claim walks the CURRENT devices on the identity document, never the rows
// here, so the rows of an unlinked device are unreachable the moment the unlink
// commits. Deleting them (discard) is therefore an idempotent follow-up rather
// than part of the unlink, and a crash between the two leaves rows nobody can
// claim. A later device reusing the id would see them again only if the id came
// back, and device-id reuse within an account is refused for the replay window
// (`gone` in devices.h).
//
// --- what is not here ----------------------------------------------------------------
//
// Claims are budgeted per CLAIMER and per TARGET (§7.5), and both budgets are
// the claim route's, declared by the application like every other rate rule.
// Exhaustion is the attack: one claimer draining a victim's pool pushes every
// new session with the victim onto the reusable last-resort key, and the
// per-target rule is what an attacker with many accounts runs into. This file
// cannot hold either budget, because it does not know who is asking.
//
// The upload route authenticates the SESSION. A stolen session can therefore
// upload one-time keys for one of the account's devices whose private halves
// the device does not hold. That breaks sessions opened with them (the device
// cannot decrypt) and reveals nothing: X3DH also needs the device's identity
// and signed prekey private keys, which no session carries.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>

#include "anvil/chat/devices.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/x25519.h"
#include "anvil/db/collections.h"

namespace anvil::chat {

namespace prekey_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kDevice = "d";
inline constexpr std::string_view kUser = "u";
inline constexpr std::string_view kKey = "k";
inline constexpr std::string_view kKeyId = "kid";
// A sub-key of the IDENTITY document's device element, not of a prekey row:
// how many one-time keys the device holds. OMITTED until its first upload.
inline constexpr std::string_view kStoredCount = "pk";
}  // namespace prekey_fields

namespace prekey_inputs {
inline constexpr std::string_view kPrekeys = "prekeys";
inline constexpr std::string_view kPrekey = "prekey";
inline constexpr std::string_view kKeyId = "kid";
inline constexpr std::string_view kDevice = "device";
}  // namespace prekey_inputs

inline constexpr std::size_t kMaxPrekeyBatch = 100;
inline constexpr std::int64_t kMaxPrekeysPerDevice = 1000;

struct OneTimePrekey final {
    crypto::X25519PublicKey key;  // 32
    std::uint32_t id;             //  4
};

static_assert(sizeof(OneTimePrekey) == 36);

// What a sender needs to open a session with one device.
struct ClaimedBundle final {
    // Identity keys, signed prekey and last-resort key, with their signatures,
    // for the sender to verify before using any of them.
    DeviceKeys keys;
    // nullopt when the device had none left: the sender uses keys.last_resort.
    std::optional<OneTimePrekey> one_time;
    Uuid device;
};

// Synchronous, on db_pool (CLAUDE.md §4), like DeviceDirectory.
class PrekeyDirectory final {
public:
    PrekeyDirectory(const db::DatabaseNames& databases,
                    const DeviceCollections& collections,
                    const DeviceConfig& config);

    // Stores a batch of one-time keys for one of `user`'s current devices and
    // clears its `low` flag.
    //
    //   ValidationFailed  empty or over kMaxPrekeyBatch (prekeys), a malformed
    //                     key (prekey), or a key id twice in the batch (kid)
    //   Forbidden         `device` is not a current device of `user`
    //   Conflict          the batch would take the device past
    //                     kMaxPrekeysPerDevice (prekeys), or a key id the
    //                     device already holds (kid)
    //
    // All or nothing: the reservation and the rows are one transaction, so a
    // refused batch leaves neither a partial set of rows nor a reserved count.
    [[nodiscard]] Status upload(mongocxx::client& client,
                                const Uuid& user,
                                const Uuid& device,
                                std::span<const OneTimePrekey> keys) const;

    // One bundle per current device of `user`, each with a one-time key taken
    // by its own find_one_and_delete, or the last-resort key and the `low` flag
    // set when there was none. Empty for an account with no device. Bounded by
    // the device ceiling.
    [[nodiscard]] Result<std::vector<ClaimedBundle>> claim(mongocxx::client& client,
                                                           const Uuid& user) const;

    // How many one-time keys the device holds, counted up to the bound.
    [[nodiscard]] Result<std::int64_t> remaining(mongocxx::client& client,
                                                 const Uuid& user,
                                                 const Uuid& device) const;

    // Deletes every row of an unlinked device. Idempotent; see the header
    // comment for why it may run late.
    [[nodiscard]] Status discard(mongocxx::client& client,
                                 const Uuid& user,
                                 const Uuid& device) const;

private:
    [[nodiscard]] mongocxx::collection identities(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection prekeys(mongocxx::client& client) const;

    DeviceDirectory devices_;
    std::string identities_database_;
    std::string prekeys_database_;
    std::string_view identities_;
    std::string_view prekeys_;
};

}  // namespace anvil::chat
