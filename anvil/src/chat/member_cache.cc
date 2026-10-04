#include "anvil/chat/member_cache.h"

#include <algorithm>
#include <utility>

#include "anvil/chat/wakes.h"

namespace anvil::chat {

// The cache refuses exactly what the publisher would refuse, so a list the
// cache hands out is never one a publish then turns down whole.
static_assert(kMaxWakeRecipients == kMaxMembers);

MemberCache::MemberCache(std::size_t max_bytes) noexcept : max_bytes_{max_bytes} {}

std::size_t MemberCache::cost(const MemberList& list) noexcept {
    return (list.users.capacity() * sizeof(Uuid)) + kMemberCacheEntryOverheadBytes;
}

Result<std::shared_ptr<const MemberList>> MemberCache::recipients(
    mongocxx::client& client, const ChatRepository& repository, const Uuid& conversation,
    std::int64_t membership_version, std::int64_t seq) {
    {
        const std::lock_guard lock{mutex_};
        const auto found = slots_.find(conversation);
        // EQUAL, not "at least": an entry read at a newer version than this
        // send's allocate saw can hold somebody who joined after this message,
        // and an older one can hold somebody who has left.
        if (found != slots_.end() &&
            found->second.list->membership_version == membership_version) {
            recency_.splice(recency_.begin(), recency_, found->second.recency);
            ++hits_;
            return found->second.list;
        }
        ++misses_;
    }

    // Outside the lock: a read held under it would make every send in the
    // process wait on one conversation's round trip.
    //
    // One past the bound, so "exactly at the bound" and "over it" are told
    // apart in one read rather than by a count first.
    const Result<std::vector<ChatRepository::MemberId>> read =
        repository.member_ids(client, conversation, static_cast<std::int32_t>(kMaxMembers) + 1);
    if (!read) { return read.error(); }
    const std::vector<ChatRepository::MemberId>& rows = read.value();
    if (rows.size() > kMaxMembers) { return fail(ErrorCode::PayloadTooLarge); }

    auto built = std::make_shared<MemberList>();
    built->membership_version = membership_version;
    built->users.reserve(rows.size());
    for (const ChatRepository::MemberId& row : rows) {
        // Read after the allocate, so a member who joined since is in the read
        // and cannot see this message (see the header).
        if (row.joined_seq <= seq) { built->users.push_back(row.user); }
    }
    built->users.shrink_to_fit();
    std::shared_ptr<const MemberList> list = std::move(built);
    keep(conversation, list);
    return list;
}

void MemberCache::keep(const Uuid& conversation, std::shared_ptr<const MemberList> list) {
    const std::size_t bytes = cost(*list);
    // Larger than the whole budget: answered, never kept, so one oversized
    // entry cannot evict everything else and then not fit anyway.
    if (bytes > max_bytes_) { return; }

    const std::lock_guard lock{mutex_};
    const auto found = slots_.find(conversation);
    if (found != slots_.end()) {
        // A send whose allocate saw an OLDER version than the entry already
        // here does not replace it: the newer one is the one later sends will
        // ask for. Equal is a concurrent miss that read the same thing.
        if (found->second.list->membership_version >= list->membership_version) { return; }
        bytes_ -= found->second.bytes;
        recency_.erase(found->second.recency);
        slots_.erase(found);
    }
    evict_to(max_bytes_ - bytes);
    recency_.push_front(conversation);
    slots_.emplace(conversation, Slot{std::move(list), recency_.begin(), bytes});
    bytes_ += bytes;
}

void MemberCache::evict_to(std::size_t budget) noexcept {
    while (bytes_ > budget && !recency_.empty()) {
        const auto found = slots_.find(recency_.back());
        if (found != slots_.end()) {
            bytes_ -= found->second.bytes;
            slots_.erase(found);
        }
        recency_.pop_back();
    }
}

std::uint64_t MemberCache::hits() const noexcept {
    const std::lock_guard lock{mutex_};
    return hits_;
}

std::uint64_t MemberCache::misses() const noexcept {
    const std::lock_guard lock{mutex_};
    return misses_;
}

std::size_t MemberCache::bytes() const noexcept {
    const std::lock_guard lock{mutex_};
    return bytes_;
}

std::size_t MemberCache::entries() const noexcept {
    const std::lock_guard lock{mutex_};
    return slots_.size();
}

}  // namespace anvil::chat
