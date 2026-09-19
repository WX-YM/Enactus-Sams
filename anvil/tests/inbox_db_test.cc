// The read path against a live cluster.
//
// The merge is the whole subject. Its two halves live in different collections
// with different read-state models, and the only way to assert that a reader sees
// one ordered stream is to put rows in both and read them back through the
// service the way a request would.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/notifications/inbox.h"
#include "anvil/notifications/publish.h"
#include "anvil/notifications/repository.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/topics.h"

namespace {

using anvil::ErrorCode;
using anvil::Locale;
using anvil::Uuid;
using anvil::testfixture::scratch_names;
namespace n = anvil::notifications;

using testapp::kTemplates;
using testapp::kTopics;
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

[[nodiscard]] Locale locale_of(std::string_view tag) {
    const std::optional<Locale> locale = Locale::from_tag(tag);
    EXPECT_TRUE(locale.has_value());
    return locale.value_or(Locale{});
}

// A UUIDv7 carries the millisecond and nothing finer, so two rows that must be
// distinguishable by time have to land in different ones. Waiting for the tick
// establishes that; it measures nothing.
void await_next_millisecond() {
    const std::int64_t at = now().time_since_epoch().count();
    while (now().time_since_epoch().count() == at) { std::this_thread::yield(); }
}

class InboxDb : public ::testing::Test {
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
        visible_ = true;
        probe_calls_ = 0;
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kNotifications)};
    }

    [[nodiscard]] static n::NotificationRepository repository() {
        return n::NotificationRepository{
            database(), n::NotificationCollections{kNotifications, kInbox, kClients}, kTopics};
    }

    [[nodiscard]] static n::PublishService publisher(const n::NotificationRepository& repo) {
        n::PublishHooks hooks{};
        hooks.may_receive = [](mongocxx::client&, const Uuid&,
                               const anvil::PermSet&) -> anvil::Result<bool> { return true; };
        hooks.enqueue_transports = [](const Uuid&, n::ChannelMask) { return anvil::ok(); };
        return n::PublishService{repo, kTemplates, std::move(hooks)};
    }

    [[nodiscard]] n::InboxService reader_service(const n::NotificationRepository& repo) {
        n::InboxHooks hooks{};
        hooks.may_see = [this](mongocxx::client&, const Uuid&,
                               const n::ResourceRef&) -> anvil::Result<bool> {
            ++probe_calls_;
            return visible_;
        };
        return n::InboxService{repo, kTemplates, std::move(hooks)};
    }

    // A client row subscribed to every broadcast topic named, fully enabled.
    [[nodiscard]] Uuid client_for(const Uuid& owner, std::span<const Topic> topics) {
        n::ClientRow row{};
        row.id = anvil::uuid::generate_v4();
        row.owner = owner;
        row.created_at = now();
        row.prefs = n::Preferences::all_enabled();
        row.type = n::ClientType::InApp;
        const Uuid since =
            anvil::uuid::v7_boundary(now().time_since_epoch().count() - 600'000);
        for (const Topic topic : topics) {
            EXPECT_TRUE(row.subs.push({anvil::kNilUuid, since, code_of(topic)}));
        }
        EXPECT_TRUE(repository().insert_client(db(), row).ok());
        return row.id;
    }

    // A broadcast notification on a global topic, published through the service so
    // the row is shaped exactly as production writes it.
    Uuid announce(std::string_view body, std::string_view key) {
        const n::NotificationRepository repo = repository();
        const std::array<n::Param, 1> params{n::Param::of('b', body)};
        n::PublishRequest request{};
        request.topic = n::global_topic(code_of(Topic::SystemAnnouncement));
        request.idempotency_key = key;
        request.tpl = id_of(Template::Announcement);
        request.params = params;
        const anvil::Result<n::PublishOutcome> out = publisher(repo).publish(db(), request, now());
        EXPECT_TRUE(out.ok()) << static_cast<int>(out.code());
        return out.ok() ? out.value().id : Uuid{};
    }

    // A targeted notification to one account, with an optional resource ref.
    Uuid notify(const Uuid& account, std::string_view key,
                const std::optional<n::ResourceRef>& ref = {}) {
        const n::NotificationRepository repo = repository();
        const std::array<n::Param, 1> params{n::Param::of('d', "Firefox on Linux")};
        n::PublishRequest request{};
        request.topic = n::scoped_topic(code_of(Topic::SessionNewDevice), account);
        request.idempotency_key = key;
        request.tpl = id_of(Template::SessionNewDevice);
        request.params = params;
        request.ref = ref;
        const anvil::Result<n::PublishOutcome> out = publisher(repo).publish(db(), request, now());
        EXPECT_TRUE(out.ok()) << static_cast<int>(out.code());
        return out.ok() ? out.value().id : Uuid{};
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
    bool                                   visible_{true};
    int                                    probe_calls_{0};
};

}  // namespace

