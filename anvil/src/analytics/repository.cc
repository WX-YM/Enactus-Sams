#include "anvil/analytics/repository.h"

// versioned-write-exempt: nothing in this subsystem is read-modify-written. The
// session upsert is a primary-key $setOnInsert whose key IS the identity it is
// establishing, so two instances writing it are writing the same document; the
// rollup upsert recomputes its whole window and $sets the result, which is what
// makes it re-runnable at all (docs/17-analytics.md §14). A version filter would
// add a re-read to both and protect neither.

#include <algorithm>
#include <chrono>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/options/update.hpp>

#include "anvil/core/uuid.h"

namespace anvil::analytics {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace ef = event_fields;
namespace sf = session_fields;
namespace rf = rollup_fields;

[[nodiscard]] db::TimeMs plus(db::TimeMs at, std::chrono::seconds seconds) noexcept {
    return at + std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
}

// The dimension slots, as an array of int32. Written even when every slot is
// absent, so every row in the collection has the same shape and the rollup's
// equality filter never has to distinguish "no dimensions" from "field missing".
void append_dimensions(bsoncxx::builder::basic::document& doc, std::string_view key,
                       const DimensionValues& values) {
    doc.append(kvp(codec::key_of(key), [&values](sub_array array) {
        for (const std::uint8_t value : values) {
            array.append(bsoncxx::types::b_int32{static_cast<std::int32_t>(value)});
        }
    }));
}

// OMITTED when nil, unlike the dimension slots: those are fixed-width and
// written unconditionally so every row has the same shape (comment above), but
// an entity id is sixteen bytes an ordinary row will never carry, and most
// rows — including every one written before this field existed — never
// declare one at all.
void append_entity(bsoncxx::builder::basic::document& doc, std::string_view key,
                   const Uuid& entity) {
    if (!is_nil(entity)) { codec::append_uuid(doc, key, entity); }
}

// Absent decodes to kNilUuid — the same value an event or bucket with no
// entity dimension carries — so a row written before this field existed reads
// identically to one that simply never used one. Only a PRESENT field of the
// wrong shape is Internal, per codec::read_optional_uuid.
[[nodiscard]] Result<Uuid> read_entity(const bsoncxx::document::view& doc, std::string_view key) {
    const Result<std::optional<Uuid>> value = codec::read_optional_uuid(doc, key);
    if (!value) { return value.error(); }
    return value.value().value_or(kNilUuid);
}

[[nodiscard]] Result<DimensionValues> read_dimensions(const bsoncxx::document::view& doc,
                                                      std::string_view key) {
    DimensionValues values = no_dimensions();
    const auto element = doc[codec::key_of(key)];
    if (!element || element.type() != bsoncxx::type::k_array) {
        return fail(ErrorCode::Internal, key);
    }
    std::size_t next = 0;
    for (const auto& item : element.get_array().value) {
        if (next >= values.size()) { break; }
        if (item.type() != bsoncxx::type::k_int32) { return fail(ErrorCode::Internal, key); }
        const std::int32_t raw = item.get_int32().value;
        // A stored index outside one byte is corruption or a writer this build
        // has never seen. Either way it must not become a valid-looking slot.
        if (raw < 0 || raw > 0xFF) { return fail(ErrorCode::Internal, key); }
        values[next++] = static_cast<std::uint8_t>(raw);
    }
    return values;
}

// `{expires_at: {$gt: now}}`, appended to every read of a lifetime-bounded
// collection. The TTL monitor lags by up to a minute, so the index alone leaves
// an expired row readable — and a row past its retention window that still
// reaches a rollup is a retention window that does not exist.
void append_not_expired(bsoncxx::builder::basic::document& filter, db::TimeMs now) {
    repo::append_not_expired(filter, ef::kExpiresAt, now);
}

[[nodiscard]] Result<EventRow> decode_event(const bsoncxx::document::view& doc) {
    EventRow row{};
    const Result<db::TimeMs> at = codec::read_time(doc, ef::kAt);
    if (!at) { return at.error(); }
    row.at = at.value();

    const Result<std::int32_t> code = codec::read_int32(doc, ef::kCode);
    if (!code) { return code.error(); }
    row.event.code = code.value();

    const Result<DimensionValues> dimensions = read_dimensions(doc, ef::kDimensions);
    if (!dimensions) { return dimensions.error(); }
    row.event.dimensions = dimensions.value();

    if (const Status read = codec::read_bytes(doc, ef::kSession, row.event.session); !read) {
        return read.error();
    }

    const Result<std::optional<Uuid>> subject = codec::read_optional_uuid(doc, ef::kSubject);
    if (!subject) { return subject.error(); }
    row.event.subject = subject.value();

    const Result<Uuid> entity = read_entity(doc, ef::kEntity);
    if (!entity) { return entity.error(); }
    row.event.entity = entity.value();

    const Result<std::int32_t> repeats = codec::read_int32(doc, ef::kRepeats);
    if (!repeats) { return repeats.error(); }
    if (repeats.value() < 0) { return fail(ErrorCode::Internal, ef::kRepeats); }
    row.repeats = static_cast<std::uint32_t>(repeats.value());
    return row;
}

}  // namespace

