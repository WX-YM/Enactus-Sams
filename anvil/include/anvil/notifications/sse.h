#pragma once

// Live notification streams: the registry, the per-connection ring, and the
// ceiling. Not the HTTP.
//
// What is here is the part that has to be right under concurrency and the part
// that decides how much memory a slow client can cost. Writing bytes to a socket
// is the application's, through whatever its framework gives it — which also
// makes every property below testable without one.
//
// --- a full ring DROPS THE CONNECTION, it does not buffer --------------------
//
// A client that stops reading is a client whose events accumulate somewhere. The
// only question is where, and every answer except "nowhere" is a memory leak with
// a network trigger: an unbounded per-connection queue is one slow reader turning
// into the process's whole heap, and a shared overflow buffer is the same thing
// with more steps.
//
// So each connection gets a FIXED ring, and a push that would overwrite an
// unread event closes the connection instead. That is safe — not merely
// tolerable — because the stream is a latency optimisation and NOT the system of
// record: the inbox is. A dropped client reconnects and reads its inbox, which is
// the same thing it does after a deploy, a network blip, or a laptop lid. The
// worst case is one extra page read; the alternative's worst case is the process.
//
// Overwriting the OLDEST event instead was the other candidate and is worse in a
// way that is invisible: the connection stays up, the client believes it is
// current, and it has silently missed something. A closed connection is a
// condition the client already handles.
//
// --- the ceiling comes from RLIMIT_NOFILE -----------------------------------
//
// An SSE connection holds a file descriptor for its entire life, so the real
// limit on concurrent streams is the process's descriptor budget and not a number
// anybody picked. Deriving it means a deployment that raises its limit gets the
// streams, and one that does not gets a clean 429 at the door rather than an
// `accept()` failure that takes down every OTHER kind of request at the same
// time — including the ones the client would use to recover.
//
// --- threading --------------------------------------------------------------
//
// `deliver()` is called from a job worker; `drain()` from whichever thread is
// writing the response. Both are safe to call concurrently, and neither blocks on
// anything but a short mutex — no I/O happens under any lock here (CLAUDE.md §4).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include "anvil/core/descriptor_budget.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/notifications/topic_spec.h"

namespace anvil::notifications {

// Process-local and monotonic. Not a Uuid: it identifies a connection for the
// life of that connection and nothing outside this process ever sees it, so
// sixteen bytes and a CSPRNG call would buy nothing.
using StreamId = std::uint64_t;

enum class SseEventKind : std::uint8_t {
    // A notification arrived. Carries its id so the client can fetch or match it.
    Notification = 0,
    // The unread count moved without a new notification — another device marked
    // something read. A badge that only ever counts up is a badge that lies.
    Unread = 1,
    // Keeps an idle connection alive through proxies that reap silent ones.
    // Carries no notification.
    Ping = 2,
    // A chat message the reader may see arrived in a conversation, carried for a
    // client whose proxy breaks WebSocket upgrades (docs/22-chat.md §8.1).
    // `notification` holds the CONVERSATION's id and nothing else is set: the
    // client catches that conversation up from its cursor. Never the message and
    // never its seq — a slot is 32 bytes, and growing it for chat would grow
    // every notification stream in the process.
    ChatWake = 3,
    // Chat wakes may have been lost: catch every conversation up from its cursor.
    // Sent when the reader's wake channel is (re)subscribed (chat/frames.h, Sync).
    ChatSync = 4,
};

// 32 bytes, trivially copyable, ordered largest-first so there is no interior
// padding. A ring of these is a flat array the CPU prefetches through, and the
// size is asserted because a field added carelessly doubles what every open
// connection costs.
struct SseEvent final {
    Uuid                        notification;   // 16
    // Monotonic PER STREAM. It is the SSE `id:` field, so a reconnecting client
    // sends it back as Last-Event-ID and the application can tell how far behind
    // it was — including, when the gap is wider than the ring, that it should
    // re-read the inbox rather than trust the stream.
    std::int64_t                sequence;       //  8
    std::int32_t                unread;         //  4
    TopicCode                   kind;           //  1
    SseEventKind                type;           //  1
    std::array<std::uint8_t, 2> reserved;       //  2
};

static_assert(sizeof(SseEvent) == 32, "SseEvent must not grow padding");
static_assert(std::is_trivially_copyable_v<SseEvent>);

// How far behind a client may fall before it is dropped.
//
// 64 events × 32 bytes = 2 KiB per open connection, so ten thousand streams is
// twenty megabytes of ring and that is the whole exposure. The number is a
// LATENCY budget rather than a correctness one: a client that has not drained 64
// events has been unresponsive for long enough that re-reading its inbox is
// cheaper than continuing to track what it missed.
inline constexpr std::size_t kStreamRingSlots = 64;

// What a connection can hand back in one drain. Equal to the ring, because a
// drain that could not empty the ring would leave a connection permanently one
// burst away from being dropped.
inline constexpr std::size_t kMaxDrain = kStreamRingSlots;

// One connection's queue.
//
// Pushed by whichever worker delivered the notification, drained by whichever
// thread is writing the response. The mutex is held for a fixed-size copy and
// nothing else — never across the write.
class SseStream final {
public:
    SseStream(StreamId id, const Uuid& reader) noexcept
        : reader_{reader}, id_{id} {}

