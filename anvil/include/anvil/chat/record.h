#pragma once

// What a conversation, a membership and a message are in the database
// (docs/22-chat.md §3.1, §4).
//
// Field names are published here, as the media and entries field names are, so
// an application's index catalogue is written against the names this library
// actually writes. An index over a column anvil does not write is an index the
// planner never uses, and the symptom is a COLLSCAN on the send path rather
// than a compile error.
//
// --- the one rule everything else is a comparison against --------------------
//
// A conversation is an ordered log with one writer of order: its `seq`. A
// member may see messages with `js <= s < ls` (ls absent while they are still a
// member). Receipts, unread counts, sync, history visibility and expiry are all
// comparisons against those numbers, so there is no second rule for "can this
// user see message N" for two code paths to disagree about.
//
// Sequence numbers are MONOTONIC, NOT DENSE. The number is allocated by one
// find_one_and_update outside any transaction (§4.1), so a crash between that
// and the insert burns one. A client learns what it missed by asking for
// `seq > cursor`, never by looking for a hole.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/chat/kind_spec.h"
#include "anvil/chat/text.h"
#include "anvil/core/types.h"
#include "anvil/crypto/digest.h"
#include "anvil/db/codec.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/sniff.h"

namespace anvil::chat {

// --- field names ------------------------------------------------------------------

namespace conversation_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kKind = "k";
// Whether the conversation is encrypted. Written once, at creation, and never
// in any update: a mode that can change later is a downgrade switch.
inline constexpr std::string_view kEncrypted = "e";
inline constexpr std::string_view kSeq = "seq";
// The membership version, bumped in every membership transaction. A send reads
// it back with the seq it allocates, which is what validates a per-process
// member cache without a second round trip (docs/22 §5.4).
inline constexpr std::string_view kMembershipVersion = "mv";
// The device-set version (docs/22 §7.4), $max-ed when a member's devices change.
inline constexpr std::string_view kDeviceSetVersion = "dsv";
inline constexpr std::string_view kTitle = "t";
inline constexpr std::string_view kDescription = "d";
inline constexpr std::string_view kIcon = "ic";
inline constexpr std::string_view kTimer = "tm";
inline constexpr std::string_view kCreatedAt = "at";
inline constexpr std::string_view kCreatedBy = "by";
// The direct-pair key (docs/22 §3.2): SHA-256 over the two ids in order and the
// encryption bit. OMITTED on every other shape, because its unique index is
// partial on the field existing.
inline constexpr std::string_view kDirectPair = "dpk";
// The mutation counter (docs/22 §4.5): $inc-ed by every edit, revoke and
// reaction change, inside the transaction that makes the change, and stamped
// on the changed message as message_fields::kMutation. Absent on a
// conversation nothing has mutated yet, which reads as zero.
inline constexpr std::string_view kMutations = "mut";
// 16 bytes the creating client minted before its first attempt, so a retried
// create finds the first conversation (docs/22 §3.1): with kCreatedBy, unique.
// OMITTED on a direct conversation and on one created without a key, because
// the unique index is partial on it existing.
inline constexpr std::string_view kClientId = "cid";
}  // namespace conversation_fields

namespace member_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kConversation = "c";
inline constexpr std::string_view kUser = "u";
inline constexpr std::string_view kRole = "r";
inline constexpr std::string_view kJoinedSeq = "js";
// OMITTED while the person is a member. Present after they leave or are
// removed, so they can still read up to it.
inline constexpr std::string_view kLeftSeq = "ls";
inline constexpr std::string_view kDelivered = "dlv";
inline constexpr std::string_view kRead = "rd";
// Whether this member's read watermark is shown to anybody else (§5.1). Their
// own unread count reads it either way.
inline constexpr std::string_view kReadPrivate = "rp";
// The chat list's order: the last activity, coalesced (docs/22 §5.3).
inline constexpr std::string_view kActivity = "act";
inline constexpr std::string_view kMutedUntil = "mu";
// OMITTED unless pinned, so the pin index holds only pinned rows.
inline constexpr std::string_view kPinned = "pin";
inline constexpr std::string_view kArchived = "ar";
// "Clear chat": nothing at or below this seq is shown to this member again.
inline constexpr std::string_view kHideBefore = "hb";
// The conversation's mutation counter when this member left, written with
// `ls` and cleared with it: a past member catches up mutations to here and no
// further, as they read messages to `ls` and no further.
inline constexpr std::string_view kLeftMutations = "lmu";
}  // namespace member_fields

