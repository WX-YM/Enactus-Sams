// versioned-write-exempt: three writes here are deliberately unversioned, and
// each is one atomic server-side operator with no read in front of it.
//
//   coalesce_notification   an upsert with `$inc` on the dedupe key. That IS the
//                           coalescing: two concurrent publishes inside the
//                           window must produce one row with a count of two, and
//                           a version filter would make one of them lose.
//   record_delivery_failure one `$inc`, so N concurrent transport failures
//                           produce N distinct counts and none is lost. A
//                           version filter would make two simultaneous failures
//                           record one.
//   fan_out / mark_read     bulk writes over rows nobody else is editing: an
//                           inbox row belongs to one reader and is written by
//                           the fan-out once and the read marker once.
//
// Every OTHER write — a client's subscriptions, its preferences, its enabled
// flag — goes through anvil/db/versioned.h, because each is a read-modify-write
// somebody could be doing from two tabs.

#include "anvil/notifications/repository.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/model/update_one.hpp>
#include <mongocxx/options/bulk_write.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/pipeline.hpp>

#include "anvil/core/uuid.h"
#include "anvil/db/versioned.h"

namespace anvil::notifications {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = notification_fields;

// The server's duplicate-key code. A re-run fan-out produces these by design.
constexpr std::int32_t kDuplicateKey = 11000;

[[nodiscard]] bsoncxx::types::b_string text_of(std::string_view value) noexcept {
    return bsoncxx::types::b_string{codec::key_of(value)};
}

[[nodiscard]] bsoncxx::types::b_binary dedupe_bin(
    const std::array<std::uint8_t, 16>& bytes) noexcept {
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary, 16, bytes.data()};
}

// --- encoding ---------------------------------------------------------------

void append_params(bsoncxx::builder::basic::document& doc, const ParamSet& params) {
    doc.append(kvp(codec::key_of(f::kParams), [&params](sub_document sub) {
        for (const StoredParam& param : params.view()) {
            // The key is ONE ASCII letter, which the template grammar already
            // guaranteed. That is what keeps `$set` and `a.b` unrepresentable in
            // this subdocument rather than merely rejected.
            const std::array<char, 1> name{param.name};
            const std::string_view key{name.data(), 1};
            if (param.type == ParamType::Number) {
                sub.append(kvp(codec::key_of(key), bsoncxx::types::b_int64{param.number}));
            } else {
                sub.append(kvp(codec::key_of(key), text_of(param.text)));
            }
        }
    }));
}

// Everything except `_id`, `count` and `dedupe`: the three the coalescing upsert
// writes only when it creates the row.
[[nodiscard]] bsoncxx::document::value notification_body(const NotificationRow& row) {
    bsoncxx::builder::basic::document doc;
    doc.append(kvp(codec::key_of(f::kKind), bsoncxx::types::b_int32{row.kind}));
    // ALWAYS BinData, never null — the nil UUID is what "unscoped" is spelled as.
    //
    // This is not a storage preference, it is what makes the read-merge plannable.
    // In MongoDB `{subject: null}` matches missing OR null, so it is not a point
    // equality and the index bounds cannot be elided to provide the sort; the
    // planner then abandons the SORT_MERGE across the `$or` and falls back to
    // walking `_id` with the topic predicate as a filter. A nil BinData is a real
    // equality and the merge survives.
    //
    // It also makes the two sides of the comparison agree: `Subscription::subject`
    // and `TopicRef::subject` are already plain Uuids where nil means unscoped, so
    // the row was the only place spelling the same idea a second way.
    codec::append_uuid(doc, f::kSubject, row.subject.value_or(kNilUuid));
    doc.append(kvp(codec::key_of(f::kTemplate), bsoncxx::types::b_int32{row.tpl}));
    append_params(doc, row.params);
    if (row.ref.has_value()) {
        doc.append(kvp(codec::key_of(f::kRef), [&row](sub_document sub) {
            sub.append(kvp(codec::key_of(f::kRefKind), bsoncxx::types::b_int32{row.ref->kind}));
            sub.append(kvp(codec::key_of(f::kRefId), codec::uuid_bin(row.ref->id)));
        }));
    } else {
        // Written as null rather than omitted, so the read path distinguishes
        // "points at nothing" from "written by a build that did not have refs" —
        // the first skips the visibility check and the second must not.
        doc.append(kvp(codec::key_of(f::kRef), bsoncxx::types::b_null{}));
    }
    codec::append_optional_uuid(doc, f::kActor, row.actor);
    doc.append(kvp(codec::key_of(f::kChannels), bsoncxx::types::b_int32{row.channels}));
    codec::append_time(doc, f::kCreatedAt, row.created_at);
    codec::append_time(doc, f::kExpiresAt, row.expires_at);
    // The outbox marker, explicitly null on insert: the sweeper's partial index
    // filters on `disp_at: null`, and a partial filter over a field that is
    // sometimes absent is a different index from the one that was reviewed.
    codec::append_optional_time(doc, f::kDispatchedAt, row.dispatched_at);
    return doc.extract();
}

[[nodiscard]] bsoncxx::document::value notification_document(const NotificationRow& row) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, f::kId, row.id);
    doc.append(bsoncxx::builder::concatenate(notification_body(row).view()));
    doc.append(kvp(codec::key_of(f::kDedupe), dedupe_bin(row.dedupe)));
    doc.append(kvp(codec::key_of(f::kCount), bsoncxx::types::b_int32{row.count}));
    return doc.extract();
}

// --- decoding ---------------------------------------------------------------

[[nodiscard]] Result<ParamSet> read_params(const bsoncxx::document::view& doc) {
    ParamSet params{};
    const bsoncxx::document::element element = doc[codec::key_of(f::kParams)];
    if (!element) { return params; }
    if (element.type() != bsoncxx::type::k_document) {
        return fail(ErrorCode::Internal, f::kParams);
    }
    for (const bsoncxx::document::element& entry : element.get_document().value) {
        // A stored key that is not one ASCII letter is corruption or an older
        // writer, and it must not become a parameter name just because it is
        // already on disk.
        if (entry.key().size() != 1 || !detail::is_ascii_letter(entry.key()[0])) {
            return fail(ErrorCode::Internal, f::kParams);
        }
        StoredParam param{};
        param.name = entry.key()[0];
        if (entry.type() == bsoncxx::type::k_int64) {
            param.type = ParamType::Number;
            param.number = entry.get_int64().value;
        } else if (entry.type() == bsoncxx::type::k_string) {
            param.type = ParamType::Text;
            param.text.assign(entry.get_string().value.data(), entry.get_string().value.size());
        } else {
            return fail(ErrorCode::Internal, f::kParams);
        }
        if (!params.push(std::move(param))) { return fail(ErrorCode::Internal, f::kParams); }
    }
    return params;
}

