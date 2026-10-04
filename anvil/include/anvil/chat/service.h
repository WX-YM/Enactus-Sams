#pragma once

// The chat service: every rule in docs/22-chat.md that is not a query.
//
// Synchronous, and called on db_pool only: every method blocks on the driver,
// and a loop thread may never reach here (CLAUDE.md §4). The handlers in
// chat/routes.h post here; nothing else should need to.
//
// --- membership changes are messages ----------------------------------------
//
// Adding, removing, leaving, promoting, renaming and setting a timer each
// write a SYSTEM message in the same transaction as the change, so every
// client sees the change at the same position in the log as everything else
// (docs/22 §3.4). Each of those transactions also $incs the conversation's
// seq and membership version, so two membership changes to one conversation
// conflict and serialise: the actor's own rights and the member count are
// therefore read INSIDE the transaction and are exact, not a snapshot a
// concurrent removal could make stale.
//
// --- every refusal about a conversation is the same NotFound ------------------
//
// Not a member, never a member, no such conversation: all three are the same
// filter (a membership row that is not there) and all three answer NotFound,
// which the route turns into the stealth 404. A member who lacks a RIGHT is
// told Forbidden, because a member already knows the conversation exists.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/audit/action.h"
#include "anvil/chat/card_spec.h"
#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/prekeys.h"
#include "anvil/chat/kind_spec.h"
#include "anvil/chat/presence.h"
#include "anvil/chat/record.h"
#include "anvil/chat/repository.h"
#include "anvil/chat/text.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/ed25519.h"
#include "anvil/crypto/secret.h"
#include "anvil/media/grant.h"
#include "anvil/media/service.h"

namespace anvil::audit {
class AuditService;
}  // namespace anvil::audit

namespace anvil::chat {

class ChatLive;
class ChatPush;
class DeviceDirectory;
class DeviceQueue;
class PrekeyDirectory;

// Who is acting: the fields of UserContext the service reads.
struct Actor final {
    Uuid    user;
    PermSet permissions;
    // The session the request came in on. A device records the session that
    // registered it, so ending that session can end the device (§7.3).
    Uuid    session{};
};

// One membership change, reported after its transaction committed.
struct MembershipEvent final {
    Uuid         conversation;
    Uuid         actor;
    Uuid         subject;
    std::int64_t seq;
    SystemEvent  event;
};

// One message, reported after it was stored.
struct MessageEvent final {
    Uuid         conversation;
    Uuid         sender;
    std::int64_t seq;
    KindCode     kind;
    MessageKind  message_kind;
};

// What happened to a device.
enum class DeviceChange : std::uint8_t { Registered, Linked, Unlinked };

// One device change, reported after it committed. The session is nil for a
// device the idle sweeper unlinked.
struct DeviceEvent final {
    Uuid         user;
    Uuid         device;
    Uuid         session;
    DeviceChange change;
};

// One report filed, reported after it was stored. Not called for a retry
// answered with the report already there.
struct ReportEvent final {
    std::int64_t from;
    std::int64_t to;
    Uuid         report;
    Uuid         conversation;
    Uuid         reporter;
};

struct ChatHooks final {
    // Whether `actor` may reach `target`: open a direct conversation with them,
    // or add them to one. Asked after blocks, which refuse first. The
    // application's notion of a contact, a message request or "only people who
    // share a group" lives here (docs/22 §3.2).
    //
    // UNSET REFUSES EVERY REACH. A missing policy is not a fact a static_assert
    // can see, and the failure it would otherwise produce — anybody messaging
    // anybody — is the abuse a messenger most often gets.
    std::function<bool(mongocxx::client&, const Uuid& actor, const Uuid& target)> may_reach;

    // After the commit, on the thread that made the change. Must not block:
    // AuditService::write_async is the call. How a membership change reaches the
    // application's audit log, since the handler is anvil's.
    std::function<void(const MembershipEvent&)> on_membership;

    // At the commit, before the message's wakes go out, so what an application
    // records is when the message was stored. Must not block: the wakes wait
    // on it. Messages are NOT audited by anvil: the volume is too high and the
    // content is private. An application with a duty to retain has this and
    // can do so knowingly (docs/22 §9).
    std::function<void(const MessageEvent&)> on_message;

    // Whether this send should bump every member's chat-list activity, or a
    // recent send already did (docs/22 §5.3). The coalescing gate: a Redis
    // `SET NX` with a short expiry per conversation is the intended one
    // (chat::redis_activity_gate). Unset bumps on every send, which is correct
    // and only costs writes; a gate that cannot reach Redis must answer true —
    // a chat list that stops reordering is a visible fault, a write
    // amplification for the length of an outage is not.
    std::function<bool(const Uuid& conversation)> claim_activity_bump;

    // When `user` last proved who they are with a primary credential (a
    // password or a passkey) on `session`, or nullopt. Asked when a FIRST
    // device is registered, which needs one inside the last five minutes: a
    // session alone is a cookie, and a stolen cookie on an account that never
    // used encryption would otherwise register the first device and own every
    // later one (docs/22 §7.3). The accounts module is the application's, so
    // the answer is too. UNSET REFUSES EVERY FIRST DEVICE.
    std::function<std::optional<db::TimeMs>(mongocxx::client&, const Uuid& user,
                                            const Uuid& session)>
        authenticated_at;

    // After a device change committed, on the thread that made it. Must not
    // block: AuditService::write_async is the call. A device change is the
    // event an account's security log most needs.
    std::function<void(const DeviceEvent&)> on_device;

