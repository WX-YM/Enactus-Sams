// Every write here that is a staff edit goes through repo::update_versioned
// (anvil/db/versioned.h). The three that do not say why at the call:
// set_flags, set_position and adjust_children.

#include "anvil/entries/repository.h"

#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find.hpp>

#include "anvil/core/uuid.h"
#include "anvil/db/versioned.h"
#include "anvil/sections/codec.h"

namespace anvil::entries {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

namespace ef = entry_fields;

[[nodiscard]] bsoncxx::stdx::string_view key(std::string_view name) noexcept {
    return db::codec::key_of(name);
}

// `{_id, k}`. The kind is in every filter by id, so an id minted under one kind
// is NotFound under every other — an id is never a way to read across kinds.
[[nodiscard]] bsoncxx::document::value identity_of(const KindSpec& kind, const Uuid& id) {
    return make_document(kvp(key(ef::kId), db::codec::uuid_bin(id)),
                         kvp(key(ef::kKind), bsoncxx::types::b_string{key(kind.shape.key)}));
}

// `live: true` for a reader; both values for an editor.
//
// Both values as an explicit $in rather than no predicate at all: the listing
// index is `{sc, live, o, _id}`, and with `live` unconstrained the planner could
// not use `o, _id` for the sort. With a two-value $in it merges two ordered
// index scans (SORT_MERGE) and still never sorts in memory.
void append_stage(bsoncxx::builder::basic::document& filter, Stage stage) {
    if (stage == Stage::Published) {
        filter.append(kvp(key(ef::kLive), bsoncxx::types::b_bool{true}));
        return;
    }
    filter.append(kvp(key(ef::kLive), [](sub_document sub) {
        sub.append(kvp("$in", bsoncxx::builder::basic::make_array(
                                  bsoncxx::types::b_bool{false}, bsoncxx::types::b_bool{true})));
    }));
}

// THE one place the draft is kept out of a reader's hands. A published read
// that forgot this would return an unpublished string, and that is the property
// the section design bought with two documents per key (document.h).
[[nodiscard]] bsoncxx::document::value published_projection() {
    return make_document(kvp(key(ef::kDraft), bsoncxx::types::b_int32{0}));
}

void apply_projection(mongocxx::options::find& options, Stage stage) {
    if (stage == Stage::Published) { options.projection(published_projection()); }
}

// `drf` for a Stage::Published read is dropped even if a projection somehow let
// it through: the decoder is the second lock on the same door.
[[nodiscard]] Result<std::optional<EntryContent>> decode_stage(const bsoncxx::document::view& doc,
                                                               std::string_view field,
                                                               const KindSpec& kind) {
    const bsoncxx::document::element element = doc[key(field)];
    if (!element || element.type() == bsoncxx::type::k_null) {
        return std::optional<EntryContent>{};
    }
    if (element.type() != bsoncxx::type::k_document) { return fail(ErrorCode::Internal, field); }
    const bsoncxx::document::view stage = element.get_document().value;

    Result<sections::SectionContent> content = sections::codec::decode_content(stage, kind.shape);
    if (!content) { return content.error(); }

    EntryContent out{};
    out.content = std::move(content).value();
    if (const Status etag = db::codec::read_bytes(stage, ef::kStageEtag, out.etag); !etag) {
        return etag.error();
    }
    const Result<db::TimeMs> at = db::codec::read_time(stage, ef::kStageAt);
    if (!at) { return at.error(); }
    const Result<Uuid> by = db::codec::read_uuid(stage, ef::kStageBy);
    if (!by) { return by.error(); }
    out.updated_at = at.value();
    out.updated_by = by.value();
    return std::optional<EntryContent>{std::move(out)};
}

// A row whose kind the registry no longer declares decodes to nullopt rather
// than to an error: the registry is the allow-list on the way out as well as on
// the way in, and a kind removed by a deploy is one nothing renders.
[[nodiscard]] Result<std::optional<EntryDocument>> decode(const bsoncxx::document::view& doc,
                                                          std::span<const KindSpec> kinds,
                                                          Stage stage) {
    const Result<std::string_view> kind_key = db::codec::read_text(doc, ef::kKind);
    if (!kind_key) { return kind_key.error(); }
    const KindSpec* kind = find_kind(kinds, kind_key.value());
    if (kind == nullptr) { return std::optional<EntryDocument>{}; }

    EntryDocument out{};
    out.kind = kind;

    const Result<Uuid> id = db::codec::read_uuid(doc, ef::kId);
    if (!id) { return id.error(); }
    out.id = id.value();

    const Result<std::optional<Uuid>> parent = db::codec::read_optional_uuid(doc, ef::kParent);
    if (!parent) { return parent.error(); }
    out.parent = parent.value();

    if (const bsoncxx::document::element slug = doc[key(ef::kSlug)];
        slug && slug.type() == bsoncxx::type::k_string) {
        const auto text = slug.get_string().value;
        out.slug.assign(text.data(), text.size());
    }

    const Result<std::int64_t> position = db::codec::read_int64(doc, ef::kPosition);
    if (!position) { return position.error(); }
    const Result<std::int32_t> flags = db::codec::read_int32(doc, ef::kFlags);
    if (!flags) { return flags.error(); }
    const Result<std::int64_t> children = db::codec::read_int64(doc, ef::kChildren);
    if (!children) { return children.error(); }
    const Result<db::TimeMs> created_at = db::codec::read_time(doc, ef::kCreatedAt);
    if (!created_at) { return created_at.error(); }
    const Result<Uuid> created_by = db::codec::read_uuid(doc, ef::kCreatedBy);
    if (!created_by) { return created_by.error(); }
    const Result<db::TimeMs> updated_at = db::codec::read_time(doc, ef::kUpdatedAt);
    if (!updated_at) { return updated_at.error(); }
    const Result<std::int64_t> version = repo::document_version(doc);
    if (!version) { return version.error(); }

    out.position = position.value();
    // Bits the kind does not declare are dropped rather than reported: a flag
    // removed from the table by a deploy is one no renderer asks about, and the
    // next toggle of any flag leaves it where it lies without harm.
    out.flags = static_cast<FlagSet>(static_cast<std::uint32_t>(flags.value()) &
                                     declared_flags(*kind));
    out.children = children.value();
    out.created_at = created_at.value();
    out.created_by = created_by.value();
    out.updated_at = updated_at.value();
    out.version = version.value();

    Result<std::optional<EntryContent>> published = decode_stage(doc, ef::kPublished, *kind);
    if (!published) { return published.error(); }
    out.published = std::move(published).value();

    if (stage == Stage::Working && kind->workflow == Workflow::Editorial) {
        Result<std::optional<EntryContent>> draft = decode_stage(doc, ef::kDraft, *kind);
        if (!draft) { return draft.error(); }
        out.draft = std::move(draft).value();
    }
    return std::optional<EntryDocument>{std::move(out)};
}

// Ascending for a manual or oldest-first kind, descending for newest-first. The
// same two keys either way, so one index serves all three.
[[nodiscard]] bsoncxx::document::value sort_of(const KindSpec& kind) {
    const std::int32_t direction = kind.ordering == Ordering::Newest ? -1 : 1;
    return make_document(kvp(key(ef::kPosition), direction), kvp(key(ef::kId), direction));
}

}  // namespace

