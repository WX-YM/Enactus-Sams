#pragma once

// The device directory: which devices an account has, their public identity
// keys, and the signature chain that admitted each one (docs/22-chat.md §7.3).
//
// --- what a device is, in the database -----------------------------------------
//
// One document per ACCOUNT in the application's identities collection, keyed by
// the user id:
//
//   _id    user id
//   dv     device-set version, $inc-ed by every link and every unlink
//   dev    the devices, at most DeviceConfig::max_devices (anvil's ceiling is
//          kMaxDevicesCeiling)
//   pend   OMITTED unless a device change has not yet been pushed to the
//          account's conversations (§7.4). Written in the SAME update as the
//          change, as its millisecond, by $max. ChatService::propagate_devices
//          clears it once every conversation's dsv has risen past it, and
//          ChatService::sweep_device_changes finishes any a crash left behind:
//          the outbox of docs/11 §3, applied to a device change.
//   gone   the last kMaxDevicesCeiling device ids unlinked from this account,
//          so a link message replayed after its device was removed cannot bring
//          it back (link_device, below)
//
// One document rather than one row per device, because every question asked of
// the set is about the WHOLE set: "is the account full", "is this the first
// device", "is the approver still linked" and "what is the version now" are
// each one predicate in the filter of the write that changes it. With a row per
// device each of them is a count or a read followed by an insert, which is
// exactly the check-then-act two concurrent links win together.
//
// --- the two ways in --------------------------------------------------------------
//
// The FIRST device is trust-on-first-use, and it is admitted only inside five
// minutes of a primary authentication (a password or passkey), not on a session
// alone. A session is a cookie, and a stolen cookie on an account that never
// used encryption would otherwise register the first device, after which every
// later device is admitted by the thief's own signatures and every encrypted
// conversation with the account is readable by them. The route supplies the
// time of that authentication; this file only refuses a stale one.
//
// Every LATER device is admitted by a signature from a device already linked to
// the same account, verified here against that device's stored Ed25519 key
// before anything is written. This is the QR-code step, and it is what makes a
// stolen session insufficient to add a device: no cookie carries a device's
// signing key. Clients verify the whole chain as well, and that is what makes a
// device a malicious OPERATOR adds visible as a changed security code (§7.1);
// the server's check cannot defend against the server.
//
// --- the signed messages, byte for byte ---------------------------------------------
//
// Every message starts with an ASCII domain string, so a signature made for one
// purpose never verifies as another. Every field after it has a fixed length,
// so the concatenation is unambiguous without length prefixes.
//
//   signed prekey   "anvil-chat-spk" (14) ‖ device id (16) ‖ signed prekey (32)
//                   = 62 bytes, signed by THE DEVICE's own signing key
//   last resort     "anvil-chat-lrk" (14) ‖ device id (16) ‖ last-resort key (32)
//                   = 62 bytes, signed by THE DEVICE's own signing key
//   link            "anvil-chat-link" (15) ‖ account id (16) ‖ new device id (16)
//                   ‖ new agreement key (32) ‖ new signing key (32)
//                   ‖ timestamp, Unix seconds, u64 big-endian (8)
//                   = 119 bytes, signed by THE APPROVER's signing key
//
// The prekeys name their device, so a signed prekey lifted from one device's
// bundle cannot be presented as another's. The link names the account, so it
// cannot be replayed into a different account.
//
// --- what is not here ----------------------------------------------------------
//
// Pushing a device change to the conversations (§7.4) is ChatService's, which
// holds the conversations; this file records the intent and stops. One-time prekeys are
// anvil/chat/prekeys.h. Routes, rate limits and the session-revocation hook that
// calls unlink_session are the routes row.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/ed25519.h"
#include "anvil/crypto/x25519.h"
#include "anvil/db/codec.h"
#include "anvil/db/collections.h"

