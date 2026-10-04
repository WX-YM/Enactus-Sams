#pragma once

// Chat, live: the one object a process builds to push (docs/22-chat.md §5.4, §8).
//
// It owns the four pieces that have to agree with each other — the member
// cache, the wake publisher, the hub and the wake subscriber — and builds them
// in the one order that is safe. The subscriber's thread calls into the hub
// from the moment it starts, so the hub is built first and the subscriber last,
// and the destructor stops the subscriber before anything it calls is gone.
// An application builds one, at boot, and keeps it for the life of the process,
// as it keeps its ChatService; nothing here is meant to be built per request.
//
// --- the SSE fallback ------------------------------------------------------------
//
// A client behind a proxy that breaks WebSocket upgrades still has the HTTP
// routes, and with `sse` configured it has wakes too, on the notification stream
// it already holds (notifications/sse.h): an application that opens a stream for
// a reader who wants chat holds a StreamLease for the stream's life. The wake
// arrives as SseEventKind::ChatWake naming the conversation, and a confirmed
// subscription as ChatSync. Typing is not carried: it is ephemeral, and a client
// on the fallback is a client the live features degrade for, not one they break.
// Nothing about writes changes, because writes were always HTTP.
//
// --- what a send does with it -------------------------------------------------
//
// ChatService::send calls message_sent() after its commit, on the db_pool
// thread the send already holds. That costs the send one Redis round trip — a
// pipelined SPUBLISH per recipient — and, on a member-cache miss, one read.
// Neither can fail the send: the message is committed, and a wake is only its
// fast path. A lost wake costs latency, never a message.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>

#include <mongocxx/client.hpp>
#include <sw/redis++/redis++.h>

#include "anvil/chat/hub.h"
#include "anvil/chat/kind_spec.h"
#include "anvil/chat/member_cache.h"
#include "anvil/chat/presence.h"
#include "anvil/chat/record.h"
#include "anvil/chat/repository.h"
#include "anvil/chat/wakes.h"
#include "anvil/core/types.h"
#include "anvil/notifications/sse.h"

namespace anvil::chat {

class ChatService;

struct ChatLiveConfig final {
    // The subscriber's own Redis connection (chat/wakes.h).
    WakeSubscriberConfig subscriber;
    HubLimits            hub{};
    // The member cache's bound. Eight MiB holds five hundred 1 024-member groups
    // or a quarter of a million direct conversations.
    std::size_t          member_cache_bytes{8U << 20U};
    // The notification streams wakes also go to, for the fallback. Null when the
    // application serves none; it must outlive this object.
    notifications::SseHub* sse{nullptr};
    // Off unless the application turns it on (chat/presence.h).
    PresenceConfig       presence{};
};

class ChatLive final {
public:
    // `redis` is the publisher's connection — RedisClient::instance() in an
    // application — and `repository` the chat repository; both must outlive
    // this object. Starts the subscriber's thread.
    ChatLive(ChatLiveConfig config, sw::redis::Redis& redis, const ChatRepository& repository);
    ~ChatLive();

    // Stops and joins the subscriber's thread and the presence tracker's.
    // Idempotent; the destructor calls it. For a shutdown that releases the
    // pools before this object is destroyed: the tracker writes last seen
    // through MongoPool from its own thread, and a pool torn down under it is a
    // use after free. Afterwards nothing here runs on a thread of its own; a
    // send still publishes its wakes on the caller's thread, and no socket on
    // this process receives one.
    void stop() noexcept;

    // A reader's SSE stream that wants chat wakes, for as long as it is held:
    // the first lease subscribes the reader's wake channel and the last one
    // released unsubscribes it, as a socket does. Move-only; an empty lease
    // holds nothing.
    class StreamLease final {
    public:
        StreamLease() noexcept = default;
        ~StreamLease();
        StreamLease(StreamLease&& other) noexcept;
        StreamLease& operator=(StreamLease&& other) noexcept;
        StreamLease(const StreamLease&) = delete;
        StreamLease& operator=(const StreamLease&) = delete;

    private:
        friend class ChatLive;
        StreamLease(ChatLive* live, const Uuid& user) noexcept : live_{live}, user_{user} {}
        ChatLive* live_{nullptr};
        Uuid      user_{};
    };

