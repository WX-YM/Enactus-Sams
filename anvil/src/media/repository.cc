// versioned-write-exempt: a media row has no `v` field and is never
// read-modify-written. The reference count moves by $inc inside the owning
// document's transaction — which is stronger than a version filter, because it
// is correct under concurrency without any re-read at all — and the two writes
// that could race, delete_if_unreferenced and clear_refs_if, carry the count
// they expect in their own filter.

#include "anvil/media/repository.h"

#include <string>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_delete.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/pipeline.hpp>

#include "anvil/analytics/counters.h"

namespace anvil::media {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = media_fields;

// Aggregation field references. Spelled once: a `$` prefix typo produces a group
// key of the literal string rather than of the field, which aggregates every row
// into one and looks like a working query.
constexpr std::string_view kAggBytes = "bytes";
constexpr std::string_view kAggCount = "count";
constexpr std::string_view kBytesRef = "$b";
constexpr std::string_view kUploaderIpRef = "$uip";

[[nodiscard]] bsoncxx::types::b_int32 ns_value(fs::Ns ns) noexcept {
    return bsoncxx::types::b_int32{ns.stored()};
}

[[nodiscard]] mongocxx::options::find media_projection() {
    mongocxx::options::find options{};
    options.projection(make_document(
        kvp(codec::key_of(f::kNamespace), 1), kvp(codec::key_of(f::kSha256), 1),
        kvp(codec::key_of(f::kOwner), 1), kvp(codec::key_of(f::kUploaderIp), 1),
        kvp(codec::key_of(f::kBytes), 1), kvp(codec::key_of(f::kWidth), 1),
        kvp(codec::key_of(f::kHeight), 1), kvp(codec::key_of(f::kMime), 1),
        kvp(codec::key_of(f::kRefs), 1), kvp(codec::key_of(f::kVariants), 1),
        kvp(codec::key_of(f::kCreatedAt), 1)));
    return options;
}

[[nodiscard]] Result<std::vector<images::VariantRecord>> decode_variants(
    const bsoncxx::document::view& doc) {
    std::vector<images::VariantRecord> variants;
    auto element = doc.find(codec::key_of(f::kVariants));
    // Absent is EMPTY, not an error: an object whose every encode was skipped
    // because this build has no delegate for the format is a legitimate row that
    // serves its master.
    if (element == doc.end()) { return variants; }
    if (element->type() != bsoncxx::type::k_array) {
        return fail(ErrorCode::Internal, f::kVariants);
    }

    for (const bsoncxx::array::element& entry : element->get_array().value) {
        if (entry.type() != bsoncxx::type::k_document) {
            return fail(ErrorCode::Internal, f::kVariants);
        }
        const bsoncxx::document::view row = entry.get_document().value;

        const Result<std::int32_t> width = codec::read_int32(row, f::kVariantWidth);
        if (!width) { return width.error(); }
        const Result<std::int32_t> height = codec::read_int32(row, f::kVariantHeight);
        if (!height) { return height.error(); }
        const Result<std::int32_t> format = codec::read_int32(row, f::kVariantFormat);
        if (!format) { return format.error(); }
        const Result<std::int64_t> bytes = codec::read_int64(row, f::kVariantBytes);
        if (!bytes) { return bytes.error(); }

        // Range-checked, never cast. A stored format outside the pipeline's own
        // set would build a path with an extension nothing wrote, and the
        // redirect would name a file Nginx cannot find — a clean 404 turning
        // into a failure inside the proxy.
        if (format.value() < 0 ||
            format.value() > static_cast<std::int32_t>(fs::Format::Webp)) {
            return fail(ErrorCode::Internal, f::kVariantFormat);
        }
        if (width.value() <= 0 || width.value() > 65535 || height.value() <= 0 ||
            height.value() > 65535 || bytes.value() < 0) {
            return fail(ErrorCode::Internal, f::kVariants);
        }

        variants.push_back(images::VariantRecord{
            .bytes = static_cast<std::uint32_t>(bytes.value()),
            .width = static_cast<std::uint16_t>(width.value()),
            .height = static_cast<std::uint16_t>(height.value()),
            .format = static_cast<fs::Format>(format.value()),
        });
    }
    return variants;
}

[[nodiscard]] Result<MediaRecord> decode(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<std::int32_t> stored_ns = codec::read_int32(doc, f::kNamespace);
    if (!stored_ns) { return stored_ns.error(); }
    const std::optional<fs::Ns> ns = fs::Ns::from_stored(stored_ns.value());
    if (!ns.has_value()) { return fail(ErrorCode::Internal, f::kNamespace); }
    const Result<crypto::Digest256> sha256 = codec::read_digest(doc, f::kSha256);
    if (!sha256) { return sha256.error(); }
    const Result<Uuid> owner = codec::read_uuid(doc, f::kOwner);
    if (!owner) { return owner.error(); }
    const Result<std::int64_t> bytes = codec::read_int64(doc, f::kBytes);
    if (!bytes) { return bytes.error(); }
    if (bytes.value() < 0) { return fail(ErrorCode::Internal, f::kBytes); }
    const Result<std::int32_t> width = codec::read_int32(doc, f::kWidth);
    if (!width) { return width.error(); }
    const Result<std::int32_t> height = codec::read_int32(doc, f::kHeight);
    if (!height) { return height.error(); }
    if (width.value() < 0 || height.value() < 0) { return fail(ErrorCode::Internal, f::kWidth); }
    const Result<fs::Mime> mime = codec::read_enum(doc, f::kMime, fs::kMaxMime);
    if (!mime) { return mime.error(); }
    const Result<std::int32_t> refs = codec::read_int32(doc, f::kRefs);
    if (!refs) { return refs.error(); }
    const Result<db::TimeMs> created = codec::read_time(doc, f::kCreatedAt);
    if (!created) { return created.error(); }
    Result<std::vector<images::VariantRecord>> variants = decode_variants(doc);
    if (!variants) { return variants.error(); }

    MediaRecord record{
        .variants = std::move(variants).value(),
        .sha256 = sha256.value(),
        .created_at = created.value(),
        .bytes = static_cast<std::uint64_t>(bytes.value()),
        .id = id.value(),
        .owner = owner.value(),
        .uploader_ip = std::nullopt,
        .width = static_cast<std::uint32_t>(width.value()),
        .height = static_cast<std::uint32_t>(height.value()),
        .refs = refs.value(),
        .ns = *ns,
        .mime = mime.value(),
    };

    // Absent rather than null on disk, so a missing element is the ordinary
    // case. Decoding it as the all-zero address would make every row without one
    // look like a single sender.
    if (doc.find(codec::key_of(f::kUploaderIp)) != doc.end()) {
        std::array<std::uint8_t, 16> ip{};
        const Status read = codec::read_bytes(doc, f::kUploaderIp, ip);
        if (!read) { return read.error(); }
        record.uploader_ip = ip;
    }
    return record;
}

}  // namespace

