#include "anvil/chat/socket.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <drogon/HttpRequest.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/route_registration.h"
#include "anvil/auth/token.h"
#include "anvil/chat/frames.h"
#include "anvil/chat/hub.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/user_context.h"
#include "anvil/db/mongo_pool.h"

namespace anvil::chat {
namespace {

using drogon::WebSocketConnectionPtr;
namespace ac = accesscontrol;

// What the process-wide handlers reach. Set once by install_chat_socket, before
// the listener starts, and never changed: Drogon constructs the controller
// itself, so there is no constructor to hand these to. Pointers to objects the
// application keeps for the life of the process, as every handler holds its
// services; the policy points into the application's constexpr route table.
struct Installed final {
    ChatLive*               live{nullptr};
    const ChatService*      service{nullptr};
    const ac::RoutePolicy*  policy{nullptr};
};

Installed g_installed{};

[[nodiscard]] std::int64_t now_unix() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::int64_t steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// When this socket last relayed typing in one conversation, on the steady clock.
struct TypingSlot final {
    Uuid         conversation;
    std::int64_t at_ms;
};

// One socket's state, reached only on the socket's own loop: the handlers,
// the timer and every posted drain run there, so none of it needs a lock. The
// ring is the one thing another thread touches, and it has its own.
struct SocketState final {
    // The budget and the authority (http/upgrade.h), 88 bytes.
    http::UpgradedConnection connection;
    SocketPtr                socket;
    trantor::EventLoop*      loop;
    trantor::TimerId         timer{trantor::InvalidTimerId};
    std::int64_t             last_heard_unix;
    std::int64_t             last_ping_unix;
    std::array<TypingSlot, kTypingSlots> typing{};
    // A re-check is waiting on an epoch resolve, so the next tick does not ask
    // again while the first answer is on its way.
    bool                     resolving{false};
};

void close_with(const WebSocketConnectionPtr& conn, std::uint16_t code) {
    // A custom code through the framework's enum: CloseCode is a plain enum
    // class over int, and RFC 6455 gives 4000–4999 to applications.
    conn->shutdown(static_cast<drogon::CloseCode>(code));
}

void send_frame(const WebSocketConnectionPtr& conn, std::span<const std::uint8_t> frame) {
    conn->send(reinterpret_cast<const char*>(frame.data()), frame.size(),
               drogon::WebSocketMessageType::Binary);
}

template <typename Frame>
void send_bodyless(const WebSocketConnectionPtr& conn) {
    std::array<std::uint8_t, frames::kHeaderBytes> bytes{};
    const std::size_t written = frames::encode(Frame{}, bytes);
    send_frame(conn, std::span<const std::uint8_t>{bytes.data(), written});
}

// On the socket's loop: everything the ring holds goes out, or the socket
// does, if the hub closed it.
void flush(const WebSocketConnectionPtr& conn) {
    const std::shared_ptr<SocketState> state = conn->getContext<SocketState>();
    if (state == nullptr) { return; }
    switch (state->socket->close_reason()) {
        case CloseReason::None: break;
        // Closed from this side already; the socket layer chose what to send.
        case CloseReason::Gone: return;
        case CloseReason::Replaced: close_with(conn, kCloseReplaced); return;
        case CloseReason::Overflow: close_with(conn, kCloseOverflow); return;
    }
    // On the stack: a full ring is eight kilobytes and one drain empties it.
    std::array<std::uint8_t, kSocketRingBytes> out{};
    const std::size_t bytes = state->socket->drain(out);
    std::size_t at = 0;
    while (at + kSocketRecordHeaderBytes <= bytes) {
        const std::size_t length =
            (std::size_t{out[at]} << 8U) | std::size_t{out[at + 1]};
        at += kSocketRecordHeaderBytes;
        if (at + length > bytes) { break; }
        send_frame(conn, std::span<const std::uint8_t>{out.data() + at, length});
        at += length;
    }
}

void finish_recheck(const WebSocketConnectionPtr& conn, SocketState& state,
                    ac::ConnectionVerdict verdict, std::int64_t now) {
    switch (verdict) {
        case ac::ConnectionVerdict::Keep: state.connection.note_recheck(now); return;
        case ac::ConnectionVerdict::Close: close_with(conn, kCloseReauthenticate); return;
        case ac::ConnectionVerdict::ResolveEpoch: break;
    }
    // Resolving touches Redis and, rarely, MongoDB, so it is the resolver's
    // thread that waits and the answer comes back to this loop. The context is
    // copied: it is 64 trivially copyable bytes and outlives nothing.
    state.resolving = true;
    const std::weak_ptr<drogon::WebSocketConnection> weak = conn;
    trantor::EventLoop* const loop = state.loop;
    const UserContext ctx = state.connection.context();
    ac::AccessControl::deps().epochs->resolve_async(
        ctx.user_id, [weak, loop, ctx](Result<std::uint64_t> resolved) {
            loop->queueInLoop([weak, ctx, resolved]() {
                const WebSocketConnectionPtr alive = weak.lock();
                if (alive == nullptr) { return; }
                const std::shared_ptr<SocketState> again = alive->getContext<SocketState>();
                if (again == nullptr) { return; }
                again->resolving = false;
                // A resolve that FAILED closes: an unreadable revocation channel
                // is not permission to skip revocation (docs/04 §8.1).
                if (!resolved) {
                    close_with(alive, kCloseReauthenticate);
                    return;
                }
                const ac::ConnectionVerdict after = ac::resume_connection_after_epoch(
                    ctx, *g_installed.policy, resolved.value());
                if (after == ac::ConnectionVerdict::Keep) {
                    again->connection.note_recheck(now_unix());
                } else {
                    close_with(alive, kCloseReauthenticate);
                }
            });
        });
}

// Whether a typing frame for `conversation` may be relayed now, recording it if
// so. A slot for the conversation inside the interval refuses; otherwise the
// conversation's slot, or the oldest, is taken.
[[nodiscard]] bool admit_typing(SocketState& state, const Uuid& conversation) noexcept {
    const std::int64_t now = steady_ms();
    constexpr std::int64_t interval =
        std::chrono::duration_cast<std::chrono::milliseconds>(kTypingInterval).count();
    TypingSlot* oldest = &state.typing[0];
    for (TypingSlot& slot : state.typing) {
        if (slot.conversation == conversation) {
            if (now - slot.at_ms < interval) { return false; }
            slot.at_ms = now;
            return true;
        }
        if (slot.at_ms < oldest->at_ms) { oldest = &slot; }
    }
    *oldest = TypingSlot{conversation, now};
    return true;
}

// Off the loop: whether the typist may be heard is a membership read.
// Dropped when db_pool is full — typing is the one thing in chat whose loss
// nobody can see.
void relay_typing(const UserContext& ctx, const Uuid& conversation) {
    ChatLive* const live = g_installed.live;
    const ChatService* const service = g_installed.service;
    const Uuid user = ctx.user_id;
    (void)Pools::db().try_post(guarded("chat-typing", [live, service, user, conversation]() {
        auto entry = db::MongoPool::instance().acquire();
        live->typing(*entry, *service, user, conversation);
    }));
}

// Every kSocketTick, on the socket's loop.
void tick(const WebSocketConnectionPtr& conn) {
    const std::shared_ptr<SocketState> state = conn->getContext<SocketState>();
    if (state == nullptr) { return; }
    const std::int64_t now = now_unix();
    if (now - state->last_heard_unix >= kSilenceLimit.count()) {
        close_with(conn, kCloseSilent);
        return;
    }
    if (now - state->last_ping_unix >= kPingInterval.count()) {
        state->last_ping_unix = now;
        send_bodyless<frames::Ping>(conn);
    }
    if (!state->resolving && state->connection.due_for_recheck(now)) {
        finish_recheck(conn, *state,
                       ac::still_authorized(state->connection.context(),
                                            state->connection.expires_at_unix(),
                                            *g_installed.policy,
                                            *ac::AccessControl::deps().epochs, now),
                       now);
    }
}

// The socket's controller. Registered by install_chat_socket and NOT by the
// framework's own sweep (`false`), so the path carries anvil's filters and no
// others (accesscontrol/route_registration.h).
class ChatSocketController final
    : public drogon::WebSocketController<ChatSocketController, false> {
public:
    // Required to exist by Drogon's registrator, and never called.
    static void initPathRouting() {}

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const WebSocketConnectionPtr& conn) override {
        // The route is Authenticated, so the filter attached a context or never
        // let the handshake through; a missing one is a wiring fault and is
        // closed, never served as somebody.
        const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
        if (ctx == nullptr || g_installed.live == nullptr) {
            conn->forceClose();
            return;
        }
        // The expiry the re-check needs and the context does not carry
        // (http/upgrade.h). The filter verified this token a moment ago, so this
        // is one more HMAC, once per socket.
        const std::int64_t now = now_unix();
        const auto& keys = ac::AccessControl::deps().keys;
        const auth::TokenResult token = auth::decode(
            req->getCookie(std::string{ac::kAccessCookieName}), *keys, now);
        if (!token.ok() || token.claims.user_id != ctx->user_id) {
            close_with(conn, kCloseReauthenticate);
            return;
        }

        trantor::EventLoop* const loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        const std::weak_ptr<drogon::WebSocketConnection> weak = conn;
        // Called from any thread when the ring has something, or the hub closed
        // the socket. One posted task per burst; the drain takes it all.
        auto notify = [weak, loop](SocketId) {
            loop->queueInLoop([weak]() {
                if (const WebSocketConnectionPtr alive = weak.lock()) { flush(alive); }
            });
        };
        // The session is the device: one socket each (chat/hub.h).
        Result<SocketPtr> opened =
            g_installed.live->hub().open(ctx->user_id, ctx->session_id, std::move(notify));
        if (!opened) {
            close_with(conn, kCloseFull);
            return;
        }
        auto state = std::make_shared<SocketState>(SocketState{
            .connection = http::UpgradedConnection{*ctx, token.claims.expires_at, now},
            .socket = std::move(opened).value(),
            .loop = loop,
            .timer = trantor::InvalidTimerId,
            .last_heard_unix = now,
            .last_ping_unix = now,
            .resolving = false});
        conn->setContext(state);
        state->timer = loop->runEvery(
            std::chrono::duration<double>{kSocketTick}, [weak]() {
                if (const WebSocketConnectionPtr alive = weak.lock()) { tick(alive); }
            });
    }