namespace anvil::chat {

// --- field names ------------------------------------------------------------------

namespace identity_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kDeviceSetVersion = "dv";
inline constexpr std::string_view kDevices = "dev";
inline constexpr std::string_view kPendingSince = "pend";
inline constexpr std::string_view kRetired = "gone";

// Sub-keys of one device.
inline constexpr std::string_view kDeviceId = "i";
inline constexpr std::string_view kSuite = "s";
inline constexpr std::string_view kAgreementKey = "ak";
inline constexpr std::string_view kSigningKey = "sk";
inline constexpr std::string_view kSignedPrekey = "spk";
inline constexpr std::string_view kSignedPrekeySignature = "sps";
inline constexpr std::string_view kLastResortKey = "lrk";
inline constexpr std::string_view kLastResortSignature = "lrs";
inline constexpr std::string_view kLinkedAt = "at";
inline constexpr std::string_view kLastSeen = "seen";
// The session that registered the device, so revoking that session unlinks it
// (§7.3: "sign out everywhere" really is everywhere). Never published.
inline constexpr std::string_view kSession = "sid";
// The link proof. All three OMITTED on the first device, which nothing signed.
inline constexpr std::string_view kApprovedBy = "by";
inline constexpr std::string_view kLinkSignature = "lsg";
inline constexpr std::string_view kLinkTimestamp = "lts";
// OMITTED unless set: a claim found the device's one-time prekeys exhausted and
// handed out its last-resort key, so it should upload more (anvil/chat/prekeys.h).
inline constexpr std::string_view kLow = "low";

// Dotted paths, for the index catalogue and the multikey queries. Spelled out
// rather than built, so they are .rodata; the static_asserts below tie each one
// to the two names it is made of.
inline constexpr std::string_view kDeviceIdPath = "dev.i";
inline constexpr std::string_view kLastSeenPath = "dev.seen";
inline constexpr std::string_view kSessionPath = "dev.sid";
}  // namespace identity_fields

namespace detail {
[[nodiscard]] constexpr bool is_path(std::string_view path,
                                     std::string_view parent,
                                     std::string_view child) noexcept {
    return path.size() == parent.size() + 1 + child.size() &&
           path.substr(0, parent.size()) == parent && path[parent.size()] == '.' &&
           path.substr(parent.size() + 1) == child;
}
}  // namespace detail

static_assert(detail::is_path(identity_fields::kDeviceIdPath,
                              identity_fields::kDevices,
                              identity_fields::kDeviceId));
static_assert(detail::is_path(identity_fields::kLastSeenPath,
                              identity_fields::kDevices,
                              identity_fields::kLastSeen));
static_assert(detail::is_path(identity_fields::kSessionPath,
                              identity_fields::kDevices,
                              identity_fields::kSession));

// What a refusal names, so a client can tell which key it got wrong. Compile-time
// constants, never a key taken from the request (input/fields.h).
namespace device_inputs {
inline constexpr std::string_view kAuthenticatedAt = "authenticated_at";
inline constexpr std::string_view kDeviceId = "device_id";
inline constexpr std::string_view kSuite = "suite";
inline constexpr std::string_view kAgreementKey = "agreement_key";
inline constexpr std::string_view kSigningKey = "signing_key";
inline constexpr std::string_view kSignedPrekey = "signed_prekey";
inline constexpr std::string_view kSignedPrekeySignature = "signed_prekey_signature";
inline constexpr std::string_view kLastResortKey = "last_resort_key";
inline constexpr std::string_view kLastResortSignature = "last_resort_signature";
inline constexpr std::string_view kApprover = "approver";
inline constexpr std::string_view kLinkSignature = "link_signature";
inline constexpr std::string_view kTimestamp = "timestamp";
// The set itself: full, already holds the id, or changed under the write.
inline constexpr std::string_view kDevices = "devices";
inline constexpr std::string_view kUsers = "users";
}  // namespace device_inputs

// --- bounds -------------------------------------------------------------------------

// anvil's ceiling on devices per account. A pairwise message is encrypted once
// per device of every member, so this multiplies every direct send, and the
// identity document is read whole on every claim. A deployment chooses below it.
inline constexpr std::uint32_t kMaxDevicesCeiling = 16;

// A primary authentication older than this does not admit a first device.
inline constexpr std::chrono::minutes kFreshAuthentication{5};
// A link message's timestamp may be this far from the server's clock either
// way. It bounds how long a captured link stays usable; `gone` covers the
// window itself.
inline constexpr std::chrono::seconds kLinkSkew{300};

