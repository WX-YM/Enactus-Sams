// The server's half of end-to-end encryption against a live replica set
// (docs/22-chat.md §7): the device-set fence and what moves it.
//
// Every device here is a real key store, made the way a client makes one, so a
// case that passes is one a real client's bundle passes, and nothing is a byte
// string the server would never be sent.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <stdexcept>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/prekeys.h"
#include "anvil/chat/service.h"
#include "anvil/auth/token.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"
#include "anvil/crypto/ed25519.h"
#include "anvil/crypto/x25519.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/session_service.h"
#include "anvil/identity/users.h"
#include "anvil/media/grant.h"
#include "anvil/media/service.h"
#include "app_fixture.h"
#include "chat_device_client.h"
#include "db_fixture.h"
#include "testapp/chat_cards.h"
#include "testapp/chat_collections.h"
#include "testapp/chat_kinds.h"
#include "testapp/perms.h"

namespace {

namespace chat = anvil::chat;
namespace crypto = anvil::crypto;
namespace identity = anvil::identity;
using anvil::ErrorCode;
using anvil::Uuid;
using anvil::db::TimeMs;
using anvil::input::Reason;
using anvil::chattest::Client;
using anvil::chattest::make_client;
using anvil::testfixture::scratch_names;

constexpr std::string_view kMedia = "media";
constexpr std::string_view kUsers = "users";
constexpr std::string_view kSessions = "user_sessions";
constexpr std::array<std::uint8_t, 64> kGrantKey = [] {
    std::array<std::uint8_t, 64> key{};
    for (std::size_t i = 0; i < key.size(); ++i) { key[i] = static_cast<std::uint8_t>(i * 5U); }
    return key;
}();
constexpr std::array<std::uint8_t, 32> kPepper{{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                               9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}};

[[nodiscard]] Uuid person() { return anvil::uuid::generate_v4(); }

[[nodiscard]] chat::Actor creator(const Uuid& user) {
    return chat::Actor{user, anvil::perm_mask(testapp::Perm::ChatCreateGroup)};
}

[[nodiscard]] std::uint64_t unix_s(TimeMs at) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count());
}

using Bytes = std::vector<std::uint8_t>;

[[nodiscard]] Bytes bytes(std::size_t count, std::uint8_t fill) { return Bytes(count, fill); }

// An encrypted send as a client builds one: a client id minted once, so a
// second message() is the same attempt, and the ciphertexts it owns.
class Outgoing final {
public:
    Outgoing(const Uuid& device, std::int64_t version, Bytes common)
        : common_{std::move(common)},
          client_id_{crypto::random_array<16>()},
          version_{version},
          device_{device} {}

    Outgoing& to(const Client& recipient, Bytes ciphertext) {
        payloads_.push_back(std::move(ciphertext));
        targets_.push_back(recipient.device.id);
        return *this;
    }

    Outgoing& paged() {
        page_ = true;
        return *this;
    }

    [[nodiscard]] chat::EncryptedMessage message() {
        entries_.clear();
        for (std::size_t i = 0; i < payloads_.size(); ++i) {
            entries_.push_back(chat::DeviceCiphertext{payloads_[i], targets_[i]});
        }
        return chat::EncryptedMessage{.client_id = client_id_,
                                      .ciphertext = common_,
                                      .devices = entries_,
                                      .attachments = {},
                                      .device_set_version = version_,
                                      .device = device_,
                                      .page = page_};
    }

private:
    std::vector<Bytes> payloads_;
    std::vector<Uuid> targets_;
    std::vector<chat::DeviceCiphertext> entries_;
    Bytes common_;
    std::array<std::uint8_t, 16> client_id_;
    std::int64_t version_;
    Uuid device_;
    bool page_{false};
};

[[nodiscard]] Outgoing outgoing(const Client& from, std::int64_t version, Bytes common = {}) {
    return Outgoing{from.device.id, version, std::move(common)};
}