std::string scope_of(const KindSpec& kind, const std::optional<Uuid>& parent) {
    std::string scope;
    scope.reserve(kind.shape.key.size() + 37);
    scope.append(kind.shape.key);
    if (parent.has_value()) {
        scope.push_back('/');
        scope.append(uuid::to_string(*parent));
    }
    return scope;
}

// --- reads -----------------------------------------------------------------------

Result<std::optional<EntryDocument>> EntryRepository::find(mongocxx::client& client,
                                                           const KindSpec& kind, const Uuid& id,
                                                           Stage stage) const {
    return repo::guarded([&]() -> Result<std::optional<EntryDocument>> {
        mongocxx::collection collection = bind(client);
        bsoncxx::builder::basic::document filter;
        filter.append(bsoncxx::builder::concatenate(identity_of(kind, id).view()));
        if (stage == Stage::Published) {
            filter.append(kvp(key(ef::kLive), bsoncxx::types::b_bool{true}));
        }
        mongocxx::options::find options{};
        apply_projection(options, stage);
        const auto found = collection.find_one(filter.view(), options);
        if (!found) { return std::optional<EntryDocument>{}; }
        return decode(found->view(), kinds_, stage);
    });
}

Result<std::optional<EntryDocument>> EntryRepository::find(mongocxx::client& client,
                                                           mongocxx::client_session& session,
                                                           const KindSpec& kind, const Uuid& id,
                                                           Stage stage) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<EntryDocument>> {
        mongocxx::collection collection = bind(client);
        mongocxx::options::find options{};
        apply_projection(options, stage);
        const auto found = collection.find_one(session, identity_of(kind, id).view(), options);
        if (!found) { return std::optional<EntryDocument>{}; }
        return decode(found->view(), kinds_, stage);
    });
}