// Days unseen before the idle sweeper unlinks a device (§7.3), unless the
// deployment passes its own.
inline constexpr std::uint32_t kDefaultIdleDays = 30;

struct DeviceConfig final {
    std::uint32_t max_devices = 5;
    // last_seen is rewritten at most this often per device. Idle is measured in
    // days, so an hour of resolution costs nothing and saves a write on almost
    // every request a device makes.
    std::chrono::minutes touch_interval{60};
};

[[nodiscard]] constexpr bool device_config_is_well_formed(const DeviceConfig& config) noexcept {
    return config.max_devices >= 1 && config.max_devices <= kMaxDevicesCeiling &&
           config.touch_interval.count() >= 1;
}

static_assert(device_config_is_well_formed(DeviceConfig{}));

// --- what a device is ---------------------------------------------------------------

// STORED, so append only; 0 is reserved so a zeroed byte is never a suite. A
// second suite (MLS, a post-quantum X3DH) is an addition here, not a migration
// (§7.2).
enum class Suite : std::uint8_t {
    Reserved = 0,
    // X25519 agreement, Ed25519 signatures, Signal-shaped X3DH and Double
    // Ratchet, sender keys for groups.
    SignalX25519Ed25519 = 1,
};

inline constexpr Suite kMaxSuite = Suite::SignalX25519Ed25519;

// The public half of one device's key store, as the client uploads it. Every
// member is a byte array, so there is no padding to order away.
struct DeviceKeys final {
    crypto::Ed25519Signature signed_prekey_signature;  // 64
    crypto::Ed25519Signature last_resort_signature;    // 64
    crypto::X25519PublicKey agreement;                 // 32
    crypto::Ed25519PublicKey signing;                  // 32
    crypto::X25519PublicKey signed_prekey;             // 32
    crypto::X25519PublicKey last_resort;               // 32
    Suite suite;                                       //  1
};

static_assert(sizeof(DeviceKeys) == 257);

// A replacement prekey and its signature by the device's own signing key, over
// prekey_message with the domain of the key it replaces.
struct SignedPrekey final {
    crypto::Ed25519Signature signature;  // 64
    crypto::X25519PublicKey key;         // 32
};

// A device being registered or linked.
struct NewDevice final {
    DeviceKeys keys;
    // Proposed by the CLIENT: 16 bytes from its CSPRNG, because the approving
    // device signs over it before the server has seen the request. Nil is
    // refused. Unique across every account (the catalogue's unique index on
    // identity_fields::kDeviceIdPath), so a second account cannot claim it.
    Uuid id;
    // The session making the request, so revoking it unlinks this device.
    Uuid session;
};

// Who admitted a device, and the signature that proves it.
struct LinkProof final {
    crypto::Ed25519Signature signature;
    std::uint64_t timestamp_s;
    Uuid approver;
};

// What any member of a conversation may know about a device: enough to encrypt
// to it and to verify the chain that admitted it. Not when it was last seen,
// which is presence, and not the session that registered it.
struct PublishedDevice final {
    std::optional<LinkProof> link;
    db::TimeMs linked_at;
    DeviceKeys keys;
    Uuid id;
};

struct AccountDevices final {
    std::vector<PublishedDevice> devices;
    std::int64_t device_set_version;
    Uuid user;
};

// Everything the identity document holds about one device.
struct DeviceRecord final {
    PublishedDevice published;
    db::TimeMs last_seen;
    Uuid session;
    bool low;
};

struct IdentityRecord final {
    std::vector<DeviceRecord> devices;
    std::optional<db::TimeMs> pending_since;
    std::int64_t device_set_version;
    Uuid user;
};

struct UnlinkedDevice final {
    Uuid user;
    Uuid device;
};

// One current device and whose it is: what an encrypted send's per-device map
// is checked against, without the keys a full listing carries.
struct DeviceOwner final {
    Uuid device;
    Uuid user;
};

// An account whose last device change has not yet reached its conversations.
struct PendingChange final {
    db::TimeMs since;
    Uuid       user;
};

// --- the signed messages ----------------------------------------------------------

inline constexpr std::string_view kSignedPrekeyDomain = "anvil-chat-spk";
inline constexpr std::string_view kLastResortDomain = "anvil-chat-lrk";
inline constexpr std::string_view kLinkDomain = "anvil-chat-link";