std::chrono::milliseconds bucket_width(Granularity granularity) noexcept {
    return granularity == Granularity::Hour ? std::chrono::milliseconds{3'600'000}
                                            : std::chrono::milliseconds{86'400'000};
}

db::TimeMs bucket_start(db::TimeMs at, Granularity granularity) noexcept {
    const std::int64_t width = bucket_width(granularity).count();
    const std::int64_t ms = at.time_since_epoch().count();
    // Floor division, so a pre-epoch instant does not land in the bucket after
    // the one it belongs to. The bucket is part of the rollup's `_id`, so an
    // off-by-one writes a second document for a window that already has one.
    const std::int64_t floored = ms >= 0 ? (ms / width) * width
                                         : ((ms - width + 1) / width) * width;
    return db::TimeMs{std::chrono::milliseconds{floored}};
}

Status EventRepository::append_many(mongocxx::client& client, std::span<const EventRow> rows,
                                    db::TimeMs now, std::chrono::seconds retention) const {
    if (rows.empty()) { return ok(); }

    return repo::guarded([&]() -> Status {
        std::vector<bsoncxx::document::value> documents;
        documents.reserve(rows.size());
        for (const EventRow& row : rows) {
            bsoncxx::builder::basic::document doc;
            // UUIDv7: time-ordered, so the collection's primary key IS the
            // rollup's cursor and the window read needs no secondary index at
            // all. Minted at WRITE time rather than at offer time, because a row
            // coalesced over a second would otherwise carry an id from before
            // the events it stands for.
            codec::append_uuid(doc, ef::kId, uuid::generate_v7());
            doc.append(kvp(codec::key_of(ef::kCode), bsoncxx::types::b_int32{row.event.code}));
            codec::append_time(doc, ef::kAt, row.at);
            doc.append(kvp(codec::key_of(ef::kSession), codec::bytes_bin(row.event.session)));
            codec::append_optional_uuid(doc, ef::kSubject, row.event.subject);
            append_dimensions(doc, ef::kDimensions, row.event.dimensions);
            append_entity(doc, ef::kEntity, row.event.entity);
            doc.append(kvp(codec::key_of(ef::kRepeats),
                           bsoncxx::types::b_int32{static_cast<std::int32_t>(row.repeats)}));
            // Retention is a TTL index over this field. It is a LIFETIME rather
            // than a history policy, so every read filters on it too.
            codec::append_time(doc, ef::kExpiresAt, plus(row.at, retention));
            documents.push_back(doc.extract());
        }

        mongocxx::options::insert options{};
        // UNORDERED, so a single rejected row does not discard every row behind
        // it. An ordered batch stops at the first failure, which here means one
        // malformed row silently costs the rest of the flush.
        options.ordered(false);

        mongocxx::collection events = bind(client);
        events.insert_many(documents, options);
        static_cast<void>(now);
        return ok();
    });
}

