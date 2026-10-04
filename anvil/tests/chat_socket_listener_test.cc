// The chat socket, served (docs/22-chat.md §8.2): a send over HTTP, a wake down
// a real WebSocket, through the real access filter, the real hub and the real
// Redis wake channel, in one process.
//
// What the unit suites cannot see lives here: that the bytes a socket carries
// are the bytes history would have returned — grants and not ids — that the
// wake reaches exactly who may see the message, and that every way a client
// can misbehave closes its socket and touches nothing else.
//
// Absence is never asserted by waiting. A wake that must NOT arrive is checked
// after a later one on the same socket has: one socket delivers in order, so
// by then the unwanted one would have come first (chat_wakes_test.cc does the
// same).

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "anvil/chat/frames.h"
#include "anvil/chat/socket.h"
#include "anvil/core/uuid.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "anvil/notifications/sse.h"
#include "chat_listener_fixture.h"
#include "ws_client.h"

namespace anvil {
namespace {

using namespace chatfixture;  // NOLINT(google-build-using-namespace)
namespace frames = chat::frames;
using testfixture::WsClient;
using testfixture::WsFrame;

constexpr std::chrono::milliseconds kDeadline{5000};

// The stack is never destroyed (chat_listener_fixture.h), so without this the
// presence tracker's thread would outlive main and write last seen through a
// MongoPool that static destruction is tearing down. Registered in this one
// file, for the reason db_fixture_check.cc gives.
class LiveShutdown final : public ::testing::Environment {
public:
    void TearDown() override {
        if (stack().live != nullptr) { stack().live->stop(); }
    }
};

const ::testing::Environment* const kLiveShutdown =
    ::testing::AddGlobalTestEnvironment(new LiveShutdown{});

// What a case reads off a socket: the frame decoded, with any inline message
// copied out of the buffer it was decoded from.
struct Received final {
    frames::DownstreamFrame frame;
    std::string             inline_message;
};

[[nodiscard]] std::string socket_headers(const Uuid& user, const Uuid& session,
                                         std::string_view origin = kAllowedOrigin) {
    std::string headers;
    if (!origin.empty()) { headers.append("Origin: ").append(origin).append("\r\n"); }
    headers.append("Cookie: ").append(cookie_for(user, session)).append("\r\n");
    return headers;
}

// The next downstream frame, skipping the server's pings, or nullopt.
[[nodiscard]] std::optional<Received> next_frame(WsClient& ws,
                                                 std::chrono::milliseconds timeout = kDeadline) {
    while (true) {
        const std::optional<WsFrame> raw = ws.next(timeout);
        if (!raw.has_value() || raw->opcode != 0x2) { return std::nullopt; }
        const auto decoded = frames::decode_downstream(raw->payload);
        EXPECT_TRUE(decoded) << "the server sent a frame its own codec refuses";
        if (!decoded) { return std::nullopt; }
        if (std::holds_alternative<frames::Ping>(decoded.value())) { continue; }
        Received out{decoded.value(), {}};
        if (const auto* wake = std::get_if<frames::Wake>(&out.frame)) {
            out.inline_message.assign(wake->inline_message.begin(), wake->inline_message.end());
        }
        return out;
    }
}

// The next wake, skipping Syncs: a Sync can arrive at any time a subscription
// is confirmed, and it says nothing a wake case asserts about.
[[nodiscard]] std::optional<Received> next_wake(WsClient& ws) {
    while (true) {
        std::optional<Received> got = next_frame(ws);
        if (!got.has_value()) { return std::nullopt; }
        if (std::holds_alternative<frames::Sync>(got->frame)) { continue; }
        return got;
    }
}

// Whether this is the user's first socket on the process, which decides
// whether a Sync is owed: the first one subscribes the user's wake channel and
// the server says so when Redis confirms it. A later one joins a subscription
// that is already live, so nothing was lost and nothing is sent.
enum class Opening : bool { First, Another };

// A socket that is live end to end: open, and — for a user's first socket —
// the wake channel confirmed (chat/frames.h, Sync). A case that sent before
// that would be asserting about a wake the protocol says can be lost.
[[nodiscard]] WsClient live_socket(const Uuid& user, const Uuid& session,
                                   Opening opening = Opening::First) {
    WsClient ws;
    EXPECT_TRUE(ws.open(testfixture::listener_port(), "/chat/socket",
                        socket_headers(user, session)))
        << "status " << ws.status();
    if (opening == Opening::First) {
        const std::optional<Received> sync = next_frame(ws);
        EXPECT_TRUE(sync.has_value() && std::holds_alternative<frames::Sync>(sync->frame))
            << "no Sync after the user's first socket opened";
    }
    return ws;
}

[[nodiscard]] WsClient live_socket(const Uuid& user, Opening opening = Opening::First) {
    return live_socket(user, uuid::generate_v7(), opening);
}

[[nodiscard]] std::int64_t send_text(const Uuid& c, const Uuid& sender, std::string_view text) {
    const Exchange sent =
        call(drogon::Post, conversation_path(c, "/messages"), sender,
             R"({"cid":")" + cid() + R"(","body":")" + std::string{text} + R"("})");
    EXPECT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();
    return json_int(sent.response->body(), "seq").value_or(-1);
}

void expect_wake(const std::optional<Received>& got, const Uuid& c, std::int64_t seq) {
    ASSERT_TRUE(got.has_value()) << "no wake within the deadline";
    const auto* wake = std::get_if<frames::Wake>(&got->frame);
    ASSERT_NE(wake, nullptr);
    EXPECT_EQ(wake->conversation, c);
    EXPECT_EQ(wake->seq, seq);
}

class ChatSocketListener : public ChatRoutesListener {
protected:
    void SetUp() override {
        ChatRoutesListener::SetUp();
        if (IsSkipped()) { return; }
        if (stack().live == nullptr) { GTEST_SKIP() << "no Redis, so no live delivery"; }
        (void)testfixture::listener_port();
    }
};

// --- the wake -------------------------------------------------------------------

TEST_F(ChatSocketListener, ASendWakesEveryMemberWithTheMessageAsHistoryWritesIt) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    WsClient theirs = live_socket(member);

