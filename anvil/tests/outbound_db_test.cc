// Outbound delivery against a live cluster.
//
// The transports here are stubs that record what they were handed and answer with
// whatever verdict the test wants. That is deliberate: what needs asserting is
// who gets a delivery, in what language, and what each verdict costs the
// endpoint — none of which is a property of SMTP or of a push service, and all of
// which is a property of rows that have to be read back to be believed.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/notifications/outbound.h"
#include "anvil/notifications/publish.h"
#include "anvil/notifications/repository.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/topics.h"

namespace {

using anvil::Locale;
using anvil::PermSet;
using anvil::Uuid;
using anvil::testfixture::scratch_names;
namespace n = anvil::notifications;

using testapp::kTemplates;
using testapp::kTopics;
using testapp::Perm;
using testapp::Template;
using testapp::Topic;

constexpr std::string_view kNotifications = "notifications";
constexpr std::string_view kInbox = "notification_inbox";
constexpr std::string_view kClients = "notification_clients";

[[nodiscard]] n::TopicCode code_of(Topic topic) noexcept {
    return static_cast<n::TopicCode>(topic);
}

[[nodiscard]] n::TemplateId id_of(Template tpl) noexcept {
    return static_cast<n::TemplateId>(tpl);
}

[[nodiscard]] anvil::db::TimeMs now() { return anvil::db::now_ms(); }

// What a transport was handed, kept so the test can assert on the delivery rather
// than on whatever the transport chose to do with it.
struct Attempt final {
    std::string title;
    std::string body;
    std::string address;
    Uuid        client;
    n::ClientType type;
};

class OutboundDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kNotifications);
        anvil::testfixture::clear_collection(**client_, kInbox);
        anvil::testfixture::clear_collection(**client_, kClients);
        attempts_.clear();
        verdict_ = n::DeliveryVerdict::Delivered;
        transport_fails_ = false;
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kNotifications)};
    }

    [[nodiscard]] static n::NotificationRepository repository() {
        return n::NotificationRepository{
            database(), n::NotificationCollections{kNotifications, kInbox, kClients}, kTopics};
    }

    [[nodiscard]] n::Transport recorder() {
        return [this](const n::Delivery& delivery) -> anvil::Result<n::DeliveryVerdict> {
            attempts_.push_back(Attempt{delivery.content.title, delivery.content.body,
                                        std::string{delivery.address}, delivery.client,
                                        delivery.type});
            if (transport_fails_) {
                return anvil::fail(anvil::ErrorCode::Internal, "transport");
            }
            return verdict_;
        };
    }

    [[nodiscard]] n::OutboundSender sender(const n::NotificationRepository& repo,
                                           bool with_email = true,
                                           bool with_webhook = true) {
        n::Transports transports{};
        transports.web_push = recorder();
        if (with_email) { transports.email = recorder(); }
        if (with_webhook) { transports.webhook = recorder(); }

        n::OutboundHooks hooks{};
        hooks.locale_of = [this](mongocxx::client&, const std::optional<Uuid>&) {
            return locale_;
        };
        hooks.may_receive = [this](mongocxx::client&, const Uuid& user,
                                   const PermSet&) -> anvil::Result<bool> {
            return !denied_.has_value() || user != *denied_;
        };
        return n::OutboundSender{repo, kTemplates, std::move(transports), std::move(hooks)};
    }

    // A client of a given type, subscribed to one topic. `owner` may be absent,
    // which is what a system-level webhook looks like.
    Uuid endpoint(n::ClientType type, Topic topic, const std::optional<Uuid>& owner,
                  std::string_view address, const std::optional<Uuid>& subject = {}) {
        n::ClientRow row{};
        row.id = anvil::uuid::generate_v4();
        row.addr.assign(address);
        row.owner = owner;
        row.created_at = now();
        row.prefs = n::Preferences::all_enabled();
        row.type = type;
        const Uuid since =
            anvil::uuid::v7_boundary(now().time_since_epoch().count() - 600'000);
        EXPECT_TRUE(row.subs.push({subject.value_or(anvil::kNilUuid), since, code_of(topic)}));
        EXPECT_TRUE(repository().insert_client(db(), row).ok());
        return row.id;
    }

    // A published, dispatched announcement.
    Uuid announce(std::string_view body, std::string_view key,
                  n::ChannelMask channels = n::kDefaultChannels) {
        const n::NotificationRepository repo = repository();
        n::PublishHooks hooks{};
        hooks.may_receive = [](mongocxx::client&, const Uuid&,
                               const PermSet&) -> anvil::Result<bool> { return true; };
        hooks.enqueue_transports = [](const Uuid&, n::ChannelMask) { return anvil::ok(); };

        const std::array<n::Param, 1> params{n::Param::of('b', body)};
        n::PublishRequest request{};
        request.topic = n::global_topic(code_of(Topic::SystemAnnouncement));
        request.idempotency_key = key;
        request.tpl = id_of(Template::Announcement);
        request.params = params;
        request.channels = channels;

        const n::PublishService service{repo, kTemplates, std::move(hooks)};
        const anvil::Result<n::PublishOutcome> out = service.publish(db(), request, now());
        EXPECT_TRUE(out.ok()) << static_cast<int>(out.code());
        return out.ok() ? out.value().id : Uuid{};
    }

    Uuid sign_in(const Uuid& account, std::string_view key) {
        const n::NotificationRepository repo = repository();
        n::PublishHooks hooks{};
        hooks.may_receive = [](mongocxx::client&, const Uuid&,
                               const PermSet&) -> anvil::Result<bool> { return true; };
        hooks.enqueue_transports = [](const Uuid&, n::ChannelMask) { return anvil::ok(); };

        const std::array<n::Param, 1> params{n::Param::of('d', "Firefox on Linux")};
        n::PublishRequest request{};
        request.topic = n::scoped_topic(code_of(Topic::SessionNewDevice), account);
        request.idempotency_key = key;
        request.tpl = id_of(Template::SessionNewDevice);
        request.params = params;

        const n::PublishService service{repo, kTemplates, std::move(hooks)};
        const anvil::Result<n::PublishOutcome> out = service.publish(db(), request, now());
        EXPECT_TRUE(out.ok()) << static_cast<int>(out.code());
        return out.ok() ? out.value().id : Uuid{};
    }

    [[nodiscard]] std::optional<n::ClientRow> reread(const Uuid& id) {
        const anvil::Result<std::optional<n::ClientRow>> row =
            repository().find_client(db(), id);
        EXPECT_TRUE(row.ok());
        return row.ok() ? row.value() : std::nullopt;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
    std::vector<Attempt>                   attempts_;
    n::DeliveryVerdict                     verdict_{n::DeliveryVerdict::Delivered};
    bool                                   transport_fails_{false};
    Locale                                 locale_{};
    std::optional<Uuid>                    denied_;
};

}  // namespace