inline constexpr std::size_t kPrekeyMessageBytes = 14 + 16 + 32;
inline constexpr std::size_t kLinkMessageBytes = 15 + 16 + 16 + 32 + 32 + 8;
static_assert(kSignedPrekeyDomain.size() == 14 && kLastResortDomain.size() == 14 &&
              kLinkDomain.size() == 15);

using PrekeyMessage = std::array<std::uint8_t, kPrekeyMessageBytes>;
using LinkMessage = std::array<std::uint8_t, kLinkMessageBytes>;

// The bytes a device signs over its own signed prekey or last-resort key. The
// domain is kSignedPrekeyDomain or kLastResortDomain.
[[nodiscard]] PrekeyMessage prekey_message(std::string_view domain,
                                           const Uuid& device,
                                           const crypto::X25519PublicKey& key) noexcept;

// The bytes an approving device signs to admit `device` to `account`.
[[nodiscard]] LinkMessage link_message(const Uuid& account,
                                       const Uuid& device,
                                       const crypto::X25519PublicKey& agreement,
                                       const crypto::Ed25519PublicKey& signing,
                                       std::uint64_t timestamp_s) noexcept;

// Every key checked as bytes: the suite is one this build knows, the X25519 keys
// are canonical and not low order, the Ed25519 key is canonical and not small
// order, and both prekey signatures verify under that key over their messages.
// ValidationFailed names the first key that failed (device_inputs). The server
// cannot judge whether a key is GOOD; a malformed one accepted here is a key
// some other client chokes on later.
[[nodiscard]] Status validate_device(const Uuid& device, const DeviceKeys& keys) noexcept;

// --- the directory ------------------------------------------------------------------

// The application's collection names (docs/01-seams.md §4). Views into its
// constexpr table, so they outlive every directory built from them.
struct DeviceCollections final {
    std::string_view identities;
    // One-time prekeys (anvil/chat/prekeys.h).
    std::string_view prekeys;
    // Per-device ciphertexts (anvil/chat/device_queue.h).
    std::string_view queue;
    // The link relay's mailbox (§7.3.1): a new device's request and its
    // approval, minutes long. Declare it with `exp` as its lifetime.
    std::string_view links;
};

// --- the link relay ----------------------------------------------------------------
//
// A new device posts its own link, because a device belongs to the session
// that posts it, so the approver's signature has to travel back to it — and a
// desktop browser usually cannot scan a code off a phone. The relay is a
// mailbox: the new device leaves its public keys under a token it shows the
// approver, the approver (same account, another session) reads them by the
// token and leaves its signature, and the new device collects it once.
//
// Only PUBLIC material is held, so a database dump of the mailbox is a list of
// public keys and signatures any member could have been shown anyway. Only
// `SHA-256(token ‖ pepper)` is stored, as for invites (CLAUDE.md §5).

namespace link_fields {
inline constexpr std::string_view kId = "_id";              // the token's digest
inline constexpr std::string_view kUser = "u";
inline constexpr std::string_view kSession = "sid";         // the requesting session
inline constexpr std::string_view kDevice = "d";
inline constexpr std::string_view kAgreementKey = "ak";
inline constexpr std::string_view kSigningKey = "sk";
inline constexpr std::string_view kExpiresAt = "exp";
// All three OMITTED until the approval.
inline constexpr std::string_view kApprover = "by";
inline constexpr std::string_view kTimestamp = "ts";
inline constexpr std::string_view kSignature = "sig";
}  // namespace link_fields

// How long a request waits for its approval. Long enough to walk to another
// device; a link message's own timestamp is good for kLinkSkew either way.
inline constexpr std::chrono::seconds kLinkRequestLifetime{600};

// One request in the mailbox.
struct LinkRequest final {
    std::optional<LinkProof>  approval;
    db::TimeMs                expires_at;
    crypto::X25519PublicKey   agreement;
    crypto::Ed25519PublicKey  signing;
    Uuid                      device;
    Uuid                      user;
    Uuid                      session;
};