class ChatE2eeDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        if (!anvil::testfixture::transactions_available()) {
            GTEST_SKIP() << "transactions need a replica set";
        }
        anvil::testfixture::ensure_indexes();
        client_ =
            std::make_unique<mongocxx::pool::entry>(anvil::db::MongoPool::instance().acquire());
        for (const std::string_view collection :
             {testapp::kChatCollections.conversations, testapp::kChatCollections.members,
              testapp::kChatCollections.messages, testapp::kChatCollections.reactions,
              testapp::kChatCollections.invites, testapp::kChatCollections.blocks,
              testapp::kDeviceCollections.identities, testapp::kDeviceCollections.prekeys,
              testapp::kDeviceCollections.queue, kMedia}) {
            anvil::testfixture::clear_collection(**client_, collection);
        }
        repo_ = std::make_unique<chat::ChatRepository>(scratch_names(), testapp::kChatCollections,
                                                       testapp::kChatKinds);
        media_ = std::make_unique<anvil::media::MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);
        devices_ = std::make_unique<chat::DeviceDirectory>(
            scratch_names(), testapp::kDeviceCollections, testapp::kDeviceConfig);
        queue_ = std::make_unique<chat::DeviceQueue>(scratch_names(),
                                                     testapp::kDeviceCollections.queue);
        prekeys_ = std::make_unique<chat::PrekeyDirectory>(
            scratch_names(), testapp::kDeviceCollections, testapp::kDeviceConfig);
        chat::ChatHooks hooks{};
        hooks.may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; };
        hooks.authenticated_at = [](mongocxx::client&, const Uuid&,
                                    const Uuid&) -> std::optional<TimeMs> {
            return anvil::db::now_ms();
        };
        hooks.on_device = [this](const chat::DeviceEvent& event) { device_events_.push_back(event); };
        service_ = std::make_unique<chat::ChatService>(chat::ChatServiceDeps{
            .repository = *repo_, .media = *media_, .grants = grants_,
            .kinds = testapp::kChatKinds, .cards = testapp::kChatCards, .invite_pepper = kPepper,
            .hooks = std::move(hooks), .devices = devices_.get(), .queue = queue_.get(),
            .prekeys = prekeys_.get()});
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }
    [[nodiscard]] const chat::ChatService& service() const { return *service_; }
    [[nodiscard]] const chat::DeviceDirectory& devices() const { return *devices_; }
    [[nodiscard]] const chat::DeviceQueue& queue() const { return *queue_; }
    [[nodiscard]] static TimeMs now() { return anvil::db::now_ms(); }

    // Every pending device change pushed out, as the requests that made them
    // would have done.
    void settle(std::initializer_list<Uuid> users) {
        for (const Uuid& user : users) { ASSERT_TRUE(service().propagate_devices(db(), user).ok()); }
    }

    [[nodiscard]] chat::MessageRecord message(const Uuid& conversation, std::int64_t seq) {
        const auto found = repo_->find_message(db(), conversation, seq, now());
        EXPECT_TRUE(found.ok() && found.value().has_value());
        return found.ok() && found.value().has_value() ? *found.value() : chat::MessageRecord{};
    }

    // A first device for `user`, written to the directory only: the pending
    // change is left for the caller to propagate, or not.
    [[nodiscard]] Client registered(const Uuid& user, TimeMs now = anvil::db::now_ms()) {
        Client client = make_client();
        EXPECT_TRUE(devices().register_first_device(db(), user, client.device, now, now).ok());
        return client;
    }

    // A second device for `user`, admitted by `approver`; directory only.
    [[nodiscard]] Client linked(const Uuid& user, const Client& approver,
                                TimeMs now = anvil::db::now_ms()) {
        Client joining = make_client();
        const std::uint64_t at = unix_s(now);
        const crypto::Ed25519Signature signature = crypto::ed25519_sign(
            approver.seed,
            chat::link_message(user, joining.device.id, joining.device.keys.agreement,
                               joining.device.keys.signing, at));
        EXPECT_TRUE(devices()
                        .link_device(db(), user, approver.device.id, joining.device, at,
                                     signature, now)
                        .ok());
        return joining;
    }

    [[nodiscard]] Uuid group(const Uuid& owner, std::vector<Uuid> members, bool encrypted) {
        const auto made = service().create(
            db(), creator(owner),
            chat::CreateConversation{"group", "Team", "", members, encrypted});
        EXPECT_TRUE(made.ok()) << static_cast<int>(made.code());
        return made.ok() ? made.value().conversation.id : Uuid{};
    }

    [[nodiscard]] Uuid direct(const Uuid& a, const Uuid& b, bool encrypted) {
        const auto opened = service().open_direct(db(), creator(a), "direct", b, encrypted);
        EXPECT_TRUE(opened.ok()) << static_cast<int>(opened.code());
        return opened.ok() ? opened.value().conversation.id : Uuid{};
    }

    [[nodiscard]] chat::ConversationRecord conversation(const Uuid& id) {
        const auto found = repo_->find_conversation(db(), id);
        EXPECT_TRUE(found.ok() && found.value().has_value());
        return found.ok() && found.value().has_value() ? *found.value()
                                                       : chat::ConversationRecord{};
    }

    [[nodiscard]] std::int64_t dsv(const Uuid& id) { return conversation(id).device_set_version; }

    [[nodiscard]] std::optional<TimeMs> pending(const Uuid& user) {
        const auto found = devices().identity(db(), user);
        EXPECT_TRUE(found.ok() && found.value().has_value());
        return found.ok() && found.value().has_value() ? found.value()->pending_since
                                                       : std::nullopt;
    }

    // Every DevicesChanged message in a conversation, oldest first.
    [[nodiscard]] std::vector<chat::MessageRecord> notices(const Uuid& id) {
        const auto page = repo_->history(db(), id, 0, std::numeric_limits<std::int64_t>::max(),
                                         100, anvil::db::now_ms());
        EXPECT_TRUE(page.ok());
        std::vector<chat::MessageRecord> out;
        if (!page.ok()) { return out; }
        for (const chat::MessageRecord& m : page.value().messages) {
            if (m.system.has_value() && m.system->event == chat::SystemEvent::DevicesChanged) {
                out.push_back(m);
            }
        }
        return out;
    }

    anvil::media::GrantKeys grants_{1, kGrantKey};
    std::unique_ptr<mongocxx::pool::entry> client_;
    std::unique_ptr<chat::ChatRepository> repo_;
    std::unique_ptr<anvil::media::MediaService> media_;
    std::unique_ptr<chat::DeviceDirectory> devices_;
    std::unique_ptr<chat::DeviceQueue> queue_;
    std::unique_ptr<chat::PrekeyDirectory> prekeys_;
    std::vector<chat::DeviceEvent> device_events_;
    std::unique_ptr<chat::ChatService> service_;
};

// --- the device-set fence and what moves it (§7.4) ------------------------------

