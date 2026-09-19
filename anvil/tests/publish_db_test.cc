// The publish path against a live cluster.
//
// Nothing here is a unit test, for the same reason nothing in
// notifications_db_test.cc is: what is being asserted is that the ORDER of the
// writes survives a process that dies between them. The outbox marker, the
// unique dedupe index and the idempotent fan-out only mean anything against a
// server that can be left half-written and then read back.
//
// The crash is simulated the only honest way — by stopping after the step the
// crash would have stopped after, and then running the sweeper the way a real
// deployment would.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/notifications/publish.h"
#include "anvil/notifications/repository.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/topics.h"

namespace {

using anvil::ErrorCode;
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

// What the hooks saw. A publish's whole job is to reach these two, so the test
// observes them rather than inferring from side effects.
struct HookLog final {
    std::vector<std::pair<Uuid, n::ChannelMask>> enqueued;
    std::vector<Uuid>                            probed;
    PermSet                                      granted;
    bool                                         grant_everyone{true};
};

class PublishDb : public ::testing::Test {
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
        log_ = HookLog{};
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kNotifications)};
    }

    [[nodiscard]] static n::NotificationRepository repository() {
        return n::NotificationRepository{
            database(), n::NotificationCollections{kNotifications, kInbox, kClients}, kTopics};
    }

    [[nodiscard]] n::PublishHooks hooks() {
        n::PublishHooks out{};
        out.may_receive = [this](mongocxx::client&, const Uuid& user,
                                 const PermSet& required) -> anvil::Result<bool> {
            log_.probed.push_back(user);
            return log_.grant_everyone || log_.granted.contains_all(required);
        };
        out.enqueue_transports = [this](const Uuid& id, n::ChannelMask mask) -> anvil::Status {
            log_.enqueued.emplace_back(id, mask);
            return anvil::ok();
        };
        return out;
    }

    // A service whose hooks record. Rebuilt per call because the repository is a
    // value and both are cheap: nothing here is worth a member's lifetime.
    [[nodiscard]] n::PublishService service(const n::NotificationRepository& repo) {
        return n::PublishService{repo, kTemplates, hooks()};
    }

    // The same service with a pressure probe wired to a fixed reading. A real
    // deployment divides a pool's depth by its capacity; what the storm breaker
    // does with the number is the same either way, and a real pool saturated on
    // purpose would make these cases about thread scheduling instead.
    [[nodiscard]] n::PublishService service_under(const n::NotificationRepository& repo,
                                                  float pressure) {
        n::PublishHooks with_probe = hooks();
        with_probe.pressure = [pressure]() { return pressure; };
        return n::PublishService{repo, kTemplates, std::move(with_probe)};
    }

    [[nodiscard]] static n::PublishRequest request_of(Topic topic, Template tpl,
                                                      std::string_view idempotency_key,
                                                      const std::optional<Uuid>& subject = {}) {
        n::PublishRequest request{};
        request.topic = subject.has_value() ? n::scoped_topic(code_of(topic), *subject)
                                            : n::global_topic(code_of(topic));
        request.idempotency_key = idempotency_key;
        request.tpl = id_of(tpl);
        return request;
    }

    // An in-app client subscribed to one topic, with every preference on.
    [[nodiscard]] Uuid subscriber(Topic topic, const Uuid& owner,
                                  const std::optional<Uuid>& subject = {}) {
        n::ClientRow row{};
        row.id = anvil::uuid::generate_v4();
        row.owner = owner;
        row.created_at = now();
        row.prefs = n::Preferences::all_enabled();
        row.type = n::ClientType::InApp;
        // `since` is the floor of a millisecond in the past, never a generated
        // v7: a generated one makes `_id > since` a coin flip for anything
        // published in the same millisecond (record.h, Subscription).
        const Uuid since = anvil::uuid::v7_boundary(
            now().time_since_epoch().count() - 60'000);
        EXPECT_TRUE(row.subs.push(
            {subject.value_or(anvil::kNilUuid), since, code_of(topic)}));
        EXPECT_TRUE(repository().insert_client(db(), row).ok());
        return row.id;
    }

    [[nodiscard]] std::size_t inbox_size(const Uuid& uid) {
        const anvil::Result<n::InboxPage> page =
            repository().page_inbox(db(), uid, {}, n::kMaxInboxPageSize, now());
        EXPECT_TRUE(page.ok());
        return page.ok() ? page.value().rows.size() : 0;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
    HookLog                                log_;
};

// The single text parameter every template below the announcement takes.
[[nodiscard]] std::array<n::Param, 1> one_text(char name, std::string_view value) {
    return {n::Param::of(name, value)};
}

}  // namespace

// --- the row, then the tail --------------------------------------------------