Result<EventPage> EventRepository::window(mongocxx::client& client, db::TimeMs from,
                                          db::TimeMs until,
                                          const std::optional<EventCursor>& after,
                                          std::int32_t limit, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<EventPage> {
        const std::int32_t page = limit > 0 ? limit : 1;

        bsoncxx::builder::basic::document filter;
        // Cursor pagination on (at, _id), never skip(n) — skip is O(n)
        // server-side, and this walks the highest-volume collection here.
        //
        // The resume clause is an $or of "a later instant" and "the same instant
        // with a later id", which is the only shape that is both total and
        // expressible against an (at, _id) index. A filter of
        // `at >= cursor.at AND _id > cursor.id` would silently drop every row at
        // a later instant whose id happened to sort lower.
        const db::TimeMs lower = after.has_value() ? after->at : from;
        filter.append(kvp(codec::key_of(ef::kAt), [lower, until](sub_document sub) {
            sub.append(kvp("$gte", codec::time_date(lower)));
            // Half-open, so two adjacent windows neither double-count a row on
            // the boundary nor drop one.
            sub.append(kvp("$lt", codec::time_date(until)));
        }));
        append_not_expired(filter, now);
        if (after.has_value()) {
            filter.append(kvp("$or", [&after](sub_array branches) {
                branches.append([&after](sub_document branch) {
                    branch.append(kvp(codec::key_of(ef::kAt), [&after](sub_document sub) {
                        sub.append(kvp("$gt", codec::time_date(after->at)));
                    }));
                });
                branches.append([&after](sub_document branch) {
                    branch.append(kvp(codec::key_of(ef::kAt), codec::time_date(after->at)));
                    branch.append(kvp(codec::key_of(ef::kId), [&after](sub_document sub) {
                        sub.append(kvp("$gt", codec::uuid_bin(after->id)));
                    }));
                });
            }));
        }

        mongocxx::options::find options{};
        options.sort(make_document(kvp(codec::key_of(ef::kAt), 1),
                                   kvp(codec::key_of(ef::kId), 1)));
        options.limit(page);
        options.projection(make_document(
            kvp(codec::key_of(ef::kId), 1), kvp(codec::key_of(ef::kCode), 1),
            kvp(codec::key_of(ef::kAt), 1), kvp(codec::key_of(ef::kSession), 1),
            kvp(codec::key_of(ef::kSubject), 1), kvp(codec::key_of(ef::kDimensions), 1),
            kvp(codec::key_of(ef::kEntity), 1), kvp(codec::key_of(ef::kRepeats), 1)));

        EventPage out{};
        out.rows.reserve(static_cast<std::size_t>(page));
        mongocxx::collection events = bind(client);
        std::optional<EventCursor> last;
        for (const bsoncxx::document::view row : events.find(filter.view(), options)) {
            Result<EventRow> decoded = decode_event(row);
            if (!decoded) { return decoded.error(); }
            const Result<Uuid> id = codec::read_uuid(row, ef::kId);
            if (!id) { return id.error(); }
            last = EventCursor{decoded.value().at, id.value()};
            out.rows.push_back(std::move(decoded).value());
        }
        // A full page means there may be more; a short one is the end. Reporting
        // a cursor on a short page would make a resumed rollup re-read a window
        // it has already counted.
        if (out.rows.size() == static_cast<std::size_t>(page)) { out.next_after = last; }
        return out;
    });
}

Result<std::int64_t> EventRepository::erase_subject(mongocxx::client& client,
                                                    const Uuid& subject) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        // ttl-filter-exempt: erasure must reach rows the TTL monitor has not
        // collected yet. This is the one read in the subsystem where "still
        // physically present" is exactly what is being asked about, and adding
        // an expiry filter would leave the expired-but-resident rows behind —
        // which is the opposite of what an erasure request means.
        mongocxx::collection events = bind(client);
        const auto removed = events.delete_many(
            make_document(kvp(codec::key_of(ef::kSubject), codec::uuid_bin(subject))));
        if (!removed) { return std::int64_t{0}; }
        return static_cast<std::int64_t>(removed->deleted_count());
    });
}