    const std::int64_t seq = send_text(c, owner, "hello over the socket");
    const std::optional<Received> got = next_wake(theirs);
    expect_wake(got, c, seq);
    ASSERT_FALSE(got->inline_message.empty()) << "a short message travels inline";

    // Byte for byte the history route's rendering of the same message.
    const Exchange page = call(drogon::Get, conversation_path(c, "/messages"), member);
    EXPECT_NE(page.response->body().find(got->inline_message), std::string::npos)
        << got->inline_message;
}

TEST_F(ChatSocketListener, TheSendersOtherDevicesAreWokenAndAStrangerIsNot) {
    const Uuid owner = uuid::generate_v4();
    const Uuid stranger = uuid::generate_v4();
    const Uuid c = group(owner);
    const Uuid others = group(stranger, {owner});
    WsClient laptop = live_socket(owner);
    WsClient outsider = live_socket(stranger);

    const std::int64_t seq = send_text(c, owner, "mine");
    expect_wake(next_wake(laptop), c, seq);

    // The sentinel: a message the stranger IS woken for, sent after. Had the
    // first one reached them, it would be the first thing on their socket.
    const std::int64_t sentinel = send_text(others, owner, "theirs");
    expect_wake(next_wake(outsider), others, sentinel);
}

TEST_F(ChatSocketListener, AnAttachmentRidesAWakeAsAGrantAndNeverAsItsId) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const auto [object, handle] = uploaded(owner);
    WsClient theirs = live_socket(member);

    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), owner,
                               R"({"cid":")" + cid() + R"(","attachments":[{"handle":")" +
                                   handle + R"(","name":"a.pdf"}]})");
    ASSERT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();
    const std::optional<Received> got = next_wake(theirs);
    ASSERT_TRUE(got.has_value());
    EXPECT_NE(got->inline_message.find(R"("grant":")"), std::string::npos)
        << got->inline_message;
    EXPECT_EQ(got->inline_message.find(uuid::to_string(object)), std::string::npos)
        << "the object id reached a client";
}

TEST_F(ChatSocketListener, AMessageTooLargeToCarryWakesWithoutIt) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    WsClient theirs = live_socket(member);

    const std::string long_text(frames::kInlineWakeBytes, 'x');
    const std::int64_t seq = send_text(c, owner, long_text);
    const std::optional<Received> got = next_wake(theirs);
    expect_wake(got, c, seq);
    EXPECT_TRUE(got->inline_message.empty()) << "the wake says fetch it";
}