TEST_F(PublishDb, APublishCommitsTheRowAndThenMarksItDispatched) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");

    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "publish-1");
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());
    EXPECT_TRUE(published.value().created);
    EXPECT_EQ(published.value().count, 1);

    const anvil::Result<std::optional<n::NotificationRow>> row =
        repo.find_notification(db(), published.value().id, now());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());
    // The marker is what a sweeper reads to decide the tail already ran. It is
    // written LAST, so its presence means the fan-out and the enqueue both
    // finished.
    EXPECT_TRUE((*row.value()).dispatched_at.has_value());
    ASSERT_EQ(log_.enqueued.size(), 1U);
    EXPECT_EQ(log_.enqueued.front().first, published.value().id);
}

TEST_F(PublishDb, ABroadcastTopicWritesNoInboxRowsAtAll) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(subscriber(Topic::ContentPublished, reader));

    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "broadcast-1");
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok());
    // ONE row however large the audience. A subscriber's view is a range query,
    // which is the entire point of FanOut::Read — an inbox row here would be the
    // write storm the strategy exists to avoid.
    EXPECT_EQ(published.value().delivered, 0);
    EXPECT_EQ(inbox_size(reader), 0U);
}

// --- idempotency -------------------------------------------------------------

TEST_F(PublishDb, ARetriedPublishReturnsTheFirstRowRatherThanWritingASecond) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "retry-me");
    request.params = params;

    const anvil::Result<n::PublishOutcome> first = service(repo).publish(db(), request, now());
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value().created);

    // The same idempotency key is what an at-least-once queue produces when it
    // redelivers the job that published the first one.
    const anvil::Result<n::PublishOutcome> second = service(repo).publish(db(), request, now());
    ASSERT_TRUE(second.ok()) << static_cast<int>(second.code());
    EXPECT_FALSE(second.value().created);
    // The SAME id, not a Conflict the caller has to interpret: the retry needs to
    // be able to dispatch the row the winner may have left stranded.
    EXPECT_EQ(second.value().id, first.value().id);
}

TEST_F(PublishDb, ADifferentIdempotencyKeyIsADifferentNotification) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");

    n::PublishRequest one = request_of(Topic::ContentPublished, Template::ContentPublished,
                                       "event-a");
    one.params = params;
    n::PublishRequest two = request_of(Topic::ContentPublished, Template::ContentPublished,
                                       "event-b");
    two.params = params;

    const anvil::Result<n::PublishOutcome> first = service(repo).publish(db(), one, now());
    const anvil::Result<n::PublishOutcome> second = service(repo).publish(db(), two, now());
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second.value().created);
    EXPECT_NE(first.value().id, second.value().id);
}

TEST_F(PublishDb, ANonCoalescingPublishWithNoIdempotencyKeyIsRefused) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "");
    request.params = params;

    // Refused rather than defaulted to something this call invents: a generated
    // key differs on the retry, and the retry is then a second notification.
    EXPECT_EQ(service(repo).publish(db(), request, now()).code(), ErrorCode::ValidationFailed);
}

TEST_F(PublishDb, CoalescingCollapsesRepeatsInsideTheWindowIntoOneRow) {
    const n::NotificationRepository repo = repository();
    const Uuid form = anvil::uuid::generate_v7();
    const std::array<n::Param, 2> params{n::Param::of('t', "Applications"),
                                         n::Param::of('n', std::int64_t{2})};

    // Two DIFFERENT events — distinct idempotency keys — on a coalescing topic.
    // They must still become one row, because the coalescing key identifies the
    // event class and not the instance.
    n::PublishRequest first = request_of(Topic::FormSubmitted, Template::FormSubmitted,
                                         "submission-1", form);
    first.params = params;
    n::PublishRequest second = request_of(Topic::FormSubmitted, Template::FormSubmitted,
                                          "submission-2", form);
    second.params = params;

    const anvil::Result<n::PublishOutcome> one = service(repo).publish(db(), first, now());
    ASSERT_TRUE(one.ok()) << static_cast<int>(one.code());
    EXPECT_TRUE(one.value().created);
    EXPECT_EQ(one.value().count, 1);

    const anvil::Result<n::PublishOutcome> two = service(repo).publish(db(), second, now());
    ASSERT_TRUE(two.ok());
    EXPECT_FALSE(two.value().created);
    EXPECT_EQ(two.value().count, 2);
    // The reader's cursor must not move under them: a coalesced repeat is the
    // same event happening again.
    EXPECT_EQ(two.value().id, one.value().id);
}