Result<std::int64_t> EventRepository::count_in_window(mongocxx::client& client,
                                                      db::TimeMs from, db::TimeMs until,
                                                      db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(codec::key_of(ef::kAt), [from, until](sub_document sub) {
            sub.append(kvp("$gte", codec::time_date(from)));
            sub.append(kvp("$lt", codec::time_date(until)));
        }));
        append_not_expired(filter, now);
        mongocxx::collection events = bind(client);
        return events.count_documents(filter.view());
    });
}

Result<bool> SessionRepository::touch(mongocxx::client& client, const VisitorId& visitor,
                                      DayNumber day, db::TimeMs now,
                                      std::chrono::seconds retention) const {
    return repo::guarded([&]() -> Result<bool> {
        // The `_id` IS the (visitor, day) pair. A compound primary key rather
        // than a unique secondary index, the `sections` precedent: the
        // collection then carries no secondary index at all, and the upsert is
        // a primary-key write that N instances converge on with no coordination.
        //
        // Field order inside `_id` is load-bearing — the server compares
        // subdocuments field-by-field — so it is built here and nowhere else.
        auto identity = make_document(kvp(codec::key_of(sf::kVisitor), codec::bytes_bin(visitor)),
                                      kvp(codec::key_of(sf::kDay), bsoncxx::types::b_int32{day}));

        bsoncxx::builder::basic::document insert;
        codec::append_time(insert, sf::kStartedAt, now);
        codec::append_time(insert, sf::kExpiresAt, plus(now, retention));

        mongocxx::options::update options{};
        options.upsert(true);

        mongocxx::collection sessions = bind(client);
        // $setOnInsert and not $set: the session's start is the FIRST event of
        // the day, and rewriting it on every subsequent event would report every
        // session as having started moments ago.
        const auto result = sessions.update_one(
            make_document(kvp(codec::key_of(sf::kId), identity.view())),
            make_document(kvp("$setOnInsert", insert.view())), options);
        if (!result) { return false; }
        // The SERVER's answer to "did this create the row", not a read that
        // raced with another instance asking the same question.
        return result->upserted_id().has_value();
    });
}

Result<std::int64_t> SessionRepository::count_day(mongocxx::client& client, DayNumber day,
                                                  db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter;
        // The day lives inside the compound `_id`, so the dotted path is what
        // rides the primary key.
        filter.append(kvp(codec::key_of("_id.day"), bsoncxx::types::b_int32{day}));
        repo::append_not_expired(filter, sf::kExpiresAt, now);
        mongocxx::collection sessions = bind(client);
        return sessions.count_documents(filter.view());
    });
}

Status RollupRepository::put(mongocxx::client& client,
                             std::span<const RollupRow> rows) const {
    if (rows.empty()) { return ok(); }

    return repo::guarded([&]() -> Status {
        mongocxx::collection rollups = bind(client);
        mongocxx::options::update options{};
        options.upsert(true);

        for (const RollupRow& row : rows) {
            bsoncxx::builder::basic::document identity;
            identity.append(kvp(codec::key_of(rf::kCode),
                                bsoncxx::types::b_int32{row.code}));
            identity.append(kvp(codec::key_of(rf::kGranularity),
                                bsoncxx::types::b_int32{
                                    static_cast<std::int32_t>(row.granularity)}));
            identity.append(kvp(codec::key_of(rf::kBucket), codec::time_date(row.bucket)));
            append_dimensions(identity, rf::kDimensions, row.dimensions);
            // LAST in the identity, and OMITTED when nil: a bucket whose event
            // declares no Entity dimension writes the exact `_id` shape it
            // always has, so a rollup already on disk before this feature
            // existed is found and $set again rather than duplicated.
            append_entity(identity, rf::kEntity, row.entity);

            bsoncxx::builder::basic::document set;
            codec::append_int64(set, rf::kCount, row.count);
            codec::append_int64(set, rf::kSessions, row.sessions);
            codec::append_time(set, rf::kComputedAt, db::now_ms());

            // $set and NEVER $inc. Every queue in this system is at-least-once,
            // so a rollup that added to what it found would double-count the
            // first time a worker was reclaimed after a lease expiry — and the
            // number would be wrong in a way nothing reports and nobody can
            // reconstruct. Running this twice produces one identical document.
            rollups.update_one(
                make_document(kvp(codec::key_of(rf::kId), identity.view())),
                make_document(kvp("$set", set.view())), options);
        }
        return ok();
    });
}