namespace message_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kConversation = "c";
inline constexpr std::string_view kSeq = "s";
inline constexpr std::string_view kSender = "u";
// 16 bytes the client minted before the first attempt; with c and u, the
// idempotency key that survives a phone queuing a message for days (§4.2).
inline constexpr std::string_view kClientId = "cid";
inline constexpr std::string_view kKind = "k";
inline constexpr std::string_view kBody = "b";
inline constexpr std::string_view kAttachments = "att";
inline constexpr std::string_view kMentions = "mn";
inline constexpr std::string_view kPreview = "pv";
inline constexpr std::string_view kSystem = "sys";
inline constexpr std::string_view kRef = "ref";
inline constexpr std::string_view kRevoked = "rv";
inline constexpr std::string_view kEdits = "ed";
inline constexpr std::string_view kEditedAt = "eat";
inline constexpr std::string_view kSentAt = "at";
// OMITTED on a message with no timer, because the sweeper's index is partial on
// it existing. NOT a TTL index: the monitor would delete a row without
// releasing its attachments' references (docs/22 §4.7).
inline constexpr std::string_view kExpiresAt = "exp";
// Present, true, on a direct message sent to somebody who had blocked the
// sender: stored and shown to the sender, never to the person who blocked them
// (docs/22 §3.6). OMITTED otherwise.
inline constexpr std::string_view kHiddenFromPeer = "hid";
// An application message kind's code and its canonical body (chat/card_spec.h).
// Both OMITTED on every other message.
inline constexpr std::string_view kCardCode = "cc";
inline constexpr std::string_view kCardBody = "cd";
// An encrypted message's COMMON ciphertext (docs/22 §7.6), opaque. OMITTED when
// the send carried only per-device ciphertexts, which are queue rows
// (chat/device_queue.h) and never the message's.
inline constexpr std::string_view kCiphertext = "ct";
// The device an encrypted message was sent from, so a recipient knows which
// session decrypts it. Encrypted messages only.
inline constexpr std::string_view kSenderDevice = "sd";
// The conversation's mutation counter at this message's last edit, revoke or
// reaction change (docs/22 §4.5). OMITTED on a message never mutated, so the
// catch-up index ({c, mu}, partial on mu existing) holds only mutated rows.
inline constexpr std::string_view kMutation = "mu";

// Sub-keys of one attachment.
inline constexpr std::string_view kAttMedia = "m";
inline constexpr std::string_view kAttNamespace = "n";
inline constexpr std::string_view kAttMime = "t";
inline constexpr std::string_view kAttWidth = "w";
inline constexpr std::string_view kAttHeight = "h";
inline constexpr std::string_view kAttDuration = "dur";
inline constexpr std::string_view kAttName = "nm";
// Sub-keys of one mention.
inline constexpr std::string_view kMentionUser = "u";
inline constexpr std::string_view kMentionOffset = "o";
inline constexpr std::string_view kMentionLength = "l";
// Sub-keys of a preview.
inline constexpr std::string_view kPreviewUrl = "url";
inline constexpr std::string_view kPreviewTitle = "t";
inline constexpr std::string_view kPreviewDescription = "d";
// Sub-keys of a system event.
inline constexpr std::string_view kEvent = "ev";
inline constexpr std::string_view kEventSubject = "su";
inline constexpr std::string_view kEventRole = "r";
inline constexpr std::string_view kEventTimer = "tm";
}  // namespace message_fields

namespace reaction_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kConversation = "c";
inline constexpr std::string_view kSeq = "s";
inline constexpr std::string_view kUser = "u";
inline constexpr std::string_view kReaction = "e";
}  // namespace reaction_fields