TEST_F(PublishDb, ADifferentSubjectDoesNotCoalesceIntoAnotherFormsNotification) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 2> params{n::Param::of('t', "Applications"),
                                         n::Param::of('n', std::int64_t{1})};

    n::PublishRequest first = request_of(Topic::FormSubmitted, Template::FormSubmitted, "a",
                                         anvil::uuid::generate_v7());
    first.params = params;
    n::PublishRequest second = request_of(Topic::FormSubmitted, Template::FormSubmitted, "b",
                                          anvil::uuid::generate_v7());
    second.params = params;

    const anvil::Result<n::PublishOutcome> one = service(repo).publish(db(), first, now());
    const anvil::Result<n::PublishOutcome> two = service(repo).publish(db(), second, now());
    ASSERT_TRUE(one.ok());
    ASSERT_TRUE(two.ok());
    // The subject is IN the coalescing key. Without it, two forms under load
    // would merge into one notification and one of the two would be invisible.
    EXPECT_TRUE(two.value().created);
    EXPECT_NE(one.value().id, two.value().id);
}

// --- who receives it ---------------------------------------------------------

TEST_F(PublishDb, AnAccountScopedPublishReachesASubjectWithNoClientRow) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "Firefox on Linux");

    n::PublishRequest request = request_of(Topic::SessionNewDevice,
                                           Template::SessionNewDevice, "signin-1", account);
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());
    // An account that has never registered a client has never subscribed to
    // anything. Resolving the audience by querying subscriptions would lose the
    // sign-in alert for exactly the account an attacker just signed into.
    EXPECT_EQ(published.value().delivered, 1);
    EXPECT_EQ(inbox_size(account), 1U);
}

TEST_F(PublishDb, ASecurityTopicIsDeliveredEvenWhenTheStoredMaskMutesIt) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();

    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.owner = account;
    row.created_at = now();
    row.type = n::ClientType::InApp;
    // Every bit off: a client row an older build wrote, or one somebody edited by
    // hand. The exemption has to live in the RULE, not in the stored mask.
    row.prefs = n::Preferences{};
    ASSERT_TRUE(repo.insert_client(db(), row).ok());

    const std::array<n::Param, 1> params = one_text('d', "Firefox on Linux");
    n::PublishRequest request = request_of(Topic::SessionNewDevice,
                                           Template::SessionNewDevice, "signin-2", account);
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok());
    // An account that can silence its own "new sign-in" alert has no alert.
    EXPECT_EQ(published.value().delivered, 1);
}

TEST_F(PublishDb, AMutedOptionalTopicIsNotDelivered) {
    const n::NotificationRepository repo = repository();
    const Uuid applicant = anvil::uuid::generate_v7();
    const Uuid form = anvil::uuid::generate_v7();

    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.owner = applicant;
    row.created_at = now();
    row.type = n::ClientType::InApp;
    row.prefs = n::Preferences::all_enabled();
    row.prefs.set(code_of(Topic::FormAccepted), n::ClientType::InApp, false);
    const Uuid since = anvil::uuid::v7_boundary(now().time_since_epoch().count() - 60'000);
    ASSERT_TRUE(row.subs.push({form, since, code_of(Topic::FormAccepted)}));
    ASSERT_TRUE(repo.insert_client(db(), row).ok());

    const std::array<n::Param, 1> params = one_text('t', "Summer Programme");
    n::PublishRequest request = request_of(Topic::FormAccepted, Template::FormAccepted,
                                           "accept-1", form);
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok());
    // user_optional is true for this topic, so the stored preference decides.
    EXPECT_EQ(published.value().delivered, 0);
    EXPECT_EQ(inbox_size(applicant), 0U);
}

TEST_F(PublishDb, ATargetedPublishFansOutToTheTopicsSubscribers) {
    const n::NotificationRepository repo = repository();
    const Uuid form = anvil::uuid::generate_v7();
    const Uuid subscribed = anvil::uuid::generate_v7();
    const Uuid elsewhere = anvil::uuid::generate_v7();

    static_cast<void>(subscriber(Topic::FormAccepted, subscribed, form));
    // Subscribed to the same KIND but a different subject. A fan-out that matched
    // the two fields independently would deliver another form's decision to them.
    static_cast<void>(subscriber(Topic::FormAccepted, elsewhere, anvil::uuid::generate_v7()));

    const std::array<n::Param, 1> params = one_text('t', "Summer Programme");
    n::PublishRequest request = request_of(Topic::FormAccepted, Template::FormAccepted,
                                           "accept-2", form);
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());
    EXPECT_EQ(published.value().delivered, 1);
    EXPECT_EQ(inbox_size(subscribed), 1U);
    EXPECT_EQ(inbox_size(elsewhere), 0U);
}