// --- who gets one ------------------------------------------------------------

TEST_F(OutboundDb, EverySubscribedEndpointOfEveryChannelGetsOne) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    const Uuid push = endpoint(n::ClientType::WebPush, Topic::SystemAnnouncement, reader,
                               "https://push.example.com/a");
    const Uuid mail = endpoint(n::ClientType::Email, Topic::SystemAnnouncement, reader,
                               "reader@example.com");
    // In-app subscribes too, and gets nothing here: its delivery IS the inbox
    // row, which was written before any of this ran.
    static_cast<void>(endpoint(n::ClientType::InApp, Topic::SystemAnnouncement, reader, ""));

    const Uuid notification = announce("Opening hours change", "a1");
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), notification, n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok()) << static_cast<int>(summary.code());
    EXPECT_EQ(summary.value().attempted, 2);
    EXPECT_EQ(summary.value().delivered, 2);

    ASSERT_EQ(attempts_.size(), 2U);
    std::vector<Uuid> reached;
    for (const Attempt& attempt : attempts_) { reached.push_back(attempt.client); }
    EXPECT_NE(std::find(reached.begin(), reached.end(), push), reached.end());
    EXPECT_NE(std::find(reached.begin(), reached.end(), mail), reached.end());
}

TEST_F(OutboundDb, AnAccountScopedTopicReachesTheSubjectsOwnEndpoints) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const Uuid stranger = anvil::uuid::generate_v7();

    // Deliberately subscribed to a DIFFERENT topic, and never to this one. An
    // account's own sign-in alert must reach its own mailbox regardless: an
    // account that never subscribed to anything is exactly the account an
    // attacker has just signed into.
    const Uuid mine = endpoint(n::ClientType::Email, Topic::SystemAnnouncement, account,
                               "me@example.com");
    const Uuid theirs = endpoint(n::ClientType::Email, Topic::SessionNewDevice, stranger,
                                 "them@example.com");

    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), sign_in(account, "s1"), n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok()) << static_cast<int>(summary.code());
    ASSERT_EQ(attempts_.size(), 1U);
    EXPECT_EQ(attempts_.front().client, mine);
    EXPECT_EQ(attempts_.front().address, "me@example.com");
    // And one person's sign-in does not go to everybody subscribed to the kind.
    EXPECT_NE(attempts_.front().client, theirs);
}

