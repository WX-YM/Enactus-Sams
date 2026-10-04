// Two processes, one conversation (docs/15 phase 19, docs/22-chat.md §5.4).
//
// This binary is its own peer. Run with kPeerFlag it is a chat server — the
// listener fixture's stack, behind the real access filter, on a port it prints
// — and nothing else; run without, it is the test, and the test spawns two of
// those against the one MongoDB and the one Redis. A send on one wakes a socket
// on the other only through what a deployment shares, because the two have
// nothing else in common: separate hubs, member caches, subscribers and
// publishers, and each its own address space.
//
// The kill case is the claim the phase is built on: a lost wake costs latency,
// never a message. A process SIGKILLed mid-burst loses whatever it had not yet
// published, and the other process's client still ends holding exactly what
// MongoDB committed, because it syncs from its cursor.
//
// The peer exits through main when its stdin closes, after stopping its live
// delivery, so LeakSanitizer reads a clean process; its exit status is
// asserted, because a leak report in a child is otherwise a line nobody reads.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/find.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/chat/frames.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "chat_listener_fixture.h"
#include "ws_client.h"

extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace anvil {
namespace {

using namespace chatfixture;  // NOLINT(google-build-using-namespace)
namespace frames = chat::frames;
using testfixture::WsClient;
using testfixture::WsFrame;

constexpr std::string_view kPeerFlag = "--chat-peer";
// A peer that cannot serve says why on this line and exits with it.
constexpr int kPeerSkipped = 3;
constexpr std::chrono::milliseconds kDeadline{10000};

// --- the peer ---------------------------------------------------------------------

int run_peer() {
    // Its stdout is the parent's pipe, read for one line; a log line there
    // would be read as the port, and one the parent stopped reading would
    // block the server once the pipe filled.
    trantor::Logger::setOutputFunction(
        [](const char* message, const std::uint64_t length) {
            std::fwrite(message, 1, length, stderr);
        },
        [] { std::fflush(stderr); });
    if (!testfixture::pool_ready() || !testfixture::transactions_available() ||
        !testfixture::pools_ready() || !testfixture::redis_ready()) {
        std::printf("skip no MongoDB replica set or no Redis\n");
        std::fflush(stdout);
        return kPeerSkipped;
    }
    testfixture::ensure_indexes();
    if (stack().live == nullptr) {
        std::printf("skip no live delivery\n");
        std::fflush(stdout);
        return kPeerSkipped;
    }
    const std::uint16_t port = testfixture::listener_port();
    std::printf("port %u\n", static_cast<unsigned>(port));
    std::fflush(stdout);

    // Serve until the parent closes our stdin, or kills us.
    std::array<char, 64> sink{};
    while (::read(STDIN_FILENO, sink.data(), sink.size()) > 0) {}
    // Before main returns and the pools go: the subscriber and the presence
    // tracker have threads of their own.
    stack().live->stop();
    return 0;
}

// One spawned peer: this binary with kPeerFlag, sharing the parent's
// environment, which carries the scratch database and the token key.
class Peer final {
public:
    Peer() = default;
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    ~Peer() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
        close_fd(stdin_);
        close_fd(stdout_);
    }

    // Empty when it is serving; otherwise why not, and `skipped` says whether
    // that is the machine's fault rather than the code's.
    [[nodiscard]] std::string start(bool& skipped) {
        std::array<int, 2> in{-1, -1};
        std::array<int, 2> out{-1, -1};
        if (::pipe(in.data()) != 0 || ::pipe(out.data()) != 0) { return "pipe failed"; }
        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, in[1]);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        std::string self = "/proc/self/exe";
        std::string flag{kPeerFlag};
        std::array<char*, 3> argv{self.data(), flag.data(), nullptr};
        const int spawned = ::posix_spawn(&pid_, "/proc/self/exe", &actions, nullptr,
                                          argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(in[0]);
        ::close(out[1]);
        stdin_ = in[1];
        stdout_ = out[0];
        if (spawned != 0) {
            pid_ = -1;
            return "posix_spawn failed";
        }

        // A sanitised first boot builds the index catalogue: generous, and over
        // the moment the line arrives.
        const std::string line = read_line(std::chrono::seconds{120});
        if (line.rfind("port ", 0) == 0) {
            port_ = static_cast<std::uint16_t>(std::stoul(line.substr(5)));
            return {};
        }
        skipped = line.rfind("skip", 0) == 0;
        return line.empty() ? "the peer printed nothing" : line;
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // SIGKILL: no destructor, no flush, no unsubscribe.
    void kill() {
        if (pid_ <= 0) { return; }
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }

    // Closes its stdin and waits for main to return. The exit status, or -1.
    [[nodiscard]] int stop() {
        close_fd(stdin_);
        if (pid_ <= 0) { return -1; }
        int status = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{60};
        while (::waitpid(pid_, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() > until) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
                pid_ = -1;
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

private:
    static void close_fd(int& fd) noexcept {
        if (fd >= 0) { ::close(fd); }
        fd = -1;
    }

    [[nodiscard]] std::string read_line(std::chrono::seconds timeout) {
        std::string line;
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < until) {
            pollfd p{stdout_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) { continue; }
            char c = 0;
            if (::read(stdout_, &c, 1) != 1) { break; }
            if (c == '\n') { return line; }
            line.push_back(c);
        }
        return line;
    }

    pid_t         pid_{-1};
    int           stdin_{-1};
    int           stdout_{-1};
    std::uint16_t port_{0};
};

// --- talking to a peer --------------------------------------------------------------

struct Answer final {
    int         status;
    std::string body;
};

// One HTTP/1.1 request on its own connection, closed by the server after the
// answer. Blocking and minimal: this process runs no event loop of its own.
[[nodiscard]] Answer http(std::uint16_t port, std::string_view method, std::string_view path,
                          const Uuid& user, std::string_view body = {}) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return {0, {}}; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {0, {}};
    }
    timeval timeout{};
    timeout.tv_sec = 10;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string request;
    request.append(method).append(" ").append(path).append(" HTTP/1.1\r\n");
    request.append("Host: 127.0.0.1\r\nConnection: close\r\n");
    request.append("Origin: ").append(kAllowedOrigin).append("\r\n");
    request.append("Cookie: ").append(cookie_for(user)).append("\r\n");
    if (!body.empty()) {
        request.append("Content-Type: application/json\r\n");
        request.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    }
    request.append("\r\n").append(body);
    std::size_t written = 0;
    while (written < request.size()) {
        const ssize_t n = ::send(fd, request.data() + written, request.size() - written,
                                 MSG_NOSIGNAL);
        if (n <= 0) {
            ::close(fd);
            return {0, {}};
        }
        written += static_cast<std::size_t>(n);
    }
    std::string response;
    std::array<char, 8192> chunk{};
    for (;;) {
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) { break; }
        response.append(chunk.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);
    const std::size_t head = response.find("\r\n\r\n");
    if (response.size() < 12 || head == std::string::npos) { return {0, {}}; }
    return {std::stoi(response.substr(9, 3)), response.substr(head + 4)};
}

// Every `"seq":N` in a history page, in order.
[[nodiscard]] std::vector<std::int64_t> seqs_in(std::string_view body) {
    std::vector<std::int64_t> out;
    constexpr std::string_view kKey = "\"seq\":";
    for (std::size_t at = body.find(kKey); at != std::string_view::npos;
         at = body.find(kKey, at + kKey.size())) {
        std::size_t end = at + kKey.size();
        std::int64_t value = 0;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') {
            value = (value * 10) + (body[end] - '0');
            ++end;
        }
        out.push_back(value);
    }
    return out;
}