Result<std::optional<EntryDocument>> EntryRepository::find_by_slug(mongocxx::client& client,
                                                                   const KindSpec& kind,
                                                                   std::string_view slug,
                                                                   Stage stage) const {
    return repo::guarded([&]() -> Result<std::optional<EntryDocument>> {
        mongocxx::collection collection = bind(client);
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(key(ef::kKind), bsoncxx::types::b_string{key(kind.shape.key)}));
        filter.append(kvp(key(ef::kSlug), bsoncxx::types::b_string{key(slug)}));
        if (stage == Stage::Published) {
            filter.append(kvp(key(ef::kLive), bsoncxx::types::b_bool{true}));
        }
        mongocxx::options::find options{};
        apply_projection(options, stage);
        const auto found = collection.find_one(filter.view(), options);
        if (!found) { return std::optional<EntryDocument>{}; }
        return decode(found->view(), kinds_, stage);
    });
}

Result<EntryPage> EntryRepository::list(mongocxx::client& client, const KindSpec& kind,
                                        std::string_view scope, const EntryQuery& query) const {
    const bool manual = kind.ordering == Ordering::Manual;
    // A manual scope comes back whole: it is bounded by the kind's capacity,
    // which kinds_are_well_formed holds under kMaxManualCapacity. One more than
    // asked for on a paged kind, so the page knows whether another follows
    // without a second query.
    const std::int32_t page = query.limit < 1 ? 1 : (query.limit > kMaxPage ? kMaxPage : query.limit);
    const std::int64_t limit =
        manual ? static_cast<std::int64_t>(kind.capacity) : static_cast<std::int64_t>(page) + 1;

    return repo::guarded([&]() -> Result<EntryPage> {
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(key(ef::kScope), bsoncxx::types::b_string{key(scope)}));
        append_stage(filter, query.stage);
        if (!manual) {
            // Always zero on a time-ordered kind. Stated as an equality so the
            // index continues into `_id` for the cursor's range.
            filter.append(kvp(key(ef::kPosition), bsoncxx::types::b_int64{0}));
            if (query.after.has_value()) {
                const std::string_view op = kind.ordering == Ordering::Newest ? "$lt" : "$gt";
                filter.append(kvp(key(ef::kId), [&](sub_document sub) {
                    sub.append(kvp(key(op), db::codec::uuid_bin(query.after->after)));
                }));
            }
        }
        if (query.flags_all != 0) {
            filter.append(kvp(key(ef::kFlags), [&](sub_document sub) {
                sub.append(kvp("$bitsAllSet",
                               bsoncxx::types::b_int32{static_cast<std::int32_t>(query.flags_all)}));
            }));
        }

        mongocxx::options::find options{};
        options.sort(sort_of(kind));
        options.limit(limit);
        apply_projection(options, query.stage);

        EntryPage out{};
        out.entries.reserve(static_cast<std::size_t>(manual ? 16 : page));
        mongocxx::collection collection = bind(client);
        std::int64_t seen = 0;
        for (const bsoncxx::document::view doc : collection.find(filter.view(), options)) {
            ++seen;
            if (!manual && seen > page) {
                // The extra row: there is a next page, and it starts after the
                // last row this one returns.
                if (!out.entries.empty()) { out.next = EntryCursor{out.entries.back().id}; }
                break;
            }
            Result<std::optional<EntryDocument>> decoded = decode(doc, kinds_, query.stage);
            if (!decoded) { return decoded.error(); }
            if (decoded.value().has_value()) { out.entries.push_back(std::move(*decoded.value())); }
        }
        return out;
    });
}