namespace invite_fields {
// The digest IS the key: only `SHA-256(token ‖ pepper)` is stored (CLAUDE.md §5).
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kConversation = "c";
inline constexpr std::string_view kUses = "n";
inline constexpr std::string_view kCap = "cap";
inline constexpr std::string_view kExpiresAt = "exp";
inline constexpr std::string_view kCreatedBy = "by";
}  // namespace invite_fields

// A member's report of a message or a range of them, for staff to review
// (docs/22 §9.2). The reporter's own note is the only text, and it is theirs.
namespace report_fields {
inline constexpr std::string_view kId = "_id";            // uuid v7: listing order
inline constexpr std::string_view kConversation = "c";
inline constexpr std::string_view kReporter = "by";
inline constexpr std::string_view kFrom = "f";            // first seq, inclusive
inline constexpr std::string_view kTo = "to";             // last seq, inclusive
inline constexpr std::string_view kNote = "n";            // OMITTED when empty
inline constexpr std::string_view kAt = "at";
}  // namespace report_fields

namespace block_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kBlocker = "u";
inline constexpr std::string_view kBlocked = "b";
}  // namespace block_fields

// The bounds the rows carry are in chat/text.h, with the other bounds a client
// enforces before it sends, so the descriptor can publish them from the
// foundation without reaching the BSON codec this header includes.

// --- what a message is -----------------------------------------------------------

// STORED, so append only.
enum class MessageKind : std::uint8_t {
    Text = 0,
    // A membership or settings change, written in the same transaction as the
    // change itself so it holds a position in the same order (§3.4).
    System = 1,
    // An application message kind: a poll, a location, a contact card.
    Card = 2,
    // Ciphertext the server cannot read (§7.6). Text, a card, a reply, a
    // reaction or an edit inside an encrypted conversation is all this kind: the
    // distinction is inside the ciphertext.
    Encrypted = 3,
};

inline constexpr MessageKind kMaxMessageKind = MessageKind::Encrypted;

struct CardRecord final {
    std::string   body;   // canonical JSON, as the binder produced and anvil re-checked
    std::uint8_t  code;
};

// STORED, so append only.
enum class SystemEvent : std::uint8_t {
    Created = 0,
    MemberAdded = 1,
    MemberRemoved = 2,
    MemberLeft = 3,
    RoleChanged = 4,
    InfoChanged = 5,
    TimerChanged = 6,
    JoinedByInvite = 7,
    // The last owner left and the longest-standing admin or member was
    // promoted, in the same transaction (§3.4).
    OwnerSucceeded = 8,
    // The subject linked or unlinked a device, in an ENCRYPTED DIRECT
    // conversation only (§7.4): the other person's client shows "security code
    // changed" at this point in the log, which is what makes a device a
    // malicious operator added visible (§7.1). Its sender is the subject.
    DevicesChanged = 9,
};

inline constexpr SystemEvent kMaxSystemEvent = SystemEvent::DevicesChanged;

struct SystemRecord final {
    std::optional<Uuid> subject;   // whom the event is about, when it is about somebody
    std::uint32_t       timer_s;   // TimerChanged only
    SystemEvent         event;
    Role                role;      // RoleChanged and OwnerSucceeded only
};

// One attachment as the row stores it. The id never leaves the server: a client
// is handed a grant per attachment, minted after its own visibility check
// (docs/22 §6.1, §6.2). Width, height, duration and name are what the SENDING
// client said about its own file, validated as untrusted input; nothing here is
// derived from storage except the type.
struct AttachmentRecord final {
    std::string   name;
    Uuid          media;
    std::uint32_t duration_ms;
    std::uint16_t width;
    std::uint16_t height;
    fs::Ns        ns;
    fs::Mime      mime;
};

struct PreviewRecord final {
    std::string url;
    std::string title;
    std::string description;
};