TEST_F(ChatE2eeDb, ADeviceChangeRaisesEveryCurrentConversationAndClearsTheMark) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid carol = person();
    const Uuid together = group(alice, {bob}, true);
    const Uuid plaintext = direct(alice, bob, false);
    const Uuid elsewhere = group(bob, {carol}, true);
    const Uuid left = group(carol, {alice}, false);
    ASSERT_TRUE(service().remove_member(db(), creator(alice), left, alice).ok());

    const std::int64_t together_before = dsv(together);
    const std::int64_t plaintext_before = dsv(plaintext);
    const std::int64_t elsewhere_before = dsv(elsewhere);
    const std::int64_t left_before = dsv(left);

    const TimeMs at = anvil::db::now_ms();
    (void)registered(alice, at);
    ASSERT_TRUE(pending(alice).has_value());
    ASSERT_TRUE(service().propagate_devices(db(), alice).ok());

    // Every conversation alice is IN, plaintext too: dsv is the fence a send
    // reads, and the mode is not the propagation's question.
    EXPECT_GT(dsv(together), together_before);
    EXPECT_GE(dsv(together), at.time_since_epoch().count());
    EXPECT_GT(dsv(plaintext), plaintext_before);
    // Not one she is not in, nor one she has left.
    EXPECT_EQ(dsv(elsewhere), elsewhere_before);
    EXPECT_EQ(dsv(left), left_before);
    EXPECT_FALSE(pending(alice).has_value());
}

TEST_F(ChatE2eeDb, ACrashBetweenTheIdentityWriteAndThePropagationIsFinishedByTheSweeper) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid together = direct(alice, bob, true);
    const std::int64_t before = dsv(together);

    // The device is written and the process dies: nothing propagated it.
    const TimeMs at = anvil::db::now_ms();
    (void)registered(alice, at);
    ASSERT_TRUE(pending(alice).has_value());
    EXPECT_EQ(dsv(together), before);

    // Inside the grace the change is the live request's to finish, not the
    // sweeper's.
    const auto early = service().sweep_device_changes(db(), at + std::chrono::seconds{30}, 10);
    ASSERT_TRUE(early.ok());
    EXPECT_EQ(early.value(), 0);
    EXPECT_EQ(dsv(together), before);

    const auto swept = service().sweep_device_changes(
        db(), at + chat::kDeviceChangeGrace + std::chrono::seconds{1}, 10);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 1);
    EXPECT_GT(dsv(together), before);
    EXPECT_FALSE(pending(alice).has_value());
    EXPECT_EQ(notices(together).size(), 1U);
}

TEST_F(ChatE2eeDb, TwoRacingDeviceChangesLeaveTheVersionAtOrPastTheLaterOne) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid together = group(alice, {bob}, true);

    const TimeMs now = anvil::db::now_ms();
    const TimeMs earlier = now - std::chrono::seconds{2};
    const TimeMs later = now + std::chrono::seconds{2};
    (void)registered(alice, earlier);
    (void)registered(bob, later);

    // The LATER change finishes first. A $max of the instant alone would
    // ignore the earlier one now; it must still move the fence, because a
    // sender may have read the device set between the two.
    ASSERT_TRUE(service().propagate_devices(db(), bob).ok());
    const std::int64_t after_later = dsv(together);
    EXPECT_GE(after_later, later.time_since_epoch().count());
    ASSERT_TRUE(service().propagate_devices(db(), alice).ok());
    EXPECT_GT(dsv(together), after_later);

    // And concurrently, from several threads at once: whatever the order, the
    // version ends past every change and never went backwards.
    const Uuid carol = person();
    const Uuid dave = person();
    const Uuid busy = group(carol, {dave}, true);
    const Client carols = registered(carol);
    const Client daves = registered(dave);
    (void)linked(carol, carols, later);
    (void)linked(dave, daves, later + std::chrono::milliseconds{1});
    const std::int64_t start = dsv(busy);
    std::vector<std::future<bool>> runs;
    for (int i = 0; i < 4; ++i) {
        runs.push_back(std::async(std::launch::async, [&, i] {
            auto entry = anvil::db::MongoPool::instance().acquire();
            return service().propagate_devices(*entry, i % 2 == 0 ? carol : dave).ok();
        }));
    }
    for (std::future<bool>& run : runs) { EXPECT_TRUE(run.get()); }
    EXPECT_GT(dsv(busy), start);
    EXPECT_GE(dsv(busy), (later + std::chrono::milliseconds{1}).time_since_epoch().count());
    EXPECT_FALSE(pending(carol).has_value());
    EXPECT_FALSE(pending(dave).has_value());
}

TEST_F(ChatE2eeDb, OnlyAnEncryptedDirectConversationIsToldAndOnlyOncePerChange) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid sealed = direct(alice, bob, true);
    const Uuid open = direct(alice, bob, false);
    const Uuid team = group(alice, {bob}, true);

    const Client first = registered(alice);
    ASSERT_TRUE(service().propagate_devices(db(), alice).ok());
    // A second run of the SAME change, as a sweeper racing the request would
    // be, finds the first one's message and writes no second.
    ASSERT_TRUE(devices().register_first_device(db(), alice, make_client().device,
                                                anvil::db::now_ms(), anvil::db::now_ms())
                    .code() == ErrorCode::Conflict);
    ASSERT_TRUE(service().propagate_devices(db(), alice).ok());

    const std::vector<chat::MessageRecord> told = notices(sealed);
    ASSERT_EQ(told.size(), 1U);
    EXPECT_EQ(told[0].sender, alice);
    ASSERT_TRUE(told[0].system->subject.has_value());
    EXPECT_EQ(*told[0].system->subject, alice);
    EXPECT_TRUE(notices(open).empty());
    EXPECT_TRUE(notices(team).empty());

    // A second change is a second notice.
    (void)linked(alice, first);
    ASSERT_TRUE(service().propagate_devices(db(), alice).ok());
    EXPECT_EQ(notices(sealed).size(), 2U);

    // Run concurrently for one change, it is still one message.
    const Uuid carol = person();
    const Uuid dave = person();
    const Uuid pair = direct(carol, dave, true);
    (void)registered(carol);
    std::vector<std::future<bool>> runs;
    for (int i = 0; i < 4; ++i) {
        runs.push_back(std::async(std::launch::async, [&] {
            auto entry = anvil::db::MongoPool::instance().acquire();
            return service().propagate_devices(*entry, carol).ok();
        }));
    }
    for (std::future<bool>& run : runs) { EXPECT_TRUE(run.get()); }
    EXPECT_EQ(notices(pair).size(), 1U);
}

