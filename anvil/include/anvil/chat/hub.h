#pragma once

// The chat hub: which sockets this process holds, and what each one still owes
// its client (docs/22-chat.md §8.2).
//
// docs/04 §8.5 refused to ship a connection registry and an outbound ring for
// WebSockets, because anvil had no producer to write into one. Chat is that
// producer — every send publishes wakes, and the wake subscriber has to hand
// them to somebody — so for chat the refusal is reversed, for the reason it
// gave. The generic refusal stands: an application's own socket still gets the
// budget and the re-check (http/upgrade.h) and nothing else.
//
// Like notifications/sse.h, this is the part that has to be right under
// concurrency and the part that decides what a slow client costs. It holds no
// socket. Writing bytes is the socket layer's (chat/socket.h), so every
// property here is testable without a network.
//
// --- a full ring DROPS THE CONNECTION ------------------------------------------
//
// SSE's policy, for SSE's reason. A client that stops reading accumulates its
// frames somewhere, and every somewhere except "nowhere" is memory with a
// network trigger. So each socket has a fixed ring, and a frame that does not
// fit closes the socket instead of waiting. That is safe because the socket is
// a latency optimisation and the log is the record: a dropped client reconnects
// and asks `seq > cursor`, which is what it does after a deploy or a tunnel.
//
// The ring bounds what waits for the event loop. What waits BEYOND it — in
// Trantor's output buffer and the kernel's — is invisible here, because Drogon
// exposes no write-buffer level for a WebSocket. The socket layer bounds that
// with a liveness deadline instead: a client that does not answer the server's
// ping is closed, and a client that is not reading cannot answer one.
//
// --- one socket per device -------------------------------------------------------
//
// A device is a session: the session id the handshake's token carries. A second
// socket from the same device closes the first with a reason the client can
// tell from a network fault (kCloseReplaced), so a tab that lost the socket to
// another tab does not reconnect and take it back. It is also how a stuck
// socket is replaced — the client opens a new one and the old one goes.
//
// --- threading ------------------------------------------------------------------
//
// open() and close() run on event-loop threads; deliver() on the wake
// subscriber's thread; drain() on the socket's own loop. All are safe
// concurrently. No I/O happens under any lock here, and a socket's own mutex is
// NEVER taken while the registry's is held (docs/11 §6): a socket closed by the
// hub is closed after the registry lock is released.
//
// The subscription callbacks are the one thing called UNDER the registry lock,
// on purpose. They record a reference count change in the subscriber's own map
// and touch no network (chat/wakes.h), and calling them in the same critical
// section as the registry change keeps a user's first-socket subscribe and
// last-socket unsubscribe in the order the sockets opened and closed. Called
// after the unlock, two threads could deliver them reversed, and the user would
// be left subscribed with no socket, or a socket with no subscription.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include "anvil/chat/frames.h"
#include "anvil/core/descriptor_budget.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::chat {

// Process-local and monotonic, for the reason sse.h's StreamId is: nothing
// outside this process ever sees it.
using SocketId = std::uint64_t;

// How many bytes of frames one socket may have waiting for its loop.
//
// Eight KiB: at least three of the largest frames (a wake carrying a 2 KiB
// message), or about two hundred and ninety of the commonest (a 28-byte wake
// that says "fetch it"). Ten thousand sockets is 80 MiB of ring at worst, and
// that is the whole exposure. Like SSE's, the number is a LATENCY budget: a
// socket whose loop has not drained eight kilobytes is a socket whose client is
// better served by reconnecting and syncing.
inline constexpr std::size_t kSocketRingBytes = 8192;

// Every frame is stored behind a two-byte length.
inline constexpr std::size_t kSocketRecordHeaderBytes = 2;
static_assert(frames::kMaxDownstreamFrameBytes + kSocketRecordHeaderBytes <= kSocketRingBytes);
static_assert(frames::kMaxDownstreamFrameBytes <= 0xFFFF, "the record length is a u16");

// Why the hub closed a socket. The socket layer turns it into a close code.
enum class CloseReason : std::uint8_t {
    // Not closed.
    None = 0,
    // The client went away, or the socket layer closed it for its own reasons
    // (a bad frame, the budget, the re-check). Nothing is sent for this one; the
    // socket layer already decided what to send.
    Gone = 1,
    // Another socket from the same device opened.
    Replaced = 2,
    // The ring was full: the client stopped reading.
    Overflow = 3,
};

// One socket's ring.
//
// Pushed by whichever thread delivers a frame; drained by the socket's loop.
// The mutex is held for a bounded copy and nothing else, never across a write.
class ChatSocket final {
public:
    ChatSocket(SocketId id, const Uuid& user, const Uuid& device) noexcept
        : user_{user}, device_{device}, id_{id} {}

    ChatSocket(const ChatSocket&) = delete;
    ChatSocket& operator=(const ChatSocket&) = delete;

    // Queues one frame. False when it does not fit — the caller's signal to
    // close this socket, never to retry — or when the socket is closed, so a
    // delivery racing a close is a no-op rather than a resurrection. A frame
    // that is empty or larger than a downstream frame can be is refused too: the
    // bug is the caller's, and the client would close on it.
    [[nodiscard]] bool push(std::span<const std::uint8_t> frame) noexcept;