    // After a member's report committed, on the thread that made it. Must not
    // block. Where an application tells its staff, opens a case, or decides
    // automatically (docs/22 §9.2): anvil stores the report and lists it, and
    // what a report MEANS is the product's.
    std::function<void(const ReportEvent&)> on_report;
};

// How a staff read of a conversation is recorded (docs/22 §9.2). The action is
// the application's, from its own audit table; anvil names none.
struct ChatReview final {
    // Every staff read is written here SYNCHRONOUSLY, before anything is shown:
    // a read whose record could not be written is refused, because reading
    // somebody's private conversation is exactly the act an audit log exists
    // to hold. Null serves no staff read at all.
    audit::AuditService*              audit{nullptr};
    std::optional<audit::AuditAction> read_action{};
};

// A conversation as staff are shown it: the conversation, its current members
// a page at a time, and where to continue.
struct Review final {
    ConversationRecord          conversation;
    std::vector<MemberRecord>   members;
    std::optional<Uuid>         next;
};

// A report as filed: false when it was a retry answered with the first.
struct FiledReport final {
    ReportRecord report;
    bool         created;
};

// The most messages one report may name, and the longest note. A report points
// a reviewer at what happened; the reviewer reads the conversation around it.
inline constexpr std::int64_t kMaxReportRange = 500;
inline constexpr std::uint32_t kMaxReportNoteCodePoints = 1000;
inline constexpr std::int32_t kMaxReportPage = 100;

struct CreateConversation final {
    std::string_view     kind;
    std::string_view     title;
    std::string_view     description;
    // The people added at creation, besides the creator. Each must pass
    // may_reach; duplicates and the creator are ignored.
    std::span<const Uuid> members;
    // Honoured only when the kind's mode is Optional; Required forces it on and
    // Never forces it off. Fixed for the conversation's life.
    bool                 encrypted;
    // 16 bytes the client minted before its first attempt (docs/22 §3.1): a
    // retry with the same key is answered with the first conversation. The
    // route requires one; all zero is a caller that will never retry (a seed
    // at boot), and its create is not idempotent.
    std::array<std::uint8_t, 16> client_id{};
};


// The caller's view of one conversation.
struct ConversationState final {
    ConversationRecord conversation;
    MemberRecord       membership;
};

// A conversation as create answers it: false when this was a retry answered
// with the conversation the first attempt made.
struct CreatedConversation final {
    ConversationRecord conversation;
    MemberRecord       membership;
    bool               created;
};

// One attachment on a send: either a fresh upload, named by the handle its
// upload answered with, or a forward of an attachment the sender can already
// see, named by where it is. Never an object id: a client is never told one
// (docs/22 §6.2), and a send that accepted one would let anybody who learned
// an id attach it to a conversation with themselves and be granted it.
struct OutgoingAttachment final {
    // Exactly one of the two.
    std::string_view handle;
    struct Forward final {
        Uuid         conversation;
        std::int64_t seq;
        std::size_t  index;
    };
    std::optional<Forward> forward;
    // What the SENDING client says about its own file, validated as untrusted
    // input and never derived from storage (docs/22 §6.2). Ignored on a forward,
    // which carries the original's.
    std::string_view name;
    std::uint32_t    duration_ms;
    std::uint16_t    width;
    std::uint16_t    height;
};

// A plaintext message as the client sent it. Views into the request body: the
// handler keeps the request alive across the pool hop (CLAUDE.md §2.2).
struct SendMessage final {
    // 16 bytes the client minted before its first attempt (docs/22 §4.2).
    std::array<std::uint8_t, 16> client_id;
    std::string_view             body;
    std::span<const MentionSpan> mentions;
    std::optional<LinkPreview>   preview;
    // The seq this replies to, which must be inside the SENDER's own visible
    // range: a reply is otherwise a way to quote a message from before you
    // joined (docs/22 §4.5).
    std::optional<std::int64_t>  reply_to;
    std::span<const OutgoingAttachment> attachments{};
    // An application message kind and its body, bound by that kind's binder.
    // The body borrows from the request's parsed document, which the handler
    // keeps alive across the pool hop.
    struct Card final {
        std::string_view          kind;
        const input::JsonValue*   body;
    };
    std::optional<Card>          card{};
};

// One per-device ciphertext of an encrypted send: opaque bytes for exactly one
// device. A view into the request's decoded body.
struct DeviceCiphertext final {
    std::span<const std::uint8_t> ciphertext;
    Uuid                          device;
};

// An encrypted message as the client sent it (docs/22 §7.6). The server checks
// who, where and for which devices, and reads none of it.
struct EncryptedMessage final {
    std::array<std::uint8_t, 16>        client_id;
    // The COMMON ciphertext, one blob every member's device reads (a sender-key
    // group message). Empty when the send is per-device only.
    std::span<const std::uint8_t>       ciphertext;
    // The PER-DEVICE ciphertexts (pairwise: every direct message, and every
    // sender-key distribution). Unless `page`, the device ids must be EXACTLY
    // the current devices of the current members other than `device`.
    std::span<const DeviceCiphertext>   devices;
    // Sealed blobs, by the handle their upload answered or as a forward. The
    // file key, the hashes, a name and dimensions are inside the ciphertext.
    std::span<const OutgoingAttachment> attachments{};
    // The conversation's device-set version the sender encrypted against: the
    // fence (§7.4). A send against any other value is refused as stale.
    std::int64_t                        device_set_version;
    // The sending device, one of the actor's current devices.
    Uuid                                device;
    // A page of a sender-key distribution in a group too big for one request:
    // the map is then a SUBSET of the current devices, still with nothing
    // extra, and the fence is what catches a stale set. Groups only, per-device
    // ciphertexts only.
    bool                                page{false};
};

// One page of a conversation's members and their published devices: what a
// sender encrypts for, and what a 409 chat.devices_stale answers with.
struct ConversationDevices final {
    // In member order. A member with no device is present with none, so a
    // client can tell "has no device" from "was not on this page".
    std::vector<AccountDevices> members;
    // The user to pass as the next `after`, when there may be more.
    std::optional<Uuid>         next;
    // Read BEFORE the lists, so a version a client holds never claims a set
    // newer than the lists it came with: a change after the read moves it on.
    std::int64_t                device_set_version;
};

// Members per page of device lists. A page is at most 64 accounts of at most
// kMaxDevicesCeiling devices, which is what one response should carry.
inline constexpr std::int32_t kMaxDevicePage = 64;

// Accounts per prekey claim. Each is a per-target budget check and a
// find_one_and_delete per device, and the answer a bundle per device of each:
// at the reference five devices about 100 KiB, and at anvil's ceiling of
// sixteen about 320 KiB, which is where a response stops being one a phone
// should hold whole. A thousand-member group is thirty-two requests.
inline constexpr std::size_t kMaxClaimBatch = 32;

// One account's part of a batch claim: its bundles, or why there are none. A
// refusal is per account, because the keys already claimed for the others in
// the same request are spent and must reach the claimer.
struct AccountClaim final {
    std::vector<ClaimedBundle> bundles;
    // NotFound: not a current member of the conversation. RateLimited: its
    // per-target budget is spent for the window.
    std::optional<ErrorCode>   refused;
    Uuid                       user;
};

struct SentMessage final {
    db::TimeMs   sent_at;
    std::int64_t seq;
    // False when this was a retry of a message already stored, which is a
    // success like any other: a retry after a lost response lands here.
    bool         created;
};

// One row of a chat list, with the counts derived by subtraction (§5.2).
struct ChatListItem final {
    ConversationRecord conversation;
    MemberRecord       membership;
    // Messages past the member's read watermark, capped. Approximate by design:
    // it counts the reader's own messages and system messages, because counting
    // exactly needs a query per conversation per page, and the client corrects
    // it when the conversation is opened.
    std::int32_t       unread;
};

inline constexpr std::int32_t kUnreadCap = 999;

// Another member's receipts for one message: who holds it and who has read it.
struct Readers final {
    std::optional<std::vector<Uuid>> read_by;
    std::vector<Uuid>                delivered_to;
};

struct ChatList final {
    // The pinned conversations, on the first page only.
    std::vector<ChatListItem>                  pinned;
    std::vector<ChatListItem>                  items;
    std::optional<std::pair<db::TimeMs, Uuid>> next;
};

// How many conversations one person may pin.
inline constexpr std::int32_t kMaxPins = 5;

// The longest a mute may be asked for as a duration. A longer one is what
// mute_forever says.
inline constexpr std::chrono::seconds kMaxMuteDuration{366L * 24 * 3600};

// The instant a mute "until further notice" is stored as: the last
// millisecond of the year 9999, which every comparison with a real clock
// reads as still muted and every client renders as a date nobody lives to.
inline constexpr db::TimeMs kMutedIndefinitely{std::chrono::milliseconds{253'402'300'799'999}};

// A member's own settings for one conversation; nullopt leaves one alone.
struct MemberPreferences final {
    // An instant, as the client computed it, or nullopt-inside to unmute.
    std::optional<std::optional<db::TimeMs>> muted_until{};
    // A duration the SERVER adds to its own clock (docs/22 §5.1): zero
    // unmutes, and at most kMaxMuteDuration. A client's only clock is the
    // device's, which is wrong by an amount it cannot measure, so "mute for
    // eight hours" is a duration on the wire and an instant only here.
    std::optional<std::chrono::seconds>      mute_for{};
    // Muted until unmuted, stored as kMutedIndefinitely.
    bool                                     mute_indefinitely{false};
    std::optional<bool>                      pinned{};
    std::optional<bool>                      archived{};
    // "Clear chat": hide everything up to the conversation's current head.
    bool                                     clear{false};
    std::optional<bool>                      read_private{};
};

// Field names for validation failures, compile-time constants (input/fields.h).
inline constexpr std::string_view kKindField = "kind";
inline constexpr std::string_view kTitleField = "title";
inline constexpr std::string_view kDescriptionField = "description";
inline constexpr std::string_view kMembersField = "members";
inline constexpr std::string_view kRoleField = "role";
inline constexpr std::string_view kBodyField = "body";
inline constexpr std::string_view kMentionsField = "mentions";
inline constexpr std::string_view kClientIdField = "cid";
inline constexpr std::string_view kReplyField = "reply_to";
inline constexpr std::string_view kPinnedField = "pinned";
inline constexpr std::string_view kMuteForField = "mute_for_s";
inline constexpr std::string_view kReactionField = "reaction";
inline constexpr std::string_view kTimerField = "timer";
inline constexpr std::string_view kInviteField = "invite";
inline constexpr std::string_view kAttachmentsField = "attachments";
inline constexpr std::string_view kCardField = "card";
inline constexpr std::string_view kCiphertextField = "ciphertext";
// Also the field a stale send's 409 names: the device set changed.
inline constexpr std::string_view kDevicesField = "devices";
inline constexpr std::string_view kDeviceField = "device";
inline constexpr std::string_view kDeviceSetField = "dsv";
inline constexpr std::string_view kPageField = "page";

// The most uses and the longest life one invite link may have.
inline constexpr std::int32_t kMaxInviteUses = 1000;
inline constexpr std::chrono::seconds kMaxInviteLifetime{30L * 24 * 3600};

// The largest page of history one request may ask for.
inline constexpr std::int32_t kMaxHistoryPage = 100;

// The largest page of members. It was 200, the only chat list above 100, and a
// descriptor's page ceiling is read as the server's ceiling for every list: one
// number for all of them lets a client size one bound.
inline constexpr std::int32_t kMaxMemberPage = 100;

// How many people one request may add. Bounded so one transaction stays a
// reasonable transaction; a bigger import is several requests.
inline constexpr std::size_t kMaxMembersPerRequest = 256;

// How long a device change may stay pending before the sweeper finishes it
// (docs/22 §7.4). The route that made the change propagates it at once; this is
// how long a process killed between the two is given to have been slow rather
// than dead, and so how long a sender can pass the fence on the old set.
inline constexpr std::chrono::seconds kDeviceChangeGrace{60};

// Conversations raised per update_many while a device change is pushed out.
inline constexpr std::int32_t kDeviceChangePage = 256;

// Everything the service is built from. References are to objects the
// application builds once at boot and that outlive the service.
struct ChatServiceDeps final {
    const ChatRepository&                 repository;
    // Releases an attachment's reference when the message holding it is revoked
    // or expires, in that same transaction (docs/07-filesystem.md §7).
    const media::MediaService&            media;
    // Opens the upload handles a send names.
    const media::GrantKeys&               grants;
    std::span<const ConversationKindSpec> kinds;
    // The application's message kinds; empty when it declares none.
    std::span<const CardSpec>             cards;
    // 32 bytes mixed into every invite token's digest, so a database dump alone
    // cannot replay a link (CLAUDE.md §5). Copied in.
    std::span<const std::uint8_t>         invite_pepper;
    ChatHooks                             hooks;
    // Live delivery (chat/live.h): every committed send wakes its members'
    // sockets through it. Null serves chat without pushing anything, which is
    // Phase 18's behaviour — every client polls. Must outlive the service.
    ChatLive*                             live{nullptr};
    // Push nudges (chat/push.h): every committed plaintext send asks for its
    // window's job through it. Null pushes nothing. Must outlive the service.
    ChatPush*                             push{nullptr};
    // The device directory (chat/devices.h) and the per-device queue
    // (chat/device_queue.h). All three with prekeys, or none: null serves no encrypted
    // conversation's device machinery, and every encrypted send, propagation
    // and device read refuses with ServiceUnavailable. Must outlive the service.
    const DeviceDirectory*                devices{nullptr};
    const DeviceQueue*                    queue{nullptr};
    // The key directory (chat/prekeys.h), with the two above or not at all.
    const PrekeyDirectory*                prekeys{nullptr};
    // Staff reads (docs/22 §9.2). Unset serves none.
    ChatReview                            review{};
};

class ChatService final {
public:
    explicit ChatService(ChatServiceDeps deps);

    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    [[nodiscard]] std::span<const ConversationKindSpec> kinds() const noexcept { return kinds_; }
    [[nodiscard]] std::span<const CardSpec> cards() const noexcept { return cards_; }
    // The keys a response mints each attachment's grant under (media/grant.h).
    [[nodiscard]] const media::GrantKeys& grants() const noexcept { return grants_; }
    [[nodiscard]] const ConversationKindSpec& kind_of(KindCode code) const noexcept {
        return kinds_[code];
    }

