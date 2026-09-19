// Phase 8 — ingest against a live cluster.
//
// Two of these are PRIVACY properties, and both are asserted by LOOKING rather
// than by trusting the path that claims them. An event requiring consent and
// offered without it is asserted absent by reading the collection back, not by
// checking the return value of the function that refused it; and the packed
// client address is asserted absent by scanning the raw BSON, not by inspecting
// the struct that was meant to omit it. A test that asks the code whether it did
// the right thing is a test the code passes by lying.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>

#include "anvil/analytics/erasure.h"
#include "anvil/analytics/ingest.h"
#include "anvil/analytics/repository.h"
#include "anvil/analytics/sessions.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "events.h"
#include "pool_gate.h"

namespace anvil::analytics {
namespace {

using anvil::testfixture::scratch_names;

constexpr std::string_view kEvents = "analytics_events";
constexpr std::string_view kSessions = "analytics_sessions";
constexpr std::string_view kRollups = "analytics_rollups";

// The reference application puts the two high-churn collections in the SECOND
// database and the rollups in the first, which is what that database was
// declared for — so every service here resolves per collection rather than being
// handed one database name.
inline constexpr AnalyticsCollections kCollections{kEvents, kSessions, kRollups};

[[nodiscard]] EventCode code_of(testapp::Event event) noexcept {
    return static_cast<EventCode>(event);
}

class AnalyticsDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kEvents);
        anvil::testfixture::clear_collection(**client_, kSessions);

        crypto::Key256 pepper;
        crypto::random_bytes(pepper.mutable_span());
        install_visitor_pepper(std::move(pepper));
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kEvents)};
    }

    [[nodiscard]] static EventSink sink(IngestConfig config = IngestConfig{}) {
        return EventSink{scratch_names(), kCollections, testapp::kEvents, config};
    }

    [[nodiscard]] std::int64_t stored_events() {
        return db()[database()][std::string{kEvents}].count_documents(
            bsoncxx::builder::basic::make_document());
    }

    [[nodiscard]] std::int64_t stored_sessions() {
        return db()[database()][std::string{kSessions}].count_documents(
            bsoncxx::builder::basic::make_document());
    }

    [[nodiscard]] static Offer offer_of(testapp::Event event, std::string_view address,
                                        bool consented,
                                        std::optional<Uuid> subject = std::nullopt) {
        DimensionValues dimensions = no_dimensions();
        dimensions[0] = static_cast<std::uint8_t>(testapp::Surface::Web);
        if (event == testapp::Event::PageViewed) {
            dimensions[1] = static_cast<std::uint8_t>(testapp::Referrer::Direct);
        }
        return Offer{code_of(event), dimensions, http::pack_address(address), subject,
                     consented};
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(AnalyticsDb, AnEventRequiringConsentOfferedWithoutItNeverReachesARow) {
    EventSink events = sink();
    // Refused at the DOOR, before the buffer — not recorded and then filtered,
    // and not recorded pseudonymously. "Recorded and then excluded from queries"
    // is a policy one forgotten $match away from being no policy at all.
    EXPECT_EQ(events.offer(offer_of(testapp::Event::PageViewed, "203.0.113.9", false)),
              EventSink::Outcome::RefusedConsent);
    EXPECT_EQ(events.buffered(), 0U);

    ASSERT_TRUE(events.flush_now(db()).ok());
    // Asserted by READING THE COLLECTION BACK rather than by trusting the path
    // that refused it.
    EXPECT_EQ(stored_events(), 0);
    EXPECT_EQ(stored_sessions(), 0);

    // And the same event WITH consent does reach one, so the case above is a
    // refusal rather than a sink that records nothing at all.
    EXPECT_EQ(events.offer(offer_of(testapp::Event::PageViewed, "203.0.113.9", true)),
              EventSink::Outcome::Recorded);
    ASSERT_TRUE(events.flush_now(db()).ok());
    EXPECT_EQ(stored_events(), 1);
}

TEST_F(AnalyticsDb, AnEventDeclaringNoConsentRequirementIsRecordedWithoutOne) {
    EventSink events = sink();
    // SignupCompleted is recorded against an account that has just been created
    // deliberately, so it declares no consent requirement.
    EXPECT_EQ(events.offer(offer_of(testapp::Event::SignupCompleted, "203.0.113.9", false)),
              EventSink::Outcome::Recorded);
    ASSERT_TRUE(events.flush_now(db()).ok());
    EXPECT_EQ(stored_events(), 1);
}

TEST_F(AnalyticsDb, ThePackedAddressAppearsNowhereInAStoredDocument) {
    EventSink events = sink();
    const std::string_view address = "198.51.100.77";
    const http::PackedAddress packed = http::pack_address(address);

    ASSERT_EQ(events.offer(offer_of(testapp::Event::PageViewed, address, true)),
              EventSink::Outcome::Recorded);
    ASSERT_TRUE(events.flush_now(db()).ok());
    ASSERT_EQ(stored_events(), 1);

    // Scanned over the RAW BSON of every document in both collections, not over
    // the struct that was meant to omit it. An IPv4 address is a 32-bit input
    // space, so a row carrying one — or an unkeyed digest of one — is reversible
    // by anybody holding a dump.
    const auto contains_address = [&packed](std::string_view collection,
                                            mongocxx::client& client,
                                            const std::string& db_name) {
        for (const bsoncxx::document::view doc :
             client[db_name][std::string{collection}].find(
                 bsoncxx::builder::basic::make_document())) {
            const std::string_view bytes{reinterpret_cast<const char*>(doc.data()),
                                         doc.length()};
            // The mapped form as stored, and the bare four v4 bytes on their
            // own: a truncation would show up as the second even though it is
            // not the first.
            const std::string_view mapped{reinterpret_cast<const char*>(packed.data()),
                                          packed.size()};
            const std::string_view quad{reinterpret_cast<const char*>(packed.data()) + 12, 4};
            if (bytes.find(mapped) != std::string_view::npos) { return true; }
            if (bytes.find(quad) != std::string_view::npos) { return true; }
        }
        return false;
    };

    EXPECT_FALSE(contains_address(kEvents, db(), database()));
    EXPECT_FALSE(contains_address(kSessions, db(), database()));
}

TEST_F(AnalyticsDb, ASessionIsOneRowPerVisitorPerDayHoweverManyEventsArrive) {
    EventSink events = sink();
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(events.offer(offer_of(testapp::Event::SignupCompleted, "203.0.113.4", true)),
                  EventSink::Outcome::Recorded);
    }
    ASSERT_TRUE(events.flush_now(db()).ok());

    // The unique key IS the sessionisation: there is no read-then-write, so N
    // instances converge on one row with no coordination at all.
    EXPECT_EQ(stored_sessions(), 1);
    EXPECT_EQ(stored_events(), 20);

    // A second visitor is a second row; the same visitor tomorrow is a third,
    // because the day is part of the digest.
    const SessionRepository sessions{database(), kSessions};
    const db::TimeMs now = db::now_ms();
    const VisitorId first = visitor_id(http::pack_address("203.0.113.4"), day_of(now));
    const Result<bool> again = sessions.touch(db(), first, day_of(now), now,
                                              std::chrono::hours{24});
    ASSERT_TRUE(again.ok());
    // The SERVER's answer to "did this create the row", not a read that raced.
    EXPECT_FALSE(again.value());

    const Result<bool> tomorrow = sessions.touch(db(), first, day_of(now) + 1, now,
                                                 std::chrono::hours{24});
    ASSERT_TRUE(tomorrow.ok());
    EXPECT_TRUE(tomorrow.value());
}