Status MediaRepository::insert(mongocxx::client& client, const NewMedia& media) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document doc;
        codec::append_uuid(doc, f::kId, media.id);
        doc.append(kvp(codec::key_of(f::kNamespace), ns_value(media.ns)));
        codec::append_digest(doc, f::kSha256, media.sha256);
        codec::append_uuid(doc, f::kOwner, media.owner);
        // OMITTED when absent. The {ns, ip} index is partial on this field
        // existing, and a null on every account-backed upload would put one
        // entry per row into an index that exists for the minority of rows that
        // have an address at all.
        if (media.uploader_ip.has_value()) {
            doc.append(kvp(codec::key_of(f::kUploaderIp), codec::bytes_bin(*media.uploader_ip)));
        }
        codec::append_int64(doc, f::kBytes, static_cast<std::int64_t>(media.bytes));
        doc.append(kvp(codec::key_of(f::kWidth),
                       bsoncxx::types::b_int32{static_cast<std::int32_t>(media.width)}));
        doc.append(kvp(codec::key_of(f::kHeight),
                       bsoncxx::types::b_int32{static_cast<std::int32_t>(media.height)}));
        codec::append_enum(doc, f::kMime, media.mime);
        doc.append(kvp(codec::key_of(f::kRefs), bsoncxx::types::b_int32{0}));
        doc.append(kvp(codec::key_of(f::kVariants), [&media](sub_array rows) {
            for (const images::VariantRecord& variant : media.variants) {
                rows.append([&variant](sub_document row) {
                    row.append(kvp(codec::key_of(f::kVariantWidth),
                                   bsoncxx::types::b_int32{variant.width}));
                    row.append(kvp(codec::key_of(f::kVariantHeight),
                                   bsoncxx::types::b_int32{variant.height}));
                    row.append(kvp(codec::key_of(f::kVariantFormat),
                                   bsoncxx::types::b_int32{
                                       static_cast<std::int32_t>(variant.format)}));
                    row.append(kvp(codec::key_of(f::kVariantBytes),
                                   bsoncxx::types::b_int64{
                                       static_cast<std::int64_t>(variant.bytes)}));
                });
            }
        }));
        codec::append_time(doc, f::kCreatedAt, db::now_ms());

        mongocxx::collection media_collection = bind(client);
        media_collection.insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<MediaRecord>> MediaRepository::find(mongocxx::client& client, fs::Ns ns,
                                                         const Uuid& id) const {
    return repo::guarded([&]() -> Result<std::optional<MediaRecord>> {
        mongocxx::collection media_collection = bind(client);
        const auto found = media_collection.find_one(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id)),
                          kvp(codec::key_of(f::kNamespace), ns_value(ns)))
                .view(),
            media_projection());
        if (!found) { return std::optional<MediaRecord>{}; }
        Result<MediaRecord> record = decode(found->view());
        if (!record) { return record.error(); }
        return std::optional<MediaRecord>{std::move(record).value()};
    });
}