Result<std::int64_t> EntryRepository::last_position(mongocxx::client& client,
                                                    std::string_view scope) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter;
        filter.append(kvp(key(ef::kScope), bsoncxx::types::b_string{key(scope)}));
        append_stage(filter, Stage::Working);
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(ef::kPosition), -1), kvp(key(ef::kId), -1)));
        options.projection(make_document(kvp(key(ef::kPosition), 1)));
        options.limit(1);
        mongocxx::collection collection = bind(client);
        const auto found = collection.find_one(filter.view(), options);
        if (!found) { return std::int64_t{-1}; }
        return db::codec::read_int64(found->view(), ef::kPosition);
    });
}

Result<std::int64_t> EntryRepository::count_scope(mongocxx::client& client,
                                                  std::string_view scope,
                                                  std::int64_t ceiling) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        mongocxx::options::count options{};
        options.limit(ceiling);
        mongocxx::collection collection = bind(client);
        return collection.count_documents(
            make_document(kvp(key(ef::kScope), bsoncxx::types::b_string{key(scope)})), options);
    });
}

Result<std::vector<Uuid>> EntryRepository::scope_ids(mongocxx::client& client,
                                                     mongocxx::client_session& session,
                                                     std::string_view scope,
                                                     std::int64_t ceiling) const {
    return repo::guarded_in_transaction([&]() -> Result<std::vector<Uuid>> {
        mongocxx::options::find options{};
        options.projection(make_document(kvp(key(ef::kId), 1)));
        options.limit(ceiling);
        mongocxx::collection collection = bind(client);
        std::vector<Uuid> ids;
        ids.reserve(static_cast<std::size_t>(ceiling < 64 ? ceiling : 64));
        for (const bsoncxx::document::view doc : collection.find(
                 session, make_document(kvp(key(ef::kScope), bsoncxx::types::b_string{key(scope)})),
                 options)) {
            const Result<Uuid> id = db::codec::read_uuid(doc, ef::kId);
            if (!id) { return id.error(); }
            ids.push_back(id.value());
        }
        return ids;
    });
}

// --- writes ----------------------------------------------------------------------

Status EntryRepository::insert(mongocxx::client& client, mongocxx::client_session& session,
                               const bsoncxx::document::view& document) const {
    return repo::guarded_in_transaction([&]() -> Status {
        mongocxx::collection collection = bind(client);
        // A taken slug surfaces as the unique index's duplicate-key error, which
        // repo::translate reports as Conflict. No read beforehand: between N
        // instances a read-then-insert is a race the index wins anyway.
        collection.insert_one(session, document);
        return ok();
    });
}

Result<std::int64_t> EntryRepository::update(mongocxx::client& client,
                                             mongocxx::client_session& session,
                                             const KindSpec& kind, const Uuid& id,
                                             std::int64_t expected_version,
                                             const bsoncxx::document::view& set_fields) const {
    mongocxx::collection collection = bind(client);
    const bsoncxx::document::value identity = identity_of(kind, id);
    return repo::update_versioned(collection, session, identity.view(), expected_version,
                                  set_fields);
}

Result<bool> EntryRepository::set_flags(mongocxx::client& client, const KindSpec& kind,
                                        const Uuid& id, FlagSet set, FlagSet clear) const {
    return repo::guarded([&]() -> Result<bool> {
        mongocxx::collection collection = bind(client);
        // versioned-write-exempt: one $bit update, atomic on the server, whose
        // AND and OR masks are disjoint (the service refuses an overlap), so it
        // commutes with every other toggle and needs no read before it.
        const auto result = collection.update_one(
            identity_of(kind, id).view(),
            make_document(kvp("$bit", [&](sub_document bit) {
                bit.append(kvp(key(ef::kFlags), [&](sub_document ops) {
                    ops.append(kvp("and", bsoncxx::types::b_int32{static_cast<std::int32_t>(
                                              ~static_cast<std::uint32_t>(clear))}));
                    ops.append(kvp("or", bsoncxx::types::b_int32{static_cast<std::int32_t>(set)}));
                }));
            }), kvp("$currentDate", make_document(kvp(key(ef::kUpdatedAt), true)))));
        return result.has_value() && result->matched_count() > 0;
    });
}