TEST_F(AnalyticsDb, ErasureRemovesEverySubjectRowAndLeavesEveryAnonymousOne) {
    EventSink events = sink();
    const Uuid subject = uuid::generate_v7();
    const Uuid other = uuid::generate_v7();

    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(events.offer(offer_of(testapp::Event::SignupCompleted, "203.0.113.4", true,
                                        subject)),
                  EventSink::Outcome::Recorded);
    }
    ASSERT_EQ(events.offer(offer_of(testapp::Event::SignupCompleted, "203.0.113.5", true,
                                    other)),
              EventSink::Outcome::Recorded);
    ASSERT_EQ(events.offer(offer_of(testapp::Event::SignupCompleted, "203.0.113.6", true)),
              EventSink::Outcome::Recorded);
    ASSERT_TRUE(events.flush_now(db()).ok());
    ASSERT_EQ(stored_events(), 5);

    const AnalyticsErasure erasure{scratch_names(), kCollections};
    const Result<std::int64_t> removed = erasure.erase(db(), subject);
    ASSERT_TRUE(removed.ok());
    EXPECT_EQ(removed.value(), 3);
    EXPECT_EQ(stored_events(), 2);

    // Rollups carry no subject and are not erased. That is the trade being made
    // explicitly: a count of signups per day is not personal data, and
    // rebuilding history after every erasure request is a cost with no
    // beneficiary.
}

TEST_F(AnalyticsDb, TheErasureScanRidesThePartialIndex) {
    // On a developer's four hundred rows an index scan and a collection scan are
    // indistinguishable by any other means, which is why this asserts through
    // explain rather than through a stopwatch.
    const auto explained = db()[database()].run_command(bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp(
            "explain",
            bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("find", std::string{kEvents}),
                bsoncxx::builder::basic::kvp(
                    "filter", bsoncxx::builder::basic::make_document(
                                  bsoncxx::builder::basic::kvp(
                                      "subj", db::codec::uuid_bin(uuid::generate_v7())))))),
        bsoncxx::builder::basic::kvp("verbosity", "queryPlanner")));

    const std::string plan = bsoncxx::to_json(explained.view());
    EXPECT_NE(plan.find("events_subject"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("\"stage\" : \"COLLSCAN\""), std::string::npos) << plan;
}

