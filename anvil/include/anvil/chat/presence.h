#pragma once

// Presence: "online" and "last seen" (docs/22-chat.md §8.3).
//
// The feature most often turned into a stalking tool, so it is OFF unless the
// application turns it on, and even then every read is answered per viewer
// through the application's hook. Off means off: no Redis key is written, no
// row is stored, and the read route answers the stealth 404.
//
// --- what is stored, and where --------------------------------------------------
//
// One Redis key per account, `anvil:chat:seen:<32 hex>`, holding a signed
// millisecond timestamp: positive is "online, heard from at", negative is
// "offline since". The process that holds the account's sockets refreshes it,
// in ONE pipelined round trip per refresh for every account it holds, not one
// per socket; a first socket marks the account online at once and a last one
// marks it offline at once, rather than at the next refresh. Online is a fresh
// positive value, so an account whose process died goes offline by itself
// within `online_window`.
//
// Last seen is that key's last value. It is also written to MongoDB, at most
// once every `last_seen_write` per account, and ONLY when the hook says that
// anybody at all may see the account's presence: a value nobody is shown is a
// value nobody needs stored. The key outlives an hour offline, and past that
// the read falls back to the row, which is at most `last_seen_write` behind.
//
// --- what is approximate, said once ------------------------------------------------
//
// An account with sockets on two processes goes "offline" when either process
// loses its last one, until the other's next refresh says otherwise. Presence
// is a hint shown as "online" or "last seen at", and it is wrong by at most a
// refresh interval; nothing in chat decides anything by it.
//
// The Presence downstream frame (chat/frames.h) stays in the grammar and is not
// sent: pushing a change to everyone allowed to see it needs a list of watchers
// per account, which is a presence registry by another name. A client asks the
// read route for the accounts on screen.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mongocxx/client.hpp>
#include <sw/redis++/redis++.h>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/db/collections.h"

namespace anvil::chat {

inline constexpr std::string_view kPresenceKeyPrefix = "anvil:chat:seen:";

// The most accounts one batch read may ask about: a chat-list page, which is
// the one screen that shows presence for many accounts at once (docs/22 §8.3).
// Each costs one ask of the application's hook, and the batch one MGET and at
// most one $in.
inline constexpr std::size_t kMaxPresenceBatch = 100;

// Whether `viewer` may see `subject`'s presence. With a NIL viewer, whether
// anybody at all may — the question that decides whether last seen is stored.
// BLOCKING is allowed (it may read the application's contacts), so it is asked
// on db_pool or the tracker's own thread, never on a loop. Unset: nobody may,
// which is what "off unless the application turns it on" means for a hook.
using PresenceHook =
    std::function<bool(mongocxx::client& client, const Uuid& viewer, const Uuid& subject)>;

struct PresenceConfig final {
    bool                     enabled{false};
    PresenceHook             may_see;
    // The application's collection for last seen, declared in its table
    // (docs/01-seams.md §4), and the databases its table maps it into; one
    // document per account, keyed by its id. Unused and unchecked while presence
    // is off.
    std::string_view         collection;
    db::DatabaseNames        databases{};
    // How often the Redis key of every account with a socket here is refreshed,
    // and how fresh it has to be to read as online. The window covers two
    // refreshes and a slow one.
    std::chrono::seconds     refresh{20};
    std::chrono::seconds     online_window{45};
    // How often, at most, an account's last seen is written to MongoDB.
    std::chrono::seconds     last_seen_write{300};
};

// One account's presence as one viewer may see it.
struct PresenceView final {
    // Absent when the viewer may not see it or it was never recorded; the two
    // are deliberately not told apart.
    std::optional<db::TimeMs> last_seen;
    bool                      online;
};

class PresenceTracker final {
public:
    // Starts the tracker's thread when presence is enabled. Throws
    // std::invalid_argument when it is enabled and `config.collection` is not in
    // the application's collection table — at boot, never on a request.
    PresenceTracker(PresenceConfig config, sw::redis::Redis& redis);
    ~PresenceTracker();

    PresenceTracker(const PresenceTracker&) = delete;
    PresenceTracker& operator=(const PresenceTracker&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }

    // From the hub, when an account's first socket on this process opens and
    // its last one closes. They record the change and return: no I/O, so they
    // are safe under the hub's registry lock. No-ops while presence is off.
    void came_online(const Uuid& user);
    void went_offline(const Uuid& user);

    // What `viewer` may see of `subject`. NotFound while presence is off. One
    // Redis read, and a MongoDB read when the key has gone; BLOCKING, so
    // db_pool. A viewer always sees themselves.
    [[nodiscard]] Result<PresenceView> view(mongocxx::client& client, const Uuid& viewer,
                                            const Uuid& subject) const;

    // view() for each of `subjects`, in their order, answered in one MGET and,
    // for the accounts whose key has gone, one $in, rather than a round trip
    // each. Every subject is still asked of the hook, and a withheld one reads
    // exactly as one never seen. NotFound while presence is off;
    // ValidationFailed past kMaxPresenceBatch. BLOCKING, so db_pool.
    [[nodiscard]] Result<std::vector<PresenceView>> view_many(
        mongocxx::client& client, const Uuid& viewer, std::span<const Uuid> subjects) const;

    // Stops and joins the thread. Idempotent.
    void stop() noexcept;

    [[nodiscard]] std::uint64_t last_seen_writes() const noexcept {
        return last_seen_writes_.load(std::memory_order_relaxed);
    }

private:
    void run() noexcept;
    // One pass: mark changes, refresh everyone held, store last seen where due.
    void pass(bool refresh_all) noexcept;

    // Declaration order is initialisation order; the thread is last.
    PresenceConfig                  config_;
    sw::redis::Redis&               redis_;
    std::string                     database_;
    std::mutex                      mutex_;
    std::condition_variable         wake_;
    // Changes since the last pass, the latest per account winning. Guarded.
    std::map<Uuid, bool>            pending_;
    bool                            stopping_{false};
    // The tracker thread's alone.
    std::set<Uuid>                  online_;
    std::map<Uuid, std::int64_t>    written_ms_;
    std::atomic<std::uint64_t>      last_seen_writes_{0};
    std::thread                     thread_;
};

}  // namespace anvil::chat