TEST_F(ChatSocketListener, AMessagePastABlockWakesOnlyItsSender) {
    const Uuid sender = uuid::generate_v4();
    const Uuid blocker = uuid::generate_v4();
    const Exchange opened = call(drogon::Put, "/chat/direct/" + uuid::to_string(blocker), sender,
                                 R"({"kind":"direct"})");
    ASSERT_EQ(opened.response->statusCode(), drogon::k200OK) << opened.response->body();
    const Uuid direct = *uuid::parse(*[&] {
        input::BodyArena arena;
        const input::JsonDocument doc = input::parse_json(opened.response->body(), arena);
        return std::optional<std::string>{
            std::string{*doc.root().find("conversation")->find("id")->as_string()}};
    }());
    // Made before the block, which refuses adding either of them to anything
    // new together; a block touches no group that already exists (§3.6).
    const Uuid sentinel_group = group(sender, {blocker});
    ASSERT_EQ(call(drogon::Put, "/chat/blocks/" + uuid::to_string(sender), blocker)
                  .response->statusCode(),
              drogon::k204NoContent);

    WsClient senders = live_socket(sender);
    WsClient blockers = live_socket(blocker);
    const std::int64_t hidden = send_text(direct, sender, "you will not see this");
    expect_wake(next_wake(senders), direct, hidden);

    const std::int64_t sentinel = send_text(sentinel_group, sender, "but this one");
    expect_wake(next_wake(blockers), sentinel_group, sentinel);
}

TEST_F(ChatSocketListener, ARemovedMemberIsNotWokenForTheNextMessage) {
    const Uuid owner = uuid::generate_v4();
    const Uuid leaving = uuid::generate_v4();
    const Uuid c = group(owner, {leaving});
    const Uuid sentinel_group = group(owner, {leaving});
    WsClient theirs = live_socket(leaving);

    expect_wake(next_wake(theirs), c, send_text(c, owner, "while a member"));
    ASSERT_EQ(call(drogon::Delete,
                   conversation_path(c, "/members/" + uuid::to_string(leaving)), owner)
                  .response->statusCode(),
              drogon::k204NoContent);
    (void)send_text(c, owner, "after the removal");
    const std::int64_t sentinel = send_text(sentinel_group, owner, "sentinel");
    expect_wake(next_wake(theirs), sentinel_group, sentinel);
}

// --- one socket per device ------------------------------------------------------

TEST_F(ChatSocketListener, ASecondSocketFromTheSameDeviceClosesTheFirstAsReplaced) {
    const Uuid user = uuid::generate_v4();
    const Uuid session = uuid::generate_v7();
    WsClient first = live_socket(user, session);
    WsClient second;
    ASSERT_TRUE(second.open(testfixture::listener_port(), "/chat/socket",
                            socket_headers(user, session)));
    std::optional<WsFrame> closed;
    while ((closed = first.next(kDeadline)).has_value() && closed->opcode != 0x8) {}
    ASSERT_TRUE(closed.has_value()) << "the first socket was never closed";
    EXPECT_EQ(closed->close_code, chat::kCloseReplaced);

    // And the second one is the live one.
    const Uuid c = group(user);
    expect_wake(next_wake(second), c, send_text(c, user, "here"));
}

// --- what a client may send -------------------------------------------------------

TEST_F(ChatSocketListener, AClientPingIsAnsweredWithAPong) {
    WsClient ws = live_socket(uuid::generate_v4());
    std::array<std::uint8_t, frames::kClientPingBytes> ping{};
    ASSERT_EQ(frames::encode(frames::ClientPing{}, ping), ping.size());
    ASSERT_TRUE(ws.send_binary(ping));
    const std::optional<Received> got = next_frame(ws);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(std::holds_alternative<frames::Pong>(got->frame));
}

