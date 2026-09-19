// The audit collection against a live cluster: what a row carries, what it does
// NOT carry, and how a page comes back.
//
// The read assertions are as much about the cursor as about the rows. Rows are
// written in batches that can share one millisecond, so a cursor on the instant
// alone serves part of a flush twice and skips the rest — which is invisible
// until a burst, and a burst is exactly when the log is read.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "anvil/audit/repository.h"
#include "anvil/core/uuid.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/audit_actions.h"

namespace {

using anvil::Uuid;
using anvil::audit::AuditAction;
using anvil::audit::AuditCursor;
using anvil::audit::AuditEntry;
using anvil::audit::AuditQuery;
using anvil::audit::AuditRepository;
using anvil::audit::AuditRow;
using anvil::testfixture::scratch_names;

constexpr std::string_view kAuditLog = "audit_log";

constexpr auto kDenied = AuditAction::of(testapp::Action::AccessDenied);
constexpr auto kPermissionChanged =
    AuditAction::of(testapp::Action::StaffPermissionChanged);

class AuditDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kAuditLog);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }
    [[nodiscard]] static AuditRepository log() {
        return AuditRepository{std::string{scratch_names().for_collection(kAuditLog)},
                               kAuditLog};
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

[[nodiscard]] std::array<std::uint8_t, 16> some_ip() noexcept {
    std::array<std::uint8_t, 16> ip{};
    ip[10] = 0xFF;
    ip[11] = 0xFF;
    ip[12] = 198;
    ip[13] = 51;
    ip[14] = 100;
    ip[15] = 3;
    return ip;
}

[[nodiscard]] AuditEntry minimal(AuditAction action) {
    return AuditEntry{.actor = std::nullopt,
                      .subject = std::nullopt,
                      .from_state = std::nullopt,
                      .to_state = std::nullopt,
                      .ip = some_ip(),
                      .action = action,
                      .code = anvil::ErrorCode::Forbidden,
                      .succeeded = false,
                      .stealthed = true};
}

[[nodiscard]] AuditEntry full(AuditAction action, const Uuid& actor, const Uuid& subject) {
    return AuditEntry{.actor = actor,
                      .subject = subject,
                      .from_state = 1,
                      .to_state = 0,
                      .ip = some_ip(),
                      .action = action,
                      .code = anvil::ErrorCode::Ok,
                      .succeeded = true,
                      .stealthed = false};
}

// --- what a row carries -----------------------------------------------------

TEST_F(AuditDb, ARowRoundTripsEveryFieldItCarries) {
    const Uuid actor = anvil::uuid::generate_v7();
    const Uuid subject = anvil::uuid::generate_v7();
    const anvil::db::TimeMs when = anvil::db::now_ms();

    ASSERT_TRUE(log().append(db(), full(kPermissionChanged, actor, subject), when).ok());

    const auto page = log().listing(db(), AuditQuery{.limit = 10});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 1U);

    const auto& row = page.value().rows[0];
    EXPECT_EQ(row.actor, actor);
    EXPECT_EQ(row.subject, subject);
    EXPECT_EQ(row.from_state, 1);
    EXPECT_EQ(row.to_state, 0);
    EXPECT_EQ(row.ip, some_ip());
    EXPECT_EQ(row.action, kPermissionChanged);
    EXPECT_EQ(row.code, anvil::ErrorCode::Ok);
    EXPECT_TRUE(row.succeeded);
    EXPECT_EQ(row.at, when);
    EXPECT_EQ(row.repeats, 1U);
}

TEST_F(AuditDb, ARowWithNothingToSayStoresNothingExtra) {
    // The rows this collection holds most of are denials carrying neither an
    // actor nor a subject. Writing those as explicit nulls would cost twelve
    // bytes each, multiplied by exactly the traffic the design exists to
    // survive.
    ASSERT_TRUE(log().append(db(), minimal(kDenied), anvil::db::now_ms()).ok());

    const auto stored =
        db()[std::string{scratch_names().for_collection(kAuditLog)}][std::string{kAuditLog}]
            .find_one(bsoncxx::builder::basic::make_document());
    ASSERT_TRUE(stored.has_value());
    const bsoncxx::document::view doc = stored->view();

    EXPECT_EQ(doc.find("actor"), doc.end());
    EXPECT_EQ(doc.find("sub"), doc.end());
    EXPECT_EQ(doc.find("from"), doc.end());
    EXPECT_EQ(doc.find("to"), doc.end());
    // The ordinary row costs nothing to store its repeat count either.
    EXPECT_EQ(doc.find("n"), doc.end());
}