TEST_F(OutboundDb, ADisabledEndpointIsNotAttempted) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const Uuid mail = endpoint(n::ClientType::Email, Topic::SessionNewDevice, account,
                               "me@example.com");
    ASSERT_TRUE(repo.disable_client(db(), mail, now(), n::DeliveryVerdict::Gone).ok());

    // The account-scoped path reads clients_of, which is an operator's listing as
    // much as a delivery scan and does not filter disabled rows — so the sender
    // has to.
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), sign_in(account, "s1"), n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok());
    EXPECT_EQ(summary.value().attempted, 0);
    EXPECT_TRUE(attempts_.empty());
}

TEST_F(OutboundDb, ANarrowedChannelMaskTakesAChannelAway) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(endpoint(n::ClientType::WebPush, Topic::SystemAnnouncement, reader,
                               "https://push.example.com/a"));
    static_cast<void>(endpoint(n::ClientType::Email, Topic::SystemAnnouncement, reader,
                               "reader@example.com"));

    const Uuid notification = announce("Members only", "a1", n::channels(n::ClientType::InApp));
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), notification, n::channels(n::ClientType::InApp), now());
    ASSERT_TRUE(summary.ok());
    // The mask travels on the job, so a publish that narrowed it is still
    // narrowed here rather than recomputed from the topic's wider default.
    EXPECT_EQ(summary.value().attempted, 0);
    EXPECT_EQ(summary.value().skipped, 2);
}

TEST_F(OutboundDb, AMutedChannelIsSkippedWhileTheOthersAreNot) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();

    n::ClientRow muted{};
    muted.id = anvil::uuid::generate_v4();
    muted.addr = "reader@example.com";
    muted.owner = reader;
    muted.created_at = now();
    muted.type = n::ClientType::Email;
    muted.prefs = n::Preferences::all_enabled();
    muted.prefs.set(code_of(Topic::SystemAnnouncement), n::ClientType::Email, false);
    const Uuid since = anvil::uuid::v7_boundary(now().time_since_epoch().count() - 600'000);
    ASSERT_TRUE(muted.subs.push({anvil::kNilUuid, since, code_of(Topic::SystemAnnouncement)}));
    ASSERT_TRUE(repo.insert_client(db(), muted).ok());

    const Uuid push = endpoint(n::ClientType::WebPush, Topic::SystemAnnouncement, reader,
                               "https://push.example.com/a");

    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), announce("Opening hours", "a1"), n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok());
    // The preference is per CHANNEL, so muting mail does not mute push.
    ASSERT_EQ(attempts_.size(), 1U);
    EXPECT_EQ(attempts_.front().client, push);
}

TEST_F(OutboundDb, AChannelWithNoTransportIsSkippedAndNotFailed) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    const Uuid mail = endpoint(n::ClientType::Email, Topic::SystemAnnouncement, reader,
                               "reader@example.com");

    // A deployment with no SMTP configured. It has not got a broken mailbox, it
    // has no mail — and recording failures here would disable every mailbox in
    // the system after five notifications.
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo, /*with_email=*/false)
            .send(db(), announce("Opening hours", "a1"), n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok());
    EXPECT_EQ(summary.value().attempted, 0);
    EXPECT_EQ(summary.value().skipped, 1);

    const std::optional<n::ClientRow> row = reread(mail);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->fail_n, 0);
    EXPECT_FALSE(row->disabled_at.has_value());
}

TEST_F(OutboundDb, ThePermissionIsRecheckedBeforeTheTransportRuns) {
    const n::NotificationRepository repo = repository();
    const Uuid form = anvil::uuid::generate_v7();
    const Uuid keeps = anvil::uuid::generate_v7();
    const Uuid lost = anvil::uuid::generate_v7();
    static_cast<void>(endpoint(n::ClientType::Email, Topic::StaffAssigned, keeps,
                               "keeps@example.com", form));
    const Uuid gone = endpoint(n::ClientType::Email, Topic::StaffAssigned, lost,
                               "lost@example.com", form);

    const n::NotificationRepository writer = repository();
    n::PublishHooks publish_hooks{};
    publish_hooks.may_receive = [](mongocxx::client&, const Uuid&,
                                   const PermSet&) -> anvil::Result<bool> { return true; };
    publish_hooks.enqueue_transports = [](const Uuid&, n::ChannelMask) { return anvil::ok(); };
    const std::array<n::Param, 1> params{n::Param::of('t', "Summer Programme")};
    n::PublishRequest request{};
    request.topic = n::scoped_topic(code_of(Topic::StaffAssigned), form);
    request.idempotency_key = "assign-1";
    request.tpl = id_of(Template::FormAccepted);
    request.params = params;
    const n::PublishService publisher{writer, kTemplates, std::move(publish_hooks)};
    const anvil::Result<n::PublishOutcome> published = publisher.publish(db(), request, now());
    ASSERT_TRUE(published.ok());

    // Lost the bit between the fan-out and the send. The inbox row exists; the
    // mail must not.
    denied_ = lost;
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), published.value().id, n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok()) << static_cast<int>(summary.code());
    ASSERT_EQ(attempts_.size(), 1U);
    EXPECT_EQ(attempts_.front().address, "keeps@example.com");
    EXPECT_NE(attempts_.front().client, gone);
}