    // --- conversations ---------------------------------------------------------------

    // A group or a channel. The creator is its owner; the members it is created
    // with see it from the Created message on.
    [[nodiscard]] Result<CreatedConversation> create(mongocxx::client& client, const Actor& actor,
                                                     const CreateConversation& request) const;

    // The direct conversation between the actor and `other` in this mode,
    // found or created (docs/22 §3.2). Two people opening each other at once get
    // one conversation.
    [[nodiscard]] Result<ConversationState> open_direct(mongocxx::client& client,
                                                        const Actor& actor,
                                                        std::string_view kind,
                                                        const Uuid& other,
                                                        bool encrypted) const;

    // The conversation and the caller's membership, current or past. A past
    // member can still read up to where they left, and is shown the head as it
    // was then.
    [[nodiscard]] Result<ConversationState> state(mongocxx::client& client, const Actor& actor,
                                                  const Uuid& conversation) const;

    // Title, description, each optional; an empty string clears it. EditInfo.
    [[nodiscard]] Status update_info(mongocxx::client& client, const Actor& actor,
                                     const Uuid& conversation,
                                     std::optional<std::string_view> title,
                                     std::optional<std::string_view> description) const;

    // --- membership ----------------------------------------------------------------

    // Current members, by user id, after `after`. A current member may list;
    // somebody who left may not, and a channel's followers may not (§2.1).
    [[nodiscard]] Result<std::vector<MemberRecord>> members(mongocxx::client& client,
                                                            const Actor& actor,
                                                            const Uuid& conversation,
                                                            const std::optional<Uuid>& after,
                                                            std::int32_t limit) const;

