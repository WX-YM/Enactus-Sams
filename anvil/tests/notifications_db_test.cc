// The notification collections against a live cluster.
//
// These cases cannot be unit tests. What they assert is a property of the SERVER
// — that a unique sparse index turns a retried publish into a no-op, that an
// upsert with `$inc` coalesces two concurrent publishes into one row with a count
// of two, that an unordered insert_many survives a re-run whose rows already
// exist, that `$max` on a watermark cannot be walked backwards — and none of that
// is observable without one.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "anvil/core/uuid.h"
#include "anvil/notifications/repository.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/topics.h"

namespace {

using anvil::ErrorCode;
using anvil::Uuid;
using anvil::testfixture::scratch_names;
namespace n = anvil::notifications;

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

// A detector rather than a comment. The point of the webhook listing being a
// projection and not a ClientRow is that it has NOWHERE to carry the sealed
// secret — and a concept is the only way to assert the absence of a member,
// because a requires-expression over a concrete type is a hard error rather than
// a false.
template <typename T>
concept CarriesSealedKeys = requires(const T& row) { row.keys; };

[[nodiscard]] anvil::db::TimeMs in_days(int days) {
    return now() + std::chrono::hours{24 * days};
}

// A UUIDv7 carries the millisecond and nothing finer, so two rows that have to be
// distinguishable BY TIME must land in different ones. Within a millisecond a v7
// orders by its 74 random bits, which is a coin flip.
//
// Waiting for the clock to tick establishes that precondition; it measures
// nothing. Without it a test passes under ASan — where a round trip comfortably
// exceeds a millisecond — and fails under `dist`, where three inserts finish
// inside one.
void await_next_millisecond() {
    const std::int64_t at = now().time_since_epoch().count();
    while (now().time_since_epoch().count() == at) { std::this_thread::yield(); }
}

[[nodiscard]] std::array<std::uint8_t, 16> dedupe_of(std::uint8_t seed) {
    std::array<std::uint8_t, 16> key{};
    key[0] = seed;
    key[15] = static_cast<std::uint8_t>(seed + 1);
    return key;
}

class NotificationDb : public ::testing::Test {
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
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kNotifications)};
    }

    [[nodiscard]] static n::NotificationRepository repository() {
        return n::NotificationRepository{
            database(), n::NotificationCollections{kNotifications, kInbox, kClients}, kTopics};
    }

    // A canonical row, ready to insert. The dedupe key is the caller's, always:
    // every queue in this system is at-least-once, so a retried publish WILL
    // happen and without one the reader receives the notification twice.
    [[nodiscard]] static n::NotificationRow row_of(Topic topic, std::uint8_t dedupe_seed,
                                                   const std::optional<Uuid>& subject = {}) {
        n::NotificationRow row{};
        row.id = anvil::uuid::generate_v7();
        row.subject = subject;
        row.created_at = now();
        row.expires_at = in_days(90);
        row.dedupe = dedupe_of(dedupe_seed);
        row.count = 1;
        row.kind = code_of(topic);
        row.tpl = id_of(Template::ContentPublished);

        n::StoredParam param{};
        param.name = 't';
        param.type = n::ParamType::Text;
        param.text = "Summer Hours";
        EXPECT_TRUE(row.params.push(std::move(param)));
        return row;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

}  // namespace

// --- notifications -----------------------------------------------------------

TEST_F(NotificationDb, ARowRoundTripsWithItsParametersAndItsRef) {
    n::NotificationRow row = row_of(Topic::ContentPublished, 1);
    row.actor = anvil::uuid::generate_v7();
    row.ref = n::ResourceRef{anvil::uuid::generate_v7(), 7};

    n::StoredParam number{};
    number.name = 'n';
    number.type = n::ParamType::Number;
    number.number = -42;
    ASSERT_TRUE(row.params.push(std::move(number)));

    ASSERT_TRUE(repository().insert_notification(db(), row).ok());

    const anvil::Result<std::optional<n::NotificationRow>> read =
        repository().find_notification(db(), row.id, now());
    ASSERT_TRUE(read.ok()) << static_cast<int>(read.code());
    ASSERT_TRUE(read.value().has_value());
    const n::NotificationRow& back = *read.value();

    EXPECT_EQ(back.kind, row.kind);
    EXPECT_EQ(back.tpl, row.tpl);
    EXPECT_EQ(back.count, 1);
    EXPECT_EQ(back.actor, row.actor);
    ASSERT_TRUE(back.ref.has_value());
    // The resource KIND is the application's numbering and anvil stores it
    // without interpreting it.
    EXPECT_EQ(back.ref->kind, 7);
    EXPECT_EQ(back.ref->id, row.ref->id);

    ASSERT_EQ(back.params.count, 2U);
    EXPECT_EQ(back.params.items[0].name, 't');
    EXPECT_EQ(back.params.items[0].text, "Summer Hours");
    // int64 on the way out because int64 went in. The decoder refuses to coerce.
    EXPECT_EQ(back.params.items[1].type, n::ParamType::Number);
    EXPECT_EQ(back.params.items[1].number, -42);
}