TEST_F(OutboundDb, AnExpiredNotificationIsNotDelivered) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(endpoint(n::ClientType::Email, Topic::SystemAnnouncement, reader,
                               "reader@example.com"));
    const Uuid notification = announce("Opening hours", "a1");

    // Read with a clock past the row's retention. E-mailing somebody about
    // something their inbox no longer shows is worse than not mailing them.
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), notification, n::kDefaultChannels,
                          now() + std::chrono::hours{24 * 400});
    ASSERT_TRUE(summary.ok());
    EXPECT_EQ(summary.value().attempted, 0);
    EXPECT_TRUE(attempts_.empty());
}

// --- what a verdict costs the endpoint ---------------------------------------

TEST_F(OutboundDb, DeliveryResetsTheFailureStreak) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const Uuid mail = endpoint(n::ClientType::Email, Topic::SessionNewDevice, account,
                               "me@example.com");
    ASSERT_TRUE(repo.record_delivery_failure(db(), mail, n::DeliveryVerdict::Transient, now())
                    .ok());
    ASSERT_TRUE(repo.record_delivery_failure(db(), mail, n::DeliveryVerdict::Transient, now())
                    .ok());

    ASSERT_TRUE(sender(repo).send(db(), sign_in(account, "s1"), n::kDefaultChannels, now()).ok());
    const std::optional<n::ClientRow> row = reread(mail);
    ASSERT_TRUE(row.has_value());
    // A transient failure that recovers must not accumulate across weeks into a
    // disable nobody can account for.
    EXPECT_EQ(row->fail_n, 0);
    EXPECT_EQ(row->last_verdict, n::DeliveryVerdict::Delivered);
}

TEST_F(OutboundDb, AGoneEndpointIsDisabledImmediatelyAndNotAfterAStreak) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const Uuid mail = endpoint(n::ClientType::Email, Topic::SessionNewDevice, account,
                               "me@example.com");

    verdict_ = n::DeliveryVerdict::Gone;
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), sign_in(account, "s1"), n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok());
    EXPECT_EQ(summary.value().disabled, 1);

    const std::optional<n::ClientRow> row = reread(mail);
    ASSERT_TRUE(row.has_value());
    // The endpoint told us it is finished. A streak would only delay believing it.
    EXPECT_TRUE(row->disabled_at.has_value());
    EXPECT_EQ(row->last_verdict, n::DeliveryVerdict::Gone);
}

TEST_F(OutboundDb, ATransientFailureCountsAndDisablesOnlyAtTheThreshold) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const Uuid mail = endpoint(n::ClientType::Email, Topic::SessionNewDevice, account,
                               "me@example.com");

    verdict_ = n::DeliveryVerdict::Transient;
    for (std::int32_t i = 1; i < n::kMaxSoftFailures; ++i) {
        ASSERT_TRUE(sender(repo)
                        .send(db(), sign_in(account, "s" + std::to_string(i)),
                              n::kDefaultChannels, now())
                        .ok());
        const std::optional<n::ClientRow> row = reread(mail);
        ASSERT_TRUE(row.has_value());
        EXPECT_EQ(row->fail_n, i);
        EXPECT_FALSE(row->disabled_at.has_value()) << i;
    }

    // At the threshold: a permanently broken endpoint that retries forever is a
    // self-inflicted outbound flood aimed at somebody else's infrastructure.
    ASSERT_TRUE(sender(repo)
                    .send(db(), sign_in(account, "final"), n::kDefaultChannels, now())
                    .ok());
    const std::optional<n::ClientRow> row = reread(mail);
    ASSERT_TRUE(row.has_value());
    EXPECT_TRUE(row->disabled_at.has_value());
}