    // False when the ring is FULL, which is the caller's signal to close this
    // connection rather than to retry. See the header comment: a full ring is a
    // client that stopped reading, and there is no safe amount of memory to
    // spend waiting for it to start again.
    //
    // Also false once the stream is closed, so a delivery racing a disconnect is
    // a no-op rather than a resurrection.
    [[nodiscard]] bool push(const SseEvent& event) noexcept;

    // Copies out everything queued, oldest first, and returns how many. The
    // caller writes them to its socket AFTER this returns — the lock is not held
    // across the write, so a slow socket cannot block a delivery thread.
    [[nodiscard]] std::size_t drain(std::span<SseEvent> out) noexcept;

    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] std::size_t queued() const noexcept;
    [[nodiscard]] const Uuid& reader() const noexcept { return reader_; }
    [[nodiscard]] StreamId id() const noexcept { return id_; }

    // Invoked after a successful push, OUTSIDE the lock, so the connection's
    // writer can be woken. It must not block and must not call back into this
    // stream: the intended body posts a drain onto an event loop and returns.
    //
    // Set once at open and not changed afterwards, which is what makes it safe to
    // read without synchronisation.
    void on_event(std::function<void(StreamId)> notify) { notify_ = std::move(notify); }

    SseStream(const SseStream&) = delete;
    SseStream& operator=(const SseStream&) = delete;

private:
    // Declaration order is initialisation order (CLAUDE.md §3.2). The ring is
    // first because it is the largest and the hottest.
    std::array<SseEvent, kStreamRingSlots> ring_{};
    mutable std::mutex                     mutex_;
    std::function<void(StreamId)>          notify_;
    const Uuid                             reader_;
    const StreamId                         id_;
    std::int64_t                           sequence_{0};
    std::size_t                            head_{0};    // next slot to write
    std::size_t                            count_{0};   // unread slots
    bool                                   closed_{false};
};

using StreamPtr = std::shared_ptr<SseStream>;

struct SseLimits final {
    // Concurrent streams across the process. Zero means "derive it", which is
    // what a deployment should do — see `descriptor_ceiling` and `kStreamShare`.
    std::size_t max_streams{0};
    // Per reader, so one account cannot hold the whole ceiling open from a
    // hundred tabs and shut every other reader out. A small number: the legitimate
    // case is a phone, a laptop and a spare tab.
    std::size_t max_per_reader{4};
};

// The ceiling, derived rather than picked. Both the budget and the share streams
// are allowed to take are in `core/descriptor_budget.h`, because streams are no
// longer the only thing in this library that holds a connection open and two
// subsystems deriving their ceilings independently spend the same descriptors
// twice.

// The registry.
//
// Two indexes, because both lookups are on a hot path and neither can afford to
// be a scan: delivery finds every stream a reader has open, and a disconnect
// finds one stream by id.
class SseHub final {
public:
    // `limits.max_streams` of zero derives the ceiling from RLIMIT_NOFILE. That
    // is the intended production configuration; an explicit number exists so a
    // test can assert the ceiling's behaviour without reshaping the process's
    // descriptor limit.
    explicit SseHub(SseLimits limits) noexcept;

    // RateLimited when the process ceiling or the per-reader ceiling is reached.
    // The caller answers 429 — a refusal at the door, while the rest of the
    // application still works, which is the whole point of having a ceiling that
    // is lower than the descriptor limit.
    [[nodiscard]] Result<StreamPtr> open(const Uuid& reader);

    // Idempotent: a connection that is closed twice — once by the client
    // disconnecting and once by the writer noticing — is the normal case, not an
    // error.
    void close(StreamId id) noexcept;

    // Fan one event to every stream this reader has open, and CLOSE any whose
    // ring is full. Returns how many streams accepted it.
    //
    // The registry lock is released before any push, so a stream's own mutex is
    // never taken while the registry's is held — the one ordering that could
    // deadlock against a concurrent open or close.
    std::size_t deliver(const Uuid& reader, const SseEvent& event);

    // Every open stream, for a ping sweep. Same lock discipline as deliver.
    std::size_t broadcast_ping(std::int32_t unread);

    [[nodiscard]] std::size_t open_streams() const noexcept;
    [[nodiscard]] std::size_t ceiling() const noexcept { return ceiling_; }

    SseHub(const SseHub&) = delete;
    SseHub& operator=(const SseHub&) = delete;

private:
    [[nodiscard]] std::vector<StreamPtr> streams_of(const Uuid& reader) const;

    // Declaration order is initialisation order. The limits and the derived
    // ceiling are const and come first.
    const SseLimits                        limits_;
    const std::size_t                      ceiling_;
    mutable std::shared_mutex              mutex_;
    // Keyed by reader, because delivery is per reader and a scan over every open
    // connection would make one notification O(all connections).
    std::map<Uuid, std::vector<StreamPtr>> by_reader_;
    std::map<StreamId, StreamPtr>          by_id_;
    StreamId                               next_id_{1};
};

}  // namespace anvil::notifications