TEST_F(ChatE2eeDb, AMembershipChangeMovesTheFenceToo) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid carol = person();
    const Uuid team = group(alice, {bob}, true);

    // A sender who read the set before carol arrived must not pass the fence
    // after: carol's devices are not in what they encrypted for.
    const std::int64_t before_join = dsv(team);
    ASSERT_TRUE(service().add_members(db(), creator(alice), team, std::vector<Uuid>{carol}).ok());
    const std::int64_t after_join = dsv(team);
    EXPECT_GT(after_join, before_join);

    // Nor one who read it before bob left: bob's devices are.
    ASSERT_TRUE(service().remove_member(db(), creator(alice), team, bob).ok());
    EXPECT_GT(dsv(team), after_join);
}

TEST_F(ChatE2eeDb, PropagatingWithoutADeviceDirectoryRefusesRatherThanPretends) {
    chat::ChatHooks hooks{};
    const chat::ChatService bare{chat::ChatServiceDeps{
        .repository = *repo_, .media = *media_, .grants = grants_, .kinds = testapp::kChatKinds,
        .cards = testapp::kChatCards, .invite_pepper = kPepper, .hooks = std::move(hooks)}};
    // Half of it is refused at construction, not at the first send.
    chat::ChatHooks half_hooks{};
    EXPECT_THROW((chat::ChatService{chat::ChatServiceDeps{
                     .repository = *repo_, .media = *media_, .grants = grants_,
                     .kinds = testapp::kChatKinds, .cards = testapp::kChatCards,
                     .invite_pepper = kPepper, .hooks = std::move(half_hooks),
                     .devices = devices_.get()}}),
                 std::invalid_argument);
    EXPECT_EQ(bare.propagate_devices(db(), person()).code(), ErrorCode::ServiceUnavailable);
    EXPECT_EQ(bare.sweep_device_changes(db(), anvil::db::now_ms(), 1).code(),
              ErrorCode::ServiceUnavailable);
}

// --- the encrypted send (§7.6) ----------------------------------------------------

TEST_F(ChatE2eeDb, ADirectSendKeepsTheCommonCiphertextAndQueuesOneRowPerOtherDevice) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    const Client laptop = linked(alice, phone);
    const Client bobs = registered(bob);
    const Uuid pair = direct(alice, bob, true);
    settle({alice, bob});

    const Bytes common = bytes(300, 1);
    const Bytes to_laptop = bytes(80, 2);
    const Bytes to_bob = bytes(90, 3);
    Outgoing out = outgoing(phone, dsv(pair), common);
    out.to(laptop, to_laptop).to(bobs, to_bob);
    const auto sent = service().send_encrypted(db(), creator(alice), pair, out.message());
    ASSERT_TRUE(sent.ok()) << static_cast<int>(sent.code()) << " " << sent.error().field;
    EXPECT_TRUE(sent.value().created);

    const chat::MessageRecord row = message(pair, sent.value().seq);
    EXPECT_EQ(row.kind, chat::MessageKind::Encrypted);
    EXPECT_EQ(row.ciphertext, common);
    ASSERT_TRUE(row.sender_device.has_value());
    EXPECT_EQ(*row.sender_device, phone.device.id);
    EXPECT_TRUE(row.body.empty());

    // Each device reads exactly its own ciphertext, and the sending device
    // has none: it holds the plaintext.
    const auto bobs_rows = queue().read(db(), bob, bobs.device.id, std::nullopt, 10, now());
    ASSERT_TRUE(bobs_rows.ok());
    ASSERT_EQ(bobs_rows.value().size(), 1U);
    EXPECT_EQ(bobs_rows.value()[0].ciphertext, to_bob);
    EXPECT_EQ(bobs_rows.value()[0].seq, sent.value().seq);
    EXPECT_EQ(bobs_rows.value()[0].sender_device, phone.device.id);
    const auto laptop_rows =
        queue().read(db(), alice, laptop.device.id, std::nullopt, 10, now());
    ASSERT_TRUE(laptop_rows.ok());
    ASSERT_EQ(laptop_rows.value().size(), 1U);
    EXPECT_EQ(laptop_rows.value()[0].ciphertext, to_laptop);
    const auto phone_rows = queue().read(db(), alice, phone.device.id, std::nullopt, 10, now());
    ASSERT_TRUE(phone_rows.ok());
    EXPECT_TRUE(phone_rows.value().empty());
    // A device id presented by the wrong account reads nothing.
    const auto stolen = queue().read(db(), alice, bobs.device.id, std::nullopt, 10, now());
    ASSERT_TRUE(stolen.ok());
    EXPECT_TRUE(stolen.value().empty());

    // Acknowledged, gone.
    const auto acked = queue().acknowledge(db(), bob, bobs.device.id, bobs_rows.value()[0].id);
    ASSERT_TRUE(acked.ok());
    EXPECT_EQ(acked.value(), 1);
    EXPECT_TRUE(queue().read(db(), bob, bobs.device.id, std::nullopt, 10, now()).value().empty());
}

