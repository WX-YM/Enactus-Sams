#pragma once

// Who a message wakes: the per-process member cache (docs/22-chat.md §5.4).
//
// Every send publishes one wake per current member, so every send needs the
// member list. Reading it from MongoDB per send is a 1 024-row read on the
// send path of a busy group, which is the cost a cache exists to remove. The
// question a cache has to answer is when it is wrong.
//
// --- validated by `mv`, at no cost --------------------------------------------
//
// Every membership change $incs the conversation's `mv` in its own transaction,
// and step 3 of a send (the seq's $inc, ChatRepository::allocate) reads `mv` back
// in the same operation. So the version the list must be at is already in hand
// when the list is wanted, and an entry is used only when its version EQUALS it.
// A hit costs no round trip, and there is no TTL to tune: an entry is exactly as
// fresh as the version it was read at.
//
// That is the property the row's case is about. A member removed on another
// process bumps `mv` with the removal; the next send's allocate returns the new
// version; the entry no longer matches and is read again without them. No
// invalidation crosses processes, because none needs to.
//
// --- a joiner is not woken for what they cannot see ---------------------------
//
// A miss reads the list AFTER the allocate, so it can include somebody who
// joined between the two. Under History::FromJoin that person cannot see the
// message, and a wake can carry the message inline — so the list is filtered to
// members whose joined seq is at or below the message's seq when it is read.
// Under History::Full a joiner's js is 0 and they pass the filter, correctly:
// they can read the message from history anyway.
//
// The entry is tagged with the version the ALLOCATE saw, never a newer one, so
// a list read late is at worst a list the next send reads again.
//
// --- bounded in bytes, not entries ---------------------------------------------
//
// One entry is 16 bytes per member: a direct conversation is 32 bytes and a
// 1 024-member group is 16 KiB. A count of entries would bound neither the
// best nor the worst case, so the bound is the bytes, and the least recently
// used entries go first. A conversation with more than kMaxMembers current
// members is refused rather than cached: nothing that size is woken per member
// (kMaxWakeRecipients in chat/wakes.h is the same number), which is why a
// channel's followers are not woken at all.
//
// --- threading ------------------------------------------------------------------
//
// Called from db_pool tasks, concurrently. An entry is immutable once built and
// handed out as shared_ptr<const>, so a reader iterating one holds no lock. The
// mutex guards the index and is never held across the read a miss makes: two
// sends that miss at once both read, and the second insert is a no-op.

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/chat/kind_spec.h"
#include "anvil/chat/repository.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::chat {

// What one entry costs besides its ids: the index node, the recency node, the
// shared_ptr's control block and the vector's header. An estimate that errs
// high, so the bound is never exceeded by the bookkeeping it does not count.
inline constexpr std::size_t kMemberCacheEntryOverheadBytes = 160;

// One conversation's current members, as of one membership version. Immutable.
struct MemberList final {
    std::vector<Uuid> users;
    std::int64_t      membership_version;
};

class MemberCache final {
public:
    // `max_bytes` bounds every entry's ids plus its overhead. An entry larger
    // than the whole bound is answered and not kept.
    explicit MemberCache(std::size_t max_bytes) noexcept;

    MemberCache(const MemberCache&) = delete;
    MemberCache& operator=(const MemberCache&) = delete;

    // The current members of `conversation` who can see `seq`, at
    // `membership_version` — the two numbers the send's allocate returned.
    //
    // A hit is a lock and a copy of one shared_ptr. A miss is one read, so this
    // BLOCKS on a miss and runs on db_pool only. PayloadTooLarge when the
    // conversation has more current members than kMaxMembers; the read's own
    // failure otherwise.
    [[nodiscard]] Result<std::shared_ptr<const MemberList>> recipients(
        mongocxx::client& client, const ChatRepository& repository, const Uuid& conversation,
        std::int64_t membership_version, std::int64_t seq);

    [[nodiscard]] std::uint64_t hits() const noexcept;
    [[nodiscard]] std::uint64_t misses() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] std::size_t entries() const noexcept;

private:
    struct Slot final {
        std::shared_ptr<const MemberList> list;
        std::list<Uuid>::iterator         recency;
        std::size_t                       bytes;
    };

    [[nodiscard]] static std::size_t cost(const MemberList& list) noexcept;
    void keep(const Uuid& conversation, std::shared_ptr<const MemberList> list);
    void evict_to(std::size_t budget) noexcept;

    // Declaration order is initialisation order. Everything below the mutex is
    // guarded by it.
    const std::size_t        max_bytes_;
    mutable std::mutex       mutex_;
    std::map<Uuid, Slot>     slots_;
    // Most recently used at the front.
    std::list<Uuid>          recency_;
    std::size_t              bytes_{0};
    std::uint64_t            hits_{0};
    std::uint64_t            misses_{0};
};

}  // namespace anvil::chat