// --- the merge ---------------------------------------------------------------

TEST_F(InboxDb, AMergedPageIsNewestFirstAcrossBothHalves) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(client_for(reader, std::array{Topic::SystemAnnouncement}));

    const Uuid first = announce("Opening hours change", "a1");
    await_next_millisecond();
    const Uuid second = notify(reader, "s1");
    await_next_millisecond();
    const Uuid third = announce("Closing early", "a2");

    const anvil::Result<n::InboxPageView> page =
        reader_service(repo).page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
    ASSERT_EQ(page.value().entries.size(), 3U);
    // A reader does not know which collection a notification came from, and the
    // order must not tell them.
    EXPECT_EQ(page.value().entries[0].notification, third);
    EXPECT_EQ(page.value().entries[1].notification, second);
    EXPECT_EQ(page.value().entries[2].notification, first);
}

TEST_F(InboxDb, AReaderWithNoClientRowStillSeesTheTargetedHalf) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    // No client row at all, so no subscriptions and no broadcast half. That must
    // not mean no notifications: a security alert reaches an account that has
    // never registered anything.
    static_cast<void>(announce("Nobody sees this", "a1"));
    const Uuid targeted = notify(reader, "s1");

    const anvil::Result<n::InboxPageView> page =
        reader_service(repo).page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().entries.size(), 1U);
    EXPECT_EQ(page.value().entries[0].notification, targeted);
}

TEST_F(InboxDb, TheCursorWalksBothHalvesWithoutRepeatingOrSkipping) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(client_for(reader, std::array{Topic::SystemAnnouncement}));

    // Alternating halves, so a cursor that only bounded one of them would either
    // repeat or skip at every boundary.
    std::vector<Uuid> order;
    for (int i = 0; i < 3; ++i) {
        order.push_back(announce("Announcement", "a" + std::to_string(i)));
        await_next_millisecond();
        order.push_back(notify(reader, "s" + std::to_string(i)));
        await_next_millisecond();
    }
    std::reverse(order.begin(), order.end());

    std::vector<Uuid> seen;
    std::optional<Uuid> cursor;
    for (int guard = 0; guard < 10; ++guard) {
        const anvil::Result<n::InboxPageView> page =
            reader_service(repo).page(db(), reader, locale_of("en"), cursor, 2, now());
        ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
        for (const n::InboxEntry& entry : page.value().entries) {
            seen.push_back(entry.notification);
        }
        if (!page.value().next_cursor.has_value()) { break; }
        cursor = page.value().next_cursor;
    }
    EXPECT_EQ(seen, order);
}

// --- rendering ---------------------------------------------------------------

