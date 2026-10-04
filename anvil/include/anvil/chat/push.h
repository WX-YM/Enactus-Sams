#pragma once

// Chat push nudges (docs/22-chat.md §8.4): a Web Push to the members a message
// has not reached, through the endpoints notifications already holds.
//
// --- one job per message, coalesced by its key --------------------------------
//
// A send asks the application's queue for ONE job, never one per recipient
// (docs/11 §7): the recipients are paged inside the job. The job's idempotency
// key is the coalescing bucket — the conversation and a fixed window of time,
// as a coalescing notification's dedupe key is (docs/11 §3) — so every send in
// one window asks for the same job and the queue keeps one. The job runs after
// the window closes and tells each recipient how many messages are waiting, so
// a burst of forty is one push saying forty, per (account, conversation).
//
// --- who is pushed ------------------------------------------------------------
//
// A current member is pushed when the newest messages hold one they can see,
// from somebody else, past their DELIVERED watermark, and at least one such
// message was sent in this job's window; unless they muted the conversation
// and were not mentioned where the kind lets a mention through.
//
// The delivered watermark rather than the hub, because the hub's has_socket()
// is one process's answer and a recipient's socket may be on another. A device
// that holds a message says so with a receipt, through whichever process it is
// connected to, and that durable watermark is the one answer every process
// shares. Its cost is the right one: a device that holds a message and has not
// said so yet is pushed anyway, which is a duplicate; nothing makes a device
// that does NOT hold a message look as though it does, so there is no miss.
//
// --- no notification rows -----------------------------------------------------
//
// Nothing here writes to notifications or to an inbox. The chat list is chat's
// inbox, and a second copy of every message there would be two read states for
// one fact, which would disagree.
//
// --- surviving SIGKILL --------------------------------------------------------
//
// The job is asked for BEFORE the message commits, after its seq and its time
// are known. A process killed after the commit has already asked; a process
// killed before it has asked for a job that finds nothing new and pushes
// nothing. A commit that lands after its job may already have run — a slow
// transaction past the window and the grace — asks again under a key of its own,
// so a slow commit is not a missed push either. Every handler is at-least-once,
// and a redelivered job pushes again, which a push's tag on the device absorbs.
//
// --- encrypted conversations ---------------------------------------------------
//
// Their push carries {c, seq} and nothing else (docs/22 §7.8): no text, no
// sender's name, no title, no count. The service worker wakes, fetches from its
// cursor, decrypts, and writes the notification itself. The server never had
// the content, so it cannot put it in a push, and the names and the title it
// does have are left out as well, because the notification a recipient sees
// should come from what their own device decrypted and nowhere else.
//
// The delivery's content is then an EMPTY title and, as its body, exactly
// encrypted_push_payload(c, seq): the payload a transport sends for it, verbatim.
// The reader and name_of hooks are never asked for it. Who is pushed is the
// same rule as for plaintext, with one difference that follows from the
// ciphertext: a mention is inside it, so nothing breaks a mute.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/chat/kind_spec.h"
#include "anvil/chat/record.h"
#include "anvil/chat/repository.h"
#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/notifications/outbound.h"
#include "anvil/notifications/repository.h"
#include "anvil/notifications/template_spec.h"
#include "anvil/notifications/topic_spec.h"
#include "anvil/timer/job_spec.h"

namespace anvil::chat {

// The job's arguments: a version byte, the conversation, the bucket.
inline constexpr std::size_t kPushArgsBytes = 1 + 16 + 8;
inline constexpr std::uint8_t kPushArgsVersion = 1;

// How many of a conversation's newest messages one job reads. A count at this
// bound means "at least this many".
inline constexpr std::int32_t kNudgeScan = 100;

// Recipients per page, and so per `$in` for their endpoints.
inline constexpr std::int32_t kNudgePage = 128;

// The most WebPush endpoints one account is read for. A browser per device,
// and a person has a handful.
inline constexpr std::int32_t kNudgeEndpointsPerAccount = 16;

// A clock may run this far ahead of the one that stamped a message, and a
// commit this close to its job's due time asks again.
inline constexpr std::chrono::seconds kNudgeClockSkew{1};

// The whole push for an encrypted conversation: `{"c":"<uuid>","seq":<n>}`, the
// conversation and the newest message waiting for the recipient.
[[nodiscard]] std::string encrypted_push_payload(const Uuid& conversation, std::int64_t seq);

// What a nudge needs to know about its recipient, from the application.
struct PushReader final {
    Locale locale;
    // False when the recipient turned message previews off: their push uses
    // the plain template and the message's text is not bound at all.
    bool   previews;
};

struct PushHooks final {
    // Ask the application's queue for its push job kind: `queue.schedule_at(kind,
    // args, due, key)`. Must be idempotent by `key`, which is what coalesces.
    // Called on the db_pool thread a send holds; BLOCKS on Redis. REQUIRED.
    std::function<Status(std::span<const std::uint8_t> args, db::TimeMs due,
                         std::string_view key)>
        enqueue;

