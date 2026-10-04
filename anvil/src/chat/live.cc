#include "anvil/chat/live.h"

#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <trantor/utils/Logger.h>

#include "anvil/chat/frames.h"
#include "anvil/chat/service.h"
#include "render.h"

namespace anvil::chat {

ChatLive::ChatLive(ChatLiveConfig config, sw::redis::Redis& redis,
                   const ChatRepository& repository)
    : repository_{repository},
      sse_{config.sse},
      members_{config.member_cache_bytes},
      publisher_{redis},
      presence_{std::move(config.presence), redis},
      // The callbacks reach the subscriber, which is built after the hub. They
      // run only when a socket opens, which cannot happen before this
      // constructor has returned. A user's first socket here is also the
      // moment they come online, and their last the moment they leave; both
      // presence calls only record the change, so they are safe under the
      // hub's lock.
      hub_{config.hub,
           HubSubscriptions{.subscribe =
                                [this](const Uuid& user) {
                                    subscriber_.subscribe(user);
                                    presence_.came_online(user);
                                },
                            .unsubscribe =
                                [this](const Uuid& user) {
                                    subscriber_.unsubscribe(user);
                                    presence_.went_offline(user);
                                }}},
      subscriber_{std::move(config.subscriber),
                  [this](const Uuid& user, std::span<const std::uint8_t> frame) {
                      deliver(user, frame);
                  },
                  [this](const Uuid& user) { resync(user); }} {}

ChatLive::~ChatLive() {
    // Before any member is destroyed: the threads call the hub and the tracker.
    stop();
}

void ChatLive::stop() noexcept {
    subscriber_.stop();
    presence_.stop();
}

// --- the fallback --------------------------------------------------------------------

ChatLive::StreamLease::~StreamLease() {
    if (live_ != nullptr) { live_->release_stream(user_); }
}

ChatLive::StreamLease::StreamLease(StreamLease&& other) noexcept
    : live_{other.live_}, user_{other.user_} {
    other.live_ = nullptr;
}

ChatLive::StreamLease& ChatLive::StreamLease::operator=(StreamLease&& other) noexcept {
    if (this != &other) {
        if (live_ != nullptr) { live_->release_stream(user_); }
        live_ = other.live_;
        user_ = other.user_;
        other.live_ = nullptr;
    }
    return *this;
}

ChatLive::StreamLease ChatLive::follow_on_stream(const Uuid& user) {
    if (sse_ == nullptr) {
        throw std::logic_error{"ChatLive::follow_on_stream: no SseHub was configured"};
    }
    const std::lock_guard lock{streams_mutex_};
    ++streams_[user];
    // Per lease, not per first lease: the subscriber counts references itself,
    // and a socket for the same reader holds one of its own.
    subscriber_.subscribe(user);
    return StreamLease{this, user};
}

void ChatLive::release_stream(const Uuid& user) noexcept {
    const std::lock_guard lock{streams_mutex_};
    const auto found = streams_.find(user);
    if (found == streams_.end()) { return; }
    if (--found->second == 0) { streams_.erase(found); }
    try {
        subscriber_.unsubscribe(user);
    } catch (...) {
        // bad_alloc only. A missed unsubscribe leaves wakes arriving for a
        // reader nobody is holding a stream for, which the subscriber drops.
    }
}

bool ChatLive::streaming(const Uuid& user) const {
    const std::lock_guard lock{streams_mutex_};
    return streams_.contains(user);
}

void ChatLive::deliver(const Uuid& user, std::span<const std::uint8_t> frame) {
    (void)hub_.deliver(user, frame);
    if (sse_ == nullptr || !streaming(user)) { return; }
    // Already validated by the subscriber; decoded again only to learn which
    // of the two things a stream can say this is.
    const Result<frames::DownstreamFrame> decoded = frames::decode_downstream(frame);
    if (!decoded) { return; }
    notifications::SseEvent event{};
    if (const auto* wake = std::get_if<frames::Wake>(&decoded.value())) {
        event.notification = wake->conversation;
        event.type = notifications::SseEventKind::ChatWake;
    } else if (const auto* mutation = std::get_if<frames::Mutation>(&decoded.value())) {
        // The stream's one word for "look at this conversation": the client
        // catches up its seq cursor and its mutation cursor alike.
        event.notification = mutation->conversation;
        event.type = notifications::SseEventKind::ChatWake;
    } else if (std::holds_alternative<frames::Sync>(decoded.value())) {
        event.type = notifications::SseEventKind::ChatSync;
    } else {
        return;
    }
    (void)sse_->deliver(user, event);
}

void ChatLive::resync(const Uuid& user) {
    (void)hub_.resync(user);
    if (sse_ == nullptr || !streaming(user)) { return; }
    notifications::SseEvent event{};
    event.type = notifications::SseEventKind::ChatSync;
    (void)sse_->deliver(user, event);
}

void ChatLive::message_sent(mongocxx::client& client, const ChatService& service,
                            const ConversationKindSpec& kind, const Uuid& conversation,
                            const MessageRecord& row,
                            const Allocation& allocation) noexcept {
    // A channel's followers are not woken: there can be a million of them, and
    // a per-user fan-out of that size on the send path is the cost the channel
    // shape exists to avoid (docs/22 §2.1, §5.4).
    if (kind.shape == Shape::Channel) { return; }
    try {
        // A message past a block is shown to its sender only (docs/22 §3.6), so
        // only the sender's own devices may hear of it. The member list is not
        // consulted: the person who blocked them must not learn by a wake what
        // the log will never show them.
        std::shared_ptr<const MemberList> cached;
        std::span<const Uuid> recipients{&row.sender, 1};
        if (!row.hidden_from_peer) {
            Result<std::shared_ptr<const MemberList>> listed = members_.recipients(
                client, repository_, conversation, allocation.membership_version, row.seq);
            if (!listed) {
                LOG_WARN << "chat wake skipped: the member list could not be read";
                return;
            }
            cached = std::move(listed).value();
            recipients = cached->users;
        }
        if (recipients.empty()) { return; }

        // The message exactly as the history route writes it, grants and all,
        // so a client that takes it from a wake holds what a fetch would have
        // given it (render.h). Larger than the inline bound, the wake says
        // "fetch it" instead.
        std::string rendered;
        rendered.reserve(frames::kInlineWakeBytes);
        const std::int64_t now_unix =
            std::chrono::duration_cast<std::chrono::seconds>(db::now_ms().time_since_epoch())
                .count();
        detail::append_message(rendered, service, row, now_unix);
        const bool carried = !row.revoked && rendered.size() <= frames::kInlineWakeBytes;
        const std::span<const std::uint8_t> message =
            carried ? std::span<const std::uint8_t>{
                          reinterpret_cast<const std::uint8_t*>(rendered.data()), rendered.size()}
                    : std::span<const std::uint8_t>{};

        std::array<std::uint8_t, frames::kMaxDownstreamFrameBytes> frame{};
        const std::size_t written = frames::encode(
            frames::Wake{.inline_message = message, .conversation = conversation, .seq = row.seq},
            frame);
        if (written == 0) {
            LOG_ERROR << "chat wake could not be encoded";
            return;
        }
        (carried ? inline_wakes_ : fetch_wakes_).fetch_add(1, std::memory_order_relaxed);
        (void)publisher_.publish(recipients, std::span<const std::uint8_t>{frame.data(), written});
    } catch (const std::exception& e) {
        LOG_ERROR << "chat wake fan-out failed: " << e.what();
    } catch (...) {
        LOG_ERROR << "chat wake fan-out failed";
    }
}

void ChatLive::message_mutated(mongocxx::client& client, const ConversationKindSpec& kind,
                               const Uuid& conversation, const Allocation& numbered,
                               const std::optional<Uuid>& only) noexcept {
    if (kind.shape == Shape::Channel) { return; }
    try {
        std::shared_ptr<const MemberList> cached;
        std::span<const Uuid> recipients;
        if (only.has_value()) {
            recipients = std::span<const Uuid>{&*only, 1};
        } else {
            // At the head: the counter is the conversation's, and a member who
            // joined after the changed message learns only that it moved, which
            // the conversation they are shown already says.
            Result<std::shared_ptr<const MemberList>> listed = members_.recipients(
                client, repository_, conversation, numbered.membership_version, numbered.seq);
            if (!listed) {
                LOG_WARN << "chat mutation wake skipped: the member list could not be read";
                return;
            }
            cached = std::move(listed).value();
            recipients = cached->users;
        }
        if (recipients.empty()) { return; }
        std::array<std::uint8_t, frames::kMutationBytes> frame{};
        const std::size_t written = frames::encode(
            frames::Mutation{.conversation = conversation, .mutation = numbered.mutations}, frame);
        if (written == 0) {
            LOG_ERROR << "chat mutation wake could not be encoded";
            return;
        }
        (void)publisher_.publish(recipients, std::span<const std::uint8_t>{frame.data(), written});
    } catch (const std::exception& e) {
        LOG_ERROR << "chat mutation wake failed: " << e.what();
    } catch (...) {
        LOG_ERROR << "chat mutation wake failed";
    }
}

void ChatLive::typing(mongocxx::client& client, const ChatService& service, const Uuid& user,
                      const Uuid& conversation) noexcept {
    try {
        // Membership first: every refusal below is the same silence, and asking
        // about a block before membership would make the cost of the silence
        // depend on who blocked whom.
        const Result<std::optional<MemberRecord>> me =
            repository_.find_member(client, conversation, user);
        if (!me || !me.value().has_value() || !me.value()->current()) { return; }
        const Result<std::optional<ConversationRecord>> found =
            repository_.find_conversation(client, conversation);
        if (!found || !found.value().has_value()) { return; }
        const ConversationRecord& row = *found.value();
        const ConversationKindSpec& kind = service.kind_of(row.kind);
        // A follower typing is not an event, and a channel's followers are never
        // woken anyway (docs/22 §2.1).
        if (kind.shape == Shape::Channel) { return; }
        if (kind.shape == Shape::Direct) {
            const Result<std::vector<MemberRecord>> pair =
                repository_.list_members(client, conversation, std::nullopt, 2);
            if (!pair) { return; }
            for (const MemberRecord& member : pair.value()) {
                if (member.user == user) { continue; }
                const Result<bool> blocked =
                    repository_.blocked_between(client, user, member.user);
                if (!blocked || blocked.value()) { return; }
            }
        }

        // The conversation's own version and head, read just now: the same
        // validation a send's allocate gives the cache, at the cost of the read
        // above, which typing needed anyway.
        const Result<std::shared_ptr<const MemberList>> listed = members_.recipients(
            client, repository_, conversation, row.membership_version, row.seq);
        if (!listed) { return; }
        std::vector<Uuid> others;
        others.reserve(listed.value()->users.size());
        for (const Uuid& member : listed.value()->users) {
            if (member != user) { others.push_back(member); }
        }
        if (others.empty()) { return; }

        std::array<std::uint8_t, frames::kTypingBytes> frame{};
        const std::size_t written =
            frames::encode(frames::Typing{.conversation = conversation, .user = user}, frame);
        if (written == 0) { return; }
        typing_relayed_.fetch_add(1, std::memory_order_relaxed);
        (void)publisher_.publish(others, std::span<const std::uint8_t>{frame.data(), written});
    } catch (const std::exception& e) {
        LOG_ERROR << "chat typing relay failed: " << e.what();
    } catch (...) {
        LOG_ERROR << "chat typing relay failed";
    }
}

}  // namespace anvil::chat
