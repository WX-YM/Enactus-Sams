// Phase 16 — the entity dimension, against a live cluster.
//
// An entity dimension trades a closed, compile-time value list for an
// application-minted UUID admitted one at a time at ingest
// (docs/17-analytics.md §19). Everything here follows analytics_db_test.cc and
// rollup_db_test.cc's own shape: a privacy or storage property is asserted by
// READING THE COLLECTION BACK, never by trusting the return value of the call
// that is supposed to have produced it.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/analytics/ingest.h"
#include "anvil/analytics/query.h"
#include "anvil/analytics/repository.h"
#include "anvil/analytics/rollup.h"
#include "anvil/analytics/sessions.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/random.h"

#include "app_fixture.h"
#include "db_fixture.h"
#include "events.h"

namespace anvil::analytics {
namespace {

using anvil::testfixture::scratch_names;

constexpr std::string_view kEvents = "analytics_events";
constexpr std::string_view kSessions = "analytics_sessions";
constexpr std::string_view kRollups = "analytics_rollups";

inline constexpr AnalyticsCollections kCollections{kEvents, kSessions, kRollups};

[[nodiscard]] EventCode code_of(testapp::Event event) noexcept {
    return static_cast<EventCode>(event);
}

// An admission hook that keeps a fixed, in-memory set current — the shape
// docs recommend a real one take: no lock needed because the test drives it
// from one thread, matching offer()'s own single-caller-at-a-time contract.
class FixedAdmission final {
public:
    void allow(const Uuid& id) { allowed_.push_back(id); }

    [[nodiscard]] EntityAdmission hook() {
        return [this](std::string_view dimension, EventCode code, const Uuid& id) {
            calls.push_back(EntityCallArgs{std::string{dimension}, code, id});
            for (const Uuid& candidate : allowed_) {
                if (candidate == id) { return true; }
            }
            return false;
        };
    }

    struct EntityCallArgs final {
        std::string dimension;
        EventCode   code;
        Uuid        id;
    };
    std::vector<EntityCallArgs> calls;

private:
    std::vector<Uuid> allowed_;
};

class EntityDimensionDb : public ::testing::Test {
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
        anvil::testfixture::clear_collection(**client_, kRollups);

        crypto::Key256 pepper;
        crypto::random_bytes(pepper.mutable_span());
        install_visitor_pepper(std::move(pepper));
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kEvents)};
    }

    [[nodiscard]] static EventSink sink(IngestConfig config) {
        return EventSink{scratch_names(), kCollections, testapp::kEvents, config};
    }

    [[nodiscard]] static Offer project_viewed(const Uuid& entity, std::string_view address,
                                              bool consented = true) {
        Offer offer{code_of(testapp::Event::ProjectViewed), no_dimensions(),
                   http::pack_address(address), std::nullopt, consented};
        offer.entity = entity;
        return offer;
    }

    [[nodiscard]] std::int64_t stored_events() {
        return db()[database()][std::string{kEvents}].count_documents(
            bsoncxx::builder::basic::make_document());
    }

    [[nodiscard]] static RollupJob rollup_job(Granularity granularity = Granularity::Hour) {
        return RollupJob{scratch_names(), kCollections, granularity};
    }

    [[nodiscard]] static EventRepository events() { return EventRepository{database(), kEvents}; }

    // Writes a ProjectViewed row directly, at an instant of the caller's
    // choosing, exactly as rollup_db_test.cc's own write() helper does for the
    // enum dimension.
    void write(const Uuid& entity, db::TimeMs at, std::uint32_t repeats,
              std::uint8_t session_byte) {
        VisitorId session{};
        session[0] = session_byte;
        const EventRow row{at,
                           Event{code_of(testapp::Event::ProjectViewed), no_dimensions(),
                                 session, std::nullopt, entity},
                           repeats};
        const std::vector<EventRow> batch{row};
        ASSERT_TRUE(events()
                        .append_many(db(), batch, db::now_ms(), std::chrono::hours{24 * 30})
                        .ok());
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

TEST_F(EntityDimensionDb, AnAdmittedEntityIsStoredAsBsonUuidBinaryAndReadBackUnchanged) {
    FixedAdmission admission;
    const Uuid project = uuid::generate_v4();
    admission.allow(project);

    EventSink events_sink = sink(IngestConfig{.entity_admission = admission.hook()});
    ASSERT_EQ(events_sink.offer(project_viewed(project, "203.0.113.9")),
              EventSink::Outcome::Recorded);
    ASSERT_TRUE(events_sink.flush_now(db()).ok());
    ASSERT_EQ(stored_events(), 1);

    // The hook saw exactly the id offered, under the dimension's own name and
    // the event's own code — never a string, never an index.
    ASSERT_EQ(admission.calls.size(), 1U);
    EXPECT_EQ(admission.calls[0].dimension, "project");
    EXPECT_EQ(admission.calls[0].code, code_of(testapp::Event::ProjectViewed));
    EXPECT_EQ(admission.calls[0].id, project);

    // Stored as BSON BinData subtype 4 — a UUID — never subtype 0 and never a
    // 36-character k_string, scanned from the RAW document rather than trusted
    // from a decoder that could paper over the wrong shape.
    for (const bsoncxx::document::view doc :
        db()[database()][std::string{kEvents}].find(bsoncxx::builder::basic::make_document())) {
        const auto ent = doc["ent"];
        ASSERT_TRUE(ent);
        ASSERT_EQ(ent.type(), bsoncxx::type::k_binary);
        const bsoncxx::types::b_binary binary = ent.get_binary();
        EXPECT_EQ(binary.sub_type, bsoncxx::binary_sub_type::k_uuid);
        ASSERT_EQ(binary.size, project.size());
        EXPECT_TRUE(std::equal(project.begin(), project.end(), binary.bytes));
    }

    const EventRepository repository{database(), kEvents};
    const db::TimeMs now = db::now_ms();
    const Result<EventPage> page = repository.window(
        db(), now - std::chrono::hours{1}, now + std::chrono::hours{1}, std::nullopt, 10, now);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 1U);
    EXPECT_EQ(page.value().rows[0].event.entity, project);
}