Status EntryRepository::set_position(mongocxx::client& client, mongocxx::client_session& session,
                                     const KindSpec& kind, const Uuid& id,
                                     std::int64_t position) const {
    return repo::guarded_in_transaction([&]() -> Status {
        mongocxx::collection collection = bind(client);
        // versioned-write-exempt: reorder() rewrites a whole scope inside one
        // transaction after checking, in that transaction's snapshot, that the
        // caller named exactly the entries the scope holds. Position is
        // placement, not content — bumping every entry's version for it would
        // make a reorder stale every open editor in the scope.
        const auto result = collection.update_one(
            session, identity_of(kind, id).view(),
            make_document(kvp("$set", make_document(kvp(key(ef::kPosition),
                                                        bsoncxx::types::b_int64{position})))));
        if (!result || result->matched_count() == 0) { return fail(ErrorCode::Conflict); }
        return ok();
    });
}

Status EntryRepository::adjust_children(mongocxx::client& client,
                                        mongocxx::client_session& session,
                                        const KindSpec& parent_kind, const Uuid& parent,
                                        std::int64_t delta, std::uint32_t capacity) const {
    return repo::guarded_in_transaction([&]() -> Status {
        mongocxx::collection collection = bind(client);
        bsoncxx::builder::basic::document filter;
        filter.append(bsoncxx::builder::concatenate(identity_of(parent_kind, parent).view()));
        if (delta > 0) {
            // The bound is in the FILTER, so it holds under any concurrency: two
            // replies racing for the last slot are two conditional increments of
            // one document, and the server serialises them.
            filter.append(kvp(key(ef::kChildren), [&](sub_document sub) {
                sub.append(kvp("$lt", bsoncxx::types::b_int64{static_cast<std::int64_t>(capacity)}));
            }));
        }
        // versioned-write-exempt: a counter moved with $inc and bounded in the
        // filter. Versioning it would make every reply stale the thread's open
        // editor, and the count has no reader who needs it to agree with one.
        const auto result = collection.update_one(
            session, filter.view(),
            make_document(kvp("$inc", make_document(kvp(key(ef::kChildren),
                                                        bsoncxx::types::b_int64{delta})))));
        if (result && result->matched_count() > 0) { return ok(); }
        if (delta <= 0) { return fail(ErrorCode::NotFound); }
        // Missed: the parent is gone, or it is full. One point read tells the
        // two apart, and only on the path that is already failing.
        const auto exists = collection.find_one(
            session, identity_of(parent_kind, parent).view(),
            mongocxx::options::find{}.projection(make_document(kvp(key(ef::kId), 1))));
        return fail(exists ? ErrorCode::Conflict : ErrorCode::NotFound);
    });
}

Result<bool> EntryRepository::remove(mongocxx::client& client, mongocxx::client_session& session,
                                     const KindSpec& kind, const Uuid& id,
                                     std::int64_t expected_version) const {
    return repo::guarded_in_transaction([&]() -> Result<bool> {
        mongocxx::collection collection = bind(client);
        bsoncxx::builder::basic::document filter;
        filter.append(bsoncxx::builder::concatenate(identity_of(kind, id).view()));
        filter.append(kvp(key(repo::kVersionField), bsoncxx::types::b_int64{expected_version}));
        // Never with children: a reply counted between the caller's read and
        // this delete makes it miss rather than orphan the reply.
        filter.append(kvp(key(ef::kChildren), bsoncxx::types::b_int64{0}));
        const auto result = collection.delete_one(session, filter.view());
        return result.has_value() && result->deleted_count() > 0;
    });
}

Result<bool> EntryRepository::claim_seed(mongocxx::client& client,
                                         mongocxx::client_session& session,
                                         std::string_view kind) const {
    const Status inserted = repo::guarded_in_transaction([&]() -> Status {
        mongocxx::collection collection = bind(client);
        // A STRING `_id`, where every entry's is binary: the two can never
        // collide, and a marker carries no `sc`, so no listing ever sees one.
        std::string id;
        id.reserve(5 + kind.size());
        id.append("seed:");
        id.append(kind);
        bsoncxx::builder::basic::document doc;
        doc.append(kvp(key(ef::kId), bsoncxx::types::b_string{key(id)}));
        db::codec::append_time(doc, ef::kCreatedAt, db::now_ms());
        collection.insert_one(session, doc.view());
        return ok();
    });
    if (inserted) { return true; }
    // Another boot, on this instance or any other, claimed it first. That is the
    // outcome the claim exists to produce, not an error.
    if (inserted.code() == ErrorCode::Conflict) { return false; }
    return inserted.error();
}

}  // namespace anvil::entries