TEST_F(PublishDb, ThePermissionIsRecheckedAgainstTheRecipientAtDispatch) {
    const n::NotificationRepository repo = repository();
    const Uuid form = anvil::uuid::generate_v7();
    const Uuid keeps_the_bit = anvil::uuid::generate_v7();
    const Uuid lost_the_bit = anvil::uuid::generate_v7();

    static_cast<void>(subscriber(Topic::StaffAssigned, keeps_the_bit, form));
    static_cast<void>(subscriber(Topic::StaffAssigned, lost_the_bit, form));

    // Only one of them still holds StaffManage. The subscription was written when
    // both did, which is exactly why the check cannot be trusted from then.
    log_.grant_everyone = false;
    log_.granted = anvil::perm_mask(Perm::StaffManage);

    n::PublishHooks gated{};
    gated.may_receive = [&](mongocxx::client&, const Uuid& user,
                            const PermSet& required) -> anvil::Result<bool> {
        log_.probed.push_back(user);
        return user == keeps_the_bit && log_.granted.contains_all(required);
    };
    gated.enqueue_transports = [&](const Uuid& id, n::ChannelMask mask) -> anvil::Status {
        log_.enqueued.emplace_back(id, mask);
        return anvil::ok();
    };

    const std::array<n::Param, 1> params = one_text('t', "Summer Programme");
    n::PublishRequest request = request_of(Topic::StaffAssigned, Template::FormAccepted,
                                           "assign-1", form);
    request.params = params;

    const n::PublishService service{repo, kTemplates, std::move(gated)};
    const anvil::Result<n::PublishOutcome> published = service.publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());
    EXPECT_EQ(published.value().delivered, 1);
    EXPECT_EQ(inbox_size(keeps_the_bit), 1U);
    EXPECT_EQ(inbox_size(lost_the_bit), 0U);
}

TEST_F(PublishDb, AGatedTopicWithNoProbeFailsClosed) {
    const n::NotificationRepository repo = repository();
    const Uuid form = anvil::uuid::generate_v7();
    const Uuid staff = anvil::uuid::generate_v7();
    static_cast<void>(subscriber(Topic::StaffAssigned, staff, form));

    // The application declared a permission-gated topic and supplied nothing to
    // ask. Delivering to everyone is the one outcome that cannot be walked back.
    n::PublishHooks incomplete{};
    incomplete.enqueue_transports = [](const Uuid&, n::ChannelMask) { return anvil::ok(); };

    const std::array<n::Param, 1> params = one_text('t', "Summer Programme");
    n::PublishRequest request = request_of(Topic::StaffAssigned, Template::FormAccepted,
                                           "assign-2", form);
    request.params = params;

    const n::PublishService service{repo, kTemplates, std::move(incomplete)};
    const anvil::Result<n::PublishOutcome> published = service.publish(db(), request, now());
    // The ROW still commits — the publish itself succeeded — but nothing was
    // delivered and the row stays in the outbox for an operator to find.
    ASSERT_TRUE(published.ok());
    EXPECT_EQ(published.value().delivered, 0);
    EXPECT_EQ(inbox_size(staff), 0U);

    const anvil::Result<std::optional<n::NotificationRow>> row =
        repo.find_notification(db(), published.value().id, now());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());
    EXPECT_FALSE((*row.value()).dispatched_at.has_value());
}

// --- what publishing refuses -------------------------------------------------

TEST_F(PublishDb, AMissingParameterIsRefusedRatherThanRenderedAsAHole) {
    const n::NotificationRepository repo = repository();
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "no-params");
    // The renderer degrades a missing placeholder to nothing, which is right for
    // a row an older build wrote and wrong for one being written now.
    EXPECT_EQ(service(repo).publish(db(), request, now()).code(), ErrorCode::ValidationFailed);
}

TEST_F(PublishDb, AParameterTheTemplateDoesNotAskForIsRefusedRatherThanDropped) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('z', "Summer Hours");
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "wrong-name");
    request.params = params;
    EXPECT_EQ(service(repo).publish(db(), request, now()).code(), ErrorCode::ValidationFailed);
}

TEST_F(PublishDb, AParameterCarryingABidiOverrideIsRefusedAtTheBoundary) {
    const n::NotificationRepository repo = repository();
    // U+202E. In a notification body it reverses the whole sentence and the
    // reader cannot see that it did, so it is refused before it is STORED rather
    // than only when it is rendered.
    const std::array<n::Param, 1> params = one_text('t', "Summer\xE2\x80\xAEsruoH");
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "bidi");
    request.params = params;
    EXPECT_EQ(service(repo).publish(db(), request, now()).code(), ErrorCode::ValidationFailed);
}