TEST_F(InboxDb, AnEntryIsRenderedInTheReadersCurrentLocale) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(notify(reader, "s1"));

    const anvil::Result<n::InboxPageView> english =
        reader_service(repo).page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(english.ok());
    ASSERT_EQ(english.value().entries.size(), 1U);
    EXPECT_EQ(english.value().entries[0].content.title, "New sign-in");
    EXPECT_EQ(english.value().entries[0].content.body, "A new sign-in from Firefox on Linux");

    // The SAME stored row. The language is the reader's current one, not the one
    // they had when it was sent — which is the whole reason a row stores a
    // template id and parameters instead of two strings.
    const anvil::Result<n::InboxPageView> arabic =
        reader_service(repo).page(db(), reader, locale_of("ar"), std::nullopt, 10, now());
    ASSERT_TRUE(arabic.ok());
    ASSERT_EQ(arabic.value().entries.size(), 1U);
    EXPECT_EQ(arabic.value().entries[0].content.title, "تسجيل دخول جديد");
    EXPECT_EQ(arabic.value().entries[0].content.body, "تسجيل دخول جديد من Firefox on Linux");
}

TEST_F(InboxDb, ACoalescedEntryRendersItsCurrentCountAndNotTheStoredOne) {
    const n::NotificationRepository repo = repository();
    const Uuid form = anvil::uuid::generate_v7();
    const Uuid staff = anvil::uuid::generate_v7();

    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.owner = staff;
    row.created_at = now();
    row.prefs = n::Preferences::all_enabled();
    row.type = n::ClientType::InApp;
    const Uuid since = anvil::uuid::v7_boundary(now().time_since_epoch().count() - 600'000);
    ASSERT_TRUE(row.subs.push({form, since, code_of(Topic::FormSubmitted)}));
    ASSERT_TRUE(repo.insert_client(db(), row).ok());

    // Three submissions inside the window. The upsert's $setOnInsert keeps the
    // FIRST publish's parameters, so a count stored as a parameter would say 1
    // forever — which is the sentence coalescing exists to avoid.
    const std::array<n::Param, 2> params{n::Param::of('t', "Applications"),
                                         n::Param::of('n', std::int64_t{1})};
    for (int i = 0; i < 3; ++i) {
        n::PublishRequest request{};
        request.topic = n::scoped_topic(code_of(Topic::FormSubmitted), form);
        request.tpl = id_of(Template::FormSubmitted);
        request.params = params;
        ASSERT_TRUE(publisher(repo).publish(db(), request, now()).ok());
    }

    const anvil::Result<n::InboxPageView> page =
        reader_service(repo).page(db(), staff, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
    ASSERT_EQ(page.value().entries.size(), 1U);
    EXPECT_EQ(page.value().entries[0].count, 3);
    EXPECT_EQ(page.value().entries[0].content.body, "3 new submissions on Applications");
}

// --- suppression -------------------------------------------------------------

TEST_F(InboxDb, ASuppressedTargetedEntryIsHiddenAndItsRowIsForgotten) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    const n::ResourceRef ref{anvil::uuid::generate_v7(), 7};
    static_cast<void>(notify(reader, "s1", ref));
    ASSERT_EQ(repo.page_inbox(db(), reader, {}, 10, now()).value().rows.size(), 1U);

    visible_ = false;
    const anvil::Result<n::InboxPageView> page =
        reader_service(repo).page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
    // Hidden ENTIRELY. A redacted placeholder still discloses that something
    // existed, and for a staff-facing topic the existence is the secret.
    EXPECT_TRUE(page.value().entries.empty());
    EXPECT_EQ(probe_calls_, 1);

    // And the row is gone: one that will never be visible again is dead weight in
    // the index the unread count rides.
    EXPECT_EQ(repo.page_inbox(db(), reader, {}, 10, now()).value().rows.size(), 0U);
}

TEST_F(InboxDb, ASuppressedBroadcastEntryIsHiddenButItsSharedRowSurvives) {
    const n::NotificationRepository repo = repository();
    const Uuid one = anvil::uuid::generate_v7();
    const Uuid two = anvil::uuid::generate_v7();
    static_cast<void>(client_for(one, std::array{Topic::SystemAnnouncement}));
    static_cast<void>(client_for(two, std::array{Topic::SystemAnnouncement}));

    // A broadcast row with a ref, so the probe has something to refuse.
    const n::NotificationRepository writer = repository();
    const std::array<n::Param, 1> params{n::Param::of('b', "Members only")};
    n::PublishRequest request{};
    request.topic = n::global_topic(code_of(Topic::SystemAnnouncement));
    request.idempotency_key = "a1";
    request.tpl = id_of(Template::Announcement);
    request.params = params;
    request.ref = n::ResourceRef{anvil::uuid::generate_v7(), 3};
    const anvil::Result<n::PublishOutcome> published =
        publisher(writer).publish(db(), request, now());
    ASSERT_TRUE(published.ok());

    visible_ = false;
    EXPECT_TRUE(reader_service(repo)
                    .page(db(), one, locale_of("en"), std::nullopt, 10, now())
                    .value()
                    .entries.empty());

    // ONE row for everybody. Deleting it because one reader may not see it would
    // take it away from every other reader who may.
    visible_ = true;
    const anvil::Result<n::InboxPageView> other =
        reader_service(repo).page(db(), two, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(other.ok());
    ASSERT_EQ(other.value().entries.size(), 1U);
    EXPECT_EQ(other.value().entries[0].notification, published.value().id);
}

TEST_F(InboxDb, ARefWithNoProbeToAskIsHiddenRatherThanShown) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(notify(reader, "s1", n::ResourceRef{anvil::uuid::generate_v7(), 7}));
    // And one with no ref at all, which skips the check entirely.
    await_next_millisecond();
    const Uuid plain = notify(reader, "s2");

    // An application that declares a resource-referencing topic and supplies
    // nothing to ask. Showing what nobody checked cannot be walked back.
    const n::InboxService service{repo, kTemplates, n::InboxHooks{}};
    const anvil::Result<n::InboxPageView> page =
        service.page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().entries.size(), 1U);
    EXPECT_EQ(page.value().entries[0].notification, plain);
}