TEST_F(NotificationDb, ARetriedPublishIsANoOpRatherThanASecondNotification) {
    const n::NotificationRow first = row_of(Topic::ContentPublished, 2);
    ASSERT_TRUE(repository().insert_notification(db(), first).ok());

    // The SAME dedupe key, a different id: exactly what an at-least-once queue
    // produces when it redelivers the job that published the first one.
    const n::NotificationRow retry = row_of(Topic::ContentPublished, 2);
    EXPECT_EQ(repository().insert_notification(db(), retry).code(), ErrorCode::Conflict);

    // And the second one does not exist, so nothing downstream can fan it out.
    const anvil::Result<std::optional<n::NotificationRow>> ghost =
        repository().find_notification(db(), retry.id, now());
    ASSERT_TRUE(ghost.ok());
    EXPECT_FALSE(ghost.value().has_value());
}

TEST_F(NotificationDb, CoalescingMergesRepeatsIntoOneRowWithACount) {
    const n::NotificationRow first = row_of(Topic::FormSubmitted, 3);
    const anvil::Result<n::CoalesceOutcome> one =
        repository().coalesce_notification(db(), first);
    ASSERT_TRUE(one.ok()) << static_cast<int>(one.code());
    EXPECT_TRUE(one.value().created);
    EXPECT_EQ(one.value().count, 1);
    EXPECT_EQ(one.value().id, first.id);

    // A second publish inside the window. One row, count two — "3 new form
    // submissions", not three rows.
    const n::NotificationRow second = row_of(Topic::FormSubmitted, 3);
    const anvil::Result<n::CoalesceOutcome> two =
        repository().coalesce_notification(db(), second);
    ASSERT_TRUE(two.ok());
    // NOT created, which is what stops the fan-out and the transports running
    // again for an event somebody has already been told about.
    EXPECT_FALSE(two.value().created);
    EXPECT_EQ(two.value().count, 2);
    // The id is the FIRST row's: a coalesced repeat is the same event happening
    // again, and the reader's cursor must not move under them.
    EXPECT_EQ(two.value().id, first.id);

    // The merged row keeps the first publish's parameters. Rewriting them would
    // make the row describe only the last repeat.
    const anvil::Result<std::optional<n::NotificationRow>> merged =
        repository().find_notification(db(), first.id, now());
    ASSERT_TRUE(merged.ok());
    ASSERT_TRUE(merged.value().has_value());
    EXPECT_EQ(merged.value()->count, 2);
    EXPECT_EQ(merged.value()->params.items[0].text, "Summer Hours");
}

TEST_F(NotificationDb, AnExpiredNotificationIsAbsentBeforeTheTtlMonitorReachesIt) {
    // The monitor runs roughly every 60 seconds, so an expired row is still
    // physically present and would still render. Every read filters on the expiry
    // explicitly, which is what makes "expired" mean invisible rather than merely
    // eligible for reaping.
    n::NotificationRow row = row_of(Topic::ContentPublished, 4);
    row.expires_at = now() - std::chrono::seconds{1};
    ASSERT_TRUE(repository().insert_notification(db(), row).ok());

    const anvil::Result<std::optional<n::NotificationRow>> read =
        repository().find_notification(db(), row.id, now());
    ASSERT_TRUE(read.ok());
    EXPECT_FALSE(read.value().has_value());

    // The document really is still there — this is a filter, not a reap.
    const auto raw = db()[database()][std::string{kNotifications}].find_one(
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("_id", anvil::db::codec::uuid_bin(row.id))));
    EXPECT_TRUE(raw.has_value());
}

TEST_F(NotificationDb, TheOutboxSweepFindsOnlyCommittedUndispatchedRows) {
    // The window a process killed between commit and enqueue leaves open.
    const n::NotificationRow stranded = row_of(Topic::ContentPublished, 5);
    ASSERT_TRUE(repository().insert_notification(db(), stranded).ok());

    const n::NotificationRow delivered = row_of(Topic::ContentPublished, 6);
    ASSERT_TRUE(repository().insert_notification(db(), delivered).ok());
    ASSERT_TRUE(repository().mark_dispatched(db(), delivered.id, now()).ok());

    // `older_than` in the future, so both rows are old enough to be swept; the
    // grace exists so a real sweeper does not race a publish about to enqueue its
    // own transports.
    const anvil::Result<std::vector<Uuid>> found =
        repository().undispatched(db(), in_days(1), 10);
    ASSERT_TRUE(found.ok()) << static_cast<int>(found.code());
    ASSERT_EQ(found.value().size(), 1U);
    EXPECT_EQ(found.value()[0], stranded.id);

    // The grace really does bound it: nothing published after the cutoff is swept.
    const anvil::Result<std::vector<Uuid>> none =
        repository().undispatched(db(), now() - std::chrono::hours{1}, 10);
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());
}