[[nodiscard]] Result<NotificationRow> read_notification(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<std::int32_t> kind = codec::read_int32(doc, f::kKind);
    if (!kind) { return kind.error(); }
    if (kind.value() < 0 || static_cast<std::size_t>(kind.value()) >= kMaxTopicKinds) {
        return fail(ErrorCode::Internal, f::kKind);
    }
    const Result<std::int32_t> tpl = codec::read_int32(doc, f::kTemplate);
    if (!tpl) { return tpl.error(); }
    if (tpl.value() < 0 || tpl.value() > 255) { return fail(ErrorCode::Internal, f::kTemplate); }
    const Result<std::optional<Uuid>> stored_subject =
        codec::read_optional_uuid(doc, f::kSubject);
    if (!stored_subject) { return stored_subject.error(); }
    // Nil is the stored spelling of "unscoped", so it decodes back to absent. A
    // row that still carries an explicit null — written before this was the
    // format — decodes the same way, which is the one direction that needs no
    // migration.
    const std::optional<Uuid> subject =
        (stored_subject.value().has_value() && !is_nil(*stored_subject.value()))
            ? stored_subject.value()
            : std::optional<Uuid>{};
    const Result<std::optional<Uuid>> actor = codec::read_optional_uuid(doc, f::kActor);
    if (!actor) { return actor.error(); }
    const Result<db::TimeMs> created_at = codec::read_time(doc, f::kCreatedAt);
    if (!created_at) { return created_at.error(); }
    const Result<db::TimeMs> expires_at = codec::read_time(doc, f::kExpiresAt);
    if (!expires_at) { return expires_at.error(); }
    const Result<std::optional<db::TimeMs>> dispatched =
        codec::read_optional_time(doc, f::kDispatchedAt);
    if (!dispatched) { return dispatched.error(); }
    const Result<std::int32_t> count = codec::read_int32(doc, f::kCount);
    if (!count) { return count.error(); }
    const Result<std::int32_t> channels = codec::read_int32(doc, f::kChannels);
    if (!channels) { return channels.error(); }
    // Range-checked rather than truncated. A mask wider than a byte is a row this
    // build did not write, and silently keeping its low bits would re-enable a
    // channel the publish that wrote the row had narrowed away.
    if (channels.value() < 0 || channels.value() > 0xFF) {
        return fail(ErrorCode::Internal, f::kChannels);
    }
    Result<ParamSet> params = read_params(doc);
    if (!params) { return params.error(); }

    NotificationRow row{};
    row.id = id.value();
    row.params = std::move(params).value();
    row.subject = subject;
    row.actor = actor.value();
    row.dispatched_at = dispatched.value();
    row.created_at = created_at.value();
    row.expires_at = expires_at.value();
    row.count = count.value();
    row.kind = static_cast<TopicCode>(kind.value());
    row.tpl = static_cast<TemplateId>(tpl.value());
    row.channels = static_cast<ChannelMask>(channels.value());

    if (const bsoncxx::document::element dedupe = doc[codec::key_of(f::kDedupe)]; dedupe) {
        if (dedupe.type() != bsoncxx::type::k_binary) {
            return fail(ErrorCode::Internal, f::kDedupe);
        }
        const bsoncxx::types::b_binary raw = dedupe.get_binary();
        if (raw.size != row.dedupe.size()) { return fail(ErrorCode::Internal, f::kDedupe); }
        for (std::size_t i = 0; i < row.dedupe.size(); ++i) { row.dedupe[i] = raw.bytes[i]; }
    }

    const bsoncxx::document::element ref = doc[codec::key_of(f::kRef)];
    if (ref && ref.type() == bsoncxx::type::k_document) {
        const bsoncxx::document::view view = ref.get_document().value;
        const Result<std::int32_t> ref_kind = codec::read_int32(view, f::kRefKind);
        if (!ref_kind) { return ref_kind.error(); }
        const Result<Uuid> ref_id = codec::read_uuid(view, f::kRefId);
        if (!ref_id) { return ref_id.error(); }
        row.ref = ResourceRef{ref_id.value(), ref_kind.value()};
    } else if (ref && ref.type() != bsoncxx::type::k_null) {
        return fail(ErrorCode::Internal, f::kRef);
    }
    return row;
}

[[nodiscard]] Result<InboxRow> read_inbox(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<Uuid> nid = codec::read_uuid(doc, f::kNid);
    if (!nid) { return nid.error(); }
    const Result<std::optional<db::TimeMs>> read_at = codec::read_optional_time(doc, f::kReadAt);
    if (!read_at) { return read_at.error(); }
    const Result<std::int32_t> kind = codec::read_int32(doc, f::kKind);
    if (!kind) { return kind.error(); }
    if (kind.value() < 0 || static_cast<std::size_t>(kind.value()) >= kMaxTopicKinds) {
        return fail(ErrorCode::Internal, f::kKind);
    }

    InboxRow row{};
    row.id = id.value();
    row.nid = nid.value();
    row.read_at = read_at.value();
    row.kind = static_cast<TopicCode>(kind.value());
    return row;
}

[[nodiscard]] Result<SubscriptionSet> read_subscriptions(const bsoncxx::document::view& doc) {
    SubscriptionSet subs{};
    const bsoncxx::document::element element = doc[codec::key_of(f::kSubs)];
    if (!element) { return subs; }
    if (element.type() != bsoncxx::type::k_array) { return fail(ErrorCode::Internal, f::kSubs); }
    for (const bsoncxx::array::element& entry : element.get_array().value) {
        if (entry.type() != bsoncxx::type::k_document) {
            return fail(ErrorCode::Internal, f::kSubs);
        }
        const bsoncxx::document::view view = entry.get_document().value;
        const Result<std::int32_t> kind = codec::read_int32(view, f::kSubKind);
        if (!kind) { return kind.error(); }
        if (kind.value() < 0 || static_cast<std::size_t>(kind.value()) >= kMaxTopicKinds) {
            return fail(ErrorCode::Internal, f::kSubs);
        }
        const Result<std::optional<Uuid>> subject = codec::read_optional_uuid(view, f::kSubSubject);
        if (!subject) { return subject.error(); }
        const Result<Uuid> since = codec::read_uuid(view, f::kSubSince);
        if (!since) { return since.error(); }

        Subscription sub{};
        sub.subject = subject.value().value_or(kNilUuid);
        sub.since = since.value();
        sub.kind = static_cast<TopicCode>(kind.value());
        if (!subs.push(sub)) { return fail(ErrorCode::Internal, f::kSubs); }
    }
    return subs;
}

void append_subscriptions(bsoncxx::builder::basic::document& doc, const SubscriptionSet& subs) {
    doc.append(kvp(codec::key_of(f::kSubs), [&subs](sub_array array) {
        for (const Subscription& sub : subs.view()) {
            array.append([&sub](sub_document entry) {
                entry.append(kvp(codec::key_of(f::kSubKind), bsoncxx::types::b_int32{sub.kind}));
                // Null rather than a nil UUID for an unscoped topic: the fan-out
                // filter matches on null, and a 16-byte zero would be a different
                // value that no `$elemMatch` written here would find.
                if (is_nil(sub.subject)) {
                    entry.append(kvp(codec::key_of(f::kSubSubject), bsoncxx::types::b_null{}));
                } else {
                    entry.append(kvp(codec::key_of(f::kSubSubject),
                                     codec::uuid_bin(sub.subject)));
                }
                entry.append(kvp(codec::key_of(f::kSubSince), codec::uuid_bin(sub.since)));
            });
        }
    }));
}

[[nodiscard]] Result<Preferences> read_prefs(const bsoncxx::document::view& doc) {
    std::array<std::uint8_t, kPrefBytes> bytes{};
    const Status read = codec::read_bytes(doc, f::kPrefs, bytes);
    if (!read) { return read.error(); }
    return Preferences::from_bytes(bytes);
}