    // The recipient's language and preview setting, which live on the
    // application's account row. Called once per recipient per job, on the job's
    // thread; an implementation that queries per call is an N+1 across a
    // group, so cache it. REQUIRED.
    std::function<PushReader(mongocxx::client& client, const Uuid& user)> reader;

    // How a sender is named in a push, `{s}`, and a direct conversation's title,
    // `{t}`. Once per distinct sender per job. Unset names nobody, and the
    // template renders the placeholder as nothing.
    std::function<std::string(mongocxx::client& client, const Uuid& user)> name_of;
};

struct PushConfig final {
    // The coalescing window: every message sent in one window is one job and
    // at most one push per recipient. Also the longest a push waits for the
    // window to close.
    std::chrono::seconds              window{5};
    // How long after its window the job runs, so a device that holds the
    // message has time to say so with a receipt and be spared the push.
    std::chrono::seconds              grace{3};
    // The application's templates (01 §7b). Bound per recipient, in their
    // locale, from: {n} the count, {t} the conversation's title (for a direct
    // conversation, the sender's name), {s} the sender's name, and, in
    // `preview` only, {b} the newest message's text. A template may use any of
    // them; one it does not use is ignored.
    notifications::TemplateId         preview;
    notifications::TemplateId         plain;
    // The application's topic for chat. Its code is what the transport is told
    // the push is (Delivery::kind), and each endpoint's preferences for it are
    // honoured, so a device can turn chat pushes off. Its channels must include
    // WebPush.
    notifications::TopicCode          topic;
};

// What one job did.
struct PushSummary final {
    std::int32_t recipients;
    notifications::OutboundSummary deliveries;
};

class ChatPush final {
public:
    // Throws std::invalid_argument for a missing hook, a template or topic the
    // tables do not declare, or a topic without WebPush: each is a wiring fault
    // that would otherwise surface as pushes that never arrive. `web_push` is
    // the application's transport, the one OutboundSender is given.
    ChatPush(PushConfig config, PushHooks hooks, const ChatRepository& chats,
             const notifications::NotificationRepository& notifications,
             std::span<const notifications::TemplateSpec> templates,
             notifications::Transport web_push);

    ChatPush(const ChatPush&) = delete;
    ChatPush& operator=(const ChatPush&) = delete;

    // A message has a seq and a time and is about to commit. Asks for
    // its window's job. NEVER THROWS and never fails the send: a queue that
    // cannot be reached is logged and counted, and the message stands.
    void before_commit(const ConversationKindSpec& kind, const Uuid& conversation,
                       db::TimeMs sent_at) noexcept;

    // The message committed. When that was too close to its job's due time for
    // the job to have seen it, asks for one more under the message's own key.
    void after_commit(const ConversationKindSpec& kind, const Uuid& conversation,
                      std::int64_t seq, db::TimeMs sent_at) noexcept;

    // The job body. BLOCKING: MongoDB, the hooks, then a network round trip per
    // endpoint, so a job worker on db_pool, never a loop thread.
    [[nodiscard]] Result<PushSummary> deliver(mongocxx::client& client,
                                              std::span<const std::uint8_t> args,
                                              db::TimeMs now) const;

    // deliver() as a job handler answers: Done, Failed for arguments that will
    // never parse, Retry for anything that might succeed later. NEVER THROWS.
    [[nodiscard]] timer::JobOutcome run(const timer::JobRunContext& ctx) const noexcept;

    // The window a message sent at `at` falls in, and when its job is due.
    [[nodiscard]] std::int64_t bucket_of(db::TimeMs at) const noexcept;
    [[nodiscard]] db::TimeMs due_of(std::int64_t bucket) const noexcept;

    [[nodiscard]] static std::array<std::uint8_t, kPushArgsBytes> encode_args(
        const Uuid& conversation, std::int64_t bucket) noexcept;

    // Jobs asked for, and asks the queue refused.
    [[nodiscard]] std::uint64_t enqueued() const noexcept {
        return enqueued_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t enqueue_failures() const noexcept {
        return enqueue_failures_.load(std::memory_order_relaxed);
    }

private:
    void enqueue(const Uuid& conversation, std::int64_t bucket, db::TimeMs due,
                 std::string_view key) noexcept;

    const PushConfig                                 config_;
    PushHooks                                        hooks_;
    const ChatRepository&                            chats_;
    const notifications::NotificationRepository&     notifications_;
    std::span<const notifications::TemplateSpec>     templates_;
    notifications::Transport                         web_push_;
    const notifications::TopicSpec*                  topic_;
    std::atomic<std::uint64_t>                       enqueued_{0};
    std::atomic<std::uint64_t>                       enqueue_failures_{0};
};

}  // namespace anvil::chat