Result<std::optional<MediaRecord>> MediaRepository::find_by_hash(
    mongocxx::client& client, fs::Ns ns, const crypto::Digest256& sha256) const {
    return repo::guarded([&]() -> Result<std::optional<MediaRecord>> {
        mongocxx::collection media_collection = bind(client);
        const auto found = media_collection.find_one(
            make_document(kvp(codec::key_of(f::kNamespace), ns_value(ns)),
                          kvp(codec::key_of(f::kSha256), codec::digest_bin(sha256)))
                .view(),
            media_projection());
        if (!found) { return std::optional<MediaRecord>{}; }
        Result<MediaRecord> record = decode(found->view());
        if (!record) { return record.error(); }
        return std::optional<MediaRecord>{std::move(record).value()};
    });
}

Status MediaRepository::adjust_refs(mongocxx::client& client, mongocxx::client_session& session,
                                    fs::Ns ns, const Uuid& id, std::int32_t delta) const {
    return repo::guarded_in_transaction([&]() -> Status {
        mongocxx::collection media_collection = bind(client);
        const auto result = media_collection.update_one(
            session,
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id)),
                          kvp(codec::key_of(f::kNamespace), ns_value(ns)))
                .view(),
            make_document(kvp("$inc", make_document(kvp(codec::key_of(f::kRefs),
                                                        bsoncxx::types::b_int32{delta}))))
                .view());
        // A missing row is NotFound rather than silently ignored. An attach that
        // referenced nothing means the owning document is about to point at a
        // file that does not exist, and the transaction this runs in is the last
        // place that can be prevented.
        if (!result.has_value() || result->matched_count() == 0) {
            return fail(ErrorCode::NotFound, f::kId);
        }
        return ok();
    });
}

