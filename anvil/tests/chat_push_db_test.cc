// Chat push nudges against a live cluster (chat/push.h, docs/22-chat.md §8.4).
//
// The queue is the application's, so here it is a recorder that keeps one job
// per idempotency key, which is the one property of the real queue a nudge
// relies on (its own cases are in timer_test.cc). Every job the sends asked for
// is then run through ChatPush::deliver, as the application's handler runs it,
// and the transport records what each endpoint would have been sent.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "anvil/crypto/base64url.h"

#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/prekeys.h"
#include "anvil/chat/push.h"
#include "anvil/chat/service.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"
#include "anvil/media/grant.h"
#include "anvil/media/service.h"
#include "anvil/notifications/repository.h"
#include "anvil/notifications/webpush.h"
#include "app_fixture.h"
#include "chat_device_client.h"
#include "db_fixture.h"
#include "testapp/chat_cards.h"
#include "testapp/chat_collections.h"
#include "testapp/chat_kinds.h"
#include "testapp/chat_push.h"
#include "testapp/perms.h"
#include "testapp/topics.h"

namespace {

namespace chat = anvil::chat;
namespace n = anvil::notifications;
using anvil::Uuid;
using anvil::testfixture::scratch_names;

constexpr std::string_view kMedia = "media";
constexpr std::string_view kNotifications = "notifications";
constexpr std::string_view kInbox = "notification_inbox";
constexpr std::string_view kClients = "notification_clients";
constexpr std::array<std::uint8_t, 32> kPepper{{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
                                               4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}};

[[nodiscard]] Uuid person() { return anvil::uuid::generate_v4(); }

[[nodiscard]] chat::Actor actor(const Uuid& user) {
    return chat::Actor{user, anvil::perm_mask(testapp::Perm::ChatCreateGroup)};
}

[[nodiscard]] anvil::Locale arabic() { return *anvil::Locale::from_tag("ar"); }

struct Job final {
    std::vector<std::uint8_t> args;
    std::string               key;
};

struct Pushed final {
    std::string  title;
    std::string  body;
    Uuid         client;
    Uuid         conversation;
    std::int32_t count;
    n::TopicCode kind;
};

class ChatPushDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        if (!anvil::testfixture::transactions_available()) {
            GTEST_SKIP() << "transactions need a replica set";
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        for (const std::string_view collection :
             {testapp::kChatCollections.conversations, testapp::kChatCollections.members,
              testapp::kChatCollections.messages, kNotifications, kInbox, kClients,
              testapp::kDeviceCollections.identities, testapp::kDeviceCollections.prekeys,
              testapp::kDeviceCollections.queue}) {
            anvil::testfixture::clear_collection(**client_, collection);
        }
        chats_ = std::make_unique<chat::ChatRepository>(scratch_names(), testapp::kChatCollections,
                                                        testapp::kChatKinds);
        notifications_ = std::make_unique<n::NotificationRepository>(
            std::string{scratch_names().for_collection(kNotifications)},
            n::NotificationCollections{kNotifications, kInbox, kClients}, testapp::kTopics);
        media_ = std::make_unique<anvil::media::MediaService>(
            std::string{scratch_names().for_collection(kMedia)}, kMedia);

        chat::PushHooks hooks{};
        hooks.enqueue = [this](std::span<const std::uint8_t> args, anvil::db::TimeMs,
                               std::string_view key) -> anvil::Status {
            // The real queue's dedupe, which outlives the job: a second ask under
            // one key is the first job, whether or not it has run.
            if (keys_.insert(std::string{key}).second) {
                jobs_.push_back(Job{{args.begin(), args.end()}, std::string{key}});
            }
            ++asks_;
            return anvil::ok();
        };
        hooks.reader = [this](mongocxx::client&, const Uuid& user) {
            const auto found = readers_.find(user);
            return found == readers_.end() ? chat::PushReader{anvil::Locale{}, true}
                                           : found->second;
        };
        hooks.name_of = [this](mongocxx::client&, const Uuid& user) {
            const auto found = names_.find(user);
            return found == names_.end() ? std::string{} : found->second;
        };
        push_ = std::make_unique<chat::ChatPush>(
            testapp::chat_push_config(std::chrono::seconds{3600}, std::chrono::seconds{0}),
            std::move(hooks), *chats_, *notifications_, testapp::kTemplates,
            [this](const n::Delivery& delivery) -> anvil::Result<n::DeliveryVerdict> {
                pushed_.push_back(Pushed{delivery.content.title, delivery.content.body,
                                         delivery.client, delivery.notification, delivery.count,
                                         delivery.kind});
                return n::DeliveryVerdict::Delivered;
            });