Result<std::vector<RollupRow>> RollupRepository::read(mongocxx::client& client,
                                                      const RollupQuery& query) const {
    return repo::guarded([&]() -> Result<std::vector<RollupRow>> {
        const std::int32_t limit = query.limit > 0 ? query.limit : 1;

        bsoncxx::builder::basic::document filter;
        // Equalities first, then the range: the leading keys of the index this
        // shape is written to ride.
        filter.append(kvp(codec::key_of("_id.code"), bsoncxx::types::b_int32{query.code}));
        filter.append(kvp(codec::key_of("_id.gran"),
                          bsoncxx::types::b_int32{
                              static_cast<std::int32_t>(query.granularity)}));
        filter.append(kvp(codec::key_of("_id.bucket"), [&query](sub_document sub) {
            sub.append(kvp("$gte", codec::time_date(query.from)));
            sub.append(kvp("$lt", codec::time_date(query.until)));
        }));
        if (query.match_dimensions) {
            filter.append(kvp(codec::key_of("_id.dims"), [&query](sub_array array) {
                for (const std::uint8_t value : query.dimensions) {
                    array.append(bsoncxx::types::b_int32{static_cast<std::int32_t>(value)});
                }
            }));
        }
        if (query.match_entity) {
            // `_id.ent` exists only on a document written with one, exactly
            // like `_id.dims` is always present: an equality against a real id
            // matches only rows that carried that id, which is the query this
            // is for. Filtering for kNilUuid is not a supported query — there
            // is no document to find, because a nil entity is never written.
            filter.append(kvp(codec::key_of("_id.ent"), codec::uuid_bin(query.entity)));
        }

        mongocxx::options::find options{};
        // Paginated by the indexed bucket key rather than skip(n), and bounded
        // like every other read: a dashboard asking for five years of hourly
        // buckets is asking for 43,800 documents.
        options.sort(make_document(kvp(codec::key_of("_id.bucket"), 1)));
        options.limit(limit);

        std::vector<RollupRow> out;
        out.reserve(static_cast<std::size_t>(limit));
        mongocxx::collection rollups = bind(client);
        for (const bsoncxx::document::view doc : rollups.find(filter.view(), options)) {
            const auto id = doc[codec::key_of(rf::kId)];
            if (!id || id.type() != bsoncxx::type::k_document) {
                return fail(ErrorCode::Internal, rf::kId);
            }
            const bsoncxx::document::view identity = id.get_document().value;

            RollupRow row{};
            const Result<db::TimeMs> bucket = codec::read_time(identity, rf::kBucket);
            if (!bucket) { return bucket.error(); }
            row.bucket = bucket.value();

            const Result<std::int32_t> code = codec::read_int32(identity, rf::kCode);
            if (!code) { return code.error(); }
            row.code = code.value();

            const Result<Granularity> granularity =
                codec::read_enum(identity, rf::kGranularity, Granularity::Day);
            if (!granularity) { return granularity.error(); }
            row.granularity = granularity.value();

            const Result<DimensionValues> dimensions =
                read_dimensions(identity, rf::kDimensions);
            if (!dimensions) { return dimensions.error(); }
            row.dimensions = dimensions.value();

            const Result<Uuid> entity = read_entity(identity, rf::kEntity);
            if (!entity) { return entity.error(); }
            row.entity = entity.value();

            const Result<std::int64_t> count = codec::read_int64(doc, rf::kCount);
            if (!count) { return count.error(); }
            row.count = count.value();

            const Result<std::int64_t> sessions = codec::read_int64(doc, rf::kSessions);
            if (!sessions) { return sessions.error(); }
            row.sessions = sessions.value();

            out.push_back(row);
        }
        return out;
    });
}

}  // namespace anvil::analytics