Result<std::optional<MediaRecord>> MediaRepository::delete_if_unreferenced(
    mongocxx::client& client, fs::Ns ns, const Uuid& id) const {
    return repo::guarded([&]() -> Result<std::optional<MediaRecord>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, id);
        filter.append(kvp(codec::key_of(f::kNamespace), ns_value(ns)));
        // The count is in the FILTER. A concurrent attach that lands first
        // leaves this matching nothing, and the caller learns it must not
        // unlink — which is the whole interlock, and it has no window because
        // there is no separate check.
        filter.append(kvp(codec::key_of(f::kRefs), bsoncxx::types::b_int32{0}));

        mongocxx::options::find_one_and_delete options{};
        // The row is needed to enumerate the files: variants are unlinked from
        // this list, never from a readdir glob.
        options.projection(make_document(
            kvp(codec::key_of(f::kNamespace), 1), kvp(codec::key_of(f::kSha256), 1),
            kvp(codec::key_of(f::kOwner), 1), kvp(codec::key_of(f::kUploaderIp), 1),
            kvp(codec::key_of(f::kBytes), 1), kvp(codec::key_of(f::kWidth), 1),
            kvp(codec::key_of(f::kHeight), 1), kvp(codec::key_of(f::kMime), 1),
            kvp(codec::key_of(f::kRefs), 1), kvp(codec::key_of(f::kVariants), 1),
            kvp(codec::key_of(f::kCreatedAt), 1)));

        mongocxx::collection media_collection = bind(client);
        const auto removed = media_collection.find_one_and_delete(filter.view(), options);
        if (!removed) { return std::optional<MediaRecord>{}; }
        Result<MediaRecord> record = decode(removed->view());
        if (!record) { return record.error(); }
        return std::optional<MediaRecord>{std::move(record).value()};
    });
}

Result<std::optional<MediaRecord>> MediaRepository::claim_unreferenced(
    mongocxx::client& client, db::TimeMs older_than) const {
    return repo::guarded([&]() -> Result<std::optional<MediaRecord>> {
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(codec::key_of(f::kRefs), bsoncxx::types::b_int32{0}));
        // The grace period. A row created moments ago and not yet attached is an
        // upload whose owning document is still being written, not an orphan.
        filter.append(kvp(codec::key_of(f::kCreatedAt), [older_than](sub_document sub) {
            sub.append(kvp("$lt", codec::time_date(older_than)));
        }));

        mongocxx::options::find_one_and_delete options{};
        options.projection(make_document(
            kvp(codec::key_of(f::kNamespace), 1), kvp(codec::key_of(f::kSha256), 1),
            kvp(codec::key_of(f::kOwner), 1), kvp(codec::key_of(f::kUploaderIp), 1),
            kvp(codec::key_of(f::kBytes), 1), kvp(codec::key_of(f::kWidth), 1),
            kvp(codec::key_of(f::kHeight), 1), kvp(codec::key_of(f::kMime), 1),
            kvp(codec::key_of(f::kRefs), 1), kvp(codec::key_of(f::kVariants), 1),
            kvp(codec::key_of(f::kCreatedAt), 1)));

        mongocxx::collection media_collection = bind(client);
        const auto claimed = media_collection.find_one_and_delete(filter.view(), options);
        if (!claimed) { return std::optional<MediaRecord>{}; }
        Result<MediaRecord> record = decode(claimed->view());
        if (!record) {
            // The row is already gone and its files are not: counted as a
            // failure because that is exactly the state the sweep exists to
            // remove, and it is invisible from anywhere else.
            analytics::count(analytics::Internal::OrphanFilesSwept,
                             analytics::SweepOutcome::Failed);
            return record.error();
        }
        // ONE claim is one orphan swept. The unlink that follows is best effort
        // by design — whatever survives it is an orphan with no row, which is
        // what the next sweep collects — so the row deletion is the sweep's unit
        // of work and the only place it can be counted exactly once.
        analytics::count(analytics::Internal::OrphanFilesSwept,
                         analytics::SweepOutcome::Deleted);
        return std::optional<MediaRecord>{std::move(record).value()};
    });
}