TEST_F(ChatE2eeDb, AStaleFenceIsRefusedAndSpendsNoSeq) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    const Client bobs = registered(bob);
    const Uuid team = group(alice, {bob}, true);
    settle({alice, bob});
    const std::int64_t version = dsv(team);
    const std::int64_t head = conversation(team).seq;

    Outgoing wrong = outgoing(phone, version - 1, bytes(10, 1));
    const auto refused = service().send_encrypted(db(), creator(alice), team, wrong.message());
    EXPECT_EQ(refused.code(), ErrorCode::Conflict);
    EXPECT_EQ(refused.error().field, chat::kDevicesField);
    EXPECT_EQ(conversation(team).seq, head);

    // Bob links a laptop: a sender holding the old version is stale, and one
    // holding the new one is not.
    (void)linked(bob, bobs);
    ASSERT_TRUE(service().propagate_devices(db(), bob).ok());
    Outgoing old = outgoing(phone, version, bytes(10, 1));
    EXPECT_EQ(service().send_encrypted(db(), creator(alice), team, old.message()).code(),
              ErrorCode::Conflict);
    EXPECT_EQ(conversation(team).seq, head);
    Outgoing fresh = outgoing(phone, dsv(team), bytes(10, 1));
    EXPECT_TRUE(service().send_encrypted(db(), creator(alice), team, fresh.message()).ok());
}

TEST_F(ChatE2eeDb, AMissingDeviceIsStaleAndAnExtraOneIsRefusedNotIgnored) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid stranger = person();
    const Client phone = registered(alice);
    const Client bobs = registered(bob);
    const Client bobs_laptop = linked(bob, bobs);
    const Client strangers = registered(stranger);
    const Uuid pair = direct(alice, bob, true);
    settle({alice, bob, stranger});
    const std::int64_t head = conversation(pair).seq;

    // Missing bob's laptop: the sender's set predates it.
    Outgoing missing = outgoing(phone, dsv(pair));
    missing.to(bobs, bytes(5, 1));
    const auto stale = service().send_encrypted(db(), creator(alice), pair, missing.message());
    EXPECT_EQ(stale.code(), ErrorCode::Conflict);
    EXPECT_EQ(stale.error().field, chat::kDevicesField);

    // An extra device: somebody not in the conversation at all.
    Outgoing extra = outgoing(phone, dsv(pair));
    extra.to(bobs, bytes(5, 1)).to(bobs_laptop, bytes(5, 2)).to(strangers, bytes(5, 3));
    const auto refused = service().send_encrypted(db(), creator(alice), pair, extra.message());
    EXPECT_EQ(refused.code(), ErrorCode::ValidationFailed);
    EXPECT_EQ(refused.error().field, chat::kDevicesField);
    EXPECT_EQ(refused.error().detail, static_cast<std::uint16_t>(Reason::NotAllowed));

    // The sending device addressing itself, and one device twice.
    Outgoing itself = outgoing(phone, dsv(pair));
    itself.to(bobs, bytes(5, 1)).to(bobs_laptop, bytes(5, 2)).to(phone, bytes(5, 3));
    EXPECT_EQ(service().send_encrypted(db(), creator(alice), pair, itself.message()).code(),
              ErrorCode::ValidationFailed);
    Outgoing twice = outgoing(phone, dsv(pair));
    twice.to(bobs, bytes(5, 1)).to(bobs, bytes(5, 2));
    EXPECT_EQ(service().send_encrypted(db(), creator(alice), pair, twice.message()).code(),
              ErrorCode::ValidationFailed);

    // None of them spent a seq or left a queue row.
    EXPECT_EQ(conversation(pair).seq, head);
    EXPECT_TRUE(queue().read(db(), bob, bobs.device.id, std::nullopt, 10, now()).value().empty());

    Outgoing exact = outgoing(phone, dsv(pair));
    exact.to(bobs, bytes(5, 1)).to(bobs_laptop, bytes(5, 2));
    EXPECT_TRUE(service().send_encrypted(db(), creator(alice), pair, exact.message()).ok());
}

TEST_F(ChatE2eeDb, TheSendingDeviceIsTheSendersOwnAndCurrent) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    const Client bobs = registered(bob);
    const Uuid team = group(alice, {bob}, true);
    settle({alice, bob});

    Outgoing borrowed = outgoing(bobs, dsv(team), bytes(10, 1));
    const auto someone_elses =
        service().send_encrypted(db(), creator(alice), team, borrowed.message());
    EXPECT_EQ(someone_elses.code(), ErrorCode::Forbidden);
    EXPECT_EQ(someone_elses.error().field, chat::kDeviceField);

    const Client laptop = linked(alice, phone);
    ASSERT_TRUE(devices().unlink_device(db(), alice, laptop.device.id, now()).value());
    ASSERT_TRUE(service().propagate_devices(db(), alice).ok());
    Outgoing unlinked = outgoing(laptop, dsv(team), bytes(10, 1));
    EXPECT_EQ(service().send_encrypted(db(), creator(alice), team, unlinked.message()).code(),
              ErrorCode::Forbidden);
}

TEST_F(ChatE2eeDb, EachModeRefusesTheOthersBody) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    (void)registered(bob);
    const Uuid sealed = group(alice, {bob}, true);
    const Uuid open = group(alice, {bob}, false);
    settle({alice, bob});

    chat::SendMessage text{};
    text.client_id = anvil::crypto::random_array<16>();
    text.body = "hello";
    EXPECT_EQ(service().send(db(), creator(alice), sealed, text).code(),
              ErrorCode::ValidationFailed);
    Outgoing cipher = outgoing(phone, dsv(open), bytes(10, 1));
    const auto refused = service().send_encrypted(db(), creator(alice), open, cipher.message());
    EXPECT_EQ(refused.code(), ErrorCode::ValidationFailed);
    EXPECT_EQ(refused.error().field, chat::kCiphertextField);
}