[[nodiscard]] Result<ClientRow> read_client(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<std::string_view> addr = codec::read_text(doc, f::kAddr);
    if (!addr) { return addr.error(); }
    const Result<ClientType> type = codec::read_enum(doc, f::kType, kMaxClientType);
    if (!type) { return type.error(); }
    const Result<DigestMode> digest = codec::read_enum(doc, f::kDigest, kMaxDigestMode);
    if (!digest) { return digest.error(); }
    const Result<DeliveryVerdict> verdict =
        codec::read_enum(doc, f::kLastVerdict, kMaxDeliveryVerdict);
    if (!verdict) { return verdict.error(); }
    const Result<std::optional<Uuid>> owner = codec::read_optional_uuid(doc, f::kOwner);
    if (!owner) { return owner.error(); }
    const Result<std::optional<Uuid>> watermark = codec::read_optional_uuid(doc, f::kWatermark);
    if (!watermark) { return watermark.error(); }
    const Result<std::optional<db::TimeMs>> disabled =
        codec::read_optional_time(doc, f::kDisabledAt);
    if (!disabled) { return disabled.error(); }
    const Result<std::optional<db::TimeMs>> last =
        codec::read_optional_time(doc, f::kLastDeliveryAt);
    if (!last) { return last.error(); }
    const Result<db::TimeMs> created = codec::read_time(doc, f::kCreatedAt);
    if (!created) { return created.error(); }
    const Result<std::int64_t> version = repo::document_version(doc);
    if (!version) { return version.error(); }
    const Result<std::int32_t> failures = codec::read_int32(doc, f::kFailures);
    if (!failures) { return failures.error(); }
    const Result<Preferences> prefs = read_prefs(doc);
    if (!prefs) { return prefs.error(); }
    Result<SubscriptionSet> subs = read_subscriptions(doc);
    if (!subs) { return subs.error(); }

    ClientRow row{};
    row.id = id.value();
    row.addr.assign(addr.value());
    row.subs = std::move(subs).value();
    row.owner = owner.value();
    row.broadcast_watermark = watermark.value();
    row.disabled_at = disabled.value();
    row.last_delivery_at = last.value();
    row.created_at = created.value();
    row.version = version.value();
    row.fail_n = failures.value();
    row.prefs = prefs.value();
    row.type = type.value();
    row.digest = digest.value();
    row.last_verdict = verdict.value();

    if (const bsoncxx::document::element keys = doc[codec::key_of(f::kKeys)]; keys) {
        if (keys.type() == bsoncxx::type::k_binary) {
            const bsoncxx::types::b_binary raw = keys.get_binary();
            row.keys.assign(raw.bytes, raw.bytes + raw.size);
        } else if (keys.type() != bsoncxx::type::k_null) {
            return fail(ErrorCode::Internal, f::kKeys);
        }
    }
    return row;
}

// Only the six fields a delivery decision reads. A full client row carries a
// 64-entry subscription array, and loading hundreds of them to deliver one
// notification is two kilobytes each of data nothing here looks at.
[[nodiscard]] bsoncxx::document::value target_projection() {
    return make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1}),
                         kvp(codec::key_of(f::kAddr), bsoncxx::types::b_int32{1}),
                         kvp(codec::key_of(f::kOwner), bsoncxx::types::b_int32{1}),
                         kvp(codec::key_of(f::kPrefs), bsoncxx::types::b_int32{1}),
                         kvp(codec::key_of(f::kType), bsoncxx::types::b_int32{1}),
                         kvp(codec::key_of(f::kDigest), bsoncxx::types::b_int32{1}),
                         kvp(codec::key_of(f::kKeys), bsoncxx::types::b_int32{1}));
}

[[nodiscard]] Result<ClientTarget> read_target(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<std::string_view> addr = codec::read_text(doc, f::kAddr);
    if (!addr) { return addr.error(); }
    const Result<ClientType> type = codec::read_enum(doc, f::kType, kMaxClientType);
    if (!type) { return type.error(); }
    const Result<DigestMode> digest = codec::read_enum(doc, f::kDigest, kMaxDigestMode);
    if (!digest) { return digest.error(); }
    const Result<std::optional<Uuid>> owner = codec::read_optional_uuid(doc, f::kOwner);
    if (!owner) { return owner.error(); }
    const Result<Preferences> prefs = read_prefs(doc);
    if (!prefs) { return prefs.error(); }

    ClientTarget target{};
    target.id = id.value();
    target.addr.assign(addr.value());
    // Absent for an in-app client, which has nothing to sign or encrypt with.
    if (const bsoncxx::document::element keys = doc[codec::key_of(f::kKeys)];
        keys && keys.type() == bsoncxx::type::k_binary) {
        const bsoncxx::types::b_binary raw = keys.get_binary();
        target.keys.assign(raw.bytes, raw.bytes + raw.size);
    }
    target.owner = owner.value();
    target.prefs = prefs.value();
    target.type = type.value();
    target.digest = digest.value();
    return target;
}

// One `$or` branch per subscribed broadcast topic: an equality on `kind` and on
// `subject`, a floor at the subscription's own `since`, and a ceiling at the
// cursor. Targeted topics are EXCLUDED — those rows were already delivered as
// inbox entries, and including them would show every reader a notification only
// one of them was sent.
// One `$match` filter per subscribed fan-out-on-read topic.
//
// Returned as a LIST rather than as a ready-made `$or`, because the two readers
// of it need different shapes: the count folds them into one `$or` (an unordered
// question, where a union is exactly right), while the page has to bound each
// branch SEPARATELY — see page_broadcast.
[[nodiscard]] std::vector<bsoncxx::document::value> broadcast_branch_filters(
    std::span<const TopicSpec> topics, std::span<const Subscription> subs,
    const std::optional<Uuid>& cursor) {
    std::vector<bsoncxx::document::value> branches;
    branches.reserve(subs.size());
    for (const Subscription& sub : subs) {
        const TopicSpec* spec = topic_spec(topics, sub.kind);
        if (spec == nullptr || spec->fanout != FanOut::Read) { continue; }
        bsoncxx::builder::basic::document branch;
        branch.append(kvp(codec::key_of(f::kKind), bsoncxx::types::b_int32{sub.kind}));
        // A point equality in both cases, including the unscoped one: see
        // notification_body on why `subject: null` would cost the merge.
        branch.append(kvp(codec::key_of(f::kSubject), codec::uuid_bin(sub.subject)));
        branch.append(kvp(codec::key_of(f::kId), [&sub, &cursor](sub_document range) {
            // The floor. A UUIDv7's leading 48 bits are the creation
            // millisecond, so a lower bound on the id IS a lower bound on
            // time — and it rides the index the scan already uses.
            range.append(kvp("$gt", codec::uuid_bin(sub.since)));
            if (cursor.has_value()) {
                range.append(kvp("$lt", codec::uuid_bin(*cursor)));
            }
        }));
        branches.push_back(branch.extract());
    }
    return branches;
}

[[nodiscard]] bsoncxx::array::value branches_as_or(
    const std::vector<bsoncxx::document::value>& branches) {
    bsoncxx::builder::basic::array out;
    for (const bsoncxx::document::value& branch : branches) { out.append(branch.view()); }
    return out.extract();
}


}  // namespace

// --- notifications ----------------------------------------------------------

Status NotificationRepository::insert_notification(mongocxx::client& client,
                                                   const NotificationRow& row) const {
    return repo::guarded([&]() -> Status {
        bind_notifications(client).insert_one(notification_document(row).view());
        return ok();
    });
}

Status NotificationRepository::insert_notification(mongocxx::client& client,
                                                   mongocxx::client_session& session,
                                                   const NotificationRow& row) const {
    return repo::guarded_in_transaction([&]() -> Status {
        bind_notifications(client).insert_one(session, notification_document(row).view());
        return ok();
    });
}