// --- fan-out and the inbox ---------------------------------------------------

TEST_F(NotificationDb, APartiallyCompletedFanOutIsSafeToReRun) {
    const n::NotificationRow row = row_of(Topic::FormAccepted, 7);
    ASSERT_TRUE(repository().insert_notification(db(), row).ok());

    const std::array<Uuid, 3> readers{anvil::uuid::generate_v7(), anvil::uuid::generate_v7(),
                                      anvil::uuid::generate_v7()};
    const anvil::Result<std::int64_t> first =
        repository().fan_out(db(), row.id, row.kind, readers, row.expires_at);
    ASSERT_TRUE(first.ok()) << static_cast<int>(first.code());
    EXPECT_EQ(first.value(), 3);

    // Exactly what the outbox sweeper does after a crash. The {uid, nid} unique
    // index absorbs the duplicates and the batch is unordered, so the rows that
    // were missing are still written.
    const std::array<Uuid, 4> again{readers[0], readers[1], readers[2],
                                    anvil::uuid::generate_v7()};
    const anvil::Result<std::int64_t> second =
        repository().fan_out(db(), row.id, row.kind, again, row.expires_at);
    ASSERT_TRUE(second.ok()) << static_cast<int>(second.code());
    EXPECT_EQ(second.value(), 1);

    // And the first reader has ONE row, not two.
    const anvil::Result<n::InboxPage> page =
        repository().page_inbox(db(), readers[0], std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page.value().rows.size(), 1U);
}

TEST_F(NotificationDb, TheInboxPagesByCursorAndNeverSkips) {
    const Uuid reader = anvil::uuid::generate_v7();
    std::vector<Uuid> ids;
    for (int i = 0; i < 5; ++i) {
        const n::NotificationRow row = row_of(Topic::FormAccepted, static_cast<std::uint8_t>(10 + i));
        ASSERT_TRUE(repository().insert_notification(db(), row).ok());
        const std::array<Uuid, 1> one{reader};
        ASSERT_TRUE(repository().fan_out(db(), row.id, row.kind, one, row.expires_at).ok());
        ids.push_back(row.id);
    }

    // The page walks the INBOX row's `_id`, which is minted at fan-out and is
    // not the notification id the page reports — and two v7 ids minted in one
    // millisecond order by their random tails (core/uuid.h). So insertion order
    // is not assertable here, and it was never the property: what a cursor over
    // an indexed key buys, and what skip(n) loses under concurrent inserts, is
    // that every row appears exactly once across the pages.
    std::vector<Uuid> seen;

    const anvil::Result<n::InboxPage> first =
        repository().page_inbox(db(), reader, std::nullopt, 2, now());
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().rows.size(), 2U);
    ASSERT_TRUE(first.value().next_cursor.has_value());
    for (const n::InboxRow& row : first.value().rows) { seen.push_back(row.nid); }

    const anvil::Result<n::InboxPage> second =
        repository().page_inbox(db(), reader, first.value().next_cursor, 2, now());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().rows.size(), 2U);
    ASSERT_TRUE(second.value().next_cursor.has_value());
    for (const n::InboxRow& row : second.value().rows) { seen.push_back(row.nid); }

    const anvil::Result<n::InboxPage> third =
        repository().page_inbox(db(), reader, second.value().next_cursor, 2, now());
    ASSERT_TRUE(third.ok());
    ASSERT_EQ(third.value().rows.size(), 1U);
    EXPECT_FALSE(third.value().next_cursor.has_value());
    seen.push_back(third.value().rows.front().nid);

    std::sort(seen.begin(), seen.end());
    std::vector<Uuid> expected = ids;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(seen, expected);
}