TEST_F(ChatE2eeDb, ARetryIsTheFirstMessageEvenAfterTheFenceMoved) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    const Client bobs = registered(bob);
    const Uuid team = group(alice, {bob}, true);
    settle({alice, bob});

    Outgoing first = outgoing(phone, dsv(team), bytes(10, 1));
    const auto sent = service().send_encrypted(db(), creator(alice), team, first.message());
    ASSERT_TRUE(sent.ok());
    (void)linked(bob, bobs);
    ASSERT_TRUE(service().propagate_devices(db(), bob).ok());

    // The response was lost and the phone retries with the same client id
    // against what is now an old version. The message is stored; re-encrypting
    // it would be a second one.
    const auto retried = service().send_encrypted(db(), creator(alice), team, first.message());
    ASSERT_TRUE(retried.ok());
    EXPECT_FALSE(retried.value().created);
    EXPECT_EQ(retried.value().seq, sent.value().seq);
}

TEST_F(ChatE2eeDb, PastABlockThePeersDevicesAreNeverQueued) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    const Client laptop = linked(alice, phone);
    const Client bobs = registered(bob);
    const Uuid pair = direct(alice, bob, true);
    settle({alice, bob});
    ASSERT_TRUE(service().block(db(), creator(bob), alice).ok());

    Outgoing out = outgoing(phone, dsv(pair));
    out.to(laptop, bytes(5, 1)).to(bobs, bytes(5, 2));
    const auto sent = service().send_encrypted(db(), creator(alice), pair, out.message());
    ASSERT_TRUE(sent.ok());
    EXPECT_TRUE(message(pair, sent.value().seq).hidden_from_peer);
    EXPECT_TRUE(queue().read(db(), bob, bobs.device.id, std::nullopt, 10, now()).value().empty());
    EXPECT_EQ(queue().read(db(), alice, laptop.device.id, std::nullopt, 10, now()).value().size(),
              1U);
}

TEST_F(ChatE2eeDb, RevokingTakesTheCommonAndEveryPerDeviceCiphertext) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    const Client bobs = registered(bob);
    const Uuid pair = direct(alice, bob, true);
    settle({alice, bob});

    Outgoing out = outgoing(phone, dsv(pair), bytes(40, 1));
    out.to(bobs, bytes(5, 2));
    const auto sent = service().send_encrypted(db(), creator(alice), pair, out.message());
    ASSERT_TRUE(sent.ok());
    ASSERT_TRUE(service().revoke(db(), creator(alice), pair, sent.value().seq).ok());

    const chat::MessageRecord row = message(pair, sent.value().seq);
    EXPECT_TRUE(row.revoked);
    EXPECT_TRUE(row.ciphertext.empty());
    EXPECT_TRUE(queue().read(db(), bob, bobs.device.id, std::nullopt, 10, now()).value().empty());
}

TEST_F(ChatE2eeDb, APageIsASubsetAndOnlyInAGroup) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid carol = person();
    const Client phone = registered(alice);
    const Client bobs = registered(bob);
    (void)registered(carol);
    const Uuid team = group(alice, {bob, carol}, true);
    const Uuid pair = direct(alice, bob, true);
    settle({alice, bob, carol});

    // A distribution page to bob alone, carol on the next page.
    Outgoing page = outgoing(phone, dsv(team));
    page.to(bobs, bytes(5, 1)).paged();
    EXPECT_TRUE(service().send_encrypted(db(), creator(alice), team, page.message()).ok());
    // Not in a direct conversation, where the whole set always fits.
    Outgoing direct_page = outgoing(phone, dsv(pair));
    direct_page.to(bobs, bytes(5, 1)).paged();
    EXPECT_EQ(service().send_encrypted(db(), creator(alice), pair, direct_page.message()).code(),
              ErrorCode::ValidationFailed);
    // Nor with a common ciphertext, which is a message and not a page.
    Outgoing mixed = outgoing(phone, dsv(team), bytes(5, 1));
    mixed.to(bobs, bytes(5, 1)).paged();
    EXPECT_EQ(service().send_encrypted(db(), creator(alice), team, mixed.message()).code(),
              ErrorCode::ValidationFailed);
}

TEST_F(ChatE2eeDb, APerDeviceCiphertextExpiresWithItsMessage) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Client phone = registered(alice);
    const Client bobs = registered(bob);
    const Uuid pair = direct(alice, bob, true);
    settle({alice, bob});
    ASSERT_TRUE(service().set_timer(db(), creator(alice), pair, testapp::kChatTimers[0]).ok());

    Outgoing out = outgoing(phone, dsv(pair));
    out.to(bobs, bytes(5, 1));
    const auto sent = service().send_encrypted(db(), creator(alice), pair, out.message());
    ASSERT_TRUE(sent.ok());
    const chat::MessageRecord row = message(pair, sent.value().seq);
    ASSERT_TRUE(row.expires_at.has_value());
    const auto rows = queue().read(db(), bob, bobs.device.id, std::nullopt, 10, now());
    ASSERT_EQ(rows.value().size(), 1U);
    // A day's timer, not the queue's thirty days.
    EXPECT_EQ(rows.value()[0].expires_at, *row.expires_at);
    EXPECT_TRUE(
        queue().read(db(), bob, bobs.device.id, std::nullopt, 10, *row.expires_at).value().empty());
}