TEST_F(PublishDb, AScopeDisagreementIsRefusedInBothDirections) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");

    // A Global topic WITH a subject writes a row no subscriber's `$or` branch
    // will ever match.
    n::PublishRequest scoped_global = request_of(Topic::ContentPublished,
                                                 Template::ContentPublished, "s1",
                                                 anvil::uuid::generate_v7());
    scoped_global.params = params;
    EXPECT_EQ(service(repo).publish(db(), scoped_global, now()).code(),
              ErrorCode::ValidationFailed);

    // An Account topic WITHOUT one fans out to nobody.
    const std::array<n::Param, 1> device = one_text('d', "Firefox on Linux");
    n::PublishRequest unscoped_account = request_of(Topic::SessionNewDevice,
                                                    Template::SessionNewDevice, "s2");
    unscoped_account.params = device;
    EXPECT_EQ(service(repo).publish(db(), unscoped_account, now()).code(),
              ErrorCode::ValidationFailed);
}

TEST_F(PublishDb, AnUnknownTopicOrTemplateIsRefusedRatherThanStored) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");

    n::PublishRequest bad_topic = request_of(Topic::ContentPublished,
                                             Template::ContentPublished, "u1");
    bad_topic.topic = n::global_topic(63);
    bad_topic.params = params;
    EXPECT_EQ(service(repo).publish(db(), bad_topic, now()).code(), ErrorCode::ValidationFailed);

    n::PublishRequest bad_template = request_of(Topic::ContentPublished,
                                                Template::ContentPublished, "u2");
    bad_template.tpl = 200;
    bad_template.params = params;
    EXPECT_EQ(service(repo).publish(db(), bad_template, now()).code(),
              ErrorCode::ValidationFailed);
}

TEST_F(PublishDb, CoalescingInsideATransactionIsRefusedRatherThanAborted) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 2> params{n::Param::of('t', "Applications"),
                                         n::Param::of('n', std::int64_t{1})};
    n::PublishRequest request = request_of(Topic::FormSubmitted, Template::FormSubmitted,
                                           "txn-coalesce", anvil::uuid::generate_v7());
    request.params = params;

    mongocxx::client_session session = db().start_session();
    // The upsert's whole purpose is that two concurrent publishes land on one
    // row. Inside a transaction the second is a write conflict that would abort
    // the caller's transaction — so the notification takes the row it is about
    // down with it.
    EXPECT_EQ(service(repo).publish(db(), session, request, now()).code(),
              ErrorCode::ValidationFailed);
}

// --- the outbox --------------------------------------------------------------

TEST_F(PublishDb, ATransactionalPublishIsNotDispatchedUntilTheCallerCommits) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "Firefox on Linux");
    n::PublishRequest request = request_of(Topic::SessionNewDevice,
                                           Template::SessionNewDevice, "txn-1", account);
    request.params = params;

    mongocxx::client_session session = db().start_session();
    session.start_transaction();
    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), session, request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());
    session.commit_transaction();

    // A transport enqueued for a transaction that then aborted is a notification
    // about something that never happened, so the tail has not run.
    EXPECT_TRUE(log_.enqueued.empty());
    EXPECT_EQ(inbox_size(account), 0U);

    // After the commit the caller — or the sweeper — runs it.
    const anvil::Result<std::int64_t> delivered =
        service(repo).dispatch(db(), published.value().id, now());
    ASSERT_TRUE(delivered.ok()) << static_cast<int>(delivered.code());
    EXPECT_EQ(delivered.value(), 1);
    EXPECT_EQ(inbox_size(account), 1U);
}

TEST_F(PublishDb, TheSweeperFinishesADispatchThatDiedAfterTheCommit) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();

    // A row committed by a process that died before it dispatched: exactly what
    // the transactional overload leaves behind.
    const std::array<n::Param, 1> params = one_text('d', "Firefox on Linux");
    n::PublishRequest request = request_of(Topic::SessionNewDevice,
                                           Template::SessionNewDevice, "stranded", account);
    request.params = params;

    mongocxx::client_session session = db().start_session();
    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), session, request, now());
    ASSERT_TRUE(published.ok());
    EXPECT_EQ(inbox_size(account), 0U);

    // The sweeper running LATER, rather than a row back-dated to look old. The
    // grace is measured against the row's UUIDv7 `_id`, which is minted from the
    // wall clock — so a back-dated `now` would move `created_at` and leave the id
    // where it was, and the test would be asserting against a row shaped like
    // nothing the code can produce.
    const anvil::Result<std::size_t> swept =
        service(repo).sweep_outbox(db(), now() + std::chrono::minutes{10}, 16);
    ASSERT_TRUE(swept.ok()) << static_cast<int>(swept.code());
    EXPECT_EQ(swept.value(), 1U);
    EXPECT_EQ(inbox_size(account), 1U);
    ASSERT_EQ(log_.enqueued.size(), 1U);
    EXPECT_EQ(log_.enqueued.front().first, published.value().id);
}