TEST_F(NotificationDb, MarkingReadTakesAWatermarkAndLeavesLaterRowsUnread) {
    const Uuid reader = anvil::uuid::generate_v7();
    std::vector<Uuid> inbox_ids;
    for (int i = 0; i < 3; ++i) {
        const n::NotificationRow row = row_of(Topic::FormAccepted, static_cast<std::uint8_t>(20 + i));
        ASSERT_TRUE(repository().insert_notification(db(), row).ok());
        const std::array<Uuid, 1> one{reader};
        ASSERT_TRUE(repository().fan_out(db(), row.id, row.kind, one, row.expires_at).ok());
    }

    const anvil::Result<n::InboxPage> before =
        repository().page_inbox(db(), reader, std::nullopt, 10, now());
    ASSERT_TRUE(before.ok());
    ASSERT_EQ(before.value().rows.size(), 3U);
    // Newest first, so index 1 is the middle row.
    const Uuid middle = before.value().rows[1].id;

    const anvil::Result<std::int64_t> marked =
        repository().mark_read_up_to(db(), reader, middle, now());
    ASSERT_TRUE(marked.ok());
    EXPECT_EQ(marked.value(), 2);

    // The client sends the highest id it has actually SEEN. A notification that
    // arrived between the client's render and this request stays unread instead
    // of being silently buried, which is what a server-side `now` would do.
    const anvil::Result<std::int64_t> unread =
        repository().count_targeted_unread(db(), reader, now());
    ASSERT_TRUE(unread.ok());
    EXPECT_EQ(unread.value(), 1);
}

TEST_F(NotificationDb, OneReaderCannotMarkAnothersRowsRead) {
    const Uuid mine = anvil::uuid::generate_v7();
    const Uuid theirs = anvil::uuid::generate_v7();
    const n::NotificationRow row = row_of(Topic::FormAccepted, 30);
    ASSERT_TRUE(repository().insert_notification(db(), row).ok());
    const std::array<Uuid, 1> one{theirs};
    ASSERT_TRUE(repository().fan_out(db(), row.id, row.kind, one, row.expires_at).ok());

    const anvil::Result<n::InboxPage> page =
        repository().page_inbox(db(), theirs, std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 1U);
    const std::array<Uuid, 1> target{page.value().rows[0].id};

    // The reader id is in the FILTER, so this matches nothing rather than
    // matching and being rejected afterwards.
    const anvil::Result<std::int64_t> marked =
        repository().mark_read_ids(db(), mine, target, now());
    ASSERT_TRUE(marked.ok());
    EXPECT_EQ(marked.value(), 0);

    const anvil::Result<std::int64_t> unread =
        repository().count_targeted_unread(db(), theirs, now());
    ASSERT_TRUE(unread.ok());
    EXPECT_EQ(unread.value(), 1);
}

TEST_F(NotificationDb, TheUnreadCountIsCappedRatherThanExact) {
    const Uuid reader = anvil::uuid::generate_v7();
    // More than the cap, so the bound is what answers rather than the backlog.
    for (int i = 0; i < static_cast<int>(n::kUnreadCap) + 10; ++i) {
        const n::NotificationRow row = row_of(Topic::FormAccepted, 40);
        n::NotificationRow unique = row;
        unique.id = anvil::uuid::generate_v7();
        unique.dedupe[1] = static_cast<std::uint8_t>(i);
        ASSERT_TRUE(repository().insert_notification(db(), unique).ok());
        const std::array<Uuid, 1> one{reader};
        ASSERT_TRUE(repository().fan_out(db(), unique.id, unique.kind, one, unique.expires_at)
                        .ok());
    }

    const anvil::Result<std::int64_t> unread =
        repository().count_targeted_unread(db(), reader, now());
    ASSERT_TRUE(unread.ok());
    // One past the cap, so "is it more than 99" is answerable without scanning
    // the rest. A badge shows "99+" anyway.
    EXPECT_EQ(unread.value(), n::kUnreadCountLimit);
}

// --- the broadcast half ------------------------------------------------------

TEST_F(NotificationDb, ABroadcastIsOneRowAndEverySubscriberSeesIt) {
    // The whole reason fan-out is per topic: this insert costs the same whether
    // three readers or twenty thousand are subscribed.
    const n::NotificationRow row = row_of(Topic::ContentPublished, 50);
    ASSERT_TRUE(repository().insert_notification(db(), row).ok());

    n::Subscription sub{};
    sub.kind = code_of(Topic::ContentPublished);
    sub.since = anvil::uuid::v7_boundary(
        (now() - std::chrono::hours{1}).time_since_epoch().count());
    const std::array<n::Subscription, 1> subs{sub};

    const anvil::Result<n::BroadcastPage> page =
        repository().page_broadcast(db(), subs, std::nullopt, 10, now());
    ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
    ASSERT_EQ(page.value().rows.size(), 1U);
    EXPECT_EQ(page.value().rows[0].id, row.id);

    // And no inbox row was written for it at all.
    const anvil::Result<std::int64_t> targeted =
        repository().count_targeted_unread(db(), anvil::uuid::generate_v7(), now());
    ASSERT_TRUE(targeted.ok());
    EXPECT_EQ(targeted.value(), 0);
}