        chat::ChatHooks chat_hooks{};
        chat_hooks.may_reach = [](mongocxx::client&, const Uuid&, const Uuid&) { return true; };
        devices_ = std::make_unique<chat::DeviceDirectory>(
            scratch_names(), testapp::kDeviceCollections, testapp::kDeviceConfig);
        queue_ = std::make_unique<chat::DeviceQueue>(scratch_names(),
                                                     testapp::kDeviceCollections.queue);
        prekeys_ = std::make_unique<chat::PrekeyDirectory>(
            scratch_names(), testapp::kDeviceCollections, testapp::kDeviceConfig);
        service_ = std::make_unique<chat::ChatService>(chat::ChatServiceDeps{
            .repository = *chats_, .media = *media_, .grants = grants_,
            .kinds = testapp::kChatKinds, .cards = testapp::kChatCards,
            .invite_pepper = kPepper, .hooks = std::move(chat_hooks), .push = push_.get(),
            .devices = devices_.get(), .queue = queue_.get(), .prekeys = prekeys_.get()});
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    // One WebPush endpoint for `user`, as notifications registers a browser.
    Uuid endpoint(const Uuid& user) {
        n::ClientRow row{};
        row.id = anvil::uuid::generate_v4();
        row.addr = "https://push.example/" + anvil::uuid::to_string(row.id);
        row.owner = user;
        row.created_at = anvil::db::now_ms();
        row.prefs = n::Preferences::all_enabled();
        row.type = n::ClientType::WebPush;
        EXPECT_TRUE(notifications_->insert_client(db(), row).ok());
        endpoints_[row.id] = user;
        return row.id;
    }

    [[nodiscard]] Uuid group(const Uuid& owner, std::vector<Uuid> members) {
        const auto made = service_->create(
            db(), actor(owner), chat::CreateConversation{"group", "Team", "", members, false});
        EXPECT_TRUE(made.ok());
        return made.value().conversation.id;
    }

    std::int64_t send(const Uuid& from, const Uuid& conversation, std::string_view body,
                      std::span<const chat::MentionSpan> mentions = {}) {
        chat::SendMessage message{};
        message.client_id = anvil::crypto::random_array<16>();
        message.body = body;
        message.mentions = mentions;
        const auto sent = service_->send(db(), actor(from), conversation, message);
        EXPECT_TRUE(sent.ok()) << static_cast<int>(sent.code());
        return sent.ok() ? sent.value().seq : 0;
    }

    void mute(const Uuid& user, const Uuid& conversation) {
        chat::MemberPreferences preferences{};
        preferences.muted_until = std::optional<anvil::db::TimeMs>{
            anvil::db::now_ms() + std::chrono::hours{1}};
        ASSERT_TRUE(service_->preferences(db(), actor(user), conversation, preferences).ok());
    }

    // Every job the sends asked for, once each, as the queue would run them.
    std::int32_t run_jobs() {
        std::int32_t recipients = 0;
        for (const Job& job : jobs_) {
            const auto done = push_->deliver(db(), job.args, anvil::db::now_ms());
            EXPECT_TRUE(done.ok()) << static_cast<int>(done.code());
            if (done.ok()) { recipients += done.value().recipients; }
        }
        jobs_.clear();
        return recipients;
    }

    [[nodiscard]] std::vector<Pushed> to(const Uuid& user) const {
        std::vector<Pushed> out;
        for (const Pushed& push : pushed_) {
            if (endpoints_.at(push.client) == user) { out.push_back(push); }
        }
        return out;
    }

    [[nodiscard]] std::int64_t rows_in(std::string_view collection) {
        return db()[std::string{scratch_names().for_collection(collection)}]
                   [std::string{collection}]
                       .count_documents({});
    }