TEST_F(PublishDb, TheSweeperLeavesAnInFlightPublishAlone) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "Firefox on Linux");
    n::PublishRequest request = request_of(Topic::SessionNewDevice,
                                           Template::SessionNewDevice, "in-flight", account);
    request.params = params;

    mongocxx::client_session session = db().start_session();
    ASSERT_TRUE(service(repo).publish(db(), session, request, now()).ok());

    // Committed three instructions ago and about to dispatch itself. Sweeping it
    // now would duplicate the work of every in-flight publish in the deployment.
    const anvil::Result<std::size_t> swept = service(repo).sweep_outbox(db(), now(), 16);
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 0U);
    EXPECT_EQ(inbox_size(account), 0U);
}

TEST_F(PublishDb, ARerunDispatchWritesNoSecondInboxRow) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "Firefox on Linux");
    n::PublishRequest request = request_of(Topic::SessionNewDevice,
                                           Template::SessionNewDevice, "rerun", account);
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok());
    EXPECT_EQ(published.value().delivered, 1);

    // The sweeper re-running a dispatch it could not prove had finished. The
    // `{uid, nid}` unique index is what makes that safe rather than merely
    // tolerable — this is the assertion that it is.
    const anvil::Result<std::int64_t> again =
        service(repo).dispatch(db(), published.value().id, now());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), 0);
    EXPECT_EQ(inbox_size(account), 1U);
}

TEST_F(PublishDb, AReplayedDispatchDoesNotWidenTheChannelsThePublishNarrowed) {
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "Firefox on Linux");

    n::PublishRequest request = request_of(Topic::SessionNewDevice,
                                           Template::SessionNewDevice, "narrow", account);
    request.params = params;
    // Deliberately NOT the topic's default, which also declares Email. A sign-in
    // alert about an address the attacker may already control must not be mailed
    // to it.
    request.channels = n::channels(n::ClientType::InApp);

    mongocxx::client_session session = db().start_session();
    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), session, request, now());
    ASSERT_TRUE(published.ok());

    ASSERT_TRUE(
        service(repo).sweep_outbox(db(), now() + std::chrono::minutes{10}, 16).ok());
    ASSERT_EQ(log_.enqueued.size(), 1U);
    // Read back off the row, not recomputed from the topic table: the sweeper is
    // a different process and the row is the only place the decision is recorded.
    EXPECT_EQ(log_.enqueued.front().second, n::channels(n::ClientType::InApp));
}

TEST_F(PublishDb, AFailedEnqueueLeavesTheRowInTheOutbox) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "enqueue-fails");
    request.params = params;

    n::PublishHooks failing{};
    failing.enqueue_transports = [](const Uuid&, n::ChannelMask) {
        return anvil::fail(ErrorCode::ServiceUnavailable, "redis");
    };

    const n::PublishService service{repo, kTemplates, std::move(failing)};
    const anvil::Result<n::PublishOutcome> published = service.publish(db(), request, now());
    // The row committed, which is the part that had to be durable. Reporting a
    // failure here would invite a retry the dedupe index would refuse anyway.
    ASSERT_TRUE(published.ok());

    const anvil::Result<std::optional<n::NotificationRow>> row =
        repo.find_notification(db(), published.value().id, now());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());
    // NOT marked: marking work that was never done is how a delivery is lost with
    // no record that it was owed.
    EXPECT_FALSE((*row.value()).dispatched_at.has_value());
}

// --- the dedupe derivation ---------------------------------------------------

TEST_F(PublishDb, TheCoalescingKeyAndTheIdempotencyKeyLiveInDifferentDomains) {
    const n::TopicSpec* coalescing = n::topic_spec(kTopics, code_of(Topic::FormSubmitted));
    const n::TopicSpec* plain = n::topic_spec(kTopics, code_of(Topic::FormAccepted));
    ASSERT_NE(coalescing, nullptr);
    ASSERT_NE(plain, nullptr);

    const Uuid subject = anvil::uuid::generate_v7();
    const anvil::db::TimeMs at = now();
    // Same subject, same template, same everything a caller controls. The domain
    // prefix is what stops one derivation colliding with the other and
    // suppressing a notification that should have been written.
    EXPECT_NE(n::PublishService::dedupe_key(*coalescing, n::scoped_topic(coalescing->code,
                                                                         subject),
                                            id_of(Template::FormSubmitted), "k", at),
              n::PublishService::dedupe_key(*plain, n::scoped_topic(plain->code, subject),
                                            id_of(Template::FormSubmitted), "k", at));
}