    // AddMember. All or nothing: one refused person refuses the request, and
    // the refusal does not say which one, because naming them would say who
    // blocked whom.
    [[nodiscard]] Status add_members(mongocxx::client& client, const Actor& actor,
                                     const Uuid& conversation,
                                     std::span<const Uuid> people) const;

    // RemoveMember, or leaving when `target` is the actor. An owner leaving last
    // hands the conversation to the longest-standing admin, else member.
    [[nodiscard]] Status remove_member(mongocxx::client& client, const Actor& actor,
                                       const Uuid& conversation, const Uuid& target) const;

    // ManageAdmins. Owner is transferable only by an owner.
    [[nodiscard]] Status set_role(mongocxx::client& client, const Actor& actor,
                                  const Uuid& conversation, const Uuid& target, Role role) const;

    // Following a channel, which anyone may: a channel is public by nature.
    [[nodiscard]] Result<ConversationState> follow(mongocxx::client& client, const Actor& actor,
                                                   const Uuid& conversation) const;

    // --- messages ---------------------------------------------------------------------

    // Post. The four steps of docs/22 §4.1: membership, an earlier attempt
    // with this client id, the seq (one $inc, outside any transaction), then
    // the row. A crash between the last two burns a seq: numbers are monotonic,
    // not dense.
    [[nodiscard]] Result<SentMessage> send(mongocxx::client& client, const Actor& actor,
                                           const Uuid& conversation,
                                           const SendMessage& message) const;