Result<std::vector<MediaRecord>> MediaRepository::list_namespace(
    mongocxx::client& client, fs::Ns ns, const std::optional<MediaCursor>& after,
    std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<MediaRecord>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;

        bsoncxx::builder::basic::document filter;
        filter.append(kvp(codec::key_of(f::kNamespace), ns_value(ns)));
        if (after.has_value()) {
            const MediaCursor cursor = *after;
            // The compound cursor, descending: strictly before the (created_at,
            // _id) pair the last page ended on. Two uploads share a millisecond
            // often enough that an instant-only cursor would serve some rows
            // twice and skip others.
            filter.append(kvp("$or", [cursor](sub_array branches) {
                branches.append([cursor](sub_document row) {
                    row.append(kvp(codec::key_of(f::kCreatedAt), [cursor](sub_document range) {
                        range.append(kvp("$lt", codec::time_date(cursor.created_at)));
                    }));
                });
                branches.append([cursor](sub_document row) {
                    row.append(kvp(codec::key_of(f::kCreatedAt),
                                   codec::time_date(cursor.created_at)));
                    row.append(kvp(codec::key_of(f::kId), [cursor](sub_document range) {
                        range.append(kvp("$lt", codec::uuid_bin(cursor.id)));
                    }));
                });
            }));
        }

        mongocxx::options::find options = media_projection();
        options.sort(make_document(kvp(codec::key_of(f::kCreatedAt), -1),
                                   kvp(codec::key_of(f::kId), -1)));
        options.limit(bounded);

        std::vector<MediaRecord> rows;
        rows.reserve(static_cast<std::size_t>(bounded));
        mongocxx::collection media_collection = bind(client);
        for (const bsoncxx::document::view doc :
             media_collection.find(filter.view(), options)) {
            Result<MediaRecord> record = decode(doc);
            if (!record) { return record.error(); }
            rows.push_back(std::move(record).value());
        }
        return rows;
    });
}

Result<NamespaceUsage> MediaRepository::usage(mongocxx::client& client, fs::Ns ns,
                                              std::int32_t top_limit) const {
    return repo::guarded([&]() -> Result<NamespaceUsage> {
        const std::int32_t bounded = top_limit > 0 ? top_limit : 1;
        mongocxx::collection media_collection = bind(client);

        NamespaceUsage usage{};

        // The WHOLE namespace, including the rows that carry no address. "How
        // much is this namespace worth" is a different question from "who sent
        // it", and answering the first from the second reports a number that
        // silently excludes every account-backed upload.
        mongocxx::pipeline totals;
        totals.match(make_document(kvp(codec::key_of(f::kNamespace), ns_value(ns))));
        totals.group(make_document(
            kvp(codec::key_of(f::kId), bsoncxx::types::b_null{}),
            kvp(codec::key_of(kAggBytes),
                make_document(kvp("$sum", bsoncxx::types::b_string{codec::key_of(kBytesRef)}))),
            // b_int64 and not b_int32: $sum preserves the width it was given,
            // and the decoder refuses an int32 where an int64 is declared —
            // deliberately, so a stored width cannot drift.
            kvp(codec::key_of(kAggCount),
                make_document(kvp("$sum", bsoncxx::types::b_int64{1})))));
        // An EMPTY namespace produces no group document at all, and the zeroes
        // above are the answer. That is why this reads as a loop over nothing
        // rather than as a first() that has to explain an absent row.
        for (const bsoncxx::document::view row : media_collection.aggregate(totals)) {
            const Result<std::int64_t> bytes = codec::read_int64(row, kAggBytes);
            if (!bytes) { return bytes.error(); }
            const Result<std::int64_t> count = codec::read_int64(row, kAggCount);
            if (!count) { return count.error(); }
            usage.bytes = bytes.value();
            usage.count = count.value();
        }

        // Only the rows that HAVE an address, which is exactly the range the
        // partial {ns, ip} index covers. The `$exists` repeats the partial
        // filter's predicate verbatim: the planner uses a partial index only
        // when it can prove the query is a subset of it.
        mongocxx::pipeline senders;
        senders.match(make_document(
            kvp(codec::key_of(f::kNamespace), ns_value(ns)),
            kvp(codec::key_of(f::kUploaderIp),
                make_document(kvp("$exists", bsoncxx::types::b_bool{true})))));
        senders.group(make_document(
            kvp(codec::key_of(f::kId),
                bsoncxx::types::b_string{codec::key_of(kUploaderIpRef)}),
            kvp(codec::key_of(kAggBytes),
                make_document(kvp("$sum", bsoncxx::types::b_string{codec::key_of(kBytesRef)}))),
            kvp(codec::key_of(kAggCount),
                make_document(kvp("$sum", bsoncxx::types::b_int64{1})))));
        senders.sort(make_document(kvp(codec::key_of(kAggBytes), -1)));
        senders.limit(bounded);

        usage.top.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view row : media_collection.aggregate(senders)) {
            UploaderTotal sender{};
            // The group key is the raw sixteen bytes, so it decodes exactly as
            // the stored field does — and a row whose key is not sixteen bytes
            // is corruption rather than a sender to report.
            if (const Status ip = codec::read_bytes(row, f::kId, sender.ip); !ip) {
                return ip.error();
            }
            const Result<std::int64_t> bytes = codec::read_int64(row, kAggBytes);
            if (!bytes) { return bytes.error(); }
            const Result<std::int64_t> count = codec::read_int64(row, kAggCount);
            if (!count) { return count.error(); }
            sender.bytes = bytes.value();
            sender.count = count.value();
            usage.top.push_back(sender);
        }
        return usage;
    });
}