// Every method is synchronous and runs on db_pool (CLAUDE.md §4). None takes a
// session: each operation is ONE conditional write on the account's document,
// which is atomic without a transaction.
class DeviceDirectory final {
public:
    // `config.max_devices` is clamped into [1, kMaxDevicesCeiling]; an
    // application asserts device_config_is_well_formed on its own table so a
    // clamped value is a compile error there rather than a surprise here.
    DeviceDirectory(const db::DatabaseNames& databases,
                    const DeviceCollections& collections,
                    const DeviceConfig& config);

    // The first device of an account, or the first after every device was
    // unlinked (the reset in §7.3; the conversations' "security code changed"
    // follows from dv moving, in the propagation row).
    //
    //   CapabilityRequired  `authenticated_at` is not within
    //                    kFreshAuthentication of `now` (or more than a minute in
    //                    its future). Not Unauthenticated: the session is fine,
    //                    and a 401 is what a client refreshes its token for.
    //   ValidationFailed nil id, or a malformed key (validate_device)
    //   Conflict         the account already has a device, the id is linked
    //                    anywhere, or it was recently unlinked from this account
    //
    // ONE conditional upsert whose filter requires the account to have no
    // device. Two first registrations racing both reach the server; one inserts
    // or matches, and the other's upsert collides on _id and is refused there.
    [[nodiscard]] Status register_first_device(mongocxx::client& client,
                                               const Uuid& user,
                                               const NewDevice& device,
                                               db::TimeMs authenticated_at,
                                               db::TimeMs now) const;

    // A later device, admitted by `approver`'s signature over link_message.
    //
    //   ValidationFailed nil id, a malformed key, or a timestamp more than
    //                    kLinkSkew from `now`
    //   Forbidden        `approver` is not a device of this account, or the
    //                    signature does not verify under its stored key
    //   Conflict         the account is full, the id is already linked anywhere
    //                    or was recently unlinked here (a replay), or the set
    //                    changed between the read and the write
    //
    // The approver's key is READ to verify the signature, and then every
    // condition the read established — the approver still linked with that very
    // key, the id absent, the set below max — is restated in the filter of the
    // one write. A device unlinked between the two cannot admit anybody.
    [[nodiscard]] Status link_device(mongocxx::client& client,
                                     const Uuid& user,
                                     const Uuid& approver,
                                     const NewDevice& device,
                                     std::uint64_t timestamp_s,
                                     const crypto::Ed25519Signature& link_signature,
                                     db::TimeMs now) const;

    // Removes the device, bumps dv, marks the change pending and remembers the
    // id in `gone`. False, and nothing written, when it was not linked: unlink
    // is idempotent. The caller then discards the device's one-time prekeys
    // (PrekeyDirectory::discard), which prekeys.h explains may run late.
    [[nodiscard]] Result<bool> unlink_device(mongocxx::client& client,
                                             const Uuid& user,
                                             const Uuid& device,
                                             db::TimeMs now) const;

    // Replaces the device's signed prekey, its last-resort key, or both, each
    // verified under the signing key stored for the device, over the same
    // signed messages registration checks (§7.3.1). At least one.
    //
    //   ValidationFailed  neither given (signed_prekey, Required), a malformed
    //                     key, or a signature that does not verify (each named)
    //   Forbidden         `device` is not a current device of `user` (device_id)
    //
    // ONE write whose filter restates the signing key that verified, so a
    // device unlinked or replaced between the read and the write changes
    // nothing. Neither dv nor any conversation's dsv moves: the set of devices
    // is the same, and raising the fence would refuse every sender in every
    // conversation of the account once for a key nobody encrypts a message to.
    // No previous key is kept: a session started from the old one is the
    // DEVICE's to finish, from the private half its own store keeps for the
    // overlap, and the server never held that half.
    [[nodiscard]] Status rotate_prekeys(mongocxx::client& client,
                                        const Uuid& user,
                                        const Uuid& device,
                                        const std::optional<SignedPrekey>& signed_prekey,
                                        const std::optional<SignedPrekey>& last_resort) const;

    // --- the link relay ---
    //
    // A request for `session`, replacing any that session left before: one
    // pending request per session, so a mailbox holds at most one row per
    // session for kLinkRequestLifetime.
    [[nodiscard]] Status put_link_request(mongocxx::client& client,
                                          const crypto::Digest256& digest,
                                          const LinkRequest& request) const;