TEST_F(ChatSocketListener, ATextFrameAndAMalformedFrameEachCloseTheSocket) {
    WsClient text = live_socket(uuid::generate_v4());
    ASSERT_TRUE(text.send_text("{\"send\":\"over the socket\"}"));
    std::optional<WsFrame> closed;
    while ((closed = text.next(kDeadline)).has_value() && closed->opcode != 0x8) {}
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->close_code, chat::kCloseBadFrame);

    WsClient garbled = live_socket(uuid::generate_v4());
    // A server frame type, sent upstream: a client replaying what it was sent.
    const std::array<std::uint8_t, 2> wrong_way{frames::kFrameVersion, 0x01};
    ASSERT_TRUE(garbled.send_binary(wrong_way));
    while ((closed = garbled.next(kDeadline)).has_value() && closed->opcode != 0x8) {}
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->close_code, chat::kCloseBadFrame);
}

TEST_F(ChatSocketListener, AHandshakeWithoutACredentialOrFromAForeignOriginIsRefused) {
    WsClient anonymous;
    EXPECT_FALSE(anonymous.open(testfixture::listener_port(), "/chat/socket",
                                "Origin: https://example.test\r\n"));
    EXPECT_NE(anonymous.status(), 101);

    WsClient foreign;
    EXPECT_FALSE(foreign.open(testfixture::listener_port(), "/chat/socket",
                              socket_headers(uuid::generate_v4(), uuid::generate_v7(),
                                             kForeignOrigin)));
    EXPECT_NE(foreign.status(), 101);
}

// --- the SSE fallback -------------------------------------------------------------

// The next event a stream holds, polled against the deadline: the stream is
// drained by whoever writes the response, and here that is the case itself.
[[nodiscard]] std::optional<notifications::SseEvent> next_event(notifications::SseStream& stream) {
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    std::array<notifications::SseEvent, 1> one{};
    while (std::chrono::steady_clock::now() < until) {
        if (stream.drain(one) == 1) { return one[0]; }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return std::nullopt;
}

TEST_F(ChatSocketListener, AWakeReachesAFallbackStreamAsTheConversationToCatchUp) {
    const Uuid owner = uuid::generate_v4();
    const Uuid reader = uuid::generate_v4();
    const Uuid c = group(owner, {reader});
    auto opened = stack().sse.open(reader);
    ASSERT_TRUE(opened.ok());
    const notifications::StreamPtr stream = opened.value();
    chat::ChatLive::StreamLease lease = stack().live->follow_on_stream(reader);

    // The subscription is confirmed the way it is for a socket.
    const auto sync = next_event(*stream);
    ASSERT_TRUE(sync.has_value());
    EXPECT_EQ(sync->type, notifications::SseEventKind::ChatSync);

    (void)send_text(c, owner, "over the fallback");
    const auto wake = next_event(*stream);
    ASSERT_TRUE(wake.has_value());
    EXPECT_EQ(wake->type, notifications::SseEventKind::ChatWake);
    EXPECT_EQ(wake->notification, c);
    stack().sse.close(stream->id());
}

TEST_F(ChatSocketListener, AStreamWithoutALeaseCarriesNoChat) {
    const Uuid owner = uuid::generate_v4();
    const Uuid reader = uuid::generate_v4();
    const Uuid c = group(owner, {reader});
    auto opened = stack().sse.open(reader);
    ASSERT_TRUE(opened.ok());
    const notifications::StreamPtr stream = opened.value();
    // A socket for the same reader, so the wake reaches this process and the
    // stream is passed over only because it holds no lease.
    WsClient socket = live_socket(reader);

    const std::int64_t seq = send_text(c, owner, "socket only");
    expect_wake(next_wake(socket), c, seq);
    // The socket has its wake, and the subscriber delivers to a reader's socket
    // and stream in one call, so had the stream been sent one it would hold it.
    EXPECT_EQ(stream->queued(), 0U);
    stack().sse.close(stream->id());
}

TEST_F(ChatSocketListener, ReleasingTheLastLeaseStopsTheStreamsWakes) {
    const Uuid owner = uuid::generate_v4();
    const Uuid reader = uuid::generate_v4();
    const Uuid c = group(owner, {reader});
    // The socket first, so the reader stays subscribed throughout and the
    // stream's silence afterwards is the lease's doing, not a lost channel.
    WsClient socket = live_socket(reader);
    auto opened = stack().sse.open(reader);
    ASSERT_TRUE(opened.ok());
    const notifications::StreamPtr stream = opened.value();
    {
        chat::ChatLive::StreamLease lease = stack().live->follow_on_stream(reader);
        const std::int64_t held = send_text(c, owner, "while leased");
        expect_wake(next_wake(socket), c, held);
        const auto wake = next_event(*stream);
        ASSERT_TRUE(wake.has_value());
        EXPECT_EQ(wake->type, notifications::SseEventKind::ChatWake);
    }
    const std::int64_t seq = send_text(c, owner, "after the lease");
    expect_wake(next_wake(socket), c, seq);
    EXPECT_EQ(stream->queued(), 0U);
    stack().sse.close(stream->id());
}

// --- mutations (docs/22-chat.md §4.5) ----------------------------------------------

void expect_mutation(const std::optional<Received>& got, const Uuid& c, std::int64_t mutation) {
    ASSERT_TRUE(got.has_value()) << "nothing within the deadline";
    const auto* changed = std::get_if<frames::Mutation>(&got->frame);
    ASSERT_NE(changed, nullptr) << "frame type " << got->frame.index();
    EXPECT_EQ(changed->conversation, c);
    EXPECT_EQ(changed->mutation, mutation);
}

TEST_F(ChatSocketListener, AnEditARevokeAndAReactionEachWakeTheMembersWithTheCounter) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const std::int64_t seq = send_text(c, owner, "to be changed");
    WsClient theirs = live_socket(member);
    WsClient mine = live_socket(owner);

    const std::string message = conversation_path(c, "/messages/" + std::to_string(seq));
    ASSERT_EQ(call(drogon::Patch, message, owner, R"({"body":"changed"})").response->statusCode(),
              drogon::k204NoContent);
    expect_mutation(next_wake(theirs), c, 1);
    // The author's other devices hold the old text too.
    expect_mutation(next_wake(mine), c, 1);

    ASSERT_EQ(call(drogon::Put, message + "/reaction", member, R"({"reaction":"x"})")
                  .response->statusCode(),
              drogon::k204NoContent);
    expect_mutation(next_wake(theirs), c, 2);
    ASSERT_EQ(call(drogon::Delete, message, owner).response->statusCode(), drogon::k204NoContent);
    expect_mutation(next_wake(theirs), c, 3);

    // And the catch-up they wake a device for reads exactly those changes.
    const Exchange changed =
        call(drogon::Get, conversation_path(c, "/messages?changed_after=0"), member);
    ASSERT_EQ(changed.response->statusCode(), drogon::k200OK);
    EXPECT_NE(changed.response->body().find(R"("revoked":true)"), std::string::npos);
    EXPECT_NE(changed.response->body().find(R"("mutation":3})"), std::string::npos)
        << changed.response->body();
}