TEST_F(ChatE2eeDb, TheDevicePageCarriesTheVersionReadBeforeIt) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid carol = person();
    const Client phone = registered(alice);
    (void)linked(alice, phone);
    (void)registered(bob);
    const Uuid team = group(alice, {bob, carol}, true);
    const Uuid open = group(alice, {bob}, false);
    settle({alice, bob});

    const auto page = service().conversation_devices(db(), creator(alice), team, std::nullopt, 2);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page.value().device_set_version, dsv(team));
    ASSERT_EQ(page.value().members.size(), 2U);
    ASSERT_TRUE(page.value().next.has_value());
    const auto rest =
        service().conversation_devices(db(), creator(alice), team, page.value().next, 2);
    ASSERT_TRUE(rest.ok());
    ASSERT_EQ(rest.value().members.size(), 1U);
    EXPECT_FALSE(rest.value().next.has_value());
    std::size_t total = 0;
    bool carol_listed = false;
    for (const auto* p : {&page.value(), &rest.value()}) {
        for (const chat::AccountDevices& member : p->members) {
            total += member.devices.size();
            // Carol never registered a device and is listed with none.
            if (member.user == carol) { carol_listed = member.devices.empty(); }
        }
    }
    EXPECT_EQ(total, 3U);
    EXPECT_TRUE(carol_listed);

    EXPECT_EQ(service().conversation_devices(db(), creator(alice), open, std::nullopt, 10).code(),
              ErrorCode::Forbidden);
    EXPECT_EQ(service()
                  .conversation_devices(db(), creator(person()), team, std::nullopt, 10)
                  .code(),
              ErrorCode::NotFound);
}

TEST_F(ChatE2eeDb, ACreatedOrOpenedConversationAnswersTheVersionItWasLeftAt) {
    const Uuid alice = person();
    const Uuid bob = person();
    // Each one's first membership transaction raises the fence, and the
    // answer must be after it: a client that sent at the version it was handed
    // would otherwise be stale on its first message.
    const auto made = service().create(db(), creator(alice),
                                       chat::CreateConversation{"group", "Team", "", {}, true});
    ASSERT_TRUE(made.ok());
    EXPECT_EQ(made.value().conversation.device_set_version, dsv(made.value().conversation.id));
    EXPECT_GT(made.value().conversation.device_set_version, 0);
    const auto opened = service().open_direct(db(), creator(alice), "direct", bob, true);
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(opened.value().conversation.device_set_version,
              dsv(opened.value().conversation.id));
}

// --- the device lifecycle through the service (§7.3) -----------------------------

TEST_F(ChatE2eeDb, AnEndedSessionUnlinksItsDeviceAndEverythingItHeld) {
    const Uuid alice = person();
    const Uuid bob = person();
    const Uuid session = anvil::uuid::generate_v7();
    chat::Actor bobs_session = creator(bob);
    bobs_session.session = session;
    const Client phone = registered(alice);
    const Client bobs = make_client();
    ASSERT_TRUE(service().register_device(db(), bobs_session, bobs.device).ok());
    const Uuid pair = direct(alice, bob, true);
    settle({alice});

    std::array<chat::OneTimePrekey, 3> keys{};
    for (std::uint32_t i = 0; i < keys.size(); ++i) {
        keys[i] = chat::OneTimePrekey{crypto::x25519_generate_keypair().public_key, i};
    }
    ASSERT_TRUE(service().upload_prekeys(db(), bobs_session, bobs.device.id, keys).ok());
    Outgoing out = outgoing(phone, dsv(pair));
    out.to(bobs, bytes(5, 1));
    ASSERT_TRUE(service().send_encrypted(db(), creator(alice), pair, out.message()).ok());
    const std::int64_t before = dsv(pair);

    // Signing out everywhere ends the session, and the device it registered
    // goes with it: its keys, its queue, and its place in the fence.
    ASSERT_TRUE(service().session_ended(db(), bob, session).ok());
    const auto mine = service().my_devices(db(), bobs_session);
    ASSERT_TRUE(mine.ok() && mine.value().has_value());
    EXPECT_TRUE(mine.value()->devices.empty());
    EXPECT_EQ(prekeys_->remaining(db(), bob, bobs.device.id).value(), 0);
    EXPECT_TRUE(queue().read(db(), bob, bobs.device.id, std::nullopt, 10, now()).value().empty());
    EXPECT_GT(dsv(pair), before);
    EXPECT_EQ(notices(pair).size(), 2U);

    ASSERT_EQ(device_events_.size(), 2U);
    EXPECT_EQ(device_events_[0].change, chat::DeviceChange::Registered);
    EXPECT_EQ(device_events_[1].change, chat::DeviceChange::Unlinked);
    EXPECT_EQ(device_events_[1].session, session);
    // A second end is nothing, and says so to nobody.
    EXPECT_TRUE(service().session_ended(db(), bob, session).ok());
    EXPECT_EQ(device_events_.size(), 2U);
}