    // An encrypted message (docs/22 §7.6), in the same four steps as send().
    // The fence is in step 3's filter, so a stale sender costs no round trip of
    // its own.
    //
    //   ValidationFailed  malformed, over a bound, a plaintext conversation, a
    //                     per-device map naming a device that is not a current
    //                     device of a current member (devices, NotAllowed), the
    //                     sending device in its own map, or a page outside a group
    //   Forbidden         no Post right, or `device` is not one of the actor's
    //                     current devices (device)
    //   Conflict          STALE (devices): the map is missing a current device,
    //                     or the fence moved. The client fetches the lists,
    //                     re-encrypts and retries with the same client id.
    //
    // The per-device ciphertexts become queue rows in the message's own
    // transaction. Past a block, the peer's rows are never written.
    [[nodiscard]] Result<SentMessage> send_encrypted(mongocxx::client& client, const Actor& actor,
                                                     const Uuid& conversation,
                                                     const EncryptedMessage& message) const;

    // One page of the current members of an ENCRYPTED conversation and their
    // published devices, after `after`, at most kMaxDevicePage. A current member
    // only; a plaintext conversation has nothing to encrypt for (Forbidden).
    [[nodiscard]] Result<ConversationDevices> conversation_devices(
        mongocxx::client& client, const Actor& actor, const Uuid& conversation,
        const std::optional<Uuid>& after, std::int32_t limit) const;

    // One page of what the caller may see, newest `limit` before `before` (or
    // the head), returned oldest first. The range is the caller's own
    // (MemberRecord::can_see), so a past member reads up to where they left
    // and nobody reads what was cleared or what came before they joined.
    [[nodiscard]] Result<HistoryPage> history(mongocxx::client& client, const Actor& actor,
                                              const Uuid& conversation,
                                              std::optional<std::int64_t> before,
                                              std::int32_t limit) const;

    // Everything visible after `after`, oldest first: the catch-up read a
    // device makes with its cursor. A gap in the seqs it returns is a burnt
    // number, never a message it missed.
    [[nodiscard]] Result<std::vector<MessageRecord>> catch_up(mongocxx::client& client,
                                                              const Actor& actor,
                                                              const Uuid& conversation,
                                                              std::int64_t after,
                                                              std::int32_t limit) const;

    // Every visible message whose mutation counter is past `after`, oldest
    // mutation first: the catch-up for edits, revokes and reaction changes,
    // which allocate no seq and so are invisible to catch_up (docs/22 §4.5).
    // A device pages with the `mutation` of the last row until a page is short.
    // A past member's stops at the counter when they left.
    [[nodiscard]] Result<std::vector<MessageRecord>> changes(mongocxx::client& client,
                                                             const Actor& actor,
                                                             const Uuid& conversation,
                                                             std::int64_t after,
                                                             std::int32_t limit) const;

    // The sender's own text message, inside the kind's edit window. The row
    // keeps the latest body and a count; edits are not chained, because a
    // history is one more copy of text its author asked to take back.
    // NotFound for a message that is not theirs, is too old, or is gone alike.
    [[nodiscard]] Status edit(mongocxx::client& client, const Actor& actor,
                              const Uuid& conversation, std::int64_t seq, std::string_view body,
                              std::span<const MentionSpan> mentions) const;

    // Delete for everyone: the sender inside the kind's revoke window, or any
    // holder of RevokeAny at any time. The content, attachments and reactions go
    // in one transaction, each attachment's reference with them; the row and
    // its seq stay, so every client holding the message learns it was revoked.
    [[nodiscard]] Status revoke(mongocxx::client& client, const Actor& actor,
                                const Uuid& conversation, std::int64_t seq) const;

    // --- receipts and the chat list -------------------------------------------------

    // Advances the caller's watermarks (docs/22 §5.1). Both are clamped to the
    // conversation's head: a client claiming to have read seq 10^9 would
    // otherwise be "read by" on every message not yet written. Reading implies
    // delivery, so delivered is at least read. Answers the membership after.
    [[nodiscard]] Result<MemberRecord> receipts(mongocxx::client& client, const Actor& actor,
                                                const Uuid& conversation, std::int64_t delivered,
                                                std::int64_t read) const;

    // Who holds `seq` and who has read it (docs/22 §5.1), for a kind whose
    // receipts are not Off (Forbidden). `read_by` is nullopt for a kind whose
    // receipts are Delivered, and leaves out members who keep their reads
    // private; `delivered_to` leaves out nobody current. Both are watermarks,
    // so one call for a sender's newest message answers every older one too.
    [[nodiscard]] Result<Readers> readers(mongocxx::client& client, const Actor& actor,
                                          const Uuid& conversation, std::int64_t seq) const;

    // One page of the caller's conversations, most recently active first.
    [[nodiscard]] Result<ChatList> chat_list(mongocxx::client& client, const Actor& actor,
                                             bool archived,
                                             const std::optional<std::pair<db::TimeMs, Uuid>>& after,
                                             std::int32_t limit) const;

    // The caller's own settings, and their membership after. Pinning is
    // bounded at kMaxPins; the count is read before the write, so two pins
    // racing can reach one over — a bound on a list a person curates by hand,
    // not a security property. At most one of muted_until, mute_for and
    // mute_indefinitely (ValidationFailed, mute_for_s, NotAllowed); a duration
    // past kMaxMuteDuration is OutOfRange.
    [[nodiscard]] Result<MemberRecord> preferences(mongocxx::client& client, const Actor& actor,
                                                   const Uuid& conversation,
                                                   const MemberPreferences& preferences) const;