Result<CoalesceOutcome> NotificationRepository::coalesce_notification(
    mongocxx::client& client, const NotificationRow& row) const {
    return repo::guarded([&]() -> Result<CoalesceOutcome> {
        mongocxx::collection collection = bind_notifications(client);

        // `$setOnInsert` for everything that identifies the row, `$inc` for the
        // count. An existing row inside the window keeps its id, its instant and
        // its params — a coalesced repeat is the SAME event happening again, and
        // rewriting the params would make the row describe only the last one.
        bsoncxx::builder::basic::document insert_only;
        codec::append_uuid(insert_only, f::kId, row.id);
        insert_only.append(bsoncxx::builder::concatenate(notification_body(row).view()));

        const bsoncxx::document::value update = make_document(
            kvp("$setOnInsert",
                [&insert_only](sub_document sub) {
                    sub.append(bsoncxx::builder::concatenate(insert_only.view()));
                }),
            kvp("$inc", [](sub_document sub) {
                sub.append(kvp(codec::key_of(f::kCount), bsoncxx::types::b_int32{1}));
            }));

        mongocxx::options::find_one_and_update options{};
        options.upsert(true);
        options.return_document(mongocxx::options::return_document::k_after);
        options.projection(make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1}),
                                         kvp(codec::key_of(f::kCount),
                                             bsoncxx::types::b_int32{1})));

        const auto updated = collection.find_one_and_update(
            make_document(kvp(codec::key_of(f::kDedupe), dedupe_bin(row.dedupe))).view(),
            update.view(), options);
        if (!updated) { return fail(ErrorCode::Internal, f::kDedupe); }

        const Result<Uuid> id = codec::read_uuid(updated->view(), f::kId);
        if (!id) { return id.error(); }
        const Result<std::int32_t> count = codec::read_int32(updated->view(), f::kCount);
        if (!count) { return count.error(); }

        // The row this call upserted carries the id it generated; an existing one
        // carries its own. That comparison IS "was this newly created", and it is
        // exact without a second round trip.
        return CoalesceOutcome{id.value(), count.value(), id.value() == row.id};
    });
}

Result<std::optional<NotificationRow>> NotificationRepository::find_notification(
    mongocxx::client& client, const Uuid& id, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<NotificationRow>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, id);
        // The TTL monitor lags by up to a minute, so an expired notification is
        // still physically present and would still render.
        repo::append_not_expired(filter, f::kExpiresAt, now);

        const auto found = bind_notifications(client).find_one(filter.view());
        if (!found) { return std::optional<NotificationRow>{}; }
        Result<NotificationRow> row = read_notification(found->view());
        if (!row) { return row.error(); }
        return std::optional<NotificationRow>{std::move(row).value()};
    });
}

Result<std::optional<NotificationRow>> NotificationRepository::find_by_dedupe(
    mongocxx::client& client, const std::array<std::uint8_t, 16>& dedupe, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<NotificationRow>> {
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(codec::key_of(f::kDedupe), dedupe_bin(dedupe)));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        const auto found = bind_notifications(client).find_one(filter.view());
        if (!found) { return std::optional<NotificationRow>{}; }
        Result<NotificationRow> row = read_notification(found->view());
        if (!row) { return row.error(); }
        return std::optional<NotificationRow>{std::move(row).value()};
    });
}

Result<std::vector<NotificationRow>> NotificationRepository::find_notifications(
    mongocxx::client& client, std::span<const Uuid> ids, db::TimeMs now) const {
    if (ids.empty()) { return std::vector<NotificationRow>{}; }

    return repo::guarded([&]() -> Result<std::vector<NotificationRow>> {
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(codec::key_of(f::kId), [ids](sub_document sub) {
            sub.append(kvp("$in", [ids](sub_array array) {
                for (const Uuid& id : ids) { array.append(codec::uuid_bin(id)); }
            }));
        }));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        mongocxx::options::find options{};
        options.limit(static_cast<std::int32_t>(ids.size()));

        std::vector<NotificationRow> rows;
        rows.reserve(ids.size());
        for (const bsoncxx::document::view& doc :
             bind_notifications(client).find(filter.view(), options)) {
            Result<NotificationRow> row = read_notification(doc);
            if (!row) { return row.error(); }
            rows.push_back(std::move(row).value());
        }
        return rows;
    });
}

Result<BroadcastPage> NotificationRepository::page_broadcast(
    mongocxx::client& client, std::span<const Subscription> subs,
    const std::optional<Uuid>& cursor, std::int32_t limit, db::TimeMs now) const {
    const std::int32_t bounded = std::clamp(limit, kMinInboxPageSize, kMaxInboxPageSize);

    return repo::guarded([&]() -> Result<BroadcastPage> {
        BroadcastPage page{};
        const std::vector<bsoncxx::document::value> branches =
            broadcast_branch_filters(topics_, subs, cursor);
        // A reader subscribed to no broadcast topic has an empty broadcast half.
        // An empty pipeline would read the whole collection, so it is not built.
        if (branches.empty()) { return page; }

        // --- why this is a union and not one `$or` -------------------------------
        //
        // An `$or` with ONE limit spends that limit on whichever branch the
        // server emits first, and every ordering available to it is the wrong
        // one. Sorted `{kind, subject, _id}` the server merges the branches
        // correctly but in TOPIC order, so the page fills from the lowest topic
        // code and `next_cursor` comes from inside that branch — applied as a
        // `$lt` to every branch on the next page, it excludes every newer row of
        // every other topic PERMANENTLY. Sorted `{_id: -1}` the order is right,
        // but only if the planner happens to choose a SORT_MERGE, and that is a
        // cost-based decision: against a small collection, or a reader who
        // matches a large fraction of rows, it walks the primary key with the
        // topic predicate as a filter instead. A notification silently lost is
        // the one failure this subsystem exists to prevent, and neither shape
        // prevents it.
        //
        // Bounding each branch on its own removes the choice. Every stage is
        // `{kind, subject}` equality plus an `_id` range, which is an IXSCAN on
        // ntf_kind_subject_id with the sort provided by the index — verified to
        // hold against an empty collection as well as a populated one, because a
        // plan that depends on collection size is a plan that changes in
        // production. The closing sort sees at most branches × (bounded + 1)
        // rows, which is the one place a merge in memory is the right answer.
        const std::int32_t per_branch = bounded + 1;
        const auto stage_for = [&](const bsoncxx::document::view& branch) {
            bsoncxx::builder::basic::document match;
            match.append(bsoncxx::builder::concatenate(branch));
            repo::append_not_expired(match, f::kExpiresAt, now);
            return match.extract();
        };
        const bsoncxx::document::value newest_first =
            make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{-1}));

        mongocxx::pipeline stages;
        stages.match(stage_for(branches.front()).view());
        stages.sort(newest_first.view());
        stages.limit(per_branch);
        for (std::size_t i = 1; i < branches.size(); ++i) {
            mongocxx::pipeline arm;
            arm.match(stage_for(branches[i]).view());
            arm.sort(newest_first.view());
            arm.limit(per_branch);
            stages.append_stage(make_document(
                kvp("$unionWith", [this, &arm](sub_document sub) {
                    sub.append(kvp("coll", codec::key_of(collection_name())));
                    sub.append(kvp("pipeline", arm.view_array()));
                })));
        }
        stages.sort(newest_first.view());
        stages.limit(per_branch);

        page.rows.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view& doc : bind_notifications(client).aggregate(stages)) {
            if (page.rows.size() == static_cast<std::size_t>(bounded)) {
                // One more row exists, so there is another page. The cursor is
                // the last row IN THIS PAGE, not the look-ahead row: the filter
                // is a strict `<`, so naming the look-ahead row would exclude it
                // from the next page and skip exactly one notification at every
                // boundary. The look-ahead is not decoded at all — its only job
                // was to answer "is there more".
                page.next_cursor = page.rows.back().id;
                break;
            }
            Result<NotificationRow> row = read_notification(doc);
            if (!row) { return row.error(); }
            page.rows.push_back(std::move(row).value());
        }
        return page;
    });
}