TEST_F(ChatE2eeDb, SigningOutEverywhereUnlinksEveryDeviceThoseSessionsRegistered) {
    // The epoch bump every revocation makes goes through the Redis mirror.
    ANVIL_REQUIRE_REDIS();
    anvil::testfixture::clear_collection(db(), kUsers);
    anvil::testfixture::clear_collection(db(), kSessions);
    const std::string database{scratch_names().for_collection(kUsers)};
    const Uuid alice = person();
    const Uuid bob = anvil::uuid::generate_v7();
    const identity::NewUser account{.id = bob,
                                    .email_normalised = "bob@example.test",
                                    .email_display = "bob@example.test",
                                    .username_normalised = "bob",
                                    .username_display = "bob",
                                    .password_hash = "$argon2id$x",
                                    .phone_e164 = {},
                                    .locale = anvil::Locale{},
                                    .status = anvil::UserStatus::Active};
    const identity::UserRepository users{database, kUsers};
    ASSERT_TRUE(users.insert(db(), account).ok());
    identity::AuthzService authz{database, kUsers};
    // Wired as an application wires it: every session revoked, by any path,
    // forwarded to the chat service.
    identity::SessionService sessions{
        database, kSessions, kUsers, kPepper,
        std::make_shared<const anvil::auth::TokenKeys>(std::uint8_t{1}, kPepper), authz,
        identity::SessionPolicy{},
        [this](mongocxx::client& client, const Uuid& user, std::span<const Uuid> ended) {
            for (const Uuid& session : ended) {
                EXPECT_TRUE(service().session_ended(client, user, session).ok());
            }
        }};
    const identity::UserAuthRecord record{.password_hash = {},
                                          .lock_until = std::nullopt,
                                          .id = bob,
                                          .effective_permissions = {},
                                          .perm_epoch = 0,
                                          .failure_count = 0,
                                          .user_type = anvil::UserType::Client,
                                          .status = anvil::UserStatus::Active,
                                          .locale = anvil::Locale{}};
    const auto signed_in = [&](std::string_view browser) {
        const auto issued = sessions.create(db(), record, identity::pack_ip("203.0.113.20"),
                                            browser, now());
        EXPECT_TRUE(issued.ok());
        chat::Actor actor = creator(bob);
        actor.session = issued.ok() ? issued.value().session_id : Uuid{};
        return actor;
    };

    // A phone and a laptop, each on a session of its own, and a third
    // session that registered nothing.
    const chat::Actor on_phone = signed_in("phone");
    const chat::Actor on_laptop = signed_in("laptop");
    (void)signed_in("desk");
    const Client phone = make_client();
    ASSERT_TRUE(service().register_device(db(), on_phone, phone.device).ok());
    const Client laptop = make_client();
    const std::uint64_t at = unix_s(now());
    ASSERT_TRUE(service()
                    .link_device(db(), on_laptop, phone.device.id, laptop.device, at,
                                 crypto::ed25519_sign(
                                     phone.seed, chat::link_message(bob, laptop.device.id,
                                                                    laptop.device.keys.agreement,
                                                                    laptop.device.keys.signing,
                                                                    at)))
                    .ok());
    (void)registered(alice);
    const Uuid pair = direct(alice, bob, true);
    settle({alice});
    const auto ids = [&] {
        std::vector<Uuid> out;
        const auto mine = service().my_devices(db(), on_phone);
        EXPECT_TRUE(mine.ok() && mine.value().has_value());
        if (!mine.ok() || !mine.value().has_value()) { return out; }
        for (const auto& device : mine.value()->devices) { out.push_back(device.published.id); }
        return out;
    };
    ASSERT_EQ(ids().size(), 2U);

    // A password change on the phone ends the other two sessions, and with
    // them the laptop, and moves the fence a sender encrypts against.
    const std::int64_t before = dsv(pair);
    ASSERT_EQ(sessions.revoke_others(db(), bob, on_phone.session).value(), 2);
    EXPECT_EQ(ids(), std::vector<Uuid>{phone.device.id});
    EXPECT_GT(dsv(pair), before);

    // Signing out everywhere ends the phone's too, and leaves nothing.
    ASSERT_EQ(sessions.revoke_all(db(), bob).value(), 1);
    EXPECT_TRUE(ids().empty());

    std::vector<Uuid> ended;
    for (const chat::DeviceEvent& event : device_events_) {
        if (event.change == chat::DeviceChange::Unlinked) { ended.push_back(event.session); }
    }
    EXPECT_EQ(ended, (std::vector<Uuid>{on_laptop.session, on_phone.session}));
}

TEST_F(ChatE2eeDb, TheIdleSweeperEndsADeviceAsAnUnlinkDoes) {
    const Uuid alice = person();
    const Uuid bob = person();
    const TimeMs long_ago = now() - std::chrono::hours{24 * 40};
    // Registered forty days ago and never seen since; the fresh-sign-in window
    // is measured against the same instant.
    const Client old = make_client();
    ASSERT_TRUE(devices().register_first_device(db(), bob, old.device, long_ago, long_ago).ok());
    const Uuid pair = direct(alice, bob, true);
    settle({bob});
    const std::int64_t before = dsv(pair);

    const auto swept = service().sweep_idle_devices(db(), now(), 30, 10);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 1);
    EXPECT_GT(dsv(pair), before);
    ASSERT_FALSE(device_events_.empty());
    EXPECT_EQ(device_events_.back().change, chat::DeviceChange::Unlinked);
    EXPECT_EQ(device_events_.back().session, Uuid{});
}

TEST_F(ChatE2eeDb, AFirstDeviceIsRefusedWhenTheApplicationCannotSayWhenItSignedIn) {
    chat::ChatHooks hooks{};
    const chat::ChatService strict{chat::ChatServiceDeps{
        .repository = *repo_, .media = *media_, .grants = grants_, .kinds = testapp::kChatKinds,
        .cards = testapp::kChatCards, .invite_pepper = kPepper, .hooks = std::move(hooks),
        .devices = devices_.get(), .queue = queue_.get(), .prekeys = prekeys_.get()}};
    const Uuid alice = person();
    const auto refused = strict.register_device(db(), creator(alice), make_client().device);
    EXPECT_EQ(refused.code(), ErrorCode::CapabilityRequired);
    EXPECT_EQ(refused.error().field, chat::device_inputs::kAuthenticatedAt);
    EXPECT_FALSE(devices().identity(db(), alice).value().has_value());
}

}  // namespace