TEST_F(NotificationDb, ANewSubscriberDoesNotSeeTheHistory) {
    const n::NotificationRow old_row = row_of(Topic::ContentPublished, 51);
    ASSERT_TRUE(repository().insert_notification(db(), old_row).ok());

    // Subscribed AFTER it was published. A subscription grants visibility
    // forward, not retroactively — otherwise a first login is three years of
    // history.
    n::Subscription sub{};
    sub.kind = code_of(Topic::ContentPublished);
    const std::int64_t marker_ms = now().time_since_epoch().count();
    sub.since = anvil::uuid::v7_boundary(marker_ms + 1);
    const std::array<n::Subscription, 1> subs{sub};

    // "Published after the marker" is only expressible once the clock has left
    // that millisecond.
    await_next_millisecond();

    const anvil::Result<n::BroadcastPage> page =
        repository().page_broadcast(db(), subs, std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(page.value().rows.empty());

    // Anything published after the marker does arrive.
    const n::NotificationRow fresh = row_of(Topic::ContentPublished, 52);
    ASSERT_TRUE(repository().insert_notification(db(), fresh).ok());
    const anvil::Result<n::BroadcastPage> after =
        repository().page_broadcast(db(), subs, std::nullopt, 10, now());
    ASSERT_TRUE(after.ok());
    ASSERT_EQ(after.value().rows.size(), 1U);
    EXPECT_EQ(after.value().rows[0].id, fresh.id);
}

TEST_F(NotificationDb, ATargetedTopicNeverAppearsInTheBroadcastHalf) {
    // A targeted row was already delivered as an inbox entry. Including it in the
    // `$or` would show every subscriber a notification only one of them was sent.
    const n::NotificationRow targeted = row_of(Topic::FormAccepted, 53);
    ASSERT_TRUE(repository().insert_notification(db(), targeted).ok());

    n::Subscription sub{};
    sub.kind = code_of(Topic::FormAccepted);
    sub.since = anvil::uuid::v7_boundary(
        (now() - std::chrono::hours{1}).time_since_epoch().count());
    const std::array<n::Subscription, 1> subs{sub};

    const anvil::Result<n::BroadcastPage> page =
        repository().page_broadcast(db(), subs, std::nullopt, 10, now());
    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(page.value().rows.empty());
}

TEST_F(NotificationDb, AReaderSubscribedToNothingGetsAnEmptyHalfRatherThanEverything) {
    const n::NotificationRow row = row_of(Topic::ContentPublished, 54);
    ASSERT_TRUE(repository().insert_notification(db(), row).ok());

    // `$or: []` is a server error and `{}` would return the whole collection, so
    // neither is attempted.
    const anvil::Result<n::BroadcastPage> page =
        repository().page_broadcast(db(), {}, std::nullopt, 10, now());
    ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
    EXPECT_TRUE(page.value().rows.empty());

    const anvil::Result<std::int64_t> count =
        repository().count_broadcast_unread(db(), {}, std::nullopt, now());
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.value(), 0);
}

TEST_F(NotificationDb, TheWatermarkAndTheJoinMarkerBothBoundTheCount) {
    std::vector<Uuid> ids;
    for (int i = 0; i < 4; ++i) {
        n::NotificationRow row = row_of(Topic::ContentPublished, static_cast<std::uint8_t>(60 + i));
        ASSERT_TRUE(repository().insert_notification(db(), row).ok());
        ids.push_back(row.id);
    }

    n::Subscription sub{};
    sub.kind = code_of(Topic::ContentPublished);
    sub.since = anvil::uuid::v7_boundary(
        (now() - std::chrono::hours{1}).time_since_epoch().count());
    const std::array<n::Subscription, 1> subs{sub};

    const anvil::Result<std::int64_t> all =
        repository().count_broadcast_unread(db(), subs, std::nullopt, now());
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all.value(), 4);

    // The watermark bounds it from below. Whichever of the two is LATER wins: a
    // watermark ahead of the join marker must not be walked back.
    //
    // The watermark is the second id in the order the INDEX walks, not the
    // second created: two v7 ids minted in the same millisecond order by their
    // random tails (core/uuid.h), and four creations in a tight loop share one
    // routinely. Sorting is what makes the expected count a hard number rather
    // than a claim about how fast the machine ran.
    std::vector<Uuid> ordered = ids;
    std::sort(ordered.begin(), ordered.end());

    const anvil::Result<std::int64_t> after =
        repository().count_broadcast_unread(db(), subs, ordered[1], now());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value(), 2);
}

// --- clients -----------------------------------------------------------------