TEST_F(EntityDimensionDb, AnEntityTheAdmissionHookRefusesNeverReachesARow) {
    FixedAdmission admission;  // allows nothing
    const Uuid project = uuid::generate_v4();

    EventSink events_sink = sink(IngestConfig{.entity_admission = admission.hook()});
    EXPECT_EQ(events_sink.offer(project_viewed(project, "203.0.113.10")),
              EventSink::Outcome::RefusedEntity);
    EXPECT_EQ(events_sink.buffered(), 0U);
    ASSERT_TRUE(events_sink.flush_now(db()).ok());
    EXPECT_EQ(stored_events(), 0);
}

TEST_F(EntityDimensionDb, NoAdmissionHookRefusesEveryEntityByDefault) {
    // DENY BY DEFAULT (CLAUDE.md §5): a spec can declare an Entity dimension
    // without anyone wiring up EntityAdmission, and the safe failure for an
    // unbounded id space nothing is vetting is to admit none of it.
    EventSink events_sink = sink(IngestConfig{});
    EXPECT_EQ(events_sink.offer(project_viewed(uuid::generate_v4(), "203.0.113.11")),
              EventSink::Outcome::RefusedEntity);
    ASSERT_TRUE(events_sink.flush_now(db()).ok());
    EXPECT_EQ(stored_events(), 0);
}

TEST_F(EntityDimensionDb, AnEventDeclaringNoEntityDimensionIgnoresAnyIdOfferedAnyway) {
    // SignupCompleted's spec carries only an enum dimension. An entity offered
    // against it is not an error — offer() simply has nowhere to put it — and
    // the stored row carries kNilUuid, exactly as one that never set the field
    // at all.
    FixedAdmission admission;
    Offer offer{code_of(testapp::Event::SignupCompleted), no_dimensions(),
               http::pack_address("203.0.113.12"), std::nullopt, true};
    offer.entity = uuid::generate_v4();

    EventSink events_sink = sink(IngestConfig{.entity_admission = admission.hook()});
    ASSERT_EQ(events_sink.offer(offer), EventSink::Outcome::Recorded);
    ASSERT_TRUE(events_sink.flush_now(db()).ok());
    ASSERT_EQ(stored_events(), 1);
    // The hook is never even consulted for an event with no entity slot.
    EXPECT_TRUE(admission.calls.empty());

    const EventRepository repository{database(), kEvents};
    const db::TimeMs now = db::now_ms();
    const Result<EventPage> page = repository.window(
        db(), now - std::chrono::hours{1}, now + std::chrono::hours{1}, std::nullopt, 10, now);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().rows.size(), 1U);
    EXPECT_TRUE(is_nil(page.value().rows[0].event.entity));
}