Result<std::int64_t> NotificationRepository::count_broadcast_unread(
    mongocxx::client& client, std::span<const Subscription> subs,
    const std::optional<Uuid>& watermark, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        std::vector<Subscription> bounded;
        bounded.reserve(subs.size());
        for (const Subscription& sub : subs) {
            Subscription copy = sub;
            // Whichever is LATER: a subscription joined after the watermark must
            // not resurrect the history the watermark already covers, and a
            // watermark ahead of the join marker must not be walked back.
            if (watermark.has_value() && *watermark > copy.since) { copy.since = *watermark; }
            bounded.push_back(copy);
        }

        const std::vector<bsoncxx::document::value> branches =
            broadcast_branch_filters(topics_, bounded, std::nullopt);
        if (branches.empty()) { return std::int64_t{0}; }

        // A count is an unordered question, so one `$or` is exactly right here:
        // nothing depends on which branch the server reaches first, and the cap
        // is a cap rather than a page boundary.
        bsoncxx::builder::basic::document filter;
        filter.append(kvp("$or", branches_as_or(branches).view()));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        mongocxx::options::count options{};
        // Bounded, so the cost does not grow with a reader who ignored
        // notifications for a year.
        options.limit(kUnreadCountLimit);
        return bind_notifications(client).count_documents(filter.view(), options);
    });
}

namespace {

// The outbox's own filter, built in one place because two callers ask about it —
// the sweeper that drains it and the gauge that measures it — and a gauge
// describing a near-miss of the queue the sweeper walks is worse than no gauge.
[[nodiscard]] bsoncxx::document::value undispatched_filter(db::TimeMs older_than) {
    // The partial index is on `disp_at: null`, so the filter repeats that
    // predicate verbatim: the planner uses a partial index only when it can
    // prove the query is a subset of it.
    bsoncxx::builder::basic::document filter;
    filter.append(kvp(codec::key_of(f::kDispatchedAt), bsoncxx::types::b_null{}));
    filter.append(kvp(codec::key_of(f::kId), [older_than](sub_document sub) {
        // A UUIDv7's leading 48 bits are the creation millisecond, so an upper
        // bound on the id IS an upper bound on creation time — and it is covered
        // by the index the scan already uses. Comparing created_at instead would
        // add a field that index does not carry.
        const Uuid boundary = uuid::v7_boundary(older_than.time_since_epoch().count());
        sub.append(kvp("$lt", codec::uuid_bin(boundary)));
    }));
    // An expired notification must not be dispatched: delivering an email for
    // something the inbox no longer shows is worse than not delivering it.
    repo::append_not_expired(filter, f::kExpiresAt, db::now_ms());
    return filter.extract();
}

}  // namespace

Result<std::vector<Uuid>> NotificationRepository::undispatched(mongocxx::client& client,
                                                               db::TimeMs older_than,
                                                               std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<Uuid>> {
        const bsoncxx::document::value filter = undispatched_filter(older_than);

        mongocxx::options::find options{};
        options.sort(make_document(kvp(codec::key_of(f::kDispatchedAt),
                                       bsoncxx::types::b_int32{1}),
                                   kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1})));
        options.limit(limit);
        options.projection(make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1})));

        std::vector<Uuid> ids;
        ids.reserve(static_cast<std::size_t>(limit));
        for (const bsoncxx::document::view& doc :
             bind_notifications(client).find(filter.view(), options)) {
            const Result<Uuid> id = codec::read_uuid(doc, f::kId);
            if (!id) { return id.error(); }
            ids.push_back(id.value());
        }
        return ids;
    });
}

Result<std::int64_t> NotificationRepository::count_undispatched(mongocxx::client& client,
                                                                db::TimeMs older_than,
                                                                std::int64_t cap) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        const bsoncxx::document::value filter = undispatched_filter(older_than);
        mongocxx::options::count options{};
        // Bounded, so measuring the backlog cannot itself become the expensive
        // thing a growing backlog does to this deployment.
        options.limit(cap);
        return bind_notifications(client).count_documents(filter.view(), options);
    });
}

Status NotificationRepository::mark_dispatched(mongocxx::client& client, const Uuid& id,
                                               db::TimeMs at) const {
    return repo::guarded([&]() -> Status {
        // versioned-write-exempt: one idempotent `$set` of a marker nobody else
        // writes. Running it twice sets the same field to a near-identical
        // instant, which is exactly what a re-run sweeper is expected to do.
        const auto updated = bind_notifications(client).update_one(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id))).view(),
            make_document(kvp("$set", [at](sub_document sub) {
                codec::append_time(sub, f::kDispatchedAt, at);
            })).view());
        if (!updated) { return fail(ErrorCode::Internal, f::kId); }
        return ok();
    });
}

// --- notification_inbox ------------------------------------------------------

Result<std::int64_t> NotificationRepository::fan_out(mongocxx::client& client, const Uuid& nid,
                                                     TopicCode kind,
                                                     std::span<const Uuid> recipients,
                                                     db::TimeMs expires_at) const {
    if (recipients.empty()) { return std::int64_t{0}; }

    return repo::guarded([&]() -> Result<std::int64_t> {
        std::vector<bsoncxx::document::value> documents;
        documents.reserve(recipients.size());
        for (const Uuid& uid : recipients) {
            bsoncxx::builder::basic::document doc;
            // UUIDv7, so the inbox scan's sort key appends rather than splitting
            // random B-tree pages.
            codec::append_uuid(doc, f::kId, uuid::generate_v7());
            codec::append_uuid(doc, f::kUid, uid);
            codec::append_uuid(doc, f::kNid, nid);
            doc.append(kvp(codec::key_of(f::kKind), bsoncxx::types::b_int32{kind}));
            doc.append(kvp(codec::key_of(f::kReadAt), bsoncxx::types::b_null{}));
            codec::append_time(doc, f::kExpiresAt, expires_at);
            documents.push_back(doc.extract());
        }

        std::vector<bsoncxx::document::view> views;
        views.reserve(documents.size());
        for (const bsoncxx::document::value& doc : documents) { views.push_back(doc.view()); }

        mongocxx::options::insert options{};
        // UNORDERED: a duplicate {uid, nid} must not abort the rest of the batch.
        // That is what makes a partially completed fan-out safe to re-run, which
        // is exactly what the outbox sweeper does after a crash.
        options.ordered(false);

        try {
            const auto result = bind_inbox(client).insert_many(views, options);
            return result ? static_cast<std::int64_t>(result->inserted_count()) : 0;
        } catch (const mongocxx::bulk_write_exception& e) {
            // Duplicate keys are the EXPECTED outcome of a re-run, not a failure:
            // that is the whole point of the {uid, nid} unique index. Anything
            // else in the batch is a real fault and is reported, because a
            // silently half-written fan-out is a notification some readers never
            // receive.
            const auto& raw = e.raw_server_error();
            if (!raw) { return fail(ErrorCode::Internal, f::kNid); }
            const bsoncxx::document::view view = raw->view();

            std::int64_t inserted = 0;
            if (const bsoncxx::document::element count = view["nInserted"];
                count && count.type() == bsoncxx::type::k_int32) {
                inserted = count.get_int32().value;
            }
            if (const bsoncxx::document::element errors = view["writeErrors"];
                errors && errors.type() == bsoncxx::type::k_array) {
                for (const bsoncxx::array::element& entry : errors.get_array().value) {
                    if (entry.type() != bsoncxx::type::k_document) {
                        return fail(ErrorCode::Internal, f::kNid);
                    }
                    const bsoncxx::document::element code = entry.get_document().value["code"];
                    if (!code || code.type() != bsoncxx::type::k_int32 ||
                        code.get_int32().value != kDuplicateKey) {
                        return fail(ErrorCode::Internal, f::kNid);
                    }
                }
            }
            return inserted;
        }
    });
}

