#pragma once

// The cross-process wake channel (docs/22-chat.md §5.4).
//
// A member's socket lives on one process and the send commits on another. The
// sending process publishes a wake per recipient USER on a Redis sharded
// pub/sub channel, and every process subscribes to the channels of the users it
// currently holds sockets for. The publisher therefore never needs to know
// where a socket is: a per-process channel would need a presence registry
// consulted on every send, wrong for as long as it lags a reconnect.
//
// --- lossy by design -----------------------------------------------------------
//
// A lost wake costs latency, never a message. Pub/sub is fire-and-forget and
// Redis is not the system of record (docs/10-timer-jobs.md §1): a socket that
// missed a wake is a device whose next sync asks `seq > cursor` and gets the
// message from MongoDB. So nothing here retries, buffers or acknowledges. In
// particular, while the subscriber is disconnected from Redis every wake for
// its users is LOST, and that is correct — `on_subscribed` fires again after
// the reconnect, which is the hub's signal to make those sockets sync.
//
// --- sharded pub/sub: what was verified ------------------------------------------
//
// SSUBSCRIBE / SPUBLISH need Redis 7. redis-plus-plus 1.3.15 (the vcpkg port)
// exposes `Subscriber::ssubscribe`, `sunsubscribe` and `on_smessage`, and
// `Redis::spublish`; its pipeline has no `spublish`, so the publisher queues
// the raw command through the pipeline's generic `command()`. One defect in
// that version: `Subscriber::sunsubscribe(StringView)` is declared and never
// defined (a link error), so unsubscribing goes through the header-defined range
// overload with a range of one. Nothing falls back to plain PUBLISH.
//
// One limit stands and is stated rather than hidden: `RedisClient` is a
// standalone `sw::redis::Redis`, not a `RedisCluster`. On a single Redis (or a
// primary with replicas) sharded pub/sub behaves exactly like pub/sub in its
// own namespace. On Redis CLUSTER a channel lives on its slot's shard, and a
// standalone connection reaches one shard only, so running on a cluster needs a
// `RedisCluster` publisher and one subscriber connection per shard — a change
// to this file, not to its callers. To keep that change local, every channel is
// subscribed by its own command and never in a multi-channel SSUBSCRIBE, which a
// cluster refuses with CROSSSLOT when the channels' slots differ.
//
// --- the payload ---------------------------------------------------------------
//
// The bytes on the channel are EXACTLY a downstream socket frame
// (anvil/chat/frames.h) — a Wake, or a Typing relayed as one (§8.3) — so a
// subscriber hands them to the socket without decoding into a struct and
// re-encoding. They are still decoded once on arrival, to validate: a Redis
// channel is not a trusted input boundary when anything else with access to
// that Redis can publish to it, and a frame the client is obliged to close on
// must never reach a socket from here.
//
// The channel is `anvil:chat:wake:<32 lowercase hex of the user id>`, built in a
// fixed buffer.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <sw/redis++/redis++.h>

#include "anvil/core/types.h"

namespace anvil::chat {

inline constexpr std::string_view kWakeChannelPrefix = "anvil:chat:wake:";
inline constexpr std::size_t kWakeChannelBytes = kWakeChannelPrefix.size() + 32;

// The channel name for one user, in a stack buffer.
struct WakeChannel final {
    std::array<char, kWakeChannelBytes> chars;

    explicit WakeChannel(const Uuid& user) noexcept;
    [[nodiscard]] std::string_view view() const noexcept {
        return {chars.data(), chars.size()};
    }
};

// The user a channel name names, or nullopt when the name is not one this file
// produces. Lowercase hex only, exactly 32 digits: a channel the publisher could
// not have written is not parsed generously.
[[nodiscard]] std::optional<Uuid> parse_wake_channel(std::string_view channel) noexcept;

// --- publishing ------------------------------------------------------------------

// The upper bound on recipients per publish: the largest conversation a kind
// may declare (kind_spec.h). A caller with more is a caller with a bug, and the
// call is refused whole rather than truncated, because a truncated fan-out is a
// silent latency fault for exactly the members past the cut.
inline constexpr std::size_t kMaxWakeRecipients = 1024;

class WakePublisher final {
public:
    // `redis` must outlive the publisher. In an application it is
    // RedisClient::instance(); a test points one at a closed port.
    explicit WakePublisher(sw::redis::Redis& redis) noexcept;

    // One SPUBLISH per recipient, all in ONE pipelined round trip: a 1 024-member
    // group is one network wait rather than 1 024 of them on the send path.
    //
    // BLOCKING — a Redis round trip — so it runs where the send already is, on
    // db_pool, after the commit. Never on an event-loop thread.
    //
    // NEVER THROWS. Every failure is logged and counted and the call returns:
    // the message is committed and a wake is only its fast path (§5.4). Returns
    // how many wakes Redis accepted; a frame that is not a well-formed
    // downstream frame, a nil recipient, or more than kMaxWakeRecipients are
    // refused and counted as failed, because the bug is the caller's and a
    // subscriber would drop the frame anyway.
    std::size_t publish(std::span<const Uuid> recipients,
                        std::span<const std::uint8_t> frame) noexcept;