    // What the actor may see of `subject`'s presence (chat/presence.h): NotFound
    // when presence is off or there is no live delivery, which the route turns
    // into the stealth 404, because a deployment that does not do presence has
    // no such route as far as a client can tell.
    [[nodiscard]] Result<PresenceView> presence(mongocxx::client& client, const Actor& actor,
                                                const Uuid& subject) const;

    // presence() for each of up to kMaxPresenceBatch accounts, in their order
    // (PresenceTracker::view_many).
    [[nodiscard]] Result<std::vector<PresenceView>> presence(mongocxx::client& client,
                                                             const Actor& actor,
                                                             std::span<const Uuid> subjects) const;

    // --- reactions ----------------------------------------------------------------------

    // React, or with nullopt take the caller's reaction back. One per person per
    // message; a second replaces the first. Not on a revoked or a system message.
    [[nodiscard]] Status react(mongocxx::client& client, const Actor& actor,
                               const Uuid& conversation, std::int64_t seq,
                               std::optional<std::string_view> reaction) const;

    // The tallies for the messages of a page the caller is reading. Seqs outside
    // the caller's range are dropped before the query, so a tally is never a
    // way to learn about a message they cannot see.
    [[nodiscard]] Result<std::vector<ChatRepository::ReactionTally>> reactions(
        mongocxx::client& client, const Actor& actor, const Uuid& conversation,
        std::span<const std::int64_t> seqs) const;

    // --- disappearing messages ----------------------------------------------------------

    // SetTimer: one of the kind's timers, or 0 for off. A system message records
    // the change, so every client shows it at the same point in the log. It
    // reaches only messages sent after it: each message carries the expiry it
    // was sent under (docs/22 §4.7).
    [[nodiscard]] Status set_timer(mongocxx::client& client, const Actor& actor,
                                   const Uuid& conversation, std::uint32_t seconds) const;

    // The expiry sweeper, for a recurring job the application registers under a
    // Redis lease (docs/10-timer-jobs.md). At most `batch` messages, each
    // removed in its own transaction with its attachments' references and its
    // reactions. NOT a TTL index: the monitor deletes a row without running any
    // code, and an attachment's reference would then stay counted forever — a
    // leak the media sweeper cannot tell from a live reference.
    // Answers how many it removed; a full batch means there may be more.
    [[nodiscard]] Result<std::int32_t> sweep_expired(mongocxx::client& client,
                                                     std::int32_t batch) const;

    // --- device changes (docs/22 §7.4) -------------------------------------------------

    // Pushes `user`'s pending device change, if there is one, to every
    // conversation they are a current member of: each one's dsv rises past the
    // change, and each ENCRYPTED DIRECT one gets a DevicesChanged message, so
    // the other person's client shows the changed security code in the log.
    // Then the pending mark is cleared, only if no later change replaced it.
    //
    // Idempotent and safe to race: the message's client id is derived from the
    // change, so a second run finds the first's message rather than writing
    // another; and dsv only rises. Called by whatever made the change, at once,
    // and by the sweeper for a change whose maker died first. ServiceUnavailable
    // without a device directory.
    [[nodiscard]] Status propagate_devices(mongocxx::client& client, const Uuid& user) const;

    // The sweeper, for a recurring job the application registers under a Redis
    // lease (docs/10-timer-jobs.md): every change pending since before
    // `now - kDeviceChangeGrace`, at most `batch` accounts. Answers how many it
    // finished; a full batch means there may be more.
    [[nodiscard]] Result<std::int32_t> sweep_device_changes(mongocxx::client& client,
                                                            db::TimeMs now,
                                                            std::int32_t batch) const;

    // --- devices, keys and the queue (docs/22 §7.3, §7.5, §7.6) ------------------------
    //
    // Every one of these refuses with ServiceUnavailable without the device
    // machinery. Each change is propagated to the account's conversations
    // before it returns (propagate_devices), and reported to on_device.

    // The caller's own identity document: every device, private fields too.
    [[nodiscard]] Result<std::optional<IdentityRecord>> my_devices(mongocxx::client& client,
                                                                   const Actor& actor) const;

    // The first device of the account, for the actor's session, only inside
    // kFreshAuthentication of the authenticated_at hook's answer
    // (CapabilityRequired, authenticated_at, otherwise: never Unauthenticated,
    // which a client takes for an expired token and answers with a refresh). DeviceDirectory's other
    // refusals as they are.
    [[nodiscard]] Status register_device(mongocxx::client& client, const Actor& actor,
                                         NewDevice device) const;

    // A later device, admitted by an existing device's signature
    // (DeviceDirectory::link_device), for the actor's session.
    [[nodiscard]] Status link_device(mongocxx::client& client, const Actor& actor,
                                     const Uuid& approver, NewDevice device,
                                     std::uint64_t timestamp_s,
                                     const crypto::Ed25519Signature& signature) const;

    // --- the link relay (docs/22 §7.3.1) ---
    //
    // A new device leaves its id and public identity keys for an approver of
    // the same account, under a token returned once (only its peppered digest
    // is stored). Replaces any request this session left. ValidationFailed
    // naming a malformed key or a nil id.
    [[nodiscard]] Result<std::string> request_link(mongocxx::client& client, const Actor& actor,
                                                   const Uuid& device,
                                                   const crypto::X25519PublicKey& agreement,
                                                   const crypto::Ed25519PublicKey& signing) const;

    // What an approver signs over, read by the token. NotFound for anything
    // but a live request of the actor's own account.
    [[nodiscard]] Result<LinkRequest> read_link_request(mongocxx::client& client,
                                                        const Actor& actor,
                                                        std::string_view token) const;