TEST_F(NotificationDb, AFreshClientIsSubscribedToItsDefaultsAndFullyEnabled) {
    const Uuid owner = anvil::uuid::generate_v7();
    const std::array<n::TopicCode, 3> defaults{code_of(Topic::ContentPublished),
                                               code_of(Topic::SessionNewDevice),
                                               code_of(Topic::FormSubmitted)};

    const anvil::Result<Uuid> created = repository().ensure_client(
        db(), n::ClientType::InApp, owner, "", defaults, now());
    ASSERT_TRUE(created.ok()) << static_cast<int>(created.code());

    const anvil::Result<std::optional<n::ClientRow>> row =
        repository().inapp_client(db(), owner);
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());

    // The resource-scoped topic is excluded BY CONSTRUCTION: whoever owns the
    // resource subscribes deliberately, because subscribing to a permission-gated
    // topic is the disclosure.
    EXPECT_EQ(row.value()->subs.count, 2U);
    EXPECT_NE(row.value()->subs.find(code_of(Topic::ContentPublished), anvil::kNilUuid),
              nullptr);
    // An account-scoped topic's subject IS the owner, which is what keeps a
    // targeted security notification from reaching every other account.
    EXPECT_NE(row.value()->subs.find(code_of(Topic::SessionNewDevice), owner), nullptr);
    EXPECT_EQ(row.value()->subs.find(code_of(Topic::FormSubmitted), anvil::kNilUuid), nullptr);

    // Every preference bit set, so a topic added by a later deploy arrives
    // enabled rather than silently muted for everyone who registered earlier.
    EXPECT_EQ(row.value()->prefs, n::Preferences::all_enabled());
}

TEST_F(NotificationDb, EnsureClientIsIdempotentAndNeverReplacesWhatIsThere) {
    const Uuid owner = anvil::uuid::generate_v7();
    const std::array<n::TopicCode, 1> defaults{code_of(Topic::ContentPublished)};

    const anvil::Result<Uuid> first =
        repository().ensure_client(db(), n::ClientType::InApp, owner, "", defaults, now());
    ASSERT_TRUE(first.ok());
    const anvil::Result<Uuid> second =
        repository().ensure_client(db(), n::ClientType::InApp, owner, "", defaults, now());
    ASSERT_TRUE(second.ok());
    // The same client, not a second one: a read-then-write would race two
    // concurrent first reads and the loser would silently replace the winner.
    EXPECT_EQ(first.value(), second.value());
}

TEST_F(NotificationDb, TheFanOutScanMatchesOnThePairAndNotOnTwoIndependentFields) {
    const Uuid subject = anvil::uuid::generate_v7();
    const Uuid other = anvil::uuid::generate_v7();

    // A client holding {FormSubmitted, other} and {FormAccepted, subject}. Two
    // dotted predicates would match it for (FormSubmitted, subject) — which is a
    // topic it never subscribed to, and on a staff-facing topic that is the
    // disclosure.
    n::ClientRow decoy{};
    decoy.id = anvil::uuid::generate_v4();
    decoy.owner = anvil::uuid::generate_v7();
    decoy.created_at = now();
    decoy.prefs = n::Preferences::all_enabled();
    decoy.type = n::ClientType::InApp;
    ASSERT_TRUE(decoy.subs.push({other, anvil::kNilUuid, code_of(Topic::FormSubmitted)}));
    ASSERT_TRUE(decoy.subs.push({subject, anvil::kNilUuid, code_of(Topic::FormAccepted)}));
    ASSERT_TRUE(repository().insert_client(db(), decoy).ok());

    n::ClientRow real{};
    real.id = anvil::uuid::generate_v4();
    real.owner = anvil::uuid::generate_v7();
    real.created_at = now();
    real.prefs = n::Preferences::all_enabled();
    real.type = n::ClientType::InApp;
    ASSERT_TRUE(real.subs.push({subject, anvil::kNilUuid, code_of(Topic::FormSubmitted)}));
    ASSERT_TRUE(repository().insert_client(db(), real).ok());

    const anvil::Result<std::vector<n::ClientTarget>> found = repository().subscribers(
        db(), n::scoped_topic(code_of(Topic::FormSubmitted), subject), std::nullopt, 50);
    ASSERT_TRUE(found.ok()) << static_cast<int>(found.code());
    ASSERT_EQ(found.value().size(), 1U);
    EXPECT_EQ(found.value()[0].id, real.id);
}

TEST_F(NotificationDb, ADisabledEndpointIsExcludedByTheFilterRatherThanAfterwards) {
    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.owner = anvil::uuid::generate_v7();
    row.created_at = now();
    row.prefs = n::Preferences::all_enabled();
    row.type = n::ClientType::Webhook;
    row.addr = "https://example.test/hook";
    ASSERT_TRUE(row.subs.push({anvil::kNilUuid, anvil::kNilUuid,
                               code_of(Topic::ContentPublished)}));
    ASSERT_TRUE(repository().insert_client(db(), row).ok());

    const anvil::Result<std::vector<n::ClientTarget>> before = repository().subscribers(
        db(), n::global_topic(code_of(Topic::ContentPublished)), std::nullopt, 50);
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before.value().size(), 1U);

    ASSERT_TRUE(repository().disable_client(db(), row.id, now(), n::DeliveryVerdict::Gone).ok());

    // Not even read, let alone delivered to.
    const anvil::Result<std::vector<n::ClientTarget>> after = repository().subscribers(
        db(), n::global_topic(code_of(Topic::ContentPublished)), std::nullopt, 50);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(after.value().empty());
}