TEST_F(AuditDb, ThereIsNoFieldForAPayloadOnDiskAtAll) {
    // Asserted by inspecting the STORED document rather than by trusting the
    // code path. The endpoint cannot leak what was never written, and that is a
    // property of the schema rather than of the encoder.
    ASSERT_TRUE(
        log().append(db(), full(kPermissionChanged, anvil::uuid::generate_v7(),
                                anvil::uuid::generate_v7()),
                     anvil::db::now_ms())
            .ok());

    const auto stored =
        db()[std::string{scratch_names().for_collection(kAuditLog)}][std::string{kAuditLog}]
            .find_one(bsoncxx::builder::basic::make_document());
    ASSERT_TRUE(stored.has_value());

    std::vector<std::string> keys;
    for (const bsoncxx::document::element& field : stored->view()) {
        keys.emplace_back(field.key().data(), field.key().size());
    }
    const std::vector<std::string> expected{"_id",  "at", "actor", "sub", "act",
                                            "code", "ok", "ip",    "from", "to"};
    EXPECT_EQ(keys, expected);
}

TEST_F(AuditDb, AStealthedFlagIsNeverStored) {
    // It exists so two consecutive denials can be told apart before they are
    // folded. It is a fold input, not a fact about the row.
    ASSERT_TRUE(log().append(db(), minimal(kDenied), anvil::db::now_ms()).ok());

    const auto stored =
        db()[std::string{scratch_names().for_collection(kAuditLog)}][std::string{kAuditLog}]
            .find_one(bsoncxx::builder::basic::make_document());
    ASSERT_TRUE(stored.has_value());
    for (const bsoncxx::document::element& field : stored->view()) {
        const std::string key{field.key().data(), field.key().size()};
        EXPECT_NE(key, "stealthed");
    }
}

TEST_F(AuditDb, ARepeatCountIsWrittenOnlyWhenItExceedsOne) {
    const anvil::db::TimeMs when = anvil::db::now_ms();
    const std::array<AuditRow, 1> folded{AuditRow{minimal(kDenied), when, 4096}};
    ASSERT_TRUE(log().append_many(db(), folded).ok());

    const auto page = log().listing(db(), AuditQuery{.limit = 10});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 1U);
    EXPECT_EQ(page.value().rows[0].repeats, 4096U);
}

TEST_F(AuditDb, AnAbsentRepeatCountReadsAsOneAndNeverAsZero) {
    // Every row written before the count existed has no such field, and
    // retention here outlives any deploy cycle — a reader treating absence as
    // zero would report that whole history as having never happened.
    auto collection =
        db()[std::string{scratch_names().for_collection(kAuditLog)}][std::string{kAuditLog}];
    ASSERT_TRUE(log().append(db(), minimal(kDenied), anvil::db::now_ms()).ok());

    const auto page = log().listing(db(), AuditQuery{.limit = 10});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 1U);
    EXPECT_EQ(page.value().rows[0].repeats, 1U);
}

// --- batching ---------------------------------------------------------------

TEST_F(AuditDb, ABatchIsOneInsertAndAnEmptyBatchIsNoInsertAtAll) {
    ASSERT_TRUE(log().append_many(db(), {}).ok());

    std::vector<AuditRow> batch;
    const anvil::db::TimeMs when = anvil::db::now_ms();
    for (int i = 0; i < 256; ++i) { batch.push_back(AuditRow{minimal(kDenied), when, 1}); }
    ASSERT_TRUE(log().append_many(db(), batch).ok());

    const auto page = log().listing(db(), AuditQuery{.limit = 300});
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page.value().rows.size(), 256U);
}

TEST_F(AuditDb, EveryRowInABatchGetsItsOwnId) {
    // The id is minted at ENCODE time, which is flush time, and it has to be
    // distinct per row or the compound cursor cannot separate two rows sharing a
    // millisecond.
    std::vector<AuditRow> batch;
    const anvil::db::TimeMs when = anvil::db::now_ms();
    for (int i = 0; i < 32; ++i) { batch.push_back(AuditRow{minimal(kDenied), when, 1}); }
    ASSERT_TRUE(log().append_many(db(), batch).ok());

    const auto page = log().listing(db(), AuditQuery{.limit = 64});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 32U);

    std::vector<Uuid> ids;
    for (const auto& row : page.value().rows) { ids.push_back(row.id); }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}

// --- the read ---------------------------------------------------------------

TEST_F(AuditDb, AListingComesBackNewestFirst) {
    const anvil::db::TimeMs base = anvil::db::now_ms();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(
            log().append(db(), minimal(kDenied), base + std::chrono::seconds{i}).ok());
    }

    const auto page = log().listing(db(), AuditQuery{.limit = 10});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 5U);
    for (std::size_t i = 1; i < page.value().rows.size(); ++i) {
        EXPECT_LE(page.value().rows[i].at, page.value().rows[i - 1].at);
    }
}