    std::unique_ptr<mongocxx::pool::entry>           client_;
    std::unique_ptr<chat::ChatRepository>            chats_;
    std::unique_ptr<n::NotificationRepository>       notifications_;
    std::unique_ptr<anvil::media::MediaService>      media_;
    anvil::media::GrantKeys grants_{1, anvil::crypto::random_array<64>()};
    std::unique_ptr<chat::ChatPush>                  push_;
    std::unique_ptr<chat::DeviceDirectory>                devices_;
    std::unique_ptr<chat::DeviceQueue>                    queue_;
    std::unique_ptr<chat::PrekeyDirectory>                prekeys_;
    std::unique_ptr<chat::ChatService>               service_;
    std::vector<Job>                                 jobs_;
    std::set<std::string>                            keys_;
    std::vector<Pushed>                              pushed_;
    std::map<Uuid, Uuid>                             endpoints_;
    std::map<Uuid, chat::PushReader>                 readers_;
    std::map<Uuid, std::string>                      names_;
    std::int32_t                                     asks_{0};
};

TEST_F(ChatPushDb, ABurstOfFortyIsOnePushSayingFortyAndWritesNoNotificationRow) {
    const Uuid sender = person();
    const Uuid reader = person();
    const Uuid id = group(sender, {reader});
    endpoint(sender);
    endpoint(reader);
    names_[sender] = "Sam";

    for (int i = 0; i < 40; ++i) { send(sender, id, "message " + std::to_string(i)); }
    EXPECT_EQ(asks_, 40) << "one ask per message, never one per recipient";
    // An hour's window holds the burst unless it straddles a boundary, and then
    // it is two jobs: never forty.
    const std::size_t jobs = jobs_.size();
    ASSERT_GE(jobs, 1U);
    ASSERT_LE(jobs, 2U);
    EXPECT_EQ(run_jobs(), static_cast<std::int32_t>(jobs));

    const std::vector<Pushed> got = to(reader);
    ASSERT_EQ(got.size(), jobs);
    EXPECT_EQ(got.back().count, 40);
    EXPECT_EQ(got.back().conversation, id);
    EXPECT_EQ(got.back().kind, static_cast<n::TopicCode>(testapp::Topic::ChatMessage));
    EXPECT_EQ(got.back().title, "Team");
    EXPECT_EQ(got.back().body, "Sam: message 39");
    EXPECT_TRUE(to(sender).empty()) << "a sender is never pushed about their own message";

    // The chat list is the inbox: nothing was written to notifications.
    EXPECT_EQ(rows_in(kNotifications), 0);
    EXPECT_EQ(rows_in(kInbox), 0);

    // The window after this one has nothing new in it, so its job, if one ran,
    // would push nobody: the forty were already pushed.
    const auto later = push_->deliver(
        db(), chat::ChatPush::encode_args(id, push_->bucket_of(anvil::db::now_ms()) + 1),
        anvil::db::now_ms());
    ASSERT_TRUE(later.ok());
    EXPECT_EQ(later.value().recipients, 0);
}

TEST_F(ChatPushDb, AMemberWhoseDeviceSaidDeliveredIsNotPushed) {
    const Uuid sender = person();
    const Uuid holding = person();
    const Uuid away = person();
    const Uuid id = group(sender, {holding, away});
    endpoint(holding);
    endpoint(away);

    const std::int64_t seq = send(sender, id, "are you there");
    // What a device with a live socket does on the wake, on whichever process.
    ASSERT_TRUE(service_->receipts(db(), actor(holding), id, seq, 0).ok());
    run_jobs();

    EXPECT_EQ(to(away).size(), 1U);
    EXPECT_TRUE(to(holding).empty());
}

TEST_F(ChatPushDb, AMutedMemberIsNotPushed) {
    const Uuid sender = person();
    const Uuid muted = person();
    const Uuid listening = person();
    const Uuid id = group(sender, {muted, listening});
    endpoint(muted);
    endpoint(listening);
    mute(muted, id);

    send(sender, id, "lunch?");
    run_jobs();

    // The job ran, which the unmuted member's push shows.
    ASSERT_EQ(to(listening).size(), 1U);
    EXPECT_TRUE(to(muted).empty());
}

TEST_F(ChatPushDb, AMentionBreaksAMuteWhereTheKindLetsIt) {
    static_assert(testapp::kChatKinds[static_cast<std::size_t>(testapp::ChatKind::Group)]
                      .mentions_break_mute);
    const Uuid sender = person();
    const Uuid muted = person();
    const Uuid other = person();
    const Uuid id = group(sender, {muted, other});
    endpoint(muted);
    endpoint(other);
    mute(muted, id);
    mute(other, id);

    const std::array<chat::MentionSpan, 1> mention{{{muted, 4, 2}}};
    send(sender, id, "hey @m, look", mention);
    run_jobs();

    EXPECT_EQ(to(muted).size(), 1U) << "a group lets a mention through a mute";
    EXPECT_TRUE(to(other).empty()) << "and only for the person it names";
}

TEST_F(ChatPushDb, AMentionDoesNotBreakAMuteWhereTheKindDoesNot) {
    static_assert(!testapp::kChatKinds[static_cast<std::size_t>(testapp::ChatKind::Direct)]
                       .mentions_break_mute);
    const Uuid sender = person();
    const Uuid muted = person();
    endpoint(muted);
    const auto opened = service_->open_direct(db(), actor(sender), "direct", muted, false);
    ASSERT_TRUE(opened.ok());
    const Uuid id = opened.value().conversation.id;
    mute(muted, id);

    const std::array<chat::MentionSpan, 1> mention{{{muted, 4, 2}}};
    send(sender, id, "hey @m, look", mention);
    ASSERT_EQ(jobs_.size(), 1U);
    EXPECT_EQ(run_jobs(), 0) << "a direct conversation's mute holds against a mention";
    EXPECT_TRUE(to(muted).empty());

    // And the same message unmuted is pushed, so the silence above was the mute.
    chat::MemberPreferences unmute{};
    unmute.muted_until = std::optional<anvil::db::TimeMs>{};
    ASSERT_TRUE(service_->preferences(db(), actor(muted), id, unmute).ok());
    send(sender, id, "hey @m, again", mention);
    // The window's job already ran — early, which only a test can make it do —
    // so the queue keeps the second ask as the first job. Run it again by hand.
    ASSERT_TRUE(jobs_.empty());
    jobs_.push_back(Job{[&] {
                            const auto args = chat::ChatPush::encode_args(
                                id, push_->bucket_of(anvil::db::now_ms()));
                            return std::vector<std::uint8_t>{args.begin(), args.end()};
                        }(),
                        "again"});
    EXPECT_EQ(run_jobs(), 1);
    ASSERT_EQ(to(muted).size(), 1U);
    EXPECT_EQ(to(muted).front().count, 2) << "both messages are past the watermark";
}

TEST_F(ChatPushDb, TheWordingIsTheRecipientsLocaleAndPreviewSetting) {
    const Uuid sender = person();
    const Uuid previewing_ar = person();
    const Uuid plain_en = person();
    const Uuid plain_ar = person();
    const Uuid id = group(sender, {previewing_ar, plain_en, plain_ar});
    endpoint(previewing_ar);
    endpoint(plain_en);
    endpoint(plain_ar);
    names_[sender] = "سامي";
    readers_[previewing_ar] = chat::PushReader{arabic(), true};
    readers_[plain_en] = chat::PushReader{anvil::Locale{}, false};
    readers_[plain_ar] = chat::PushReader{arabic(), false};

    send(sender, id, "the secret plan");
    send(sender, id, "part two");
    run_jobs();

    ASSERT_EQ(to(previewing_ar).size(), 1U);
    EXPECT_EQ(to(previewing_ar).front().body, "سامي: part two");
    ASSERT_EQ(to(plain_en).size(), 1U);
    EXPECT_EQ(to(plain_en).front().title, "Team");
    EXPECT_EQ(to(plain_en).front().body, "2 new messages");
    ASSERT_EQ(to(plain_ar).size(), 1U);
    EXPECT_EQ(to(plain_ar).front().body, "2 رسائل جديدة");
    for (const Pushed& push : pushed_) {
        if (endpoints_.at(push.client) == previewing_ar) { continue; }
        EXPECT_EQ(push.body.find("part two"), std::string::npos)
            << "a reader with previews off was sent the text";
    }
}

// --- encrypted conversations carry nothing (docs/22-chat.md §7.8) ------------

TEST_F(ChatPushDb, AnEncryptedConversationsPushIsTheConversationAndTheSeqAndNothingElse) {
    const Uuid alice = person();
    const Uuid bob = person();
    names_[alice] = "Alice Sender";
    const anvil::db::TimeMs at = anvil::db::now_ms();
    const anvil::chattest::Client phone = anvil::chattest::make_client();
    const anvil::chattest::Client bobs = anvil::chattest::make_client();
    ASSERT_TRUE(devices_->register_first_device(db(), alice, phone.device, at, at).ok());
    ASSERT_TRUE(devices_->register_first_device(db(), bob, bobs.device, at, at).ok());
    ASSERT_TRUE(service_->propagate_devices(db(), alice).ok());
    ASSERT_TRUE(service_->propagate_devices(db(), bob).ok());
    const auto opened = service_->open_direct(db(), actor(alice), "direct", bob, true);
    ASSERT_TRUE(opened.ok());
    const Uuid c = opened.value().conversation.id;
    (void)endpoint(bob);

    // What the sender's device encrypted: the server stores it and pushes none
    // of it, in any spelling.
    const std::string secret = "meet me at the old bridge at nine";
    const std::vector<std::uint8_t> common(secret.begin(), secret.end());
    const std::vector<std::uint8_t> to_bob(secret.rbegin(), secret.rend());
    const std::array<chat::DeviceCiphertext, 1> entries{
        chat::DeviceCiphertext{to_bob, bobs.device.id}};
    std::int64_t newest = 0;
    for (int i = 0; i < 3; ++i) {
        const auto sent = service_->send_encrypted(
            db(), actor(alice), c,
            chat::EncryptedMessage{.client_id = anvil::crypto::random_array<16>(),
                                   .ciphertext = common,
                                   .devices = entries,
                                   .attachments = {},
                                   .device_set_version = opened.value().conversation.device_set_version,
                                   .device = phone.device.id,
                                   .page = false});
        ASSERT_TRUE(sent.ok()) << static_cast<int>(sent.code());
        newest = sent.value().seq;
    }
    EXPECT_EQ(run_jobs(), 1);

    const std::vector<Pushed> pushes = to(bob);
    ASSERT_EQ(pushes.size(), 1U);
    EXPECT_TRUE(pushes[0].title.empty());
    EXPECT_EQ(pushes[0].body, chat::encrypted_push_payload(c, newest));

    // The payload as a transport sends it, sealed to a browser's subscription
    // and opened with its private key, as the browser opens it (docs/11 §8).
    const anvil::Result<n::VapidKey> browser = n::generate_vapid_key();
    ASSERT_TRUE(browser.ok());
    const std::array<std::uint8_t, n::kAuthSecretBytes> auth =
        anvil::crypto::random_array<n::kAuthSecretBytes>();
    const anvil::Result<std::vector<std::uint8_t>> sealed = n::encrypt_payload(
        n::PushSubscription{"https://push.example/x", browser.value().public_key, auth},
        pushes[0].body);
    ASSERT_TRUE(sealed.ok());
    const anvil::Result<std::string> opened_payload =
        n::decrypt_payload(browser.value().private_key, auth, sealed.value());
    ASSERT_TRUE(opened_payload.ok());
    const std::string& payload = opened_payload.value();
    EXPECT_EQ(payload, R"({"c":")" + anvil::uuid::to_string(c) + R"(","seq":)" +
                           std::to_string(newest) + "}");
    EXPECT_EQ(payload.find("bridge"), std::string::npos);
    EXPECT_EQ(payload.find(anvil::crypto::base64url_encode(common)), std::string::npos);
    EXPECT_EQ(payload.find(anvil::crypto::base64url_encode(to_bob)), std::string::npos);
    EXPECT_EQ(payload.find("Alice"), std::string::npos);
    // And the sender, whose own device holds the message, is not pushed.
    EXPECT_TRUE(to(alice).empty());
}

}  // namespace