Result<InboxPage> NotificationRepository::page_inbox(mongocxx::client& client, const Uuid& uid,
                                                     const std::optional<Uuid>& cursor,
                                                     std::int32_t limit, db::TimeMs now) const {
    const std::int32_t bounded = std::clamp(limit, kMinInboxPageSize, kMaxInboxPageSize);

    return repo::guarded([&]() -> Result<InboxPage> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUid, uid);
        if (cursor.has_value()) {
            filter.append(kvp(codec::key_of(f::kId), [&cursor](sub_document sub) {
                sub.append(kvp("$lt", codec::uuid_bin(*cursor)));
            }));
        }
        repo::append_not_expired(filter, f::kExpiresAt, now);

        mongocxx::options::find options{};
        options.sort(make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{-1})));
        options.limit(bounded + 1);

        InboxPage page{};
        page.rows.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view& doc :
             bind_inbox(client).find(filter.view(), options)) {
            if (page.rows.size() == static_cast<std::size_t>(bounded)) {
                // The last row IN THIS PAGE, not the look-ahead row. See
                // page_broadcast.
                page.next_cursor = page.rows.back().id;
                break;
            }
            const Result<InboxRow> row = read_inbox(doc);
            if (!row) { return row.error(); }
            page.rows.push_back(row.value());
        }
        return page;
    });
}

Result<std::int64_t> NotificationRepository::count_targeted_unread(mongocxx::client& client,
                                                                   const Uuid& uid,
                                                                   db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        // The partial index is {uid, read_at} filtered on `read_at: null`, and the
        // filter repeats the null equality so the planner can use it.
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUid, uid);
        filter.append(kvp(codec::key_of(f::kReadAt), bsoncxx::types::b_null{}));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        mongocxx::options::count options{};
        options.limit(kUnreadCountLimit);
        return bind_inbox(client).count_documents(filter.view(), options);
    });
}

Result<std::int64_t> NotificationRepository::mark_read_up_to(mongocxx::client& client,
                                                             const Uuid& uid, const Uuid& up_to,
                                                             db::TimeMs at) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        // versioned-write-exempt: an inbox row belongs to ONE reader, and marking
        // one read twice sets the same field. There is nothing for a second
        // writer to lose.
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUid, uid);
        filter.append(kvp(codec::key_of(f::kId), [&up_to](sub_document sub) {
            sub.append(kvp("$lte", codec::uuid_bin(up_to)));
        }));
        // Already-read rows are excluded so the instant records when a row was
        // FIRST read rather than when it was last acknowledged.
        filter.append(kvp(codec::key_of(f::kReadAt), bsoncxx::types::b_null{}));

        const auto updated = bind_inbox(client).update_many(
            filter.view(), make_document(kvp("$set", [at](sub_document sub) {
                               codec::append_time(sub, f::kReadAt, at);
                           })).view());
        if (!updated) { return fail(ErrorCode::Internal, f::kUid); }
        return static_cast<std::int64_t>(updated->modified_count());
    });
}

Result<std::int64_t> NotificationRepository::mark_read_ids(mongocxx::client& client,
                                                           const Uuid& uid,
                                                           std::span<const Uuid> ids,
                                                           db::TimeMs at) const {
    if (ids.empty()) { return std::int64_t{0}; }

    return repo::guarded([&]() -> Result<std::int64_t> {
        // versioned-write-exempt: see mark_read_up_to.
        //
        // `uid` is in the filter and not a check on the result: a reader marking
        // somebody else's row read must match nothing, rather than match and then
        // be rejected after the write.
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUid, uid);
        filter.append(kvp(codec::key_of(f::kId), [ids](sub_document sub) {
            sub.append(kvp("$in", [ids](sub_array array) {
                for (const Uuid& id : ids) { array.append(codec::uuid_bin(id)); }
            }));
        }));
        filter.append(kvp(codec::key_of(f::kReadAt), bsoncxx::types::b_null{}));

        const auto updated = bind_inbox(client).update_many(
            filter.view(), make_document(kvp("$set", [at](sub_document sub) {
                               codec::append_time(sub, f::kReadAt, at);
                           })).view());
        if (!updated) { return fail(ErrorCode::Internal, f::kUid); }
        return static_cast<std::int64_t>(updated->modified_count());
    });
}

Result<std::int64_t> NotificationRepository::forget_inbox_rows(mongocxx::client& client,
                                                               const Uuid& uid,
                                                               std::span<const Uuid> ids) const {
    if (ids.empty()) { return std::int64_t{0}; }

    return repo::guarded([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUid, uid);
        filter.append(kvp(codec::key_of(f::kId), [ids](sub_document sub) {
            sub.append(kvp("$in", [ids](sub_array array) {
                for (const Uuid& id : ids) { array.append(codec::uuid_bin(id)); }
            }));
        }));

        const auto deleted = bind_inbox(client).delete_many(filter.view());
        if (!deleted) { return fail(ErrorCode::Internal, f::kUid); }
        return static_cast<std::int64_t>(deleted->deleted_count());
    });
}

// --- notification_clients ----------------------------------------------------

Status NotificationRepository::insert_client(mongocxx::client& client,
                                             const ClientRow& row) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document doc;
        codec::append_uuid(doc, f::kId, row.id);
        codec::append_optional_uuid(doc, f::kOwner, row.owner);
        codec::append_enum(doc, f::kType, row.type);
        doc.append(kvp(codec::key_of(f::kAddr), text_of(row.addr)));
        // SEALED before it arrives. This layer never sees a plaintext webhook
        // secret or push key, and there is no read path that decrypts one back.
        if (row.keys.empty()) {
            doc.append(kvp(codec::key_of(f::kKeys), bsoncxx::types::b_null{}));
        } else {
            doc.append(kvp(codec::key_of(f::kKeys),
                           bsoncxx::types::b_binary{
                               bsoncxx::binary_sub_type::k_binary,
                               static_cast<std::uint32_t>(row.keys.size()), row.keys.data()}));
        }
        append_subscriptions(doc, row.subs);
        const std::array<std::uint8_t, kPrefBytes> prefs = row.prefs.to_bytes();
        doc.append(kvp(codec::key_of(f::kPrefs), codec::bytes_bin(prefs)));
        doc.append(kvp(codec::key_of(f::kFailures), bsoncxx::types::b_int32{row.fail_n}));
        codec::append_optional_time(doc, f::kDisabledAt, row.disabled_at);
        codec::append_optional_time(doc, f::kLastDeliveryAt, row.last_delivery_at);
        codec::append_enum(doc, f::kLastVerdict, row.last_verdict);
        codec::append_enum(doc, f::kDigest, row.digest);
        codec::append_optional_uuid(doc, f::kWatermark, row.broadcast_watermark);
        codec::append_time(doc, f::kCreatedAt, row.created_at);
        repo::append_initial_version(doc);

        bind_clients(client).insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<ClientRow>> NotificationRepository::find_client(mongocxx::client& client,
                                                                     const Uuid& id) const {
    return repo::guarded([&]() -> Result<std::optional<ClientRow>> {
        const auto found = bind_clients(client).find_one(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id))).view());
        if (!found) { return std::optional<ClientRow>{}; }
        Result<ClientRow> row = read_client(found->view());
        if (!row) { return row.error(); }
        return std::optional<ClientRow>{std::move(row).value()};
    });
}

Result<std::vector<ClientRow>> NotificationRepository::clients_of(mongocxx::client& client,
                                                                  const Uuid& owner,
                                                                  std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<ClientRow>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kOwner, owner);

        mongocxx::options::find options{};
        options.sort(make_document(kvp(codec::key_of(f::kType), bsoncxx::types::b_int32{1}),
                                   kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1})));
        options.limit(limit);

        std::vector<ClientRow> rows;
        rows.reserve(static_cast<std::size_t>(limit));
        for (const bsoncxx::document::view& doc :
             bind_clients(client).find(filter.view(), options)) {
            Result<ClientRow> row = read_client(doc);
            if (!row) { return row.error(); }
            rows.push_back(std::move(row).value());
        }
        return rows;
    });
}