TEST_F(ChatSocketListener, AMutationReachesAFallbackStreamAsTheConversationToCatchUp) {
    const Uuid owner = uuid::generate_v4();
    const Uuid reader = uuid::generate_v4();
    const Uuid c = group(owner, {reader});
    const std::int64_t seq = send_text(c, owner, "to be edited");
    auto opened = stack().sse.open(reader);
    ASSERT_TRUE(opened.ok());
    const notifications::StreamPtr stream = opened.value();
    chat::ChatLive::StreamLease lease = stack().live->follow_on_stream(reader);
    const auto sync = next_event(*stream);
    ASSERT_TRUE(sync.has_value());

    ASSERT_EQ(call(drogon::Patch, conversation_path(c, "/messages/" + std::to_string(seq)), owner,
                   R"({"body":"edited"})")
                  .response->statusCode(),
              drogon::k204NoContent);
    const auto wake = next_event(*stream);
    ASSERT_TRUE(wake.has_value());
    EXPECT_EQ(wake->type, notifications::SseEventKind::ChatWake);
    EXPECT_EQ(wake->notification, c);
    stack().sse.close(stream->id());
}

// --- typing ------------------------------------------------------------------------

void type_in(WsClient& ws, const Uuid& c) {
    std::array<std::uint8_t, frames::kClientTypingBytes> frame{};
    ASSERT_EQ(frames::encode(frames::ClientTyping{.conversation = c}, frame), frame.size());
    ASSERT_TRUE(ws.send_binary(frame));
}

