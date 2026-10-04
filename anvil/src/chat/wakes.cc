#include "anvil/chat/wakes.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <trantor/utils/Logger.h>

#include "anvil/chat/frames.h"

namespace anvil::chat {
namespace {

constexpr std::string_view kHex = "0123456789abcdef";

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

[[nodiscard]] sw::redis::StringView as_redis(std::string_view text) noexcept {
    return sw::redis::StringView{text.data(), text.size()};
}

[[nodiscard]] std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    // Byte access to the payload's storage; uint8_t may alias any object.
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// The connection options for the subscriber's own connection. The socket
// timeout IS the poll interval: consume() returning on it is how the thread
// sees queued (un)subscribes and the stop flag without an interrupt mechanism.
[[nodiscard]] sw::redis::ConnectionOptions subscriber_options(const WakeSubscriberConfig& config) {
    try {
        sw::redis::ConnectionOptions options =
            sw::redis::Uri{config.url}.connection_options();
        options.connect_timeout = config.connect_timeout;
        options.socket_timeout = config.poll_interval;
        options.keep_alive = true;
        options.name = config.client_name;
        return options;
    } catch (const sw::redis::Error&) {
        // The driver's message can quote the URL, and a URL can carry a
        // password; the caller learns which argument was wrong and no more.
        throw std::invalid_argument("WakeSubscriber: the Redis URL does not parse");
    }
}

}  // namespace

WakeChannel::WakeChannel(const Uuid& user) noexcept : chars{} {
    std::size_t at = 0;
    for (const char c : kWakeChannelPrefix) { chars[at++] = c; }
    for (const std::uint8_t byte : user) {
        chars[at++] = kHex[byte >> 4U];
        chars[at++] = kHex[byte & 0x0FU];
    }
}

std::optional<Uuid> parse_wake_channel(std::string_view channel) noexcept {
    if (channel.size() != kWakeChannelBytes || !channel.starts_with(kWakeChannelPrefix)) {
        return std::nullopt;
    }
    const std::string_view digits = channel.substr(kWakeChannelPrefix.size());
    Uuid user{};
    for (std::size_t i = 0; i < user.size(); ++i) {
        const int high = hex_value(digits[2 * i]);
        const int low = hex_value(digits[(2 * i) + 1]);
        if (high < 0 || low < 0) { return std::nullopt; }
        user[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return user;
}

// --- publishing ------------------------------------------------------------------

WakePublisher::WakePublisher(sw::redis::Redis& redis) noexcept
    : redis_{redis}, published_{0}, publish_failed_{0} {}

std::size_t WakePublisher::publish(std::span<const Uuid> recipients,
                                   std::span<const std::uint8_t> frame) noexcept {
    if (recipients.empty()) { return 0; }
    if (recipients.size() > kMaxWakeRecipients) {
        LOG_ERROR << "chat wake refused: " << recipients.size() << " recipients, the bound is "
                  << kMaxWakeRecipients;
        publish_failed_.fetch_add(recipients.size(), std::memory_order_relaxed);
        return 0;
    }
    // Checked here as well as on arrival: every subscriber would drop it, so
    // publishing it would be N round trips' worth of fan-out for nothing, and
    // the bug is cheaper to find on the side that wrote it.
    if (!frames::decode_downstream(frame).ok()) {
        LOG_ERROR << "chat wake refused: the payload is not a downstream frame";
        publish_failed_.fetch_add(recipients.size(), std::memory_order_relaxed);
        return 0;
    }

    const sw::redis::StringView payload{reinterpret_cast<const char*>(frame.data()),
                                        frame.size()};
    try {
        // A pool connection, not a new one: `pipeline()` defaults to opening a
        // fresh TCP connection per call, which on the send path would be a
        // connect per message.
        sw::redis::Pipeline pipe = redis_.pipeline(false);
        std::size_t queued = 0;
        for (const Uuid& user : recipients) {
            if (is_nil(user)) { continue; }
            // The command is formatted into the connection's output buffer
            // inside command(), so one stack buffer per iteration is enough.
            const WakeChannel channel{user};
            pipe.command("SPUBLISH", as_redis(channel.view()), payload);
            ++queued;
        }
        if (queued == 0) {
            publish_failed_.fetch_add(recipients.size(), std::memory_order_relaxed);
            return 0;
        }
        sw::redis::QueuedReplies replies = pipe.exec();
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < replies.size(); ++i) {
            try {
                (void)replies.get<long long>(i);
                ++accepted;
            } catch (const sw::redis::Error&) {
                // An error reply for one channel. Counted, not logged per
                // recipient: a 1 024-member fan-out against a misconfigured
                // server would be 1 024 identical lines.
            }
        }
        published_.fetch_add(accepted, std::memory_order_relaxed);
        publish_failed_.fetch_add(recipients.size() - accepted, std::memory_order_relaxed);
        if (accepted != recipients.size()) {
            LOG_WARN << "chat wakes partly lost: " << (recipients.size() - accepted) << " of "
                     << recipients.size();
        }
        return accepted;
    } catch (const std::exception& e) {
        // Lost, and that is the contract: the message is committed and its
        // recipients' next sync finds it (docs/22-chat.md §5.4).
        LOG_WARN << "chat wakes lost, Redis unavailable: " << e.what();
    } catch (...) {
        LOG_WARN << "chat wakes lost, Redis unavailable";
    }
    publish_failed_.fetch_add(recipients.size(), std::memory_order_relaxed);
    return 0;
}

// --- subscribing -----------------------------------------------------------------

WakeSubscriber::WakeSubscriber(WakeSubscriberConfig config, WakeDelivery deliver,
                               WakeSubscribed on_subscribed)
    : connection_{subscriber_options(config)},
      config_{std::move(config)},
      deliver_{std::move(deliver)},
      on_subscribed_{std::move(on_subscribed)},
      pending_mutex_{},
      pending_{},
      refs_{},
      delivered_{0},
      dropped_malformed_{0},
      running_{true},
      thread_{[this]() { run(); }} {}

WakeSubscriber::~WakeSubscriber() { stop(); }

void WakeSubscriber::subscribe(const Uuid& user) {
    const std::lock_guard lock{pending_mutex_};
    ++pending_[user];
}

void WakeSubscriber::unsubscribe(const Uuid& user) {
    const std::lock_guard lock{pending_mutex_};
    --pending_[user];
}

void WakeSubscriber::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) { return; }
    // consume() returns within one poll interval, so the join is bounded by it
    // (plus one backoff slice when the thread is between connections).
    if (thread_.joinable()) { thread_.join(); }
}