TEST_F(NotificationDb, ConcurrentFailuresEachCountAndTheLastAttemptIsOneWrite) {
    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.owner = anvil::uuid::generate_v7();
    row.created_at = now();
    row.prefs = n::Preferences::all_enabled();
    row.type = n::ClientType::WebPush;
    row.addr = "https://push.test/abc";
    ASSERT_TRUE(repository().insert_client(db(), row).ok());

    // N failures produce N distinct counts and none is lost — which is why this
    // one write is unversioned.
    for (std::int32_t expected = 1; expected <= 3; ++expected) {
        const anvil::Result<std::int32_t> streak = repository().record_delivery_failure(
            db(), row.id, n::DeliveryVerdict::Transient, now());
        ASSERT_TRUE(streak.ok()) << static_cast<int>(streak.code());
        EXPECT_EQ(streak.value(), expected);
    }

    const anvil::Result<std::optional<n::ClientRow>> failing =
        repository().find_client(db(), row.id);
    ASSERT_TRUE(failing.ok());
    ASSERT_TRUE(failing.value().has_value());
    EXPECT_EQ(failing.value()->fail_n, 3);
    // "Never tried" and "tried and it worked" are different rows on an operator's
    // screen, which is why the INSTANT is the optional and the verdict is not.
    EXPECT_TRUE(failing.value()->last_delivery_at.has_value());
    EXPECT_EQ(failing.value()->last_verdict, n::DeliveryVerdict::Transient);

    ASSERT_TRUE(repository().clear_delivery_failures(db(), row.id, now()).ok());
    const anvil::Result<std::optional<n::ClientRow>> healthy =
        repository().find_client(db(), row.id);
    ASSERT_TRUE(healthy.ok());
    EXPECT_EQ(healthy.value()->fail_n, 0);
    EXPECT_EQ(healthy.value()->last_verdict, n::DeliveryVerdict::Delivered);
}

TEST_F(NotificationDb, SubscriptionsAndPreferencesAreVersioned) {
    const Uuid owner = anvil::uuid::generate_v7();
    const std::array<n::TopicCode, 1> defaults{code_of(Topic::ContentPublished)};
    ASSERT_TRUE(
        repository().ensure_client(db(), n::ClientType::InApp, owner, "", defaults, now()).ok());

    const anvil::Result<std::optional<n::ClientRow>> row =
        repository().inapp_client(db(), owner);
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(row.value().has_value());

    n::Preferences muted = n::Preferences::all_enabled();
    muted.set(code_of(Topic::ContentPublished), n::ClientType::WebPush, false);

    const anvil::Result<std::int64_t> first = repository().replace_preferences(
        db(), row.value()->id, row.value()->version, muted);
    ASSERT_TRUE(first.ok()) << static_cast<int>(first.code());
    EXPECT_EQ(first.value(), row.value()->version + 1);

    // The second tab read the same version and loses, rather than silently
    // overwriting the first.
    EXPECT_EQ(
        repository().replace_preferences(db(), row.value()->id, row.value()->version, muted)
            .code(),
        ErrorCode::VersionMismatch);

    const anvil::Result<std::optional<n::ClientRow>> back =
        repository().inapp_client(db(), owner);
    ASSERT_TRUE(back.ok());
    EXPECT_FALSE(back.value()->prefs.enabled(code_of(Topic::ContentPublished),
                                             n::ClientType::WebPush));
}

TEST_F(NotificationDb, TheWatermarkOnlyEverMovesForward) {
    const Uuid owner = anvil::uuid::generate_v7();
    const std::array<n::TopicCode, 1> defaults{code_of(Topic::ContentPublished)};
    ASSERT_TRUE(
        repository().ensure_client(db(), n::ClientType::InApp, owner, "", defaults, now()).ok());

    const Uuid earlier = anvil::uuid::v7_boundary(1000);
    const Uuid later = anvil::uuid::v7_boundary(2000);

    ASSERT_TRUE(repository().advance_watermark(db(), owner, later).ok());
    // `$max` is what makes this correct without a version: two concurrent marks
    // must leave the LATER one standing, and a read-modify-write would let the
    // earlier win and silently un-read everything between them.
    ASSERT_TRUE(repository().advance_watermark(db(), owner, earlier).ok());

    const anvil::Result<n::ReadState> state = repository().read_state(db(), owner);
    ASSERT_TRUE(state.ok());
    ASSERT_TRUE(state.value().broadcast_watermark.has_value());
    EXPECT_EQ(*state.value().broadcast_watermark, later);
}