Result<std::vector<MediaRecord>> MediaRepository::list_by_ip(
    mongocxx::client& client, fs::Ns ns, const std::array<std::uint8_t, 16>& ip,
    std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<MediaRecord>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;

        mongocxx::options::find options = media_projection();
        // Unsorted, deliberately. The caller purges what it is handed and asks
        // again, so an order costs a blocking sort to arrange rows that are
        // about to be deleted.
        options.limit(bounded);

        std::vector<MediaRecord> rows;
        rows.reserve(static_cast<std::size_t>(bounded));
        mongocxx::collection media_collection = bind(client);
        for (const bsoncxx::document::view doc : media_collection.find(
                 make_document(kvp(codec::key_of(f::kNamespace), ns_value(ns)),
                               kvp(codec::key_of(f::kUploaderIp), codec::bytes_bin(ip)))
                     .view(),
                 options)) {
            Result<MediaRecord> record = decode(doc);
            if (!record) { return record.error(); }
            rows.push_back(std::move(record).value());
        }
        return rows;
    });
}

Result<std::vector<MediaRecord>> MediaRepository::scan_after(mongocxx::client& client,
                                                             const std::optional<Uuid>& after,
                                                             std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<MediaRecord>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;

        bsoncxx::builder::basic::document filter;
        if (after.has_value()) {
            const Uuid cursor = *after;
            filter.append(kvp(codec::key_of(f::kId), [cursor](sub_document sub) {
                sub.append(kvp("$gt", codec::uuid_bin(cursor)));
            }));
        }

        mongocxx::options::find options = media_projection();
        // The `_id` walk itself provides the order, so there is no sort stage
        // and no skip — which is the point of a cursor over an index key.
        options.sort(make_document(kvp(codec::key_of(f::kId), 1)));
        options.limit(bounded);

        std::vector<MediaRecord> rows;
        rows.reserve(static_cast<std::size_t>(bounded));
        mongocxx::collection media_collection = bind(client);
        for (const bsoncxx::document::view doc :
             media_collection.find(filter.view(), options)) {
            Result<MediaRecord> record = decode(doc);
            if (!record) { return record.error(); }
            rows.push_back(std::move(record).value());
        }
        return rows;
    });
}

Result<bool> MediaRepository::clear_refs_if(mongocxx::client& client, fs::Ns ns, const Uuid& id,
                                            std::int32_t expected) const {
    return repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, id);
        filter.append(kvp(codec::key_of(f::kNamespace), ns_value(ns)));
        // The expected count is the interlock. Any concurrent attach or release
        // moves it, so a brand-new reference cannot have its count zeroed by a
        // sweeper acting on a question it asked a moment earlier.
        filter.append(kvp(codec::key_of(f::kRefs), bsoncxx::types::b_int32{expected}));

        mongocxx::collection media_collection = bind(client);
        const auto result = media_collection.update_one(
            filter.view(),
            make_document(kvp("$set", make_document(kvp(codec::key_of(f::kRefs),
                                                        bsoncxx::types::b_int32{0}))))
                .view());
        return result.has_value() && result->matched_count() == 1;
    });
}

}  // namespace anvil::media