struct MessageRecord final {
    std::string                   body;
    // The common ciphertext of an Encrypted message; empty otherwise, and empty
    // on one that carried only per-device ciphertexts.
    std::vector<std::uint8_t>     ciphertext;
    std::vector<AttachmentRecord> attachments;
    std::vector<MentionSpan>      mentions;
    std::optional<PreviewRecord>  preview;
    std::optional<SystemRecord>   system;
    std::optional<CardRecord>     card;
    std::optional<std::int64_t>   ref;
    std::optional<db::TimeMs>     edited_at;
    std::optional<db::TimeMs>     expires_at;
    // The sending device of an Encrypted message.
    std::optional<Uuid>           sender_device;
    db::TimeMs                    sent_at;
    std::int64_t                  seq;
    Uuid                          id;
    // Read back from the row; ignored on insert, where the caller names it.
    Uuid                          conversation;
    Uuid                          sender;
    std::array<std::uint8_t, 16>  client_id;
    std::int32_t                  edits;
    // The conversation's mutation counter when this message last changed, or
    // zero when it never has (§4.5).
    std::int64_t                  mutation;
    MessageKind                   kind;
    // Content and attachments gone, row and seq kept, so every client that has
    // the message can be told it was taken back (§4.5).
    bool                          revoked;
    bool                          hidden_from_peer;

    // Whether `viewer` is shown this message at all: everything, except a
    // direct message sent past a block, which only its sender sees.
    [[nodiscard]] bool shown_to(const Uuid& viewer) const noexcept {
        return !hidden_from_peer || viewer == sender;
    }
};

// --- what a conversation and a membership are -------------------------------------

struct ConversationRecord final {
    std::string                      title;
    std::string                      description;
    std::optional<Uuid>              icon;
    std::optional<crypto::Digest256> direct_pair;
    // The creating client's idempotency key, when it sent one.
    std::optional<std::array<std::uint8_t, 16>> client_id;
    db::TimeMs                       created_at;
    std::int64_t                     seq;
    std::int64_t                     membership_version;
    std::int64_t                     device_set_version;
    // Edits, revokes and reaction changes so far (§4.5).
    std::int64_t                     mutations;
    Uuid                             id;
    Uuid                             created_by;
    std::uint32_t                    timer_s;
    KindCode                         kind;
    bool                             encrypted;
};

struct MemberRecord final {
    std::optional<std::int64_t> left_seq;
    // Present exactly when left_seq is.
    std::optional<std::int64_t> left_mutations;
    std::optional<db::TimeMs>   muted_until;
    db::TimeMs                  activity;
    std::int64_t                joined_seq;
    std::int64_t                delivered;
    std::int64_t                read;
    std::int64_t                hide_before;
    Uuid                        id;
    Uuid                        conversation;
    Uuid                        user;
    Role                        role;
    bool                        read_private;
    bool                        pinned;
    bool                        archived;

    [[nodiscard]] constexpr bool current() const noexcept { return !left_seq.has_value(); }

    // The one visibility rule (see the header comment).
    [[nodiscard]] constexpr bool can_see(std::int64_t seq) const noexcept {
        return seq >= joined_seq && seq > hide_before && (!left_seq.has_value() || seq < *left_seq);
    }
};

// What allocating a sequence number reads back, in the same operation.
struct Allocation final {
    std::int64_t  seq;
    std::int64_t  membership_version;
    std::int64_t  device_set_version;
    std::int64_t  mutations;
    std::uint32_t timer_s;
    KindCode      kind;
    bool          encrypted;
};

struct ReportRecord final {
    std::string  note;
    db::TimeMs   at;
    std::int64_t from;
    std::int64_t to;
    Uuid         id;
    Uuid         conversation;
    Uuid         reporter;
};

// The direct-pair key (docs/22 §3.2). The two ids in byte order, so A→B and B→A
// are one key, and the encryption bit, so one pair can hold one conversation of
// each mode and the two can never be merged into a mode switch.
[[nodiscard]] crypto::Digest256 direct_pair_key(const Uuid& a, const Uuid& b, bool encrypted);

}  // namespace anvil::chat