Result<std::optional<ClientRow>> NotificationRepository::inapp_client(mongocxx::client& client,
                                                                      const Uuid& owner) const {
    return repo::guarded([&]() -> Result<std::optional<ClientRow>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kOwner, owner);
        codec::append_enum(filter, f::kType, ClientType::InApp);

        const auto found = bind_clients(client).find_one(filter.view());
        if (!found) { return std::optional<ClientRow>{}; }
        Result<ClientRow> row = read_client(found->view());
        if (!row) { return row.error(); }
        return std::optional<ClientRow>{std::move(row).value()};
    });
}

Result<std::int64_t> NotificationRepository::replace_subscriptions(
    mongocxx::client& client, const Uuid& id, std::int64_t expected_version,
    const SubscriptionSet& subs) const {
    mongocxx::collection collection = bind_clients(client);
    bsoncxx::builder::basic::document body;
    append_subscriptions(body, subs);
    const bsoncxx::document::value identity =
        make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id)));
    return repo::update_versioned(collection, identity.view(), expected_version, body.view());
}

Result<std::int64_t> NotificationRepository::replace_preferences(
    mongocxx::client& client, const Uuid& id, std::int64_t expected_version,
    const Preferences& prefs) const {
    mongocxx::collection collection = bind_clients(client);
    const std::array<std::uint8_t, kPrefBytes> bytes = prefs.to_bytes();
    const bsoncxx::document::value body =
        make_document(kvp(codec::key_of(f::kPrefs), codec::bytes_bin(bytes)));
    const bsoncxx::document::value identity =
        make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id)));
    return repo::update_versioned(collection, identity.view(), expected_version, body.view());
}

Result<std::int64_t> NotificationRepository::replace_enabled(mongocxx::client& client,
                                                             const Uuid& id,
                                                             std::int64_t expected_version,
                                                             bool enabled,
                                                             db::TimeMs at) const {
    mongocxx::collection collection = bind_clients(client);
    bsoncxx::builder::basic::document body;
    // Enabling CLEARS the failure streak as well. An operator who re-enables a
    // webhook after fixing it and is disabled again by the count that disabled it
    // the first time has been given a control that does nothing.
    if (enabled) {
        body.append(kvp(codec::key_of(f::kDisabledAt), bsoncxx::types::b_null{}));
        body.append(kvp(codec::key_of(f::kFailures), bsoncxx::types::b_int32{0}));
    } else {
        codec::append_time(body, f::kDisabledAt, at);
    }
    const bsoncxx::document::value identity =
        make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id)));
    return repo::update_versioned(collection, identity.view(), expected_version, body.view());
}

Result<std::vector<ClientTarget>> NotificationRepository::subscribers(
    mongocxx::client& client, const TopicRef& topic, const std::optional<Uuid>& after,
    std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<ClientTarget>> {
        // $elemMatch, NOT two independent dotted predicates. `subs.kind == k AND
        // subs.subject == s` matches a client whose array holds {k, other} and
        // {otherk, s} — which is a different client from the one that subscribed
        // to (k, s). The distinction is the difference between delivering a
        // staff-only topic to the right client and to a wrong one.
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(codec::key_of(f::kSubs), [&topic](sub_document sub) {
            sub.append(kvp("$elemMatch", [&topic](sub_document match) {
                match.append(kvp(codec::key_of(f::kSubKind),
                                 bsoncxx::types::b_int32{topic.kind}));
                if (is_scoped(topic)) {
                    match.append(kvp(codec::key_of(f::kSubSubject),
                                     codec::uuid_bin(topic.subject)));
                } else {
                    match.append(kvp(codec::key_of(f::kSubSubject), bsoncxx::types::b_null{}));
                }
            }));
        }));
        // Excluded in the FILTER, not after the fact: a disabled endpoint must not
        // even be read, let alone delivered to.
        filter.append(kvp(codec::key_of(f::kDisabledAt), bsoncxx::types::b_null{}));
        if (after.has_value()) {
            filter.append(kvp(codec::key_of(f::kId), [&after](sub_document sub) {
                sub.append(kvp("$gt", codec::uuid_bin(*after)));
            }));
        }

        mongocxx::options::find options{};
        options.sort(make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1})));
        options.limit(limit);
        options.projection(target_projection());

        std::vector<ClientTarget> targets;
        targets.reserve(static_cast<std::size_t>(limit));
        for (const bsoncxx::document::view& doc :
             bind_clients(client).find(filter.view(), options)) {
            Result<ClientTarget> target = read_target(doc);
            if (!target) { return target.error(); }
            targets.push_back(std::move(target).value());
        }
        return targets;
    });
}

Result<std::int32_t> NotificationRepository::record_delivery_failure(
    mongocxx::client& client, const Uuid& id, DeliveryVerdict verdict, db::TimeMs at) const {
    return repo::guarded([&]() -> Result<std::int32_t> {
        // versioned-write-exempt: one atomic `$inc`, so N concurrent failures
        // produce N distinct counts and none is lost. A version filter would make
        // two simultaneous transport failures record one.
        const bsoncxx::document::value update = make_document(
            kvp("$inc",
                [](sub_document sub) {
                    sub.append(kvp(codec::key_of(f::kFailures), bsoncxx::types::b_int32{1}));
                }),
            // Recorded by the SAME operation that moves the counter. A second
            // write beside it would double the write volume of a path that runs
            // once per subscriber per broadcast, to record when something the
            // first write already described happened.
            kvp("$set", [verdict, at](sub_document sub) {
                codec::append_time(sub, f::kLastDeliveryAt, at);
                codec::append_enum(sub, f::kLastVerdict, verdict);
            }));

        mongocxx::options::find_one_and_update options{};
        options.return_document(mongocxx::options::return_document::k_after);
        options.projection(
            make_document(kvp(codec::key_of(f::kFailures), bsoncxx::types::b_int32{1})));

        const auto updated = bind_clients(client).find_one_and_update(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id))).view(),
            update.view(), options);
        if (!updated) { return fail(ErrorCode::NotFound, f::kId); }
        return codec::read_int32(updated->view(), f::kFailures);
    });
}

Status NotificationRepository::clear_delivery_failures(mongocxx::client& client, const Uuid& id,
                                                       db::TimeMs at) const {
    return repo::guarded([&]() -> Status {
        // versioned-write-exempt: resetting a soft-failure counter after a success
        // is idempotent and order-independent.
        const auto updated = bind_clients(client).update_one(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id))).view(),
            make_document(kvp("$set", [at](sub_document sub) {
                sub.append(kvp(codec::key_of(f::kFailures), bsoncxx::types::b_int32{0}));
                codec::append_time(sub, f::kLastDeliveryAt, at);
                codec::append_enum(sub, f::kLastVerdict, DeliveryVerdict::Delivered);
            })).view());
        if (!updated) { return fail(ErrorCode::Internal, f::kId); }
        return ok();
    });
}

Status NotificationRepository::disable_client(mongocxx::client& client, const Uuid& id,
                                              db::TimeMs at, DeliveryVerdict verdict) const {
    return repo::guarded([&]() -> Status {
        // versioned-write-exempt: disabling is idempotent, and it is driven by a
        // transport that has already been told the endpoint is gone. Making it
        // wait for a version would mean a dead endpoint stayed enabled because
        // two workers learned it was dead at once.
        const auto updated = bind_clients(client).update_one(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id))).view(),
            make_document(kvp("$set", [at, verdict](sub_document sub) {
                codec::append_time(sub, f::kDisabledAt, at);
                codec::append_time(sub, f::kLastDeliveryAt, at);
                codec::append_enum(sub, f::kLastVerdict, verdict);
            })).view());
        if (!updated) { return fail(ErrorCode::Internal, f::kId); }
        return ok();
    });
}