TEST_F(NotificationDb, AWebhookListingHasNowhereToPutTheSecret) {
    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.created_at = now();
    row.prefs = n::Preferences::all_enabled();
    row.type = n::ClientType::Webhook;
    row.addr = "https://example.test/hook";
    // A sealed secret, stored. It is shown once at registration and there is no
    // read path that decrypts it back.
    row.keys = {1, 2, 3, 4, 5};
    ASSERT_TRUE(repository().insert_client(db(), row).ok());

    const anvil::Result<std::vector<n::WebhookRow>> listed =
        repository().list_webhooks(db(), 50);
    ASSERT_TRUE(listed.ok()) << static_cast<int>(listed.code());
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value()[0].url, "https://example.test/hook");
    EXPECT_TRUE(listed.value()[0].enabled);
    // The projection does not fetch `keys` and the type has no field to put one
    // in, so "shown once" is a property of two layers rather than of a handler
    // that remembers to strip it. Asserted against the type that DOES carry one,
    // so the distinction is proved rather than assumed.
    static_assert(CarriesSealedKeys<n::ClientRow>);
    static_assert(!CarriesSealedKeys<n::WebhookRow>,
                  "a webhook listing row must have nowhere to carry a sealed secret");
}

TEST_F(NotificationDb, DeletingSomebodyElsesClientMatchesNothing) {
    const Uuid mine = anvil::uuid::generate_v7();
    const Uuid theirs = anvil::uuid::generate_v7();

    n::ClientRow row{};
    row.id = anvil::uuid::generate_v4();
    row.owner = theirs;
    row.created_at = now();
    row.prefs = n::Preferences::all_enabled();
    row.type = n::ClientType::Email;
    row.addr = "someone@example.test";
    ASSERT_TRUE(repository().insert_client(db(), row).ok());

    // Ownership is part of the FILTER, so the answer is byte-identical to
    // deleting one that does not exist.
    const anvil::Result<bool> refused = repository().delete_client(db(), row.id, mine);
    ASSERT_TRUE(refused.ok());
    EXPECT_FALSE(refused.value());

    const anvil::Result<bool> allowed = repository().delete_client(db(), row.id, theirs);
    ASSERT_TRUE(allowed.ok());
    EXPECT_TRUE(allowed.value());
}

TEST_F(NotificationDb, ThePageIsNewestFirstAcrossEveryBranchAndNotOneBranchAtATime) {
    // Two subscribed broadcast topics. The reader's page has to be the newest
    // rows ACROSS both — a page that exhausts one topic before showing any of the
    // other is not a bounded view of a merged stream, it is one topic's feed with
    // the other's silently behind it.
    std::vector<Uuid> published;
    for (int i = 0; i < 3; ++i) {
        const n::NotificationRow row = row_of(Topic::ContentPublished,
                                              static_cast<std::uint8_t>(200 + i));
        ASSERT_TRUE(repository().insert_notification(db(), row).ok());
        published.push_back(row.id);
        // The rows have to be orderable by time for "newest first" to mean
        // anything at all.
        await_next_millisecond();
    }
    // Newest of all, on the HIGHER topic code — so branch order and time order
    // disagree, which is the only arrangement that can tell them apart.
    const n::NotificationRow newest = row_of(Topic::SystemAnnouncement, 210);
    ASSERT_TRUE(repository().insert_notification(db(), newest).ok());

    const Uuid since = anvil::uuid::v7_boundary(now().time_since_epoch().count() - 60'000);
    const std::array<n::Subscription, 2> subs{
        n::Subscription{anvil::kNilUuid, since, code_of(Topic::ContentPublished)},
        n::Subscription{anvil::kNilUuid, since, code_of(Topic::SystemAnnouncement)}};

    const anvil::Result<n::BroadcastPage> page =
        repository().page_broadcast(db(), subs, std::nullopt, 2, now());
    ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
    ASSERT_EQ(page.value().rows.size(), 2U);
    EXPECT_EQ(page.value().rows[0].id, newest.id);
    EXPECT_EQ(page.value().rows[1].id, published[2]);

    // And the cursor must not strand it: paging on a cursor taken from one branch
    // excludes every newer row of every OTHER branch, permanently.
    ASSERT_TRUE(page.value().next_cursor.has_value());
    const anvil::Result<n::BroadcastPage> second =
        repository().page_broadcast(db(), subs, page.value().next_cursor, 10, now());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().rows.size(), 2U);
    EXPECT_EQ(second.value().rows[0].id, published[1]);
    EXPECT_EQ(second.value().rows[1].id, published[0]);
}
