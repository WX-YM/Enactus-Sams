#include "anvil/chat/push.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <trantor/utils/Logger.h>

#include "anvil/core/uuid.h"
#include "anvil/http/json_writer.h"

namespace anvil::chat {
namespace {

namespace n = notifications;

// One recipient a page decided to push, and what their push says.
struct Recipient final {
    Uuid                      user;
    const NudgeMessage*       newest;
    std::optional<PushReader> reader;
    std::int32_t              count;
};

[[nodiscard]] std::int64_t millis(std::chrono::seconds span) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(span).count();
}

[[nodiscard]] std::int64_t epoch_ms(db::TimeMs at) noexcept {
    return at.time_since_epoch().count();
}

// "chat:push:<conversation>:<bucket>", and the late key with the seq after it.
[[nodiscard]] std::string key_of(const Uuid& conversation, std::int64_t bucket) {
    const std::array<char, 22> id = uuid::to_base64url(conversation);
    std::string key{"chat:push:"};
    key.append(id.data(), id.size());
    key.push_back(':');
    key.append(std::to_string(bucket));
    return key;
}

[[nodiscard]] bool names(const NudgeMessage& message, const Uuid& user) noexcept {
    return std::find(message.mentioned.begin(), message.mentioned.end(), user) !=
           message.mentioned.end();
}

// What one member is owed, or nullopt for nothing. The messages are newest
// first, so the walk stops at the first one the member's watermark covers.
[[nodiscard]] std::optional<Recipient> owed(const MemberRecord& member,
                                            const ConversationKindSpec& kind,
                                            std::span<const NudgeMessage> messages,
                                            db::TimeMs window_start, db::TimeMs now) {
    const std::int64_t seen = std::max(member.delivered, member.hide_before);
    Recipient out{member.user, nullptr, std::nullopt, 0};
    bool fresh = false;
    bool mentioned = false;
    for (const NudgeMessage& message : messages) {
        if (message.seq <= seen) { break; }
        if (!member.can_see(message.seq)) { continue; }
        if (message.sender == member.user) { continue; }
        // A direct message past a block is shown to its sender only, and a
        // revoked or system message is nothing to be told about.
        if (message.hidden_from_peer || message.revoked) { continue; }
        if (message.kind == MessageKind::System) { continue; }
        if (out.newest == nullptr) { out.newest = &message; }
        ++out.count;
        fresh = fresh || message.sent_at >= window_start;
        mentioned = mentioned || names(message, member.user);
    }
    // Nothing waiting, or only what an earlier window's job already pushed.
    if (out.count == 0 || !fresh) { return std::nullopt; }
    const bool muted = member.muted_until.has_value() && *member.muted_until > now;
    if (muted && !(kind.mentions_break_mute && mentioned)) { return std::nullopt; }
    return out;
}

}  // namespace

std::string encrypted_push_payload(const Uuid& conversation, std::int64_t seq) {
    std::string out;
    out.reserve(64);
    out += '{';
    http::append_json_key(out, "c");
    http::append_json_uuid(out, conversation);
    out += ',';
    http::append_json_key(out, "seq");
    http::append_json_int(out, seq);
    out += '}';
    return out;
}

ChatPush::ChatPush(PushConfig config, PushHooks hooks, const ChatRepository& chats,
                   const n::NotificationRepository& notifications,
                   std::span<const n::TemplateSpec> templates, n::Transport web_push)
    : config_{config},
      hooks_{std::move(hooks)},
      chats_{chats},
      notifications_{notifications},
      templates_{templates},
      web_push_{std::move(web_push)},
      topic_{n::topic_spec(notifications.topics(), config.topic)} {
    if (!hooks_.enqueue) { throw std::invalid_argument{"chat push: no enqueue hook"}; }
    if (!hooks_.reader) { throw std::invalid_argument{"chat push: no reader hook"}; }
    if (!web_push_) { throw std::invalid_argument{"chat push: no web push transport"}; }
    if (config_.window.count() <= 0 || config_.grace.count() < 0) {
        throw std::invalid_argument{"chat push: the window must be positive"};
    }
    if (n::template_of(templates_, config_.preview) == nullptr ||
        n::template_of(templates_, config_.plain) == nullptr) {
        throw std::invalid_argument{"chat push: a template the table does not declare"};
    }
    if (topic_ == nullptr) {
        throw std::invalid_argument{"chat push: a topic the table does not declare"};
    }
    if (!n::has_channel(topic_->default_channels, n::ClientType::WebPush)) {
        throw std::invalid_argument{"chat push: the topic does not include WebPush"};
    }
}