Result<bool> NotificationRepository::delete_client(mongocxx::client& client, const Uuid& id,
                                                   const std::optional<Uuid>& owner) const {
    return repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, id);
        // Ownership is part of the FILTER, never a check on what came back:
        // deleting somebody else's endpoint must match nothing, so the answer is
        // byte-identical to deleting one that does not exist.
        if (owner.has_value()) { codec::append_uuid(filter, f::kOwner, *owner); }

        const auto deleted = bind_clients(client).delete_one(filter.view());
        if (!deleted) { return fail(ErrorCode::Internal, f::kId); }
        return deleted->deleted_count() == 1;
    });
}

Result<std::vector<WebhookRow>> NotificationRepository::list_webhooks(mongocxx::client& client,
                                                                      std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<WebhookRow>> {
        bsoncxx::builder::basic::document filter;
        codec::append_enum(filter, f::kType, ClientType::Webhook);

        mongocxx::options::find options{};
        options.sort(make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1})));
        options.limit(limit);
        // `keys` is NOT projected. The listing type has no field to put a sealed
        // secret in, and the query does not fetch one either — so "shown once at
        // registration" is a property of two layers rather than of a handler that
        // remembers to strip it.
        options.projection(make_document(
            kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kAddr), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kSubs), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kFailures), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kDisabledAt), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kLastDeliveryAt), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kLastVerdict), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kCreatedAt), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(repo::kVersionField), bsoncxx::types::b_int32{1})));

        std::vector<WebhookRow> rows;
        rows.reserve(static_cast<std::size_t>(limit));
        for (const bsoncxx::document::view& doc :
             bind_clients(client).find(filter.view(), options)) {
            const Result<Uuid> id = codec::read_uuid(doc, f::kId);
            if (!id) { return id.error(); }
            const Result<std::string_view> url = codec::read_text(doc, f::kAddr);
            if (!url) { return url.error(); }
            const Result<std::optional<db::TimeMs>> disabled =
                codec::read_optional_time(doc, f::kDisabledAt);
            if (!disabled) { return disabled.error(); }
            const Result<std::optional<db::TimeMs>> last =
                codec::read_optional_time(doc, f::kLastDeliveryAt);
            if (!last) { return last.error(); }
            const Result<DeliveryVerdict> verdict =
                codec::read_enum(doc, f::kLastVerdict, kMaxDeliveryVerdict);
            if (!verdict) { return verdict.error(); }
            const Result<db::TimeMs> created = codec::read_time(doc, f::kCreatedAt);
            if (!created) { return created.error(); }
            const Result<std::int64_t> version = repo::document_version(doc);
            if (!version) { return version.error(); }
            const Result<std::int32_t> failures = codec::read_int32(doc, f::kFailures);
            if (!failures) { return failures.error(); }
            Result<SubscriptionSet> subs = read_subscriptions(doc);
            if (!subs) { return subs.error(); }

            WebhookRow row{};
            row.url.assign(url.value());
            row.subs = std::move(subs).value();
            row.last_delivery_at = last.value();
            row.id = id.value();
            row.created_at = created.value();
            row.version = version.value();
            row.fail_n = failures.value();
            row.enabled = !disabled.value().has_value();
            row.last_verdict = verdict.value();
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

// --- read state --------------------------------------------------------------

Status NotificationRepository::advance_watermark(mongocxx::client& client, const Uuid& owner,
                                                 const Uuid& up_to) const {
    return repo::guarded([&]() -> Status {
        // versioned-write-exempt: `$max` is what makes this correct without a
        // version. A watermark only ever moves FORWARD, and two concurrent marks
        // must leave the later one standing — a read-modify-write would let the
        // earlier one win and silently un-read everything between them.
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kOwner, owner);
        codec::append_enum(filter, f::kType, ClientType::InApp);

        const auto updated = bind_clients(client).update_one(
            filter.view(), make_document(kvp("$max", [&up_to](sub_document sub) {
                               sub.append(kvp(codec::key_of(f::kWatermark),
                                              codec::uuid_bin(up_to)));
                           })).view());
        if (!updated) { return fail(ErrorCode::Internal, f::kWatermark); }
        return ok();
    });
}

Result<ReadState> NotificationRepository::read_state(mongocxx::client& client,
                                                     const Uuid& owner) const {
    return repo::guarded([&]() -> Result<ReadState> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kOwner, owner);
        codec::append_enum(filter, f::kType, ClientType::InApp);

        // Only the watermark comes back. Reading a whole client row — a 64-entry
        // subscription array and a sealed key blob — to look at one id is
        // network, BSON decode CPU and heap for data this never touches.
        mongocxx::options::find options{};
        options.projection(
            make_document(kvp(codec::key_of(f::kWatermark), bsoncxx::types::b_int32{1})));

        const auto found = bind_clients(client).find_one(filter.view(), options);
        ReadState state{};
        if (!found) { return state; }
        const Result<std::optional<Uuid>> watermark =
            codec::read_optional_uuid(found->view(), f::kWatermark);
        if (!watermark) { return watermark.error(); }
        state.broadcast_watermark = watermark.value();
        return state;
    });
}

Result<Uuid> NotificationRepository::ensure_client(mongocxx::client& client, ClientType type,
                                                   const Uuid& owner, std::string_view addr,
                                                   std::span<const TopicCode> defaults,
                                                   db::TimeMs now) const {
    const Result<std::optional<ClientRow>> existing =
        type == ClientType::InApp ? inapp_client(client, owner)
                                  : Result<std::optional<ClientRow>>{std::optional<ClientRow>{}};
    if (!existing) { return existing.error(); }
    if (existing.value().has_value()) { return existing.value()->id; }

    ClientRow row{};
    row.id = uuid::generate_v4();
    row.addr.assign(addr);
    row.owner = owner;
    row.created_at = now;
    row.version = repo::kInitialVersion;
    // Every bit set, so a topic added by a later deploy arrives ENABLED rather
    // than silently muted for everyone who registered before it existed.
    row.prefs = Preferences::all_enabled();
    row.type = type;
    row.digest = DigestMode::None;
    row.last_verdict = DeliveryVerdict::Delivered;

    // A client subscribed to NOTHING receives nothing, and it presents as an inbox
    // that comes back empty because the read had no branch to build rather than
    // because there was no news. Which topics a fresh client gets is the
    // application's policy, so the set arrives as a parameter.
    const Uuid since = uuid::v7_boundary(now.time_since_epoch().count());
    for (const TopicCode kind : defaults) {
        const TopicSpec* spec = topic_spec(topics_, kind);
        if (spec == nullptr) { continue; }
        // A resource-scoped topic has no blanket subscription by construction:
        // whoever owns the resource subscribes deliberately, because subscribing
        // to a permission-gated topic IS the disclosure.
        if (spec->scope == Scope::Resource) { continue; }
        Subscription sub{};
        sub.subject = spec->scope == Scope::Account ? owner : kNilUuid;
        sub.since = since;
        sub.kind = kind;
        if (!row.subs.push(sub)) { break; }
    }

    // Insert-if-absent, tolerating the duplicate-key error rather than reading
    // first: a read-then-write races two concurrent first reads, and the loser
    // silently replaces the winner's subscriptions.
    const Status inserted = insert_client(client, row);
    if (inserted) { return row.id; }
    if (inserted.code() != ErrorCode::Conflict) { return inserted.error(); }

    const Result<std::optional<ClientRow>> raced = inapp_client(client, owner);
    if (!raced) { return raced.error(); }
    if (!raced.value().has_value()) { return fail(ErrorCode::Internal, f::kOwner); }
    return raced.value()->id;
}

}  // namespace anvil::notifications