    // Copies out every queued record — `u16 length ‖ frame`, oldest first — and
    // returns the bytes written. The caller writes the frames AFTER this
    // returns. `out` holds a full ring, so one drain always empties it.
    [[nodiscard]] std::size_t drain(std::span<std::uint8_t, kSocketRingBytes> out) noexcept;

    // Closes the ring, drops whatever it held, and records the FIRST reason it
    // was closed for. Returns whether this call was the one that closed it.
    bool close(CloseReason reason) noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] CloseReason close_reason() const noexcept;
    [[nodiscard]] std::size_t queued_bytes() const noexcept;
    [[nodiscard]] const Uuid& user() const noexcept { return user_; }
    [[nodiscard]] const Uuid& device() const noexcept { return device_; }
    [[nodiscard]] SocketId id() const noexcept { return id_; }

    // Invoked OUTSIDE the lock when the socket's writer has something to do: a
    // push into an empty ring, or a close. A push into a ring that already held
    // something does not invoke it again — the drain that the first push asked
    // for takes everything. The intended body posts a drain onto the socket's
    // loop and returns; it must not block and must not call back into this
    // socket.
    //
    // Set once, before the socket is registered, and never changed: that is
    // what makes reading it without the lock safe.
    void on_ready(std::function<void(SocketId)> notify) { notify_ = std::move(notify); }

private:
    // Declaration order is initialisation order; the ring is first because it
    // is the largest and the hottest.
    std::array<std::uint8_t, kSocketRingBytes> ring_{};
    mutable std::mutex                         mutex_;
    std::function<void(SocketId)>              notify_;
    const Uuid                                 user_;
    const Uuid                                 device_;
    const SocketId                             id_;
    std::size_t                                head_{0};   // next byte to write
    std::size_t                                count_{0};  // bytes queued
    CloseReason                                reason_{CloseReason::None};
};

using SocketPtr = std::shared_ptr<ChatSocket>;

struct HubLimits final {
    // Concurrent sockets across the process. Zero derives the ceiling from
    // RLIMIT_NOFILE through kUpgradeShare, which is what a deployment should
    // do; a number exists so a test can reach the ceiling.
    std::size_t max_sockets{0};
    // Per account, counting one per device. A small number: the legitimate case
    // is a phone, a laptop and a browser, and the ceiling is shared by everyone.
    // The device directory's max_devices is the natural value where an
    // application has one (chat/devices.h).
    std::size_t max_per_account{5};
};

// The subscriber's half of a socket's life: the first socket a user opens on
// this process subscribes their wake channel and the last one to close
// unsubscribes it. Both must be cheap and must not block (see the header).
// Either may be empty, which a test without Redis uses.
struct HubSubscriptions final {
    std::function<void(const Uuid&)> subscribe;
    std::function<void(const Uuid&)> unsubscribe;
};

class ChatHub final {
public:
    ChatHub(HubLimits limits, HubSubscriptions subscriptions);

    ChatHub(const ChatHub&) = delete;
    ChatHub& operator=(const ChatHub&) = delete;

    // Registers a socket for `user` on `device`, closing (Replaced) any socket
    // that device already had. RateLimited when the process ceiling or the
    // account's is reached — a replacement does not count against either,
    // because it frees the slot it takes. `notify` is set on the socket before
    // anybody else can see it.
    [[nodiscard]] Result<SocketPtr> open(const Uuid& user, const Uuid& device,
                                         std::function<void(SocketId)> notify);

    // Removes the socket and closes its ring with `reason` (Gone for a socket
    // the socket layer is closing itself). Idempotent: a socket closed by the
    // hub and then reported closed by its connection is the normal case.
    void close(SocketId id, CloseReason reason) noexcept;

    // Queues one downstream frame on every socket `user` has open, and closes
    // (Overflow) any whose ring it does not fit. Returns how many accepted it.
    // `frame` is copied before this returns. The registry lock is released
    // before any push.
    std::size_t deliver(const Uuid& user, std::span<const std::uint8_t> frame);

    // Every open socket of `user` gets a Sync frame: the wake subscriber
    // confirmed their channel, and anything published before that is lost.
    std::size_t resync(const Uuid& user);

    // Whether `user` has a socket open on THIS process. A single process's
    // answer and never the system's; push nudges must not rely on it.
    [[nodiscard]] bool has_socket(const Uuid& user) const;

    [[nodiscard]] std::size_t open_sockets() const noexcept;
    [[nodiscard]] std::size_t ceiling() const noexcept { return ceiling_; }
    [[nodiscard]] std::uint64_t overflowed() const noexcept;

private:
    [[nodiscard]] std::vector<SocketPtr> sockets_of(const Uuid& user) const;

    // Declaration order is initialisation order.
    const HubLimits                        limits_;
    const std::size_t                      ceiling_;
    const HubSubscriptions                 subscriptions_;
    mutable std::shared_mutex              mutex_;
    // Keyed by user, because delivery is per user; at most max_per_account
    // sockets each, so a vector scan is the cheap search.
    std::map<Uuid, std::vector<SocketPtr>> by_user_;
    std::map<SocketId, SocketPtr>          by_id_;
    SocketId                               next_id_{1};
    // A counter, not synchronisation: relaxed, and outside the registry lock.
    std::atomic<std::uint64_t>             overflowed_{0};
};

}  // namespace anvil::chat