    void handleNewMessage(const WebSocketConnectionPtr& conn, std::string&& message,
                          const drogon::WebSocketMessageType& type) override {
        const std::shared_ptr<SocketState> state = conn->getContext<SocketState>();
        if (state == nullptr) { return; }
        const std::int64_t now = now_unix();
        switch (type) {
            case drogon::WebSocketMessageType::Binary: break;
            // Control frames: the framework answers a ping itself. They still
            // say the client is there, and still count against the budget.
            case drogon::WebSocketMessageType::Ping:
            case drogon::WebSocketMessageType::Pong:
                if (state->connection.admit_frame(message.size(), now) ==
                    http::FrameVerdict::Close) {
                    close_with(conn, kCloseBadFrame);
                    return;
                }
                state->last_heard_unix = now;
                return;
            case drogon::WebSocketMessageType::Close: return;
            case drogon::WebSocketMessageType::Text:
            case drogon::WebSocketMessageType::Unknown:
                close_with(conn, kCloseBadFrame);
                return;
        }
        // The size first and without spending budget, then the budget, then the
        // grammar: each is cheaper than the next.
        if (state->connection.admit_frame(message.size(), now) == http::FrameVerdict::Close) {
            close_with(conn, kCloseBadFrame);
            return;
        }
        const Result<frames::UpstreamFrame> frame = frames::decode_upstream(
            std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(message.data()),
                                          message.size()});
        if (!frame) {
            // Counted by reason, never logged by content: the bytes are exactly
            // what a hostile client would choose (chat/frames.h).
            LOG_DEBUG << "chat socket closed on " << frame.error().field;
            close_with(conn, kCloseBadFrame);
            return;
        }
        state->last_heard_unix = now;
        if (std::holds_alternative<frames::ClientPing>(frame.value())) {
            send_bodyless<frames::Pong>(conn);
        } else if (const auto* typing = std::get_if<frames::ClientTyping>(&frame.value())) {
            if (admit_typing(*state, typing->conversation)) {
                relay_typing(state->connection.context(), typing->conversation);
            }
        }
        // A ClientPong is the answer to a ping, and hearing it was the point.
    }