    // The approver's signature left for the requester: verified here first,
    // under the approver's stored key and over link_message, exactly as
    // link_device will verify it, so a bad one is refused to the approver and
    // not discovered by the requester. NotFound as read_link_request; Forbidden
    // when `approver` is not one of the actor's devices (approver) or the
    // signature does not verify (link_signature); ValidationFailed for a
    // timestamp past kLinkSkew; Conflict when it was already approved.
    [[nodiscard]] Status approve_link_request(mongocxx::client& client, const Actor& actor,
                                              std::string_view token, const Uuid& approver,
                                              std::uint64_t timestamp_s,
                                              const crypto::Ed25519Signature& signature) const;

    // The approval, once, to the session that asked: nullopt while it is still
    // pending, and gone from the mailbox once returned. NotFound for any other
    // session, any other account, and anything expired or already collected.
    [[nodiscard]] Result<std::optional<LinkProof>> collect_link_approval(
        mongocxx::client& client, const Actor& actor, std::string_view token) const;

    // One of the actor's devices replacing its signed prekey, its last-resort
    // key or both (DeviceDirectory::rotate_prekeys), and marked seen.
    [[nodiscard]] Status rotate_prekeys(mongocxx::client& client, const Actor& actor,
                                        const Uuid& device,
                                        const std::optional<SignedPrekey>& signed_prekey,
                                        const std::optional<SignedPrekey>& last_resort) const;

    // One of the actor's devices unlinked, its keys and queue discarded.
    // Idempotent: an unknown device is a success that changed nothing.
    [[nodiscard]] Status unlink_device(mongocxx::client& client, const Actor& actor,
                                       const Uuid& device) const;

    // A session ended: whatever device it registered is unlinked, so "sign
    // out everywhere" really is everywhere (§7.3). The application calls this
    // wherever its sessions are revoked.
    [[nodiscard]] Status session_ended(mongocxx::client& client, const Uuid& user,
                                       const Uuid& session) const;

    // The idle sweeper: devices unseen for `idle_days`, from at most `batch`
    // accounts, unlinked as an explicit unlink is. For a recurring job under a
    // Redis lease. Answers how many it unlinked.
    [[nodiscard]] Result<std::int32_t> sweep_idle_devices(mongocxx::client& client,
                                                          db::TimeMs now,
                                                          std::uint32_t idle_days,
                                                          std::int32_t batch) const;

    // One-time prekeys for one of the actor's devices (PrekeyDirectory::upload).
    [[nodiscard]] Status upload_prekeys(mongocxx::client& client, const Actor& actor,
                                        const Uuid& device,
                                        std::span<const OneTimePrekey> keys) const;

    // A bundle per current device of `target`, each with its own one-time key
    // or the last-resort key. Only between two current members of one
    // ENCRYPTED conversation; anybody else is NotFound, as if not there.
    //
    // `admit` is the per-TARGET budget (§7.5), asked after both memberships are
    // established and before any key leaves its pool, and its refusal is the
    // answer. After, so a stranger refused as NotFound spends nothing of the
    // victim's budget: otherwise anybody could lock everybody out of claiming
    // against an account by asking about it. Unset admits.
    //
    // A batch: up to kMaxClaimBatch accounts, duplicates once, answered in
    // their order. The CALLER's membership and the conversation are checked
    // first, and refuse the whole request; each target's membership and
    // budget are then that target's answer alone.
    [[nodiscard]] Result<std::vector<AccountClaim>> claim_bundles(
        mongocxx::client& client, const Actor& actor, const Uuid& conversation,
        std::span<const Uuid> targets,
        const std::function<Status(const Uuid& target)>& admit = {}) const;

    // One of the actor's current devices' queue after `after`, and recording
    // that the device was seen, which is what keeps a reading device from the
    // idle sweeper.
    [[nodiscard]] Result<std::vector<QueuedCiphertext>> device_queue(
        mongocxx::client& client, const Actor& actor, const Uuid& device,
        const std::optional<Uuid>& after, std::int32_t limit) const;

    // Everything in that queue up to and including `through`, deleted.
    [[nodiscard]] Result<std::int64_t> acknowledge_queue(mongocxx::client& client,
                                                         const Actor& actor, const Uuid& device,
                                                         const Uuid& through) const;

    // --- staff review and reports (docs/22 §9.2) ------------------------------------
    //
    // The REVIEWER is not checked here: the routes are behind a permission the
    // application names in its route table, and the access filter refuses
    // anybody without it before a handler runs. What is checked here is the
    // conversation: it exists (NotFound), its kind is reviewable and it is not
    // encrypted (Forbidden, conversation), and the read is recorded first
    // (ServiceUnavailable when it cannot be, or when no review is configured).
    // Nothing a staff read does moves a member's watermark or appears in any
    // member's answer: it is not a membership.

    // The conversation and a page of its current members.
    [[nodiscard]] Result<Review> review(mongocxx::client& client, const Actor& reviewer,
                                        const std::array<std::uint8_t, 16>& ip,
                                        const Uuid& conversation, const std::optional<Uuid>& after,
                                        std::int32_t limit) const;

    // A page of the whole retained log, newest `limit` before `before`, oldest
    // first: every message the server still holds, hidden ones included.
    [[nodiscard]] Result<HistoryPage> review_history(mongocxx::client& client,
                                                     const Actor& reviewer,
                                                     const std::array<std::uint8_t, 16>& ip,
                                                     const Uuid& conversation,
                                                     std::optional<std::int64_t> before,
                                                     std::int32_t limit) const;

    // A current member reports the messages `from`..`to`, inclusive, both inside
    // what they can see, at most kMaxReportRange, with an optional note of
    // their own. The kind must be reviewable and the conversation plaintext
    // (Forbidden). A retry over the same range is the first report.
    [[nodiscard]] Result<FiledReport> report(mongocxx::client& client, const Actor& actor,
                                             const Uuid& conversation, std::int64_t from,
                                             std::int64_t to, std::string_view note) const;