TEST_F(EntityDimensionDb, TheRollupGroupsByEntitySeparatelyAndASeriesCanBeNarrowedToOne) {
    const db::TimeMs now = db::now_ms();
    const db::TimeMs bucket = bucket_start(now - std::chrono::hours{2}, Granularity::Hour);

    const Uuid first = uuid::generate_v4();
    const Uuid second = uuid::generate_v4();

    // Unequal totals — 5 against 8 — so the grouped assertion below can check
    // COUNT ORDER without depending on how the two random ids happen to
    // compare byte-for-byte.
    write(first, bucket + std::chrono::minutes{1}, 3, 1);
    write(first, bucket + std::chrono::minutes{2}, 2, 2);
    write(second, bucket + std::chrono::minutes{3}, 8, 1);

    const Result<BucketReport> rolled = rollup_job().roll_bucket(db(), bucket, now);
    ASSERT_TRUE(rolled.ok());
    EXPECT_EQ(rolled.value().rows_read, 3);
    // Two entities, two rollup documents — grouped by entity exactly as an
    // enum dimension combination would be.
    EXPECT_EQ(rolled.value().rows_written, 2);

    const AnalyticsQuery reader{scratch_names(), kCollections};
    const TimeRange range{bucket, bucket + std::chrono::hours{1}};

    const Result<std::vector<Bucket>> only_first = reader.counts_over_time_for_entity(
        db(), code_of(testapp::Event::ProjectViewed), first, range, Granularity::Hour);
    ASSERT_TRUE(only_first.ok());
    ASSERT_EQ(only_first.value().size(), 1U);
    EXPECT_EQ(only_first.value()[0].count, 5);      // 3 + 2
    EXPECT_EQ(only_first.value()[0].sessions, 2);

    const Result<std::vector<Bucket>> only_second = reader.counts_over_time_for_entity(
        db(), code_of(testapp::Event::ProjectViewed), second, range, Granularity::Hour);
    ASSERT_TRUE(only_second.ok());
    ASSERT_EQ(only_second.value().size(), 1U);
    EXPECT_EQ(only_second.value()[0].count, 8);
    EXPECT_EQ(only_second.value()[0].sessions, 1);

    // Grouped rather than narrowed: every entity that appeared, each with its
    // own total, HIGHEST COUNT FIRST.
    const Result<std::vector<EntityCount>> grouped = reader.counts_by_entity(
        db(), code_of(testapp::Event::ProjectViewed), range, Granularity::Hour);
    ASSERT_TRUE(grouped.ok());
    ASSERT_EQ(grouped.value().size(), 2U);
    EXPECT_EQ(grouped.value()[0].entity, second);
    EXPECT_EQ(grouped.value()[0].count, 8);
    EXPECT_EQ(grouped.value()[1].entity, first);
    EXPECT_EQ(grouped.value()[1].count, 5);

    // The id round-trips to the canonical string a JSON response would carry —
    // never the raw bytes and never an index.
    EXPECT_EQ(uuid::to_string(grouped.value()[0].entity), uuid::to_string(second));
}

TEST_F(EntityDimensionDb, AnEventWithNoEntityRollsUpWithTheNilPlaceholderExcludedFromGrouping) {
    // SignupCompleted carries an enum dimension only — exercised here through
    // the rollup and the entity-grouped query, to prove neither path was
    // changed by this feature for a table that never uses it.
    const db::TimeMs now = db::now_ms();
    const db::TimeMs bucket = bucket_start(now - std::chrono::hours{2}, Granularity::Hour);

    DimensionValues web = no_dimensions();
    web[0] = static_cast<std::uint8_t>(testapp::Surface::Web);
    VisitorId session{};
    session[0] = 9;
    const EventRow row{bucket + std::chrono::minutes{1},
                       Event{code_of(testapp::Event::SignupCompleted), web, session,
                             std::nullopt},
                       4};
    ASSERT_TRUE(events()
                    .append_many(db(), std::vector<EventRow>{row}, db::now_ms(),
                                std::chrono::hours{24 * 30})
                    .ok());

    ASSERT_TRUE(rollup_job().roll_bucket(db(), bucket, now).ok());

    const AnalyticsQuery reader{scratch_names(), kCollections};
    const TimeRange range{bucket, bucket + std::chrono::hours{1}};
    const Result<std::vector<Bucket>> series = reader.counts_over_time(
        db(), code_of(testapp::Event::SignupCompleted), range, Granularity::Hour);
    ASSERT_TRUE(series.ok());
    ASSERT_EQ(series.value().size(), 1U);
    EXPECT_EQ(series.value()[0].count, 4);

    // No entity was ever offered, so grouping by entity finds nothing to
    // report — a nil placeholder is not an id, and the fold discards it.
    const Result<std::vector<EntityCount>> grouped = reader.counts_by_entity(
        db(), code_of(testapp::Event::SignupCompleted), range, Granularity::Hour);
    ASSERT_TRUE(grouped.ok());
    EXPECT_TRUE(grouped.value().empty());
}

}  // namespace
}  // namespace anvil::analytics