    void handleConnectionClosed(const WebSocketConnectionPtr& conn) override {
        const std::shared_ptr<SocketState> state = conn->getContext<SocketState>();
        if (state == nullptr) { return; }
        state->loop->invalidateTimer(state->timer);
        g_installed.live->hub().close(state->socket->id(), CloseReason::Gone);
        conn->clearContext();
    }
};

[[nodiscard]] std::string pattern_of(std::span<const descriptor::RouteDescription> descriptions,
                                     std::string_view id) {
    for (const descriptor::RouteDescription& candidate : descriptions) {
        if (candidate.id != id) { continue; }
        if (candidate.pattern.find('{') != std::string_view::npos ||
            candidate.method != ac::RouteMethod::Get) {
            throw std::invalid_argument{"chat socket route '" + std::string{id} +
                                        "' must be a GET with no path placeholders"};
        }
        return std::string{candidate.pattern};
    }
    throw std::invalid_argument{"chat socket route id '" + std::string{id} +
                                "' is not in the route descriptions"};
}

}  // namespace

void install_chat_socket(ChatLive& live, const ChatService& service,
                         std::span<const accesscontrol::RoutePolicy> routes,
                         std::span<const descriptor::RouteDescription> descriptions,
                         std::string_view route_id) {
    std::string pattern = pattern_of(descriptions, route_id);
    const ac::RoutePolicy* policy = ac::policy_for(routes, pattern, ac::RouteMethod::Get);
    // register_websocket_route throws on a missing policy as well; this one is
    // the pointer the re-check needs, which must name the same row.
    if (policy == nullptr) {
        throw std::logic_error{"chat socket route '" + pattern + "' has no policy"};
    }
    g_installed = Installed{.live = &live, .service = &service, .policy = policy};
    ac::register_websocket_route(routes, std::move(pattern),
                                 ChatSocketController::classTypeName());
}

}  // namespace anvil::chat