TEST_F(PublishDb, ACoalescingKeyIsStableInsideItsWindowAndChangesAcrossIt) {
    const n::TopicSpec* spec = n::topic_spec(kTopics, code_of(Topic::FormSubmitted));
    ASSERT_NE(spec, nullptr);
    const Uuid subject = anvil::uuid::generate_v7();
    const n::TopicRef topic = n::scoped_topic(spec->code, subject);
    const n::TemplateId tpl = id_of(Template::FormSubmitted);

    // Anchored to the start of a bucket, so "one second later" is inside the
    // window and "one window later" is not, whatever the wall clock happens to be
    // when the suite runs.
    const std::int64_t window_ms = static_cast<std::int64_t>(spec->coalesce_window_s) * 1000;
    const std::int64_t anchor = (now().time_since_epoch().count() / window_ms) * window_ms;
    const auto at = [](std::int64_t ms) {
        return anvil::db::TimeMs{std::chrono::milliseconds{ms}};
    };

    EXPECT_EQ(n::PublishService::dedupe_key(*spec, topic, tpl, "", at(anchor)),
              n::PublishService::dedupe_key(*spec, topic, tpl, "", at(anchor + 1000)));
    EXPECT_NE(n::PublishService::dedupe_key(*spec, topic, tpl, "", at(anchor)),
              n::PublishService::dedupe_key(*spec, topic, tpl, "", at(anchor + window_ms)));
}

// --- the storm breaker -------------------------------------------------------
//
// Both gates, against a live cluster, because what each one is asserted to leave
// behind is a STATE in the database: nothing at all for gate 1, and a row whose
// `dispatched_at` is unset for gate 2 — which is byte-for-byte what a process
// killed between commit and enqueue leaves, and is why gate 2 needed no recovery
// path of its own.

TEST_F(PublishDb, AnOptionalTopicIsRefusedOutrightAndWritesNothing) {
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");

    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "storm-1");
    request.params = params;

    const anvil::Result<n::PublishOutcome> dropped =
        service_under(repo, 0.90F).publish(db(), request, now());
    ASSERT_FALSE(dropped.ok());
    // "Shed deliberately, may be retried" — distinct from Internal so a client
    // and a circuit breaker can tell a fault from an outage.
    EXPECT_EQ(dropped.code(), ErrorCode::ServiceUnavailable);
    EXPECT_TRUE(log_.enqueued.empty());

    // Nothing was written, proved by the dedupe index rather than by a count:
    // the same idempotency key publishes CLEAN once the pressure is gone. Had
    // gate 1 written a row and then refused, this second call would come back
    // with `created == false` and the first row's id.
    const anvil::Result<n::PublishOutcome> retried =
        service_under(repo, 0.0F).publish(db(), request, now());
    ASSERT_TRUE(retried.ok()) << static_cast<int>(retried.code());
    EXPECT_TRUE(retried.value().created);
}

TEST_F(PublishDb, ATopicTheReaderCannotSilenceSurvivesTheSamePressure) {
    // session.new_device, at a pressure that dropped the topic above. It is
    // committed, and the only thing that happened to it is that its tail was
    // left for the sweeper.
    const n::NotificationRepository repo = repository();
    // No client row: an Account-scoped topic needs none, because the subject IS
    // the reader — which is what stops a security alert being lost to an account
    // that never subscribed to anything.
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "a new phone");

    n::PublishRequest request = request_of(Topic::SessionNewDevice, Template::SessionNewDevice,
                                           "storm-2", account);
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service_under(repo, 0.99F).publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());
    EXPECT_TRUE(published.value().created);
    // Delayed, not delivered. `delivered == 0` here means "not yet", never
    // "nobody" — which is the one reading of this field a caller must not take.
    EXPECT_EQ(published.value().delivered, 0);
    EXPECT_TRUE(log_.enqueued.empty());

    const anvil::Result<std::optional<n::NotificationRow>> row =
        repo.find_notification(db(), published.value().id, now());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());
    EXPECT_FALSE((*row.value()).dispatched_at.has_value());
    EXPECT_EQ(inbox_size(account), 0U);
}