[[nodiscard]] Uuid group_on(std::uint16_t port, const Uuid& owner, const Uuid& member) {
    const Answer made =
        http(port, "POST", "/chat/conversations", owner,
             R"({"cid":")" + cid() + R"(","kind":"group","title":"Peers","members":[")" +
                 uuid::to_string(member) +
                 R"("]})");
    EXPECT_EQ(made.status, 201) << made.body;
    constexpr std::string_view kId = "\"id\":\"";
    const std::size_t at = made.body.find(kId);
    if (at == std::string::npos) { return Uuid{}; }
    return uuid::parse(std::string_view{made.body}.substr(at + kId.size(), 36)).value_or(Uuid{});
}

// A send; the seq it was answered with, or nullopt when there was no answer.
[[nodiscard]] std::optional<std::int64_t> send_on(std::uint16_t port, const Uuid& c,
                                                  const Uuid& sender, std::string_view text) {
    const Answer sent = http(port, "POST", conversation_path(c, "/messages"), sender,
                             R"({"cid":")" + cid() + R"(","body":")" + std::string{text} +
                                 R"("})");
    if (sent.status != 201) { return std::nullopt; }
    return json_int(sent.body, "seq");
}

// Catch-up from `cursor` to the head, as a device syncs: pages of the most a
// page may hold until one comes back empty. Every seq it was given, and the
// cursor it ends at.
[[nodiscard]] std::pair<std::set<std::int64_t>, std::int64_t> sync_on(std::uint16_t port,
                                                                      const Uuid& c,
                                                                      const Uuid& reader,
                                                                      std::int64_t cursor) {
    std::set<std::int64_t> held;
    for (;;) {
        const Answer page =
            http(port, "GET",
                 conversation_path(c, "/messages?limit=100&after=" + std::to_string(cursor)),
                 reader);
        EXPECT_EQ(page.status, 200) << page.body;
        const std::vector<std::int64_t> seqs = seqs_in(page.body);
        if (page.status != 200 || seqs.empty()) { break; }
        held.insert(seqs.begin(), seqs.end());
        cursor = std::max(cursor, *std::max_element(seqs.begin(), seqs.end()));
    }
    return {held, cursor};
}