    // The live request under `digest` for an account, or nullopt for anything
    // else: unknown, expired and another account's alike.
    [[nodiscard]] Result<std::optional<LinkRequest>> find_link_request(
        mongocxx::client& client, const crypto::Digest256& digest, const Uuid& user,
        db::TimeMs now) const;

    // Writes the approval, only while there is none and the request is live.
    // False when nothing matched.
    [[nodiscard]] Result<bool> approve_link_request(mongocxx::client& client,
                                                    const crypto::Digest256& digest,
                                                    const Uuid& user, const LinkProof& proof,
                                                    db::TimeMs now) const;

    // The approved request, removed in the same operation, for the session
    // that asked; nullopt when there is none approved for it.
    [[nodiscard]] Result<std::optional<LinkRequest>> collect_link_approval(
        mongocxx::client& client, const crypto::Digest256& digest, const Uuid& user,
        const Uuid& session, db::TimeMs now) const;

    // The device `session` registered, unlinked; nullopt when there was none.
    // The hook a session revocation calls (§7.3).
    [[nodiscard]] Result<std::optional<Uuid>> unlink_session(mongocxx::client& client,
                                                             const Uuid& user,
                                                             const Uuid& session,
                                                             db::TimeMs now) const;

    // The published devices of each account, in one $in. Refused when `users`
    // holds more than `limit` ids; an account with no document is absent from
    // the answer, which is in no particular order.
    [[nodiscard]] Result<std::vector<AccountDevices>> devices_of(mongocxx::client& client,
                                                                 std::span<const Uuid> users,
                                                                 std::int32_t limit) const;

    // Every current device of each account, projected to its id, in one $in.
    // Refused when `users` holds more than `limit` ids. The read an encrypted
    // send's per-device map is checked against: at a group's thousand members it
    // is a thousand small documents, not a thousand bundles of keys.
    [[nodiscard]] Result<std::vector<DeviceOwner>> device_owners(mongocxx::client& client,
                                                                 std::span<const Uuid> users,
                                                                 std::int32_t limit) const;

    // The whole document, private fields included. For the service and tests.
    [[nodiscard]] Result<std::optional<IdentityRecord>> identity(mongocxx::client& client,
                                                                 const Uuid& user) const;

    // Records that `device` was seen at `now`, COALESCED: the write matches only
    // when the stored last_seen is older than touch_interval, so a busy device
    // costs one write an hour rather than one per request. True when written.
    [[nodiscard]] Result<bool> touch(mongocxx::client& client,
                                     const Uuid& user,
                                     const Uuid& device,
                                     db::TimeMs now) const;

    // The idle sweeper: devices unseen for `idle_days`, from at most `batch`
    // accounts, each unlinked by its own conditional update that restates the
    // idleness, so a device touched after the scan survives it. Answers what
    // was unlinked, so the caller can discard their prekeys.
    [[nodiscard]] Result<std::vector<UnlinkedDevice>> unlink_idle(mongocxx::client& client,
                                                                  db::TimeMs now,
                                                                  std::uint32_t idle_days,
                                                                  std::int32_t batch) const;

    // Accounts whose change has been pending since before `before`, at most
    // `limit`, oldest first. The sweeper's scan (§7.4).
    [[nodiscard]] Result<std::vector<PendingChange>> pending_changes(mongocxx::client& client,
                                                                     db::TimeMs before,
                                                                     std::int32_t limit) const;

    // Clears the pending mark, ONLY while it still says `since`. A change made
    // during the propagation has $max-ed it later, and clearing that would
    // drop a change nobody pushed. True when cleared.
    [[nodiscard]] Result<bool> clear_pending(mongocxx::client& client,
                                             const Uuid& user,
                                             db::TimeMs since) const;

private:
    [[nodiscard]] mongocxx::collection identities(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection links(mongocxx::client& client) const;

    std::string database_;
    std::string links_database_;
    std::string_view links_;
    // "dev.<max_devices - 1>": a filter that this path does not exist is "the
    // set has room for one more", evaluated by the server inside the write.
    std::string full_path_;
    std::string_view collection_;
    DeviceConfig config_;
};

}  // namespace anvil::chat