std::int64_t ChatPush::bucket_of(db::TimeMs at) const noexcept {
    const std::int64_t window = millis(config_.window);
    const std::int64_t ms = epoch_ms(at);
    // Floor, so a clock before the epoch still maps monotonically.
    const std::int64_t quotient = ms / window;
    return (ms % window < 0) ? quotient - 1 : quotient;
}

db::TimeMs ChatPush::due_of(std::int64_t bucket) const noexcept {
    return db::TimeMs{std::chrono::milliseconds{((bucket + 1) * millis(config_.window)) +
                                                millis(config_.grace)}};
}

std::array<std::uint8_t, kPushArgsBytes> ChatPush::encode_args(const Uuid& conversation,
                                                                std::int64_t bucket) noexcept {
    std::array<std::uint8_t, kPushArgsBytes> args{};
    args[0] = kPushArgsVersion;
    std::copy(conversation.begin(), conversation.end(), args.begin() + 1);
    const auto value = static_cast<std::uint64_t>(bucket);
    for (std::size_t i = 0; i < 8; ++i) {
        args[17 + i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
    return args;
}

void ChatPush::enqueue(const Uuid& conversation, std::int64_t bucket, db::TimeMs due,
                       std::string_view key) noexcept {
    try {
        const std::array<std::uint8_t, kPushArgsBytes> args = encode_args(conversation, bucket);
        if (const Status asked = hooks_.enqueue(args, due, key); !asked) {
            enqueue_failures_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN << "chat push: the queue refused a nudge";
            return;
        }
        enqueued_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        enqueue_failures_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR << "chat push: the enqueue hook threw";
    }
}

void ChatPush::before_commit(const ConversationKindSpec& kind, const Uuid& conversation,
                             db::TimeMs sent_at) noexcept {
    // A channel's followers are not pushed, as they are not woken (22 §5.4):
    // each post would be a push to an unbounded audience.
    if (kind.shape == Shape::Channel) { return; }
    const std::int64_t bucket = bucket_of(sent_at);
    try {
        enqueue(conversation, bucket, due_of(bucket), key_of(conversation, bucket));
    } catch (...) {
        enqueue_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ChatPush::after_commit(const ConversationKindSpec& kind, const Uuid& conversation,
                            std::int64_t seq, db::TimeMs sent_at) noexcept {
    if (kind.shape == Shape::Channel) { return; }
    const std::int64_t bucket = bucket_of(sent_at);
    const db::TimeMs now = db::now_ms();
    // The job is due no earlier than this, on a clock that may run ahead of
    // ours. A commit before that point is seen by the job; one after it may
    // not be, and asks for a job of its own. Rare: the window and the grace
    // are seconds, and a send's transaction is milliseconds.
    if (now + kNudgeClockSkew < due_of(bucket)) { return; }
    try {
        std::string key = key_of(conversation, bucket);
        key.push_back(':');
        key.append(std::to_string(seq));
        enqueue(conversation, bucket, now + config_.grace, key);
    } catch (...) {
        enqueue_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

Result<PushSummary> ChatPush::deliver(mongocxx::client& client,
                                      std::span<const std::uint8_t> args,
                                      db::TimeMs now) const {
    PushSummary summary{};
    if (args.size() != kPushArgsBytes || args[0] != kPushArgsVersion) {
        return fail(ErrorCode::ValidationFailed, "args");
    }
    Uuid conversation{};
    std::copy(args.begin() + 1, args.begin() + 17, conversation.begin());
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        raw |= static_cast<std::uint64_t>(args[17 + i]) << (8U * i);
    }
    const auto bucket = static_cast<std::int64_t>(raw);
    const db::TimeMs window_start{std::chrono::milliseconds{bucket * millis(config_.window)}};

    const Result<std::optional<ConversationRecord>> found =
        chats_.find_conversation(client, conversation);
    if (!found) { return found.error(); }
    if (!found.value().has_value()) { return summary; }
    const ConversationRecord& record = *found.value();
    if (record.kind >= chats_.kinds().size()) { return fail(ErrorCode::Internal, "kind"); }
    const ConversationKindSpec& kind = chats_.kinds()[record.kind];
    if (kind.shape == Shape::Channel) { return summary; }

    const Result<std::vector<NudgeMessage>> recent =
        chats_.recent_for_nudge(client, conversation, record.seq + 1, kNudgeScan, now);
    if (!recent) { return recent.error(); }
    const std::span<const NudgeMessage> messages{recent.value()};
    // Nothing from this window is there: revoked into a system row, expired, or
    // a send that asked for its job and then never committed.
    if (std::none_of(messages.begin(), messages.end(), [&](const NudgeMessage& message) {
            return message.sent_at >= window_start;
        })) {
        return summary;
    }

    // Names, once per distinct sender per job.
    std::vector<std::pair<Uuid, std::string>> named;
    const auto name_of = [&](const Uuid& user) -> const std::string& {
        for (const auto& [who, name] : named) {
            if (who == user) { return name; }
        }
        named.emplace_back(user, hooks_.name_of ? hooks_.name_of(client, user) : std::string{});
        return named.back().second;
    };

    std::optional<Uuid> after;
    std::vector<Recipient> page;
    std::vector<Uuid> owners;
    page.reserve(kNudgePage);
    owners.reserve(kNudgePage);
    for (;;) {
        const Result<std::vector<MemberRecord>> members =
            chats_.list_members(client, conversation, after, kNudgePage);
        if (!members) { return members.error(); }
        if (members.value().empty()) { break; }
        page.clear();
        owners.clear();
        for (const MemberRecord& member : members.value()) {
            after = member.user;
            if (std::optional<Recipient> due = owed(member, kind, messages, window_start, now)) {
                owners.push_back(due->user);
                page.push_back(std::move(*due));
            }
        }
        summary.recipients += static_cast<std::int32_t>(page.size());

        const Result<std::vector<n::ClientTarget>> targets = notifications_.push_targets(
            client, owners, static_cast<std::int32_t>(owners.size()) * kNudgeEndpointsPerAccount);
        if (!targets) { return targets.error(); }
        for (const n::ClientTarget& target : targets.value()) {
            if (summary.deliveries.attempted >= n::kMaxDeliveriesPerSend) { return summary; }
            const auto who = std::find_if(page.begin(), page.end(), [&](const Recipient& r) {
                return target.owner.has_value() && r.user == *target.owner;
            });
            if (who == page.end()) { continue; }
            // A device that turned the chat topic off for WebPush.
            if (!n::should_deliver(*topic_, n::ClientType::WebPush, n::kDefaultChannels,
                                   target.prefs)) {
                ++summary.deliveries.skipped;
                continue;
            }
            n::Delivery delivery{};
            if (record.encrypted) {
                // {c, seq} and nothing else (header comment): no template, so
                // neither hook is asked and nothing they know can be bound.
                delivery.content =
                    n::Rendered{std::string{}, encrypted_push_payload(conversation, who->newest->seq)};
            } else {
                if (!who->reader.has_value()) { who->reader = hooks_.reader(client, who->user); }
                const PushReader& reader = *who->reader;

                const std::string& sender = name_of(who->newest->sender);
                const std::string_view title =
                    kind.shape == Shape::Direct ? std::string_view{sender}
                                                : std::string_view{record.title};
                std::array<n::Param, 4> bound{};
                std::size_t count = 0;
                const auto bind = [&](const n::Param& param) {
                    // A value that cannot be shown is left out and renders as
                    // nothing, rather than costing the recipient the push.
                    if (n::check_param(param)) { bound[count++] = param; }
                };
                bind(n::Param::of('n', static_cast<std::int64_t>(who->count)));
                bind(n::Param::of('t', title));
                bind(n::Param::of('s', std::string_view{sender}));
                if (reader.previews) {
                    bind(n::Param::of('b', std::string_view{who->newest->text}));
                }
                Result<n::Rendered> content =
                    n::render(templates_, reader.previews ? config_.preview : config_.plain,
                              reader.locale, std::span<const n::Param>{bound.data(), count});
                if (!content) { return content.error(); }
                delivery.content = std::move(content).value();
            }
            delivery.address = target.addr;
            delivery.keys = target.keys;
            delivery.client = target.id;
            delivery.notification = conversation;
            delivery.count = who->count;
            delivery.kind = topic_->code;
            delivery.type = n::ClientType::WebPush;

            ++summary.deliveries.attempted;
            const Result<n::DeliveryVerdict> verdict = web_push_(delivery);
            if (const Status recorded = n::record_verdict(
                    notifications_, client, target.id,
                    verdict ? verdict.value() : n::DeliveryVerdict::Rejected, now,
                    summary.deliveries);
                !recorded) {
                return recorded.error();
            }
        }
        if (members.value().size() < static_cast<std::size_t>(kNudgePage)) { break; }
    }
    return summary;
}

timer::JobOutcome ChatPush::run(const timer::JobRunContext& ctx) const noexcept {
    try {
        const Result<PushSummary> done = deliver(*ctx.client, ctx.args, db::now_ms());
        if (done) { return timer::JobOutcome::Done; }
        // Arguments that do not parse never will.
        if (done.code() == ErrorCode::ValidationFailed) { return timer::JobOutcome::Failed; }
        LOG_WARN << "chat push: a nudge failed and will be retried";
        return timer::JobOutcome::Retry;
    } catch (const std::exception& failure) {
        LOG_ERROR << "chat push: " << failure.what();
    } catch (...) {
        LOG_ERROR << "chat push: a nudge threw";
    }
    return timer::JobOutcome::Retry;
}

}  // namespace anvil::chat