void expect_typing(const std::optional<Received>& got, const Uuid& c, const Uuid& who) {
    ASSERT_TRUE(got.has_value()) << "nothing within the deadline";
    const auto* typing = std::get_if<frames::Typing>(&got->frame);
    ASSERT_NE(typing, nullptr) << "frame type " << got->frame.index();
    EXPECT_EQ(typing->conversation, c);
    EXPECT_EQ(typing->user, who);
}

TEST_F(ChatSocketListener, TypingReachesTheOtherMembersAndNotTheTypistsOtherDevices) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    WsClient typist = live_socket(owner);
    WsClient phone = live_socket(owner, Opening::Another);
    WsClient theirs = live_socket(member);

    type_in(typist, c);
    expect_typing(next_wake(theirs), c, owner);
    // The sentinel for the typist's own phone: had the typing reached it, it
    // would come before this wake.
    const std::int64_t seq = send_text(c, member, "after the typing");
    expect_wake(next_wake(phone), c, seq);
}

TEST_F(ChatSocketListener, TypingIsRelayedOnceEveryThreeSecondsPerConversation) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const Uuid other = group(owner, {member});
    WsClient typist = live_socket(owner);
    WsClient theirs = live_socket(member);

    type_in(typist, c);
    type_in(typist, c);
    type_in(typist, c);
    // Another conversation has a budget of its own.
    type_in(typist, other);
    // One relay each, in either order: each is its own task on db_pool.
    const std::optional<Received> first = next_wake(theirs);
    const std::optional<Received> second = next_wake(theirs);
    ASSERT_TRUE(first.has_value() && second.has_value()) << "nothing within the deadline";
    const auto* one = std::get_if<frames::Typing>(&first->frame);
    const auto* two = std::get_if<frames::Typing>(&second->frame);
    ASSERT_TRUE(one != nullptr && two != nullptr);
    EXPECT_EQ(std::set<Uuid>({one->conversation, two->conversation}), std::set<Uuid>({c, other}));
    EXPECT_EQ(one->user, owner);
    EXPECT_EQ(two->user, owner);
    const std::int64_t seq = send_text(c, owner, "sentinel");
    expect_wake(next_wake(theirs), c, seq);
}

TEST_F(ChatSocketListener, TypingFromANonMemberOrPastABlockIsDroppedSilently) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid stranger = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const Exchange direct = call(drogon::Put, "/chat/direct/" + uuid::to_string(member), owner,
                                 R"({"kind":"direct"})");
    ASSERT_EQ(direct.response->statusCode(), drogon::k200OK);
    input::BodyArena arena;
    const input::JsonDocument doc = input::parse_json(direct.response->body(), arena);
    const Uuid pair = *uuid::parse(*doc.root().find("conversation")->find("id")->as_string());
    ASSERT_EQ(call(drogon::Put, "/chat/blocks/" + uuid::to_string(owner), member)
                  .response->statusCode(),
              drogon::k204NoContent);

    WsClient outsider = live_socket(stranger);
    WsClient blocked = live_socket(owner);
    WsClient theirs = live_socket(member);
    type_in(outsider, c);
    type_in(blocked, pair);

    // Neither reached the member, and the stranger's socket is still open:
    // a refusal here is silence, never a close the stranger could time.
    const std::int64_t seq = send_text(c, owner, "sentinel");
    expect_wake(next_wake(theirs), c, seq);
    std::array<std::uint8_t, frames::kClientPingBytes> ping{};
    ASSERT_EQ(frames::encode(frames::ClientPing{}, ping), ping.size());
    ASSERT_TRUE(outsider.send_binary(ping));
    const std::optional<Received> pong = next_frame(outsider);
    ASSERT_TRUE(pong.has_value());
    EXPECT_TRUE(std::holds_alternative<frames::Pong>(pong->frame));
}

// --- presence ----------------------------------------------------------------------

[[nodiscard]] std::string presence_of(const Uuid& viewer, const Uuid& subject) {
    const Exchange got =
        call(drogon::Get, "/chat/presence/" + uuid::to_string(subject), viewer);
    EXPECT_EQ(got.response->statusCode(), drogon::k200OK) << got.response->body();
    return std::string{got.response->body()};
}