TEST_F(OutboundDb, ATransportThatCannotFormARequestIsRejectedRatherThanRetried) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const Uuid mail = endpoint(n::ClientType::Email, Topic::SessionNewDevice, account,
                               "me@example.com");

    transport_fails_ = true;
    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), sign_in(account, "s1"), n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok());
    EXPECT_EQ(summary.value().disabled, 1);

    const std::optional<n::ClientRow> row = reread(mail);
    ASSERT_TRUE(row.has_value());
    // A request that was never made cannot be blamed on the endpoint, and will
    // not start working on a retry either.
    EXPECT_EQ(row->last_verdict, n::DeliveryVerdict::Rejected);
    EXPECT_TRUE(row->disabled_at.has_value());
}

// --- rendering ---------------------------------------------------------------

TEST_F(OutboundDb, EachEndpointIsRenderedInItsOwnReadersLanguage) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    static_cast<void>(endpoint(n::ClientType::Email, Topic::SessionNewDevice, account,
                               "me@example.com"));

    const std::optional<Locale> arabic = Locale::from_tag("ar");
    ASSERT_TRUE(arabic.has_value());
    locale_ = *arabic;

    ASSERT_TRUE(sender(repo).send(db(), sign_in(account, "s1"), n::kDefaultChannels, now()).ok());
    ASSERT_EQ(attempts_.size(), 1U);
    // Two subscribers to one topic do not necessarily read the same language, so
    // the render is per endpoint rather than once per notification.
    EXPECT_EQ(attempts_.front().title, "تسجيل دخول جديد");
}

TEST_F(OutboundDb, AnOwnerlessWebhookReceivesAnUngatedTopic) {
    const n::NotificationRepository repo = repository();
    // A system-level webhook: no owner at all. It has no inbox to write to, which
    // is why the fan-out skipped it — but it is still an endpoint.
    const Uuid hook = endpoint(n::ClientType::Webhook, Topic::ContentPublished, std::nullopt,
                               "https://hooks.example.com/anvil");

    const n::NotificationRepository writer = repository();
    n::PublishHooks hooks{};
    hooks.may_receive = [](mongocxx::client&, const Uuid&,
                           const PermSet&) -> anvil::Result<bool> { return true; };
    hooks.enqueue_transports = [](const Uuid&, n::ChannelMask) { return anvil::ok(); };
    const std::array<n::Param, 1> params{n::Param::of('t', "Summer Hours")};
    n::PublishRequest request{};
    request.topic = n::global_topic(code_of(Topic::ContentPublished));
    request.idempotency_key = "c1";
    request.tpl = id_of(Template::ContentPublished);
    request.params = params;
    const n::PublishService publisher{writer, kTemplates, std::move(hooks)};
    const anvil::Result<n::PublishOutcome> published = publisher.publish(db(), request, now());
    ASSERT_TRUE(published.ok());

    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), published.value().id, n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok());
    ASSERT_EQ(attempts_.size(), 1U);
    EXPECT_EQ(attempts_.front().client, hook);
    EXPECT_EQ(attempts_.front().type, n::ClientType::Webhook);
}

TEST_F(OutboundDb, AGatedTopicSkipsAnEndpointWithNoAccountBehindIt) {
    const n::NotificationRepository repo = repository();
    const Uuid form = anvil::uuid::generate_v7();
    // A permission-gated topic and an endpoint with no owner. There is nobody to
    // check the permission against, and "nobody" is not "everybody" — so it is
    // refused. An integration that needs a gated topic registers under an account
    // that holds the bit.
    static_cast<void>(endpoint(n::ClientType::Webhook, Topic::FormSubmitted, std::nullopt,
                               "https://hooks.example.com/anvil", form));

    const n::NotificationRepository writer = repository();
    n::PublishHooks hooks{};
    hooks.may_receive = [](mongocxx::client&, const Uuid&,
                           const PermSet&) -> anvil::Result<bool> { return true; };
    hooks.enqueue_transports = [](const Uuid&, n::ChannelMask) { return anvil::ok(); };
    const std::array<n::Param, 2> params{n::Param::of('t', "Applications"),
                                         n::Param::of('n', std::int64_t{1})};
    n::PublishRequest request{};
    request.topic = n::scoped_topic(code_of(Topic::FormSubmitted), form);
    request.tpl = id_of(Template::FormSubmitted);
    request.params = params;
    const n::PublishService publisher{writer, kTemplates, std::move(hooks)};
    const anvil::Result<n::PublishOutcome> published = publisher.publish(db(), request, now());
    ASSERT_TRUE(published.ok());

    const anvil::Result<n::OutboundSummary> summary =
        sender(repo).send(db(), published.value().id, n::kDefaultChannels, now());
    ASSERT_TRUE(summary.ok());
    EXPECT_EQ(summary.value().attempted, 0);
    EXPECT_TRUE(attempts_.empty());
}