// --- preferences -------------------------------------------------------------

TEST_F(InboxDb, AMutedBroadcastTopicLeavesThePageAndTheCount) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();

    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.owner = reader;
    row.created_at = now();
    row.prefs = n::Preferences::all_enabled();
    row.type = n::ClientType::InApp;
    const Uuid since = anvil::uuid::v7_boundary(now().time_since_epoch().count() - 600'000);
    ASSERT_TRUE(row.subs.push({anvil::kNilUuid, since, code_of(Topic::SystemAnnouncement)}));
    ASSERT_TRUE(repo.insert_client(db(), row).ok());

    static_cast<void>(announce("Opening hours change", "a1"));
    ASSERT_EQ(reader_service(repo)
                  .page(db(), reader, locale_of("en"), std::nullopt, 10, now())
                  .value()
                  .entries.size(),
              1U);

    // The broadcast half writes no per-reader row, so there was never a moment at
    // which a preference could be applied on the way in. If it does not take
    // effect at read, muting a broadcast topic does nothing whatsoever.
    n::Preferences muted = n::Preferences::all_enabled();
    muted.set(code_of(Topic::SystemAnnouncement), n::ClientType::InApp, false);
    ASSERT_TRUE(repo.replace_preferences(db(), row.id, 1, muted).ok());

    const anvil::Result<n::InboxPageView> page =
        reader_service(repo).page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(page.value().entries.empty());
    EXPECT_EQ(reader_service(repo).unread(db(), reader, now()).value(), 0);
}

// --- read state --------------------------------------------------------------

