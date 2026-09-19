// versioned-write-exempt: this collection is append-only. There is no update and
// no delete on it at all, so there is no read-modify-write for a version filter
// to protect — the absence of those methods is the mechanism, and adding a
// version field would imply a writer that does not exist.

#include "anvil/audit/repository.h"

#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/insert.hpp>

#include "anvil/core/uuid.h"

namespace anvil::audit {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = audit_fields;

// One row, encoded. The id is minted HERE, at encode time, which is why the
// cursor cannot be `_id` alone: `at` is when the event happened and the id is
// when the batch was assembled, and under a burst those are the two instants
// furthest apart.
[[nodiscard]] bsoncxx::document::value encode(const AuditRow& row) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, f::kId, uuid::generate_v7());
    codec::append_time(doc, f::kAt, row.at);
    // OMITTED when absent rather than written null. A denial under flood carries
    // neither an actor nor a subject, and those are the rows this collection
    // holds most of — twelve bytes each, multiplied by the traffic the design
    // exists to survive.
    if (row.entry.actor.has_value()) { codec::append_uuid(doc, f::kActor, *row.entry.actor); }
    if (row.entry.subject.has_value()) {
        codec::append_uuid(doc, f::kSubject, *row.entry.subject);
    }
    doc.append(kvp(codec::key_of(f::kAction), bsoncxx::types::b_int32{row.entry.action.stored()}));
    codec::append_enum(doc, f::kCode, row.entry.code);
    doc.append(kvp(codec::key_of(f::kSucceeded), bsoncxx::types::b_bool{row.entry.succeeded}));
    doc.append(kvp(codec::key_of(f::kIp), codec::bytes_bin(row.entry.ip)));
    if (row.entry.from_state.has_value()) {
        doc.append(kvp(codec::key_of(f::kFromState),
                       bsoncxx::types::b_int32{*row.entry.from_state}));
    }
    if (row.entry.to_state.has_value()) {
        doc.append(kvp(codec::key_of(f::kToState), bsoncxx::types::b_int32{*row.entry.to_state}));
    }
    // Omitted at one, which is every row that was not folded. The ordinary row
    // then costs nothing to store, and a reader treats an absent field as one.
    if (row.repeats > 1) {
        doc.append(kvp(codec::key_of(f::kRepeats),
                       bsoncxx::types::b_int64{static_cast<std::int64_t>(row.repeats)}));
    }
    return doc.extract();
}

[[nodiscard]] Result<AuditView> decode(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<db::TimeMs> at = codec::read_time(doc, f::kAt);
    if (!at) { return at.error(); }
    const Result<std::int32_t> action = codec::read_int32(doc, f::kAction);
    if (!action) { return action.error(); }
    const Result<ErrorCode> code = codec::read_enum(doc, f::kCode, kMaxErrorCode);
    if (!code) { return code.error(); }
    const Result<bool> succeeded = codec::read_bool(doc, f::kSucceeded);
    if (!succeeded) { return succeeded.error(); }

    AuditView view{
        .actor = std::nullopt,
        .subject = std::nullopt,
        .from_state = std::nullopt,
        .to_state = std::nullopt,
        .ip = {},
        .id = id.value(),
        .at = at.value(),
        .repeats = 1,
        .action = AuditAction::from_stored(action.value()),
        .code = code.value(),
        .succeeded = succeeded.value(),
    };

    const Status ip = codec::read_bytes(doc, f::kIp, view.ip);
    if (!ip) { return ip.error(); }

    // The four optional fields are ABSENT rather than null on disk, so a missing
    // element is the ordinary case and not an error.
    if (doc.find(codec::key_of(f::kActor)) != doc.end()) {
        const Result<Uuid> actor = codec::read_uuid(doc, f::kActor);
        if (!actor) { return actor.error(); }
        view.actor = actor.value();
    }
    if (doc.find(codec::key_of(f::kSubject)) != doc.end()) {
        const Result<Uuid> subject = codec::read_uuid(doc, f::kSubject);
        if (!subject) { return subject.error(); }
        view.subject = subject.value();
    }
    if (doc.find(codec::key_of(f::kFromState)) != doc.end()) {
        const Result<std::int32_t> from = codec::read_int32(doc, f::kFromState);
        if (!from) { return from.error(); }
        view.from_state = from.value();
    }
    if (doc.find(codec::key_of(f::kToState)) != doc.end()) {
        const Result<std::int32_t> to = codec::read_int32(doc, f::kToState);
        if (!to) { return to.error(); }
        view.to_state = to.value();
    }
    if (doc.find(codec::key_of(f::kRepeats)) != doc.end()) {
        const Result<std::int64_t> repeats = codec::read_int64(doc, f::kRepeats);
        if (!repeats) { return repeats.error(); }
        if (repeats.value() < 1) { return fail(ErrorCode::Internal, f::kRepeats); }
        view.repeats = static_cast<std::uint32_t>(repeats.value());
    }
    return view;
}

}  // namespace