// Polled: a first socket is recorded by the tracker's next pass, not at once.
[[nodiscard]] bool comes_online(const Uuid& viewer, const Uuid& subject) {
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < until) {
        const std::string body = presence_of(viewer, subject);
        input::BodyArena arena;
        const input::JsonDocument doc = input::parse_json(body, arena);
        const input::JsonValue* online =
            doc.ok() && doc.root().is_object() ? doc.root().find("online") : nullptr;
        if (online != nullptr && online->as_bool() == std::optional<bool>{true}) {
            const input::JsonValue* seen = doc.root().find("last_seen");
            EXPECT_TRUE(seen != nullptr && !seen->is_null()) << body;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}

TEST_F(ChatSocketListener, PresenceIsAnsweredPerViewerAndWithheldReadsAsNeverSeen) {
    const Uuid viewer = uuid::generate_v4();
    const Uuid subject = uuid::generate_v4();
    const std::string never = presence_of(viewer, uuid::generate_v4());
    EXPECT_EQ(never, R"({"online":false,"last_seen":null})");

    WsClient theirs = live_socket(subject);
    EXPECT_TRUE(comes_online(viewer, subject));

    // The hook hides this account from everybody but itself. Its own view says
    // it is online, so the tracker has recorded it, and only then is the other
    // viewer's answer worth comparing: the bytes of an account never seen.
    WsClient shy = live_socket(shy_account());
    EXPECT_TRUE(comes_online(shy_account(), shy_account()));
    EXPECT_EQ(presence_of(viewer, shy_account()), never);
}

TEST_F(ChatSocketListener, ABatchOfPresenceIsEachAccountAsTheSingleReadWouldAnswerIt) {
    const Uuid viewer = uuid::generate_v4();
    const Uuid online = uuid::generate_v4();
    const Uuid unknown = uuid::generate_v4();
    WsClient theirs = live_socket(online);
    EXPECT_TRUE(comes_online(viewer, online));
    WsClient shy = live_socket(shy_account());
    EXPECT_TRUE(comes_online(shy_account(), shy_account()));

    // In the order asked, a duplicate once, and a withheld account byte for
    // byte the entry of one nobody has heard of.
    const std::string ids = uuid::to_string(online) + "," + uuid::to_string(shy_account()) + "," +
                            uuid::to_string(unknown) + "," + uuid::to_string(online);
    const Exchange batch = call(drogon::Get, "/chat/presence?users=" + ids, viewer);
    ASSERT_EQ(batch.response->statusCode(), drogon::k200OK) << batch.response->body();
    const std::string_view body = batch.response->body();
    const auto entry = [](const Uuid& who, std::string_view rest) {
        return R"({"user":")" + uuid::to_string(who) + R"(",)" + std::string{rest};
    };
    EXPECT_NE(body.find(entry(online, R"("online":true,"last_seen":")")), std::string_view::npos)
        << body;
    EXPECT_NE(body.find(entry(shy_account(), R"("online":false,"last_seen":null})")),
              std::string_view::npos);
    EXPECT_NE(body.find(entry(unknown, R"("online":false,"last_seen":null})")),
              std::string_view::npos);
    EXPECT_LT(body.find(uuid::to_string(online)), body.find(uuid::to_string(shy_account())));
    EXPECT_LT(body.find(uuid::to_string(shy_account())), body.find(uuid::to_string(unknown)));
    EXPECT_EQ(body.find(uuid::to_string(online), body.find(uuid::to_string(unknown))),
              std::string_view::npos)
        << "a duplicate is answered once";

    // More than a page is refused by name, and so is a malformed id.
    std::string many;
    for (std::size_t i = 0; i <= chat::kMaxPresenceBatch; ++i) {
        if (i != 0) { many += ','; }
        many += uuid::to_string(uuid::generate_v4());
    }
    const Exchange too_many = call(drogon::Get, "/chat/presence?users=" + many, viewer);
    EXPECT_EQ(too_many.response->statusCode(), drogon::k400BadRequest);
    EXPECT_NE(too_many.response->body().find(R"("users":"TOO_LONG")"), std::string::npos);
    EXPECT_EQ(call(drogon::Get, "/chat/presence?users=nope", viewer).response->statusCode(),
              drogon::k400BadRequest);
}

}  // namespace
}  // namespace anvil