TEST_F(AuditDb, TheCursorSeparatesRowsThatShareOneMillisecond) {
    // A whole batch can share one instant, so an instant-only cursor would serve
    // part of a flush twice and skip the rest. This is the case that proves the
    // pair.
    std::vector<AuditRow> batch;
    const anvil::db::TimeMs when = anvil::db::now_ms();
    for (int i = 0; i < 10; ++i) { batch.push_back(AuditRow{minimal(kDenied), when, 1}); }
    ASSERT_TRUE(log().append_many(db(), batch).ok());

    std::vector<Uuid> seen;
    std::optional<AuditCursor> cursor;
    for (int page_number = 0; page_number < 5; ++page_number) {
        const auto page = log().listing(db(), AuditQuery{.after = cursor, .limit = 3});
        ASSERT_TRUE(page.ok());
        for (const auto& row : page.value().rows) { seen.push_back(row.id); }
        if (!page.value().next.has_value()) { break; }
        cursor = page.value().next;
    }

    EXPECT_EQ(seen.size(), 10U);
    std::sort(seen.begin(), seen.end());
    // Nothing served twice.
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
}

TEST_F(AuditDb, TheWindowSurvivesPagination) {
    // Both the window and the cursor bound the same field, and BSON is an
    // ordered list of keys — so appending them as two clauses would let the
    // second shadow the first and the window would silently stop applying on
    // page two.
    const anvil::db::TimeMs base = anvil::db::now_ms();
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(
            log().append(db(), minimal(kDenied), base + std::chrono::seconds{i}).ok());
    }

    const AuditQuery windowed{.from = base + std::chrono::seconds{5},
                              .to = base + std::chrono::seconds{8},
                              .limit = 2};
    const auto first = log().listing(db(), windowed);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().rows.size(), 2U);
    ASSERT_TRUE(first.value().next.has_value());

    AuditQuery second_page = windowed;
    second_page.after = first.value().next;
    const auto second = log().listing(db(), second_page);
    ASSERT_TRUE(second.ok());

    for (const auto& row : second.value().rows) {
        EXPECT_GE(row.at, base + std::chrono::seconds{5});
        EXPECT_LT(row.at, base + std::chrono::seconds{8});
    }
    // Half-open: three seconds of a ten-second span, so three rows across both
    // pages and not four.
    EXPECT_EQ(first.value().rows.size() + second.value().rows.size(), 3U);
}

TEST_F(AuditDb, FilteringByActorAndByTargetNarrowsIndependently) {
    const Uuid actor = anvil::uuid::generate_v7();
    const Uuid subject = anvil::uuid::generate_v7();
    const anvil::db::TimeMs when = anvil::db::now_ms();

    ASSERT_TRUE(log().append(db(), full(kPermissionChanged, actor, subject), when).ok());
    ASSERT_TRUE(log()
                    .append(db(),
                            full(kPermissionChanged, anvil::uuid::generate_v7(),
                                 anvil::uuid::generate_v7()),
                            when)
                    .ok());

    const auto by_actor = log().listing(db(), AuditQuery{.actor = actor, .limit = 10});
    ASSERT_TRUE(by_actor.ok());
    ASSERT_EQ(by_actor.value().rows.size(), 1U);
    EXPECT_EQ(by_actor.value().rows[0].subject, subject);

    const auto by_target = log().listing(db(), AuditQuery{.target = subject, .limit = 10});
    ASSERT_TRUE(by_target.ok());
    ASSERT_EQ(by_target.value().rows.size(), 1U);
    EXPECT_EQ(by_target.value().rows[0].actor, actor);
}

TEST_F(AuditDb, FilteringByActionUsesTheApplicationsOwnValue) {
    const anvil::db::TimeMs when = anvil::db::now_ms();
    ASSERT_TRUE(log().append(db(), minimal(kDenied), when).ok());
    ASSERT_TRUE(log()
                    .append(db(),
                            full(kPermissionChanged, anvil::uuid::generate_v7(),
                                 anvil::uuid::generate_v7()),
                            when)
                    .ok());

    const auto denials = log().listing(db(), AuditQuery{.action = kDenied, .limit = 10});
    ASSERT_TRUE(denials.ok());
    ASSERT_EQ(denials.value().rows.size(), 1U);
    EXPECT_EQ(denials.value().rows[0].action, kDenied);
}

TEST_F(AuditDb, APageReportsANextCursorOnlyWhenThereIsMore) {
    const anvil::db::TimeMs base = anvil::db::now_ms();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(
            log().append(db(), minimal(kDenied), base + std::chrono::seconds{i}).ok());
    }

    // Answered by the WALK — one row past the page — rather than by a second
    // count over the whole retention window.
    const auto exact = log().listing(db(), AuditQuery{.limit = 3});
    ASSERT_TRUE(exact.ok());
    EXPECT_EQ(exact.value().rows.size(), 3U);
    EXPECT_FALSE(exact.value().next.has_value());

    const auto partial = log().listing(db(), AuditQuery{.limit = 2});
    ASSERT_TRUE(partial.ok());
    EXPECT_EQ(partial.value().rows.size(), 2U);
    EXPECT_TRUE(partial.value().next.has_value());
}

TEST_F(AuditDb, TheEmptyQueryIsTheWholeLog) {
    ASSERT_TRUE(log().append(db(), minimal(kDenied), anvil::db::now_ms()).ok());
    const auto page = log().listing(db(), AuditQuery{.limit = 10});
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page.value().rows.size(), 1U);
}

}  // namespace