TEST_F(PublishDb, TheSweeperFinishesADeferredDispatchAndDeliversExactlyOnce) {
    // Gate 2's deferral and the crash path reach the same place, which is the
    // whole argument for deferring rather than dropping. `grace` is zero so the
    // sweep does not have to wait out kOutboxGrace.
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "a new phone");

    n::PublishRequest request = request_of(Topic::SessionNewDevice, Template::SessionNewDevice,
                                           "storm-3", account);
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service_under(repo, 0.99F).publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());

    const anvil::Result<std::size_t> swept =
        service(repo).sweep_outbox(db(), now(), 32, std::chrono::seconds{0});
    ASSERT_TRUE(swept.ok());
    EXPECT_EQ(swept.value(), 1U);

    const anvil::Result<std::optional<n::NotificationRow>> row =
        repo.find_notification(db(), published.value().id, now());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());
    EXPECT_TRUE((*row.value()).dispatched_at.has_value());

    // ONE row across both attempts. The tail is idempotent, so the test that
    // matters is not that the sweep ran but that running it after a deferral
    // does not deliver a second time.
    EXPECT_EQ(inbox_size(account), 1U);
    ASSERT_EQ(log_.enqueued.size(), 1U);

    // And a second sweep finds nothing, because the marker is now set.
    const anvil::Result<std::size_t> again =
        service(repo).sweep_outbox(db(), now(), 32, std::chrono::seconds{0});
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), 0U);
    EXPECT_EQ(inbox_size(account), 1U);
}

TEST_F(PublishDb, WithNoProbeNothingIsShedNoMatterWhat) {
    // The configuration every consumer that predates the storm breaker has. No
    // probe means no sample, no verdict and no counter — and the publish path is
    // exactly the one the rest of this file asserts.
    const n::NotificationRepository repo = repository();
    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");

    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "storm-4");
    request.params = params;

    const anvil::Result<n::PublishOutcome> published =
        service(repo).publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());

    const anvil::Result<std::optional<n::NotificationRow>> row =
        repo.find_notification(db(), published.value().id, now());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());
    EXPECT_TRUE((*row.value()).dispatched_at.has_value());
}

TEST_F(PublishDb, AProbeThatThrowsIsNotAReasonToLoseANotification) {
    const n::NotificationRepository repo = repository();
    n::PublishHooks broken = hooks();
    broken.pressure = []() -> float { throw std::runtime_error{"a consumer's probe"}; };
    const n::PublishService publisher{repo, kTemplates, std::move(broken)};

    const std::array<n::Param, 1> params = one_text('t', "Summer Hours");
    n::PublishRequest request = request_of(Topic::ContentPublished,
                                           Template::ContentPublished, "storm-5");
    request.params = params;

    const anvil::Result<n::PublishOutcome> published = publisher.publish(db(), request, now());
    ASSERT_TRUE(published.ok()) << static_cast<int>(published.code());
    const anvil::Result<std::optional<n::NotificationRow>> row =
        repo.find_notification(db(), published.value().id, now());
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());
    EXPECT_TRUE((*row.value()).dispatched_at.has_value());
}

TEST_F(PublishDb, TheGaugeMeasuresTheQueueTheSweeperActuallyWalks) {
    // The backlog gate 2 creates, which is the failure mode the design
    // introduces and is invisible without this number. Asserted through
    // count_undispatched, because that IS what the gauge samples — reading the
    // cell back would assert the metric registry rather than the query.
    const n::NotificationRepository repo = repository();
    const Uuid account = anvil::uuid::generate_v7();
    const std::array<n::Param, 1> params = one_text('d', "a new phone");

    for (int i = 0; i < 3; ++i) {
        n::PublishRequest request =
            request_of(Topic::SessionNewDevice, Template::SessionNewDevice,
                       i == 0 ? "depth-0" : (i == 1 ? "depth-1" : "depth-2"), account);
        request.params = params;
        ASSERT_TRUE(service_under(repo, 0.99F).publish(db(), request, now()).ok());
    }

    const anvil::Result<std::int64_t> depth =
        repo.count_undispatched(db(), now(), n::kOutboxDepthCap);
    ASSERT_TRUE(depth.ok());
    EXPECT_EQ(depth.value(), 3);

    // Capped, so measuring the backlog cannot become the expensive thing a
    // growing backlog does to the deployment.
    const anvil::Result<std::int64_t> capped = repo.count_undispatched(db(), now(), 2);
    ASSERT_TRUE(capped.ok());
    EXPECT_EQ(capped.value(), 2);

    // The sampler itself reaches the registry, which is absent in this process —
    // a no-op by design, so what is asserted here is that it neither throws nor
    // reports a failure of the read behind it.
    EXPECT_TRUE(service(repo).sample_outbox_depth(db(), now(), std::chrono::seconds{0}).ok());

    // And once the sweeper has drained it the number is zero, which is what makes
    // a non-zero reading mean something.
    ASSERT_TRUE(service(repo).sweep_outbox(db(), now(), 32, std::chrono::seconds{0}).ok());
    const anvil::Result<std::int64_t> drained =
        repo.count_undispatched(db(), now(), n::kOutboxDepthCap);
    ASSERT_TRUE(drained.ok());
    EXPECT_EQ(drained.value(), 0);
}