TEST_F(InboxDb, MarkingReadClearsBothHalvesWithOneWatermark) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(client_for(reader, std::array{Topic::SystemAnnouncement}));

    static_cast<void>(announce("Opening hours change", "a1"));
    await_next_millisecond();
    static_cast<void>(notify(reader, "s1"));

    const n::InboxService service = reader_service(repo);
    ASSERT_EQ(service.unread(db(), reader, now()).value(), 2);

    const anvil::Result<n::InboxPageView> page =
        service.page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().entries.size(), 2U);
    // The highest id the client actually rendered — never a server-side `now`,
    // which would bury anything that arrived since.
    const Uuid seen = page.value().entries.front().id;

    ASSERT_TRUE(service.mark_read(db(), reader, seen, now()).ok());
    // Advancing one half and not the other leaves a badge that will not clear and
    // no way for the reader to find what is keeping it lit.
    EXPECT_EQ(service.unread(db(), reader, now()).value(), 0);

    const anvil::Result<n::InboxPageView> after =
        service.page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(after.ok());
    ASSERT_EQ(after.value().entries.size(), 2U);
    EXPECT_TRUE(after.value().entries[0].read);
    EXPECT_TRUE(after.value().entries[1].read);
}

TEST_F(InboxDb, AnythingThatArrivedAfterTheClientsMarkerStaysUnread) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(client_for(reader, std::array{Topic::SystemAnnouncement}));

    static_cast<void>(announce("Read this", "a1"));
    await_next_millisecond();

    const n::InboxService service = reader_service(repo);
    const anvil::Result<n::InboxPageView> page =
        service.page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().entries.size(), 1U);
    const Uuid seen = page.value().entries.front().id;

    // Arrived between the client's render and its mark-read request.
    static_cast<void>(announce("Do not bury this", "a2"));
    await_next_millisecond();
    static_cast<void>(notify(reader, "s1"));

    ASSERT_TRUE(service.mark_read(db(), reader, seen, now()).ok());
    EXPECT_EQ(service.unread(db(), reader, now()).value(), 2);
}

TEST_F(InboxDb, TheWatermarkCannotBeWalkedBackwards) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    static_cast<void>(client_for(reader, std::array{Topic::SystemAnnouncement}));

    static_cast<void>(announce("First", "a1"));
    await_next_millisecond();
    static_cast<void>(announce("Second", "a2"));

    const n::InboxService service = reader_service(repo);
    const anvil::Result<n::InboxPageView> page =
        service.page(db(), reader, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().entries.size(), 2U);
    const Uuid newest = page.value().entries[0].id;
    const Uuid oldest = page.value().entries[1].id;

    ASSERT_TRUE(service.mark_read(db(), reader, newest, now()).ok());
    EXPECT_EQ(service.unread(db(), reader, now()).value(), 0);

    // A stale request carrying the older marker. `$max` is what keeps it from
    // silently un-reading everything between the two.
    ASSERT_TRUE(service.mark_read(db(), reader, oldest, now()).ok());
    EXPECT_EQ(service.unread(db(), reader, now()).value(), 0);
}

TEST_F(InboxDb, MarkingSpecificIdsTouchesOnlyTheReadersOwnRows) {
    const n::NotificationRepository repo = repository();
    const Uuid reader = anvil::uuid::generate_v7();
    const Uuid stranger = anvil::uuid::generate_v7();
    static_cast<void>(notify(reader, "s1"));
    static_cast<void>(notify(stranger, "s2"));

    const n::InboxService service = reader_service(repo);
    const anvil::Result<n::InboxPageView> theirs =
        service.page(db(), stranger, locale_of("en"), std::nullopt, 10, now());
    ASSERT_TRUE(theirs.ok());
    ASSERT_EQ(theirs.value().entries.size(), 1U);

    // A client-supplied list naming somebody else's row. The filter carries the
    // reader's own id, so it matches nothing.
    const std::array<Uuid, 1> other{theirs.value().entries.front().id};
    const anvil::Result<std::int64_t> marked = service.mark_read(db(), reader, other, now());
    ASSERT_TRUE(marked.ok());
    EXPECT_EQ(marked.value(), 0);
    EXPECT_EQ(service.unread(db(), stranger, now()).value(), 1);
}