TEST_F(AnalyticsDb, AWindowWalkResumesWhereItStopped) {
    EventSink events = sink();
    for (int i = 0; i < 25; ++i) {
        // Distinct dimensions so nothing coalesces and the walk has 25 rows to
        // page through.
        DimensionValues dimensions = no_dimensions();
        dimensions[0] = static_cast<std::uint8_t>(i % 3);
        dimensions[1] = static_cast<std::uint8_t>(i % 2);
        ASSERT_EQ(events.offer(Offer{code_of(testapp::Event::PageViewed), dimensions,
                                     http::pack_address("203.0.113.4"), std::nullopt, true}),
                  EventSink::Outcome::Recorded);
    }
    ASSERT_TRUE(events.flush_now(db()).ok());

    const EventRepository repository{database(), kEvents};
    const db::TimeMs now = db::now_ms();
    const db::TimeMs from = now - std::chrono::hours{1};
    const db::TimeMs until = now + std::chrono::hours{1};

    std::size_t seen = 0;
    std::optional<EventCursor> after;
    int pages = 0;
    for (;;) {
        const Result<EventPage> page = repository.window(db(), from, until, after, 7, now);
        ASSERT_TRUE(page.ok());
        seen += page.value().rows.size();
        ++pages;
        if (!page.value().next_after.has_value()) { break; }
        after = page.value().next_after;
        ASSERT_LT(pages, 20) << "the cursor is not advancing";
    }
    // Every row exactly once. A cursor that dropped rows at a shared instant, or
    // re-read them, would show up here and nowhere else.
    EXPECT_EQ(seen, stored_events());
}

TEST_F(AnalyticsDb, ARowPastItsRetentionIsNotReadBackEvenBeforeTheMonitorReachesIt) {
    // A TTL index is a garbage collector, not an access control: the monitor
    // runs roughly every sixty seconds, so the row below is still physically
    // present. The explicit expiry filter is what keeps it out of a rollup.
    EventSink events = sink(IngestConfig{.retention = std::chrono::seconds{0}});
    ASSERT_EQ(events.offer(offer_of(testapp::Event::SignupCompleted, "203.0.113.4", true)),
              EventSink::Outcome::Recorded);
    ASSERT_TRUE(events.flush_now(db()).ok());
    ASSERT_EQ(stored_events(), 1) << "the row should still be resident";

    const EventRepository repository{database(), kEvents};
    const db::TimeMs now = db::now_ms() + std::chrono::seconds{1};
    const Result<EventPage> page = repository.window(
        db(), now - std::chrono::hours{1}, now + std::chrono::hours{1}, std::nullopt, 10, now);
    ASSERT_TRUE(page.ok());
    EXPECT_TRUE(page.value().rows.empty());
}

// --- a refused flush is not attempted again per event ------------------------

TEST_F(AnalyticsDb, ASaturatedPoolCostsOneRefusalAndNotOnePerEvent) {
    // The same shape the audit sink carries, in the sink it was copied FROM:
    // post() puts a refused batch back, and offer() posts a flush the moment the
    // buffer crosses kBatchRows, so with the queue still full every subsequent
    // event rebuilt the whole 512-row task to be refused again — with a LOG_WARN
    // each time, at event rate, which is the shape docs/00-architecture.md §9
    // forbids by name.
    //
    // Behaviour rows do not coalesce here because each offer carries its own
    // address and therefore its own visitor, so kBatchRows events is kBatchRows
    // rows in the buffer rather than one folded window.
    if (!anvil::testfixture::pools_ready()) { GTEST_SKIP() << "thread pools unavailable"; }
    anvil::testfixture::PoolGate gate{Pools::analytics()};
    gate.block();
    gate.fill_queue();

    EventSink events = sink();
    for (std::size_t i = 0; i < EventSink::kBatchRows * 2; ++i) {
        const std::string address = "198.51.100." + std::to_string(i % 256);
        ASSERT_EQ(events.offer(offer_of(testapp::Event::SignupCompleted, address, true)),
                  EventSink::Outcome::Recorded);
    }

    EXPECT_EQ(events.refused_flushes(), 1U)
        << "one refusal is what tells the sink the pool is full; the rest are the storm";
    EXPECT_EQ(events.deferred_flushes(), EventSink::kBatchRows)
        << "every event past the first batch must be buffered, not re-flushed";
    EXPECT_EQ(events.buffered(), EventSink::kBatchRows * 2);
    EXPECT_EQ(events.dropped_conversions(), 0U);
    EXPECT_EQ(events.dropped_behaviour(), 0U);

    // Nothing was lost by waiting: the rows are still there to be written.
    ASSERT_TRUE(events.flush_now(db()).ok());
    EXPECT_EQ(stored_events(), static_cast<std::int64_t>(EventSink::kBatchRows * 2));

    gate.drain();
}

}  // namespace
}  // namespace anvil::analytics