void WakeSubscriber::on_message(std::string_view channel, std::string_view payload) noexcept {
    const std::optional<Uuid> user = parse_wake_channel(channel);
    if (!user) {
        dropped_malformed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A message already in flight when the last socket's unsubscribe was
    // applied. Nobody here wants it; it is neither malformed nor delivered.
    if (!refs_.contains(*user)) { return; }

    const std::span<const std::uint8_t> frame = as_bytes(payload);
    if (!frames::decode_downstream(frame).ok()) {
        // Nothing the publisher writes fails this, so whatever did was written
        // by something else with access to this Redis. The bytes are not
        // logged: they are exactly what an attacker would choose.
        dropped_malformed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    delivered_.fetch_add(1, std::memory_order_relaxed);
    if (!deliver_) { return; }
    try {
        deliver_(*user, frame);
    } catch (const std::exception& e) {
        // Let out, this ends the one thread that reads every wake for the
        // process, and every user's sockets go quiet with it (CLAUDE.md §4).
        LOG_ERROR << "chat wake delivery callback threw: " << e.what();
    } catch (...) {
        LOG_ERROR << "chat wake delivery callback threw";
    }
}

void WakeSubscriber::on_confirmed(std::string_view channel) noexcept {
    const std::optional<Uuid> user = parse_wake_channel(channel);
    if (!user || !refs_.contains(*user) || !on_subscribed_) { return; }
    try {
        on_subscribed_(*user);
    } catch (const std::exception& e) {
        LOG_ERROR << "chat wake subscribed callback threw: " << e.what();
    } catch (...) {
        LOG_ERROR << "chat wake subscribed callback threw";
    }
}

void WakeSubscriber::run() noexcept {
    std::chrono::milliseconds backoff = config_.reconnect_initial;
    std::vector<Uuid> to_subscribe;
    std::vector<Uuid> to_unsubscribe;

    // Folds the callers' queued deltas into refs_ and returns nothing: the
    // transitions are left in the two vectors for the caller to send. refs_ is
    // brought fully up to date BEFORE any command is sent, so a connection that
    // dies mid-send loses no change — the reconnect re-subscribes from refs_.
    const auto apply_pending = [&]() {
        std::map<Uuid, std::int64_t> deltas;
        {
            const std::lock_guard lock{pending_mutex_};
            deltas.swap(pending_);
        }
        to_subscribe.clear();
        to_unsubscribe.clear();
        for (const auto& [user, delta] : deltas) {
            if (delta == 0) { continue; }
            const auto found = refs_.find(user);
            const std::int64_t before = (found == refs_.end()) ? 0 : found->second;
            // Clamped at zero: an unbalanced unsubscribe is the caller's bug,
            // and letting the count go negative would make the NEXT socket's
            // subscribe silently a no-op.
            const std::int64_t after = std::max<std::int64_t>(0, before + delta);
            if (after == 0) {
                if (found != refs_.end()) {
                    refs_.erase(found);
                    to_unsubscribe.push_back(user);
                }
                continue;
            }
            refs_[user] = static_cast<std::uint32_t>(
                std::min<std::int64_t>(after, std::numeric_limits<std::uint32_t>::max()));
            if (before == 0) { to_subscribe.push_back(user); }
        }
    };

    while (running_.load(std::memory_order_acquire)) {
        try {
            to_subscribe.clear();
            to_unsubscribe.clear();
            // A connection per attempt: after a loss the old one is in an
            // unknown protocol state and there is nothing on it worth keeping.
            sw::redis::Redis redis{connection_};
            sw::redis::Subscriber subscriber = redis.subscriber();
            subscriber.on_smessage([this](std::string channel, std::string message) {
                on_message(channel, message);
            });
            subscriber.on_meta([this](sw::redis::Subscriber::MsgType type,
                                      sw::redis::OptionalString channel, long long /*count*/) {
                if (type == sw::redis::Subscriber::MsgType::SSUBSCRIBE && channel) {
                    on_confirmed(*channel);
                }
            });

            // Everyone referenced, again. Wakes published while this process
            // was disconnected are gone; the confirmations this produces are
            // the hub's cue to make those sockets sync.
            apply_pending();
            for (const auto& [user, count] : refs_) {
                subscriber.ssubscribe(as_redis(WakeChannel{user}.view()));
            }
            backoff = config_.reconnect_initial;

            while (running_.load(std::memory_order_acquire)) {
                apply_pending();
                // One channel per command, never a batch: on a cluster a
                // multi-channel SSUBSCRIBE across slots is CROSSSLOT.
                for (const Uuid& user : to_subscribe) {
                    subscriber.ssubscribe(as_redis(WakeChannel{user}.view()));
                }
                for (const Uuid& user : to_unsubscribe) {
                    // The range overload, with a range of one. redis-plus-plus
                    // 1.3.15 DECLARES sunsubscribe(StringView) but never
                    // defines it, so the single-channel call is a link error;
                    // the template is header-defined and sends the same
                    // command.
                    const WakeChannel channel{user};
                    const std::array<sw::redis::StringView, 1> one{as_redis(channel.view())};
                    subscriber.sunsubscribe(one.begin(), one.end());
                }
                try {
                    subscriber.consume();
                } catch (const sw::redis::TimeoutError&) {
                    // Expected once per idle poll interval.
                    continue;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN << "chat wake subscriber reconnecting; wakes are lost until it does: "
                     << e.what();
        } catch (...) {
            LOG_WARN << "chat wake subscriber reconnecting; wakes are lost until it does";
        }

        // Sliced, so stop() is never kept waiting a whole backoff.
        const auto until = std::chrono::steady_clock::now() + backoff;
        while (running_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::min(config_.poll_interval, backoff));
        }
        backoff = std::min(backoff * 2, config_.reconnect_max);
    }
}

}  // namespace anvil::chat