// What MongoDB holds for the conversation: every committed seq.
[[nodiscard]] std::set<std::int64_t> committed(const Uuid& c) {
    auto client = db::MongoPool::instance().acquire();
    const std::string_view collection = testapp::kChatCollections.messages;
    mongocxx::options::find options{};
    options.projection(bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("s", 1), bsoncxx::builder::basic::kvp("_id", 0)));
    std::set<std::int64_t> out;
    for (const bsoncxx::document::view row :
         (*client)[std::string{testfixture::scratch_names().for_collection(collection)}]
                  [std::string{collection}]
                      .find(bsoncxx::builder::basic::make_document(
                                bsoncxx::builder::basic::kvp("c", db::codec::uuid_bin(c))),
                            options)) {
        out.insert(row["s"].get_int64().value);
    }
    return out;
}

[[nodiscard]] std::string socket_headers(const Uuid& user) {
    std::string headers;
    headers.append("Origin: ").append(kAllowedOrigin).append("\r\n");
    headers.append("Cookie: ").append(cookie_for(user)).append("\r\n");
    return headers;
}

// The next downstream frame, the server's pings skipped, or nullopt.
[[nodiscard]] std::optional<frames::DownstreamFrame> next_frame(
    WsClient& ws, std::chrono::milliseconds timeout = kDeadline) {
    while (true) {
        const std::optional<WsFrame> raw = ws.next(timeout);
        if (!raw.has_value() || raw->opcode != 0x2) { return std::nullopt; }
        auto decoded = frames::decode_downstream(raw->payload);
        if (!decoded) { return std::nullopt; }
        frames::DownstreamFrame frame = std::move(decoded).value();
        if (std::holds_alternative<frames::Ping>(frame)) { continue; }
        // The inline message borrows the raw payload, which goes out of scope
        // here; nothing these cases assert reads it.
        if (auto* wake = std::get_if<frames::Wake>(&frame)) { wake->inline_message = {}; }
        return frame;
    }
}

// The reader's first socket on `port`, live once Redis confirmed its channel.
[[nodiscard]] WsClient live_socket(std::uint16_t port, const Uuid& user) {
    WsClient ws;
    EXPECT_TRUE(ws.open(port, "/chat/socket", socket_headers(user))) << "status " << ws.status();
    const auto sync = next_frame(ws);
    EXPECT_TRUE(sync.has_value() && std::holds_alternative<frames::Sync>(*sync))
        << "no Sync after the reader's first socket opened";
    return ws;
}

class ChatPeers : public ::testing::Test {
protected:
    void SetUp() override {
        if (!testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << testfixture::test_uri();
        }
        if (!testfixture::transactions_available()) {
            GTEST_SKIP() << "transactions need a replica set";
        }
        ANVIL_REQUIRE_REDIS();
        testfixture::ensure_indexes();
        for (Peer* peer : {&a_, &b_}) {
            bool skipped = false;
            const std::string why_not = peer->start(skipped);
            if (skipped) { GTEST_SKIP() << "a peer could not serve: " << why_not; }
            ASSERT_TRUE(why_not.empty()) << why_not;
        }
    }

    // B's exit status: zero only if it stopped cleanly with nothing leaked.
    void TearDown() override {
        if (b_.port() != 0) { EXPECT_EQ(b_.stop(), 0) << "peer B did not exit cleanly"; }
        if (a_.port() != 0) { (void)a_.stop(); }
    }

    Peer a_;
    Peer b_;
};

TEST_F(ChatPeers, ASendOnOneProcessWakesASocketOnTheOther) {
    const Uuid owner = uuid::generate_v4();
    const Uuid reader = uuid::generate_v4();
    const Uuid c = group_on(a_.port(), owner, reader);
    WsClient on_b = live_socket(b_.port(), reader);

    const std::optional<std::int64_t> seq = send_on(a_.port(), c, owner, "across processes");
    ASSERT_TRUE(seq.has_value());
    std::optional<frames::DownstreamFrame> got;
    do { got = next_frame(on_b); } while (got.has_value() && std::holds_alternative<frames::Sync>(*got));
    ASSERT_TRUE(got.has_value()) << "no wake on B for a send on A";
    const auto* wake = std::get_if<frames::Wake>(&*got);
    ASSERT_NE(wake, nullptr);
    EXPECT_EQ(wake->conversation, c);
    EXPECT_EQ(wake->seq, *seq);
}