    // The tallies on a page a staff read shows: nobody's own, so `mine` is
    // always false.
    [[nodiscard]] Result<std::vector<ChatRepository::ReactionTally>> review_reactions(
        mongocxx::client& client, const Uuid& conversation,
        std::span<const std::int64_t> seqs) const;

    // Reports, newest first, for staff; the same permission as the review.
    [[nodiscard]] Result<std::vector<ReportRecord>> reports(mongocxx::client& client,
                                                            const std::optional<Uuid>& after,
                                                            std::int32_t limit) const;

    // --- invites and blocks --------------------------------------------------------------

    // CreateInvite. A multi-use link with a cap and a lifetime; the token is
    // 256 CSPRNG bits, returned once, and only its peppered digest is stored.
    [[nodiscard]] Result<std::string> create_invite(mongocxx::client& client, const Actor& actor,
                                                    const Uuid& conversation, std::int32_t uses,
                                                    std::chrono::seconds lifetime) const;

    // CreateInvite. False when there was no such link for this conversation.
    [[nodiscard]] Result<bool> revoke_invite(mongocxx::client& client, const Actor& actor,
                                             const Uuid& conversation,
                                             std::string_view token) const;

    // Joins through a link. Unknown, expired, revoked, spent, and a full
    // conversation all answer NotFound alike: a link is shared where its owner
    // cannot see, and telling a holder why it stopped working tells them whether
    // the group still exists (docs/22 §3.5). Joining while already a member is
    // that membership, and spends no use.
    [[nodiscard]] Result<ConversationState> join(mongocxx::client& client, const Actor& actor,
                                                 std::string_view token) const;

    // A block stops direct conversations in both directions and touches no
    // group (docs/22 §3.6). The blocked person is not told: their direct sends
    // to the blocker are stored and shown only to them.
    [[nodiscard]] Status block(mongocxx::client& client, const Actor& actor,
                               const Uuid& target) const;
    [[nodiscard]] Status unblock(mongocxx::client& client, const Actor& actor,
                                 const Uuid& target) const;

private:
    // A card through its kind's binder, re-checked as a bounded JSON object.
    [[nodiscard]] Result<CardRecord> bind_card(const SendMessage::Card& card) const;

    // The attachments of a send, resolved to what the row stores, or the
    // refusal. Every handle is the sender's own and every forward is something
    // the sender can see, in the kind's media namespace, or for an encrypted
    // send its sealed one.
    [[nodiscard]] Result<std::vector<AttachmentRecord>> resolve_attachments(
        mongocxx::client& client, const Actor& actor, const ConversationKindSpec& kind,
        std::span<const OutgoingAttachment> attachments, bool sealed) const;

    // Whether a direct message from the actor is stored past a block (§3.6):
    // Forbidden when the actor blocked the other person, true when the other
    // person blocked the actor. False for every other shape.
    [[nodiscard]] Result<bool> hidden_past_block(mongocxx::client& client, const Actor& actor,
                                                 const Uuid& conversation,
                                                 const ConversationKindSpec& kind) const;

    // Everything after a send's commit, in its order: the hook, the wake, the
    // push's late ask, and the chat-list bump.
    void after_send(mongocxx::client& client, const ConversationKindSpec& kind,
                    const Uuid& conversation, KindCode code, const MessageRecord& row,
                    const Allocation& allocation, db::TimeMs now) const;

    // A committed edit, revoke or reaction change, told to the members'
    // sockets as a Mutation frame. A message past a block tells its sender only.
    void announce_mutation(mongocxx::client& client, const ConversationKindSpec& kind,
                           const Uuid& conversation, const Allocation& numbered,
                           const Mutated& message) const;

    // The caller's CURRENT membership and the kind, or NotFound.
    [[nodiscard]] Result<ConversationState> current(mongocxx::client& client, const Actor& actor,
                                                    const Uuid& conversation) const;

    [[nodiscard]] Status reachable(mongocxx::client& client, const Uuid& actor,
                                   const Uuid& target) const;

    // Every person a message names is a current member, in one round trip.
    [[nodiscard]] Status mentioned_are_members(mongocxx::client& client, const Uuid& conversation,
                                               std::span<const MentionSpan> mentions) const;

    void report(const MembershipEvent& event) const noexcept;
    void report_device(const DeviceEvent& event) const noexcept;

    // ServiceUnavailable unless the device machinery was given.
    [[nodiscard]] Status require_devices() const;

    // Forbidden (device) unless `device` is one of the actor's current devices.
    [[nodiscard]] Status own_device(mongocxx::client& client, const Actor& actor,
                                    const Uuid& device) const;

    // After a device left the account: its keys and queue discarded, the change
    // reported when `changed`, and propagated.
    [[nodiscard]] Status finish_unlink(mongocxx::client& client, const Uuid& user,
                                       const Uuid& device, const Uuid& session,
                                       bool changed) const;

    const ChatRepository&                 repository_;
    const media::MediaService&            media_;
    std::span<const ConversationKindSpec> kinds_;
    std::span<const CardSpec>             cards_;
    ChatHooks                             hooks_;
    crypto::SecretBuffer<32>              invite_pepper_;
    const media::GrantKeys&               grants_;
    ChatLive* const                       live_;
    ChatPush* const                       push_;
    const DeviceDirectory* const          devices_;
    const DeviceQueue* const              queue_;
    const PrekeyDirectory* const          prekeys_;
    const ChatReview                      review_;

    // The conversation a staff read may show, recorded as read.
    [[nodiscard]] Result<ConversationRecord> open_for_review(
        mongocxx::client& client, const Actor& reviewer, const std::array<std::uint8_t, 16>& ip,
        const Uuid& conversation) const;
};

}  // namespace anvil::chat
