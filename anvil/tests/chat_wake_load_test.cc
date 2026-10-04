// Phase 19's gate: wake latency on a busy group, beside a thousand idle sockets.
//
// The send path of chat_load_test.cc, now with live delivery: eight senders into
// one sixteen-member group whose every member holds a socket, while a thousand
// other accounts hold sockets that nothing is sent to. Each send's commit is
// timed by the message hook, which the service calls at the commit and before
// the wake; each wake is timed as it is read off its socket. Both clocks are
// this process's steady clock, so the difference is commit to arrival through
// the real path — the member cache, the render, SPUBLISH, the subscriber's
// thread, the hub's ring, the loop's write and the kernel.
//
// The numbers are RECORDED and never asserted (docs/16): a latency threshold is
// the flaky kind of test. What is asserted is what only load can falsify:
//
//   1. No send fails, and no socket is closed under the load.
//   2. Every member's socket receives a wake for every message, once.
//   3. On one socket, wakes arrive in the order of their seqs for each sender.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "anvil/chat/activity_gate.h"
#include "anvil/chat/frames.h"
#include "anvil/core/uuid.h"
#include "chat_listener_fixture.h"
#include "ws_client.h"

namespace anvil {
namespace {

using namespace chatfixture;  // NOLINT(google-build-using-namespace)
namespace frames = chat::frames;
using testfixture::WsClient;
using testfixture::WsFrame;

constexpr int kSenders = 8;
constexpr int kPerSender = 250;
constexpr int kMembers = 16;
constexpr int kIdle = 1000;
constexpr int kTotal = kSenders * kPerSender;
// Seqs run past the message count by the group's own system messages.
constexpr std::size_t kSeqSlots = 4096;

// The presence tracker's thread outlives the cases unless it is stopped
// before the pools go, as chat_socket_listener_test.cc says.
class LiveShutdown final : public ::testing::Environment {
public:
    void TearDown() override {
        if (stack().live != nullptr) { stack().live->stop(); }
    }
};

const ::testing::Environment* const kLiveShutdown =
    ::testing::AddGlobalTestEnvironment(new LiveShutdown{});

[[nodiscard]] std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::string socket_headers(const Uuid& user) {
    std::string headers;
    headers.append("Origin: ").append(kAllowedOrigin).append("\r\n");
    headers.append("Cookie: ").append(cookie_for(user)).append("\r\n");
    return headers;
}

// A user's first socket, live once its wake channel is confirmed.
[[nodiscard]] bool open_live(WsClient& ws, const Uuid& user) {
    if (!ws.open(testfixture::listener_port(), "/chat/socket", socket_headers(user))) {
        return false;
    }
    const std::optional<WsFrame> raw = ws.next(std::chrono::seconds{10});
    if (!raw.has_value() || raw->opcode != 0x2) { return false; }
    const auto decoded = frames::decode_downstream(raw->payload);
    return decoded && std::holds_alternative<frames::Sync>(decoded.value());
}

[[nodiscard]] double percentile(std::vector<std::int64_t>& samples, double p) {
    if (samples.empty()) { return 0.0; }
    const auto at = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(at),
                     samples.end());
    return static_cast<double>(samples[at]) / 1e6;
}

// One member's socket and what arrived on it.
// `arrivals` is the reader thread's alone until it is joined; the counter is
// what the main thread watches meanwhile.
struct Member final {
    WsClient                                           ws;
    std::vector<std::pair<std::int64_t, std::int64_t>> arrivals;   // (seq, ns)
    std::atomic<std::size_t>                           count{0};
    std::atomic<bool>                                  closed{false};
};

TEST(ChatWakeLoad, ABusyGroupBesideAThousandIdleSocketsRecordsItsWakeLatency) {
    if (!testfixture::pool_ready()) {
        GTEST_SKIP() << "no MongoDB at " << testfixture::test_uri();
    }
    if (!testfixture::transactions_available()) {
        GTEST_SKIP() << "transactions need a replica set";
    }
    ANVIL_REQUIRE_REDIS();
    ASSERT_TRUE(testfixture::pools_ready());
    testfixture::ensure_indexes();
    if (stack().live == nullptr) { GTEST_SKIP() << "no live delivery"; }

    // The commit clock, by seq, and the activity gate a deployment runs.
    auto committed = std::make_unique<std::array<std::atomic<std::int64_t>, kSeqSlots>>();
    for (auto& slot : *committed) { slot.store(0, std::memory_order_relaxed); }
    Uuid conversation{};
    stack_hooks().claim_activity_bump = chat::redis_activity_gate();
    stack_hooks().on_message = [&](const chat::MessageEvent& event) {
        if (event.conversation != conversation) { return; }
        if (event.seq > 0 && static_cast<std::size_t>(event.seq) < kSeqSlots) {
            (*committed)[static_cast<std::size_t>(event.seq)].store(now_ns(),
                                                                    std::memory_order_release);
        }
    };

    std::vector<Uuid> people;
    for (int i = 0; i < kMembers; ++i) { people.push_back(uuid::generate_v4()); }
    {
        auto setup = db::MongoPool::instance().acquire();
        const std::vector<Uuid> others(people.begin() + 1, people.end());
        const Result<chat::CreatedConversation> made = stack().service.create(
            *setup, chat::Actor{people.front(), perm_mask(testapp::Perm::ChatCreateGroup)},
            chat::CreateConversation{"group", "Hot", "", others, false});
        ASSERT_TRUE(made.ok()) << static_cast<int>(made.code());
        conversation = made.value().conversation.id;
    }

    // A thousand accounts with a socket each and nothing sent to them: the
    // registry, the subscriber's channel set and the pings all carry them.
    std::vector<WsClient> idle(kIdle);
    int idle_open = 0;
    for (WsClient& ws : idle) {
        if (open_live(ws, uuid::generate_v4())) { ++idle_open; }
    }
    ASSERT_EQ(idle_open, kIdle) << "not every idle socket opened";

    std::vector<std::unique_ptr<Member>> members;
    for (const Uuid& person : people) {
        auto member = std::make_unique<Member>();
        ASSERT_TRUE(open_live(member->ws, person));
        member->arrivals.reserve(kTotal + 64);
        members.push_back(std::move(member));
    }

    std::atomic<bool> reading{true};
    std::vector<std::thread> readers;
    for (const auto& member : members) {
        readers.emplace_back([&reading, &conversation, m = member.get()] {
            while (reading.load(std::memory_order_acquire)) {
                const std::optional<WsFrame> raw = m->ws.next(std::chrono::milliseconds{100});
                if (!raw.has_value()) {
                    if (!m->ws.is_open()) {
                        m->closed.store(true, std::memory_order_release);
                        return;
                    }
                    continue;
                }
                const std::int64_t at = now_ns();
                if (raw->opcode == 0x8) {
                    m->closed.store(true, std::memory_order_release);
                    return;
                }
                if (raw->opcode != 0x2) { continue; }
                const auto decoded = frames::decode_downstream(raw->payload);
                if (!decoded) { continue; }
                if (const auto* wake = std::get_if<frames::Wake>(&decoded.value());
                    wake != nullptr && wake->conversation == conversation) {
                    m->arrivals.emplace_back(wake->seq, at);
                    m->count.fetch_add(1, std::memory_order_release);
                }
                // A member answers the server's pings, or a run longer than the
                // silence limit would close its socket.
                if (std::holds_alternative<frames::Ping>(decoded.value())) {
                    std::array<std::uint8_t, frames::kClientPongBytes> pong{};
                    (void)m->ws.send_binary(std::span<const std::uint8_t>{
                        pong.data(), frames::encode(frames::ClientPong{}, pong)});
                }
            }
        });
    }

    std::atomic<int> failed{0};
    std::array<std::vector<std::int64_t>, kSenders> sent{};
    std::vector<std::thread> senders;
    const auto started = std::chrono::steady_clock::now();
    for (int s = 0; s < kSenders; ++s) {
        senders.emplace_back([&, s] {
            auto entry = db::MongoPool::instance().acquire();
            const chat::Actor sender{people[static_cast<std::size_t>(s)], PermSet{}};
            for (int i = 0; i < kPerSender; ++i) {
                chat::SendMessage message{};
                message.client_id = crypto::random_array<16>();
                const std::string body = "message " + std::to_string(i);
                message.body = body;
                const Result<chat::SentMessage> done =
                    stack().service.send(*entry, sender, conversation, message);
                if (!done) {
                    failed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                sent[static_cast<std::size_t>(s)].push_back(done.value().seq);
            }
        });
    }
    for (std::thread& sender : senders) { sender.join(); }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // Every member's socket is owed a wake for every message.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    for (const auto& member : members) {
        while (std::chrono::steady_clock::now() < until &&
               !member->closed.load(std::memory_order_acquire) &&
               member->count.load(std::memory_order_acquire) < static_cast<std::size_t>(kTotal)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    reading.store(false, std::memory_order_release);
    for (std::thread& reader : readers) { reader.join(); }
    stack_hooks() = StackHooks{};

    // 1.
    EXPECT_EQ(failed.load(), 0);
    std::vector<std::int64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(kTotal) * kMembers);
    std::size_t missing = 0;
    for (const auto& member : members) {
        EXPECT_FALSE(member->closed.load()) << "a member's socket was closed under load";
        // 2. Once each.
        std::vector<std::int64_t> seqs;
        for (const auto& [seq, at] : member->arrivals) {
            seqs.push_back(seq);
            const std::int64_t commit =
                (*committed)[static_cast<std::size_t>(seq)].load(std::memory_order_acquire);
            if (commit != 0) { latencies.push_back(at - commit); }
        }
        std::sort(seqs.begin(), seqs.end());
        EXPECT_TRUE(std::adjacent_find(seqs.begin(), seqs.end()) == seqs.end());
        missing += static_cast<std::size_t>(kTotal) - std::min<std::size_t>(seqs.size(), kTotal);
        // 3. Per sender, in order.
        for (const auto& mine : sent) {
            std::vector<std::int64_t> order;
            for (const auto& [seq, at] : member->arrivals) {
                if (std::binary_search(mine.begin(), mine.end(), seq)) { order.push_back(seq); }
            }
            EXPECT_TRUE(std::is_sorted(order.begin(), order.end()));
        }
    }
    EXPECT_EQ(missing, 0U) << "wakes that never reached a member's socket";

    const double per_second = kTotal / seconds;
    const double p50 = percentile(latencies, 0.50);
    const double p99 = percentile(latencies, 0.99);
    RecordProperty("sends", kTotal);
    RecordProperty("idle_sockets", kIdle);
    RecordProperty("member_sockets", kMembers);
    RecordProperty("sends_per_second", static_cast<int>(per_second));
    RecordProperty("wake_p50_us", static_cast<int>(p50 * 1000));
    RecordProperty("wake_p99_us", static_cast<int>(p99 * 1000));
    std::printf("chat wakes: %d sends by %d senders to %d members beside %d idle sockets in "
                "%.2f s, %.0f sends/s; commit to wake over %zu arrivals: p50 %.2f ms, "
                "p99 %.2f ms\n",
                kTotal, kSenders, kMembers, kIdle, seconds, per_second, latencies.size(), p50,
                p99);
}

}  // namespace
}  // namespace anvil