TEST_F(ChatPeers, AProcessKilledMidBurstCostsTheOtherProcessesClientNothing) {
    const Uuid owner = uuid::generate_v4();
    const Uuid reader = uuid::generate_v4();
    const Uuid c = group_on(a_.port(), owner, reader);
    WsClient on_b = live_socket(b_.port(), reader);
    // The device syncs on opening, as the protocol says, and keeps its cursor.
    const auto [opened, cursor] = sync_on(b_.port(), c, reader, 0);

    // The device's wakes, read as they come.
    std::atomic<bool> reading{true};
    std::mutex woken_mutex;
    std::set<std::int64_t> woken;
    std::thread device{[&] {
        while (reading.load(std::memory_order_acquire)) {
            const auto got = next_frame(on_b, std::chrono::milliseconds{100});
            if (!got.has_value()) { continue; }
            if (const auto* wake = std::get_if<frames::Wake>(&*got)) {
                const std::lock_guard lock{woken_mutex};
                woken.insert(wake->seq);
            }
        }
    }};

    // Eight senders into A until it dies under them: enough in flight at the
    // kill that some are committed and never answered, and some allocated a
    // seq and never committed.
    constexpr int kSenders = 8;
    constexpr int kKillAfter = 40;
    std::atomic<int> answered{0};
    std::mutex acked_mutex;
    std::set<std::int64_t> acked;
    std::vector<std::thread> senders;
    senders.reserve(kSenders);
    for (int s = 0; s < kSenders; ++s) {
        senders.emplace_back([&, s] {
            for (int i = 0; i < 500; ++i) {
                const std::optional<std::int64_t> seq =
                    send_on(a_.port(), c, owner, "burst " + std::to_string(s) + "." +
                                                     std::to_string(i));
                if (!seq.has_value()) { return; }
                answered.fetch_add(1, std::memory_order_relaxed);
                const std::lock_guard lock{acked_mutex};
                acked.insert(*seq);
            }
        });
    }
    while (answered.load(std::memory_order_relaxed) < kKillAfter) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    a_.kill();
    for (std::thread& sender : senders) { sender.join(); }

    // B still serves, and a send through it is the sentinel: once its wake has
    // arrived on this one socket, every earlier wake that was ever coming has.
    const std::optional<std::int64_t> after_kill = send_on(b_.port(), c, owner, "still here");
    ASSERT_TRUE(after_kill.has_value());
    const auto sentinel_until = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < sentinel_until) {
        {
            const std::lock_guard lock{woken_mutex};
            if (woken.contains(*after_kill)) { break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    reading.store(false, std::memory_order_release);
    device.join();
    ASSERT_TRUE(woken.contains(*after_kill)) << "B did not wake its own socket after A died";

    // A commit A sent before it died lands on its own; give it a moment.
    std::this_thread::sleep_for(std::chrono::seconds{2});

    // The device recovers by sync from the cursor it already held.
    const auto [caught_up, end] = sync_on(b_.port(), c, reader, cursor);
    std::set<std::int64_t> held = opened;
    held.insert(woken.begin(), woken.end());
    held.insert(caught_up.begin(), caught_up.end());
    const std::set<std::int64_t> truth = committed(c);

    EXPECT_GE(acked.size(), static_cast<std::size_t>(kKillAfter));
    EXPECT_GT(woken.size(), 1U) << "A's sends never woke the socket on B";
    EXPECT_TRUE(std::includes(truth.begin(), truth.end(), woken.begin(), woken.end()))
        << "a wake named a message that was never committed";
    EXPECT_TRUE(std::includes(truth.begin(), truth.end(), acked.begin(), acked.end()))
        << "a send A answered is not in MongoDB";
    EXPECT_EQ(held, truth) << "after syncing, the device does not hold exactly what was committed";
    RecordProperty("answered_before_kill", static_cast<int>(acked.size()));
    RecordProperty("committed", static_cast<int>(truth.size()));
    RecordProperty("woken", static_cast<int>(woken.size()));
}

}  // namespace
}  // namespace anvil

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view{argv[1]} == anvil::kPeerFlag) {
        return anvil::run_peer();
    }
    // What a peer must share with this process to be the same deployment: the
    // database, and the key a cookie is signed with. Set before any peer is
    // spawned, so every one inherits them.
    ::setenv("ANVIL_TEST_SCRATCH_DB", anvil::testfixture::scratch_database().c_str(), 1);
    std::string hex;
    for (const std::uint8_t byte : anvil::crypto::random_array<32>()) {
        constexpr std::string_view kHex = "0123456789abcdef";
        hex.push_back(kHex[byte >> 4U]);
        hex.push_back(kHex[byte & 0x0FU]);
    }
    ::setenv(anvil::chatfixture::kTokenKeyEnv, hex.c_str(), 0);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