    // Plain atomics rather than rows in analytics/internal_metrics.h: that table
    // is a documented seam (docs/17-analytics.md §3) and growing it is its own
    // change. Relaxed reads; these are counters, not synchronisation.
    [[nodiscard]] std::uint64_t published() const noexcept {
        return published_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t publish_failed() const noexcept {
        return publish_failed_.load(std::memory_order_relaxed);
    }

private:
    sw::redis::Redis& redis_;
    std::atomic<std::uint64_t> published_;
    std::atomic<std::uint64_t> publish_failed_;
};

// --- subscribing -----------------------------------------------------------------

struct WakeSubscriberConfig final {
    // The subscriber's OWN connection, separate from RedisClient's pool: a
    // subscribed connection can issue nothing but (un)subscribe commands, so it
    // can never be a pool connection.
    std::string url;
    std::chrono::milliseconds connect_timeout{500};
    // The socket timeout of that connection, and therefore how long a queued
    // subscribe or unsubscribe can wait before the thread sees it, and how long
    // stop() can wait for the thread. The thread wakes this often while idle;
    // at 100 ms that is ten empty reads a second, which is nothing.
    std::chrono::milliseconds poll_interval{100};
    // Reconnect backoff after a lost connection: doubled per failed attempt from
    // the first value to the cap, and reset by a connection that subscribed.
    // Without it a down Redis turns this thread into a spinning core.
    std::chrono::milliseconds reconnect_initial{100};
    std::chrono::milliseconds reconnect_max{5000};
    // CLIENT SETNAME for that connection, so an operator reading CLIENT LIST
    // can tell which process holds which subscriber. Empty sets none.
    std::string client_name;
};

// Called on the subscriber thread with a validated downstream frame for `user`.
//
// It MUST NOT BLOCK: it runs on the one thread that reads every wake for this
// process, so a slow callback delays every user's wakes behind it. The hub's
// callback finds the user's sockets and copies the frame into their rings.
//
// LIFETIME: `frame` is valid only for the duration of the call. A callback that
// keeps the bytes past its return — queues them, posts them to a loop thread —
// copies them first (CLAUDE.md §2.2). An exception from it is caught, logged
// and the frame counted as delivered; it never reaches the thread.
using WakeDelivery =
    std::function<void(const Uuid& user, std::span<const std::uint8_t> frame)>;

// Called on the subscriber thread when Redis CONFIRMS a user's subscription —
// the first time, and again after every reconnect. From that moment wakes for
// the user flow; anything published before it may have been lost. The hub uses
// it to tell that user's sockets to sync, which closes the window between a
// socket's initial sync and its subscription taking effect, and the window of a
// Redis outage. Same rules as WakeDelivery: must not block, exceptions caught.
// May be empty.
using WakeSubscribed = std::function<void(const Uuid& user)>;

// One dedicated thread — not a pool task, because a subscribe loop never
// returns, and a pool slot held forever is a pool one slot smaller for every
// other task (CLAUDE.md §4).
//
// THREADING. subscribe() and unsubscribe() are called from event-loop threads
// and never block on Redis: they record the change in a small map under a mutex
// that is never held across I/O, and the subscriber thread applies it at its
// next poll. The reference counts themselves belong to the subscriber thread
// alone, so the delivery path reads them with no lock.
class WakeSubscriber final {
public:
    // Starts the thread. Throws std::invalid_argument on a URL that does not
    // parse, which is configure time, not request time.
    WakeSubscriber(WakeSubscriberConfig config, WakeDelivery deliver,
                   WakeSubscribed on_subscribed);
    ~WakeSubscriber();

    WakeSubscriber(const WakeSubscriber&) = delete;
    WakeSubscriber& operator=(const WakeSubscriber&) = delete;
    WakeSubscriber(WakeSubscriber&&) = delete;
    WakeSubscriber& operator=(WakeSubscriber&&) = delete;

    // Reference-counted per user: the first subscribe SSUBSCRIBEs the user's
    // channel and the matching last unsubscribe SUNSUBSCRIBEs it, so a user
    // with three sockets on this process is one subscription. Each call takes
    // effect within one poll interval; an unsubscribe with no matching
    // subscribe is ignored. Never blocks on I/O; throws only std::bad_alloc.
    void subscribe(const Uuid& user);
    void unsubscribe(const Uuid& user);

    // Stops and joins the thread. Idempotent; the destructor calls it. After it
    // returns no callback is running and none will run.
    void stop() noexcept;

    [[nodiscard]] std::uint64_t delivered() const noexcept {
        return delivered_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dropped_malformed() const noexcept {
        return dropped_malformed_.load(std::memory_order_relaxed);
    }

private:
    void run() noexcept;
    void on_message(std::string_view channel, std::string_view payload) noexcept;
    void on_confirmed(std::string_view channel) noexcept;

    // Declared largest first, then by lifetime; the thread is LAST so it is
    // constructed after everything it reads and joined (in the destructor's
    // stop()) before any of it is destroyed.
    sw::redis::ConnectionOptions connection_;
    WakeSubscriberConfig config_;
    WakeDelivery deliver_;
    WakeSubscribed on_subscribed_;

    // Written by callers under pending_mutex_, drained by the thread. A map of
    // net deltas, not a log of calls: a socket that opens and closes inside one
    // poll is no Redis traffic at all, and the size is bounded by distinct users
    // touched per poll rather than by calls.
    std::mutex pending_mutex_;
    std::map<Uuid, std::int64_t> pending_;

    // The subscriber thread's alone.
    std::map<Uuid, std::uint32_t> refs_;

    std::atomic<std::uint64_t> delivered_;
    std::atomic<std::uint64_t> dropped_malformed_;
    std::atomic<bool> running_;
    std::thread thread_;
};

}  // namespace anvil::chat