Status AuditRepository::append(mongocxx::client& client, const AuditEntry& entry,
                               db::TimeMs at) const {
    return repo::guarded([&]() -> Status {
        const AuditRow row{entry, at, 1};
        mongocxx::collection log = bind(client);
        log.insert_one(encode(row).view());
        return ok();
    });
}

Status AuditRepository::append_many(mongocxx::client& client,
                                    std::span<const AuditRow> rows) const {
    if (rows.empty()) { return ok(); }

    return repo::guarded([&]() -> Status {
        std::vector<bsoncxx::document::value> documents;
        documents.reserve(rows.size());
        for (const AuditRow& row : rows) { documents.push_back(encode(row)); }

        mongocxx::options::insert options{};
        // UNORDERED, so a single rejected row does not discard every row behind
        // it. An ordered batch stops at the first failure, which in a forensic
        // record means one malformed row silently costs the 255 that followed.
        options.ordered(false);

        mongocxx::collection log = bind(client);
        log.insert_many(documents, options);
        return ok();
    });
}

Result<AuditPage> AuditRepository::listing(mongocxx::client& client,
                                           const AuditQuery& query) const {
    return repo::guarded([&]() -> Result<AuditPage> {
        const std::int32_t limit = query.limit > 0 ? query.limit : 1;

        bsoncxx::builder::basic::document filter;
        // Equalities first, and each is intended as the LEADING key of an index
        // whose remaining keys are `(at, -1)` and `(_id, -1)`. A shape carrying
        // two of them rides whichever index the planner prefers and applies the
        // other as a residual — either way the walk provides the ordering, so no
        // combination pays an in-memory sort.
        if (query.action.has_value()) {
            filter.append(kvp(codec::key_of(f::kAction),
                              bsoncxx::types::b_int32{query.action->stored()}));
        }
        if (query.actor.has_value()) { codec::append_uuid(filter, f::kActor, *query.actor); }
        if (query.target.has_value()) { codec::append_uuid(filter, f::kSubject, *query.target); }

        // The window and the cursor are both bounds on `at`, and they are
        // appended as ONE clause rather than two: BSON is an ordered list of
        // keys, so a second `at` would shadow the first and the window a caller
        // asked for would silently stop applying on page two.
        //
        // No expiry predicate here, deliberately. This collection's TTL index is
        // a RETENTION POLICY over history rather than a document lifetime, so
        // the rule every other TTL'd collection carries would filter this one
        // down to nothing (anvil/db/collection_spec.h).
        const std::optional<db::TimeMs> from = query.from;
        const std::optional<db::TimeMs> to = query.to;
        const std::optional<AuditCursor> after = query.after;
        if (from.has_value() || to.has_value() || after.has_value()) {
            filter.append(kvp(codec::key_of(f::kAt), [from, to, after](sub_document sub) {
                if (from.has_value()) { sub.append(kvp("$gte", codec::time_date(*from))); }
                if (to.has_value()) { sub.append(kvp("$lt", codec::time_date(*to))); }
                // `$lte` and not `$lt`: the row AT the cursor's instant is
                // excluded by the `_id` half below, and a strict bound here
                // would drop every other row sharing that millisecond — which,
                // with batches of 256, is most of a flush.
                if (after.has_value()) {
                    sub.append(kvp("$lte", codec::time_date(after->at)));
                }
            }));
        }
        if (after.has_value()) {
            const AuditCursor cursor = *after;
            filter.append(kvp("$or", [cursor](sub_array branches) {
                branches.append([cursor](sub_document sub) {
                    sub.append(kvp(codec::key_of(f::kAt), [cursor](sub_document range) {
                        range.append(kvp("$lt", codec::time_date(cursor.at)));
                    }));
                });
                branches.append([cursor](sub_document sub) {
                    sub.append(kvp(codec::key_of(f::kAt), codec::time_date(cursor.at)));
                    sub.append(kvp(codec::key_of(f::kId), [cursor](sub_document range) {
                        range.append(kvp("$lt", codec::uuid_bin(cursor.id)));
                    }));
                });
            }));
        }

        mongocxx::options::find options{};
        options.sort(make_document(kvp(codec::key_of(f::kAt), -1),
                                   kvp(codec::key_of(f::kId), -1)));
        // One PAST the page, so "is there more" is answered by the walk rather
        // than by a second count over four hundred days of rows.
        options.limit(limit + 1);

        AuditPage page{};
        page.rows.reserve(static_cast<std::size_t>(limit));
        for (const bsoncxx::document::view doc : bind(client).find(filter.view(), options)) {
            if (page.rows.size() == static_cast<std::size_t>(limit)) {
                const AuditView& last = page.rows.back();
                page.next = AuditCursor{last.id, last.at};
                break;
            }
            Result<AuditView> decoded = decode(doc);
            if (!decoded) { return decoded.error(); }
            page.rows.push_back(std::move(decoded).value());
        }
        return page;
    });
}

}  // namespace anvil::audit