    // Throws std::logic_error when no SseHub was configured: a stream that asks
    // for chat on a deployment that cannot carry it is a wiring fault, and a
    // lease that silently carried nothing would hide it.
    [[nodiscard]] StreamLease follow_on_stream(const Uuid& user);

    ChatLive(const ChatLive&) = delete;
    ChatLive& operator=(const ChatLive&) = delete;
    ChatLive(ChatLive&&) = delete;
    ChatLive& operator=(ChatLive&&) = delete;

    // After a message is committed, on the thread that committed it. Publishes
    // a wake to every current member who can see it — the sender included, for
    // their other devices — carrying the message as history renders it when it
    // fits kInlineWakeBytes. A message hidden past a block wakes its sender
    // only. A channel wakes nobody (chat/member_cache.h). BLOCKS on Redis;
    // NEVER THROWS.
    void message_sent(mongocxx::client& client, const ChatService& service,
                      const ConversationKindSpec& kind, const Uuid& conversation,
                      const MessageRecord& row, const Allocation& allocation) noexcept;

    // After an edit, a revoke or a reaction change is committed (docs/22
    // §4.5): a Mutation frame carrying the conversation's new counter, to every
    // current member at the membership version the mutation read, or to `only`
    // when the message is shown to one person. A channel wakes nobody. BLOCKS
    // on Redis; NEVER THROWS.
    void message_mutated(mongocxx::client& client, const ConversationKindSpec& kind,
                         const Uuid& conversation, const Allocation& numbered,
                         const std::optional<Uuid>& only) noexcept;

    // A member is typing in `conversation` (docs/22 §8.3): relayed as a Typing
    // frame to the conversation's other current members, through their wake
    // channels, so it reaches whichever process holds their sockets. Never
    // stored. Dropped — silently, because the typist is owed no answer — when
    // the typist is not a current member, the conversation is a channel, or it
    // is a direct conversation with a block either way; membership is asked
    // first, so a refusal cannot say who blocked whom. BLOCKS on MongoDB and
    // Redis, so db_pool; NEVER THROWS. The once-every-three-seconds bound is the
    // socket's, per connection, before it posts here.
    void typing(mongocxx::client& client, const ChatService& service, const Uuid& user,
                const Uuid& conversation) noexcept;

    [[nodiscard]] ChatHub& hub() noexcept { return hub_; }
    [[nodiscard]] const PresenceTracker& presence() const noexcept { return presence_; }
    [[nodiscard]] MemberCache& members() noexcept { return members_; }
    [[nodiscard]] WakePublisher& publisher() noexcept { return publisher_; }
    [[nodiscard]] WakeSubscriber& subscriber() noexcept { return subscriber_; }
    [[nodiscard]] const ChatRepository& repository() const noexcept { return repository_; }

    // Wakes whose message was rendered inline, and wakes that said "fetch it".
    [[nodiscard]] std::uint64_t inline_wakes() const noexcept {
        return inline_wakes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t fetch_wakes() const noexcept {
        return fetch_wakes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t typing_relayed() const noexcept {
        return typing_relayed_.load(std::memory_order_relaxed);
    }

private:
    // The subscriber's two callbacks: to the sockets, and to any lease's stream.
    void deliver(const Uuid& user, std::span<const std::uint8_t> frame);
    void resync(const Uuid& user);
    void release_stream(const Uuid& user) noexcept;
    [[nodiscard]] bool streaming(const Uuid& user) const;

    // Declaration order is construction order, and it is the point of this
    // class: the subscriber is last because its thread calls the hub.
    const ChatRepository&           repository_;
    notifications::SseHub* const    sse_;
    MemberCache                     members_;
    WakePublisher                   publisher_;
    // Before the hub, whose first-socket and last-socket callbacks reach it.
    PresenceTracker                 presence_;
    ChatHub                         hub_;
    mutable std::mutex              streams_mutex_;
    // Leases per reader. Guarded by streams_mutex_, which is also held across
    // the subscriber's subscribe and unsubscribe so one reader's are recorded
    // in the order the leases were taken and released (chat/hub.h says why).
    std::map<Uuid, std::uint32_t>   streams_;
    std::atomic<std::uint64_t>      inline_wakes_{0};
    std::atomic<std::uint64_t>      fetch_wakes_{0};
    std::atomic<std::uint64_t>      typing_relayed_{0};
    WakeSubscriber                  subscriber_;
};

}  // namespace anvil::chat
