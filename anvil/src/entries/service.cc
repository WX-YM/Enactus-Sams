#include "anvil/entries/service.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/exception/exception.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/core/uuid.h"
#include "anvil/crypto/fast_hash.h"
#include "anvil/db/codec.h"
#include "anvil/db/versioned.h"
#include "anvil/redis/redis_client.h"
#include "anvil/sections/codec.h"
#include "anvil/sections/payload.h"
#include "anvil/sections/service.h"

namespace anvil::entries {
namespace {

using bsoncxx::builder::basic::kvp;

namespace ef = entry_fields;

[[nodiscard]] bsoncxx::stdx::string_view key(std::string_view name) noexcept {
    return db::codec::key_of(name);
}

// Thrown to abort a transaction from inside the driver's callback, for the same
// reason SectionService throws one: the driver retries a transient error for
// us, and a BUSINESS failure must unwind rather than return and let the
// callback commit what it just decided against.
struct AbortTransaction final : std::exception {
    explicit AbortTransaction(Failure f) noexcept : failure{f} {}
    [[nodiscard]] const char* what() const noexcept override { return "entry write aborted"; }
    Failure failure;
};

[[nodiscard]] EntryContent make_stage(const KindSpec& kind, sections::SectionContent content,
                                      const Uuid& actor) {
    EntryContent stage{};
    stage.etag = sections::content_etag(kind.shape, content);
    stage.content = std::move(content);
    stage.updated_at = db::now_ms();
    stage.updated_by = actor;
    return stage;
}

// `{data, media, etag, at, by}`. `data` and `media` are the section codec's own
// encoding, so a field type is stored the same way wherever it appears.
[[nodiscard]] bsoncxx::document::value stage_document(const KindSpec& kind,
                                                      const EntryContent& stage) {
    bsoncxx::builder::basic::document doc;
    sections::codec::append_content(doc, kind.shape, stage.content);
    doc.append(kvp(key(ef::kStageEtag), db::codec::bytes_bin(stage.etag)));
    db::codec::append_time(doc, ef::kStageAt, stage.updated_at);
    db::codec::append_uuid(doc, ef::kStageBy, stage.updated_by);
    return doc.extract();
}

// A whole subdocument or an explicit null, never an omitted key: a `$set` that
// did not mention the stage would leave the old copy in place, and unpublishing
// would publish nothing away.
void append_stage(bsoncxx::builder::basic::document& doc, std::string_view field,
                  const KindSpec& kind, const std::optional<EntryContent>& stage) {
    if (stage.has_value()) {
        doc.append(kvp(key(field), bsoncxx::types::b_document{stage_document(kind, *stage).view()}));
    } else {
        doc.append(kvp(key(field), bsoncxx::types::b_null{}));
    }
}

// Every distinct media id an entry holds, across both copies, sorted. An entry
// holds ONE reference per distinct id however many copies name it: counting per
// copy would make publish — which makes two copies name the same id — a leak of
// one reference per image per publish.
[[nodiscard]] std::vector<Uuid> media_of(const std::optional<EntryContent>& published,
                                         const std::optional<EntryContent>& draft) {
    std::vector<Uuid> ids;
    const auto collect = [&ids](const std::optional<EntryContent>& stage) {
        if (!stage.has_value()) { return; }
        for (const sections::SectionImage& image : stage->content.images) {
            ids.push_back(image.media_id);
        }
    };
    collect(published);
    collect(draft);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Attaches what `after` holds and `before` did not, releases the reverse, inside
// the caller's transaction — so the counts commit with the write that changed
// them or not at all (docs/12-sections-cms.md §5, the same rule).
void move_references(const media::MediaService& media, mongocxx::client& client,
                     mongocxx::client_session& session, fs::Ns ns,
                     const std::vector<Uuid>& before, const std::vector<Uuid>& after) {
    for (const Uuid& id : after) {
        if (std::binary_search(before.begin(), before.end(), id)) { continue; }
        if (const Status attached = media.attach(client, session, ns, id); !attached) {
            throw AbortTransaction{attached.error()};
        }
    }
    for (const Uuid& id : before) {
        if (std::binary_search(after.begin(), after.end(), id)) { continue; }
        if (const Status released = media.release(client, session, ns, id); !released) {
            throw AbortTransaction{released.error()};
        }
    }
}

// Runs `body` in a transaction, turning an AbortTransaction into its failure and
// a driver exception into ServiceUnavailable. `what` names the write in the log.
template <typename Fn>
[[nodiscard]] Status transact(mongocxx::client& client, std::string_view what, Fn&& body) {
    try {
        auto session = client.start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) { body(*txn); });
    } catch (const AbortTransaction& aborted) {
        return aborted.failure;
    } catch (const mongocxx::exception& e) {
        LOG_ERROR << "entry " << std::string{what} << " transaction failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable);
    }
    return ok();
}

[[nodiscard]] bool same_copy(const std::optional<EntryContent>& a,
                             const std::optional<EntryContent>& b) noexcept {
    if (a.has_value() != b.has_value()) { return false; }
    return !a.has_value() || a->etag == b->etag;
}

// The guard, applied to a row already read. NotFound rather than Forbidden: a
// member probing other members' posts must learn nothing an absent id would not
// tell them.
[[nodiscard]] Status check_guard(const EntryDocument& entry, const WriteGuard& guard) noexcept {
    if (guard.author.has_value() && entry.created_by != *guard.author) {
        return fail(ErrorCode::NotFound);
    }
    return ok();
}

[[nodiscard]] Status check_slug(const KindSpec& kind, std::string_view slug) noexcept {
    if (kind.slug == SlugRule::None) {
        return slug.empty() ? ok() : Status{fail(ErrorCode::ValidationFailed, "slug")};
    }
    return is_wellformed_slug(slug) ? ok() : Status{fail(ErrorCode::ValidationFailed, "slug")};
}

}  // namespace

EntryService::EntryService(std::string database, std::string_view collection,
                           std::span<const KindSpec> kinds, media::MediaService& media,
                           EntryServiceConfig config)
    : database_{std::move(database)},
      kinds_{kinds},
      media_{media},
      config_{std::move(config)},
      channel_{std::string{config_.channel_prefix} + ":entries"},
      entries_{database_, collection, kinds_},
      listening_{false} {}

EntryService::~EntryService() { stop_invalidation_listener(); }

Status EntryService::check_images(mongocxx::client& client, const KindSpec& kind,
                                  const sections::SectionContent& content) const {
    return sections::verify_images(client, media_, config_.image_namespace, kind.shape, content);
}

// --- reads -----------------------------------------------------------------------

Result<std::optional<EntryDocument>> EntryService::find(mongocxx::client& client,
                                                        const KindSpec& kind, const Uuid& id,
                                                        Stage stage) const {
    return entries_.find(client, kind, id, stage);
}

Result<std::optional<EntryDocument>> EntryService::find_by_slug(mongocxx::client& client,
                                                                const KindSpec& kind,
                                                                std::string_view slug,
                                                                Stage stage) const {
    // A malformed slug cannot name anything, so it is answered without a query —
    // the same NotFound an unknown well-formed one gets.
    if (kind.slug != SlugRule::Unique || !is_wellformed_slug(slug)) {
        return std::optional<EntryDocument>{};
    }
    return entries_.find_by_slug(client, kind, slug, stage);
}

Result<EntryPage> EntryService::list(mongocxx::client& client, const KindSpec& kind,
                                     const EntryQuery& query) const {
    if (kind.parent.empty() == query.parent.has_value()) {
        return fail(ErrorCode::ValidationFailed, "parent");
    }
    if ((query.flags_all & static_cast<FlagSet>(~declared_flags(kind))) != 0) {
        return fail(ErrorCode::ValidationFailed, "flags");
    }
    return entries_.list(client, kind, scope_of(kind, query.parent), query);
}

// --- create ----------------------------------------------------------------------

Result<EntryWriteOutcome> EntryService::create(mongocxx::client& client, const KindSpec& kind,
                                               const NewEntry& entry, const Uuid& actor) {
    const bool child = !kind.parent.empty();
    if (child != entry.parent.has_value()) { return fail(ErrorCode::ValidationFailed, "parent"); }
    if (const Status slug = check_slug(kind, entry.slug); !slug) { return slug.error(); }
    if ((entry.flags & static_cast<FlagSet>(~declared_flags(kind))) != 0) {
        return fail(ErrorCode::ValidationFailed, "flags");
    }

    sections::SectionContent content = sections::canonicalise(kind.shape, entry.content);
    if (const std::optional<sections::BindError> missing =
            sections::check_required(kind.shape, content)) {
        return fail(ErrorCode::ValidationFailed, missing->field);
    }
    if (const Status images = check_images(client, kind, content); !images) {
        return images.error();
    }

    const std::string scope = scope_of(kind, entry.parent);
    if (!child) {
        // A root kind's capacity is checked by counting, which two concurrent
        // creates can both pass: the bound can be exceeded by the number of
        // creators racing for the last slot, and is documented as such
        // (docs/20-entries.md §4). A child's bound is exact — it is a
        // conditional increment of the parent, inside the transaction below.
        const Result<std::int64_t> held =
            entries_.count_scope(client, scope, static_cast<std::int64_t>(kind.capacity));
        if (!held) { return held.error(); }
        if (held.value() >= static_cast<std::int64_t>(kind.capacity)) {
            return fail(ErrorCode::Conflict, "capacity");
        }
    }

    std::int64_t position = 0;
    if (kind.ordering == Ordering::Manual) {
        const Result<std::int64_t> last = entries_.last_position(client, scope);
        if (!last) { return last.error(); }
        position = last.value() + 1;
    }

    const bool live = kind.workflow == Workflow::Immediate;
    const EntryContent stage = make_stage(kind, std::move(content), actor);
    const Uuid id = uuid::generate_v7();

    bsoncxx::builder::basic::document doc;
    db::codec::append_uuid(doc, ef::kId, id);
    doc.append(kvp(key(ef::kKind), bsoncxx::types::b_string{key(kind.shape.key)}));
    doc.append(kvp(key(ef::kScope), bsoncxx::types::b_string{key(scope)}));
    db::codec::append_optional_uuid(doc, ef::kParent, entry.parent);
    // Omitted, not null, on a kind with no slugs: the unique index is partial on
    // the field EXISTING, and a null would be one more value to collide on.
    if (!entry.slug.empty()) {
        doc.append(kvp(key(ef::kSlug), bsoncxx::types::b_string{key(entry.slug)}));
    }
    db::codec::append_int64(doc, ef::kPosition, position);
    doc.append(kvp(key(ef::kFlags), bsoncxx::types::b_int32{static_cast<std::int32_t>(entry.flags)}));
    db::codec::append_int64(doc, ef::kChildren, 0);
    doc.append(kvp(key(ef::kLive), bsoncxx::types::b_bool{live}));
    append_stage(doc, ef::kPublished, kind,
                 live ? std::optional<EntryContent>{stage} : std::optional<EntryContent>{});
    append_stage(doc, ef::kDraft, kind,
                 live ? std::optional<EntryContent>{} : std::optional<EntryContent>{stage});
    db::codec::append_time(doc, ef::kCreatedAt, stage.updated_at);
    db::codec::append_uuid(doc, ef::kCreatedBy, actor);
    db::codec::append_time(doc, ef::kUpdatedAt, stage.updated_at);
    repo::append_initial_version(doc);
    const bsoncxx::document::value row = doc.extract();

    const std::vector<Uuid> held_media = media_of(std::optional<EntryContent>{stage}, std::nullopt);
    const KindSpec* parent_kind = child ? find_kind(kinds_, kind.parent) : nullptr;

    const Status written = transact(client, "create", [&](mongocxx::client_session& txn) {
        if (const Status inserted = entries_.insert(client, txn, row.view()); !inserted) {
            throw AbortTransaction{inserted.error()};
        }
        move_references(media_, client, txn, config_.image_namespace, {}, held_media);
        if (parent_kind != nullptr) {
            if (const Status counted = entries_.adjust_children(client, txn, *parent_kind,
                                                                *entry.parent, 1, kind.capacity);
                !counted) {
                throw AbortTransaction{counted.error().code == ErrorCode::NotFound
                                           ? fail(ErrorCode::NotFound, "parent")
                                           : fail(ErrorCode::Conflict, "capacity")};
            }
        }
    });
    if (!written) { return written.error(); }

    if (live) { publish_invalidation(kind.shape.key); }
    return EntryWriteOutcome{id, repo::kInitialVersion, crypto::etag_of(stage.etag)};
}

// --- edit, publish, unpublish ------------------------------------------------------

// The one write every stage change goes through: both copies as they are to be,
// the slug if it moves, the version the caller read, and the media accounting
// between the entry before and after.
Result<EntryWriteOutcome> EntryService::replace_stages(
    mongocxx::client& client, const KindSpec& kind, const EntryDocument& before,
    std::int64_t expected_version, const std::optional<EntryContent>& published,
    const std::optional<EntryContent>& draft, const std::optional<std::string>& slug,
    const EntryContent& changed) {
    bsoncxx::builder::basic::document set;
    append_stage(set, ef::kPublished, kind, published);
    if (kind.workflow == Workflow::Editorial) { append_stage(set, ef::kDraft, kind, draft); }
    set.append(kvp(key(ef::kLive), bsoncxx::types::b_bool{published.has_value()}));
    if (slug.has_value()) {
        set.append(kvp(key(ef::kSlug), bsoncxx::types::b_string{key(*slug)}));
    }
    const bsoncxx::document::value fields = set.extract();

    const std::vector<Uuid> held_before = media_of(before.published, before.draft);
    const std::vector<Uuid> held_after = media_of(published, draft);

    std::int64_t version = 0;
    const Status written = transact(client, "write", [&](mongocxx::client_session& txn) {
        const Result<std::int64_t> updated =
            entries_.update(client, txn, kind, before.id, expected_version, fields.view());
        if (!updated) { throw AbortTransaction{updated.error()}; }
        move_references(media_, client, txn, config_.image_namespace, held_before, held_after);
        version = updated.value();
    });
    if (!written) { return written.error(); }

    // Only when READERS would see a difference. A draft save changes nothing on
    // the public site, and waking every instance's page cache for one would be
    // a refill per keystroke-save.
    const bool slug_moved = slug.has_value() && (before.live() || published.has_value());
    if (!same_copy(before.published, published) || slug_moved) {
        publish_invalidation(kind.shape.key);
    }
    return EntryWriteOutcome{before.id, version, crypto::etag_of(changed.etag)};
}

Result<EntryWriteOutcome> EntryService::write(mongocxx::client& client, const KindSpec& kind,
                                              const Uuid& id, std::int64_t expected_version,
                                              const EntryEdit& edit, const Uuid& actor,
                                              const WriteGuard& guard) {
    const Result<std::optional<EntryDocument>> stored =
        entries_.find(client, kind, id, Stage::Working);
    if (!stored) { return stored.error(); }
    if (!stored.value().has_value()) { return fail(ErrorCode::NotFound); }
    const EntryDocument& before = *stored.value();
    if (const Status allowed = check_guard(before, guard); !allowed) { return allowed.error(); }

    std::optional<std::string> slug;
    if (edit.slug.has_value()) {
        if (const Status valid = check_slug(kind, *edit.slug); !valid) { return valid.error(); }
        if (kind.slug == SlugRule::Unique && *edit.slug != before.slug) { slug = *edit.slug; }
    }

    const EntryContent* base = before.working();
    if (base == nullptr) { return fail(ErrorCode::Internal); }
    sections::SectionContent merged =
        sections::canonicalise(kind.shape, sections::merge(base->content, edit.patch));
    if (const std::optional<sections::BindError> missing =
            sections::check_required(kind.shape, merged)) {
        return fail(ErrorCode::ValidationFailed, missing->field);
    }
    if (const Status images = check_images(client, kind, merged); !images) {
        return images.error();
    }

    const EntryContent changed = make_stage(kind, std::move(merged), actor);
    if (kind.workflow == Workflow::Editorial) {
        return replace_stages(client, kind, before, expected_version, before.published,
                              std::optional<EntryContent>{changed}, slug, changed);
    }
    return replace_stages(client, kind, before, expected_version,
                          std::optional<EntryContent>{changed}, std::nullopt, slug, changed);
}

Result<EntryWriteOutcome> EntryService::publish(mongocxx::client& client, const KindSpec& kind,
                                                const Uuid& id, std::int64_t expected_version,
                                                const Uuid& actor) {
    if (kind.workflow != Workflow::Editorial) {
        return fail(ErrorCode::ValidationFailed, "workflow");
    }
    const Result<std::optional<EntryDocument>> stored =
        entries_.find(client, kind, id, Stage::Working);
    if (!stored) { return stored.error(); }
    if (!stored.value().has_value()) { return fail(ErrorCode::NotFound); }
    const EntryDocument& before = *stored.value();
    // Every Editorial entry is created with a draft and no write removes it.
    if (!before.draft.has_value()) { return fail(ErrorCode::Internal); }

    // The published copy records who PUBLISHED it and when, which is the fact a
    // reader of the audit trail wants; the draft keeps who wrote it.
    EntryContent published = *before.draft;
    published.updated_at = db::now_ms();
    published.updated_by = actor;
    return replace_stages(client, kind, before, expected_version,
                          std::optional<EntryContent>{published}, before.draft, std::nullopt,
                          published);
}

Result<EntryWriteOutcome> EntryService::unpublish(mongocxx::client& client, const KindSpec& kind,
                                                  const Uuid& id, std::int64_t expected_version,
                                                  const Uuid& actor) {
    (void)actor;
    if (kind.workflow != Workflow::Editorial) {
        return fail(ErrorCode::ValidationFailed, "workflow");
    }
    const Result<std::optional<EntryDocument>> stored =
        entries_.find(client, kind, id, Stage::Working);
    if (!stored) { return stored.error(); }
    if (!stored.value().has_value()) { return fail(ErrorCode::NotFound); }
    const EntryDocument& before = *stored.value();
    if (!before.draft.has_value()) { return fail(ErrorCode::Internal); }
    return replace_stages(client, kind, before, expected_version, std::nullopt, before.draft,
                          std::nullopt, *before.draft);
}

// --- placement ---------------------------------------------------------------------

Status EntryService::set_flags(mongocxx::client& client, const KindSpec& kind, const Uuid& id,
                               FlagSet set, FlagSet clear, const WriteGuard& guard) {
    const FlagSet declared = declared_flags(kind);
    if ((set & clear) != 0 || ((set | clear) & static_cast<FlagSet>(~declared)) != 0) {
        return fail(ErrorCode::ValidationFailed, "flags");
    }
    if (guard.author.has_value()) {
        const Result<std::optional<EntryDocument>> stored =
            entries_.find(client, kind, id, Stage::Working);
        if (!stored) { return stored.error(); }
        if (!stored.value().has_value()) { return fail(ErrorCode::NotFound); }
        if (const Status allowed = check_guard(*stored.value(), guard); !allowed) {
            return allowed;
        }
    }
    const Result<bool> found = entries_.set_flags(client, kind, id, set, clear);
    if (!found) { return found.error(); }
    if (!found.value()) { return fail(ErrorCode::NotFound); }
    // Unconditionally: whether the entry is live is not known without a read,
    // and a refill for a flag on a draft is one wasted refill on an act staff
    // perform by hand.
    publish_invalidation(kind.shape.key);
    return ok();
}

Status EntryService::reorder(mongocxx::client& client, const KindSpec& kind,
                             const std::optional<Uuid>& parent, std::span<const Uuid> order) {
    if (kind.ordering != Ordering::Manual) { return fail(ErrorCode::ValidationFailed, "ordering"); }
    if (kind.parent.empty() == parent.has_value()) {
        return fail(ErrorCode::ValidationFailed, "parent");
    }
    if (order.size() > kind.capacity) { return fail(ErrorCode::Conflict); }

    std::vector<Uuid> wanted{order.begin(), order.end()};
    std::sort(wanted.begin(), wanted.end());
    if (std::adjacent_find(wanted.begin(), wanted.end()) != wanted.end()) {
        return fail(ErrorCode::Conflict);
    }

    const std::string scope = scope_of(kind, parent);
    const Status written = transact(client, "reorder", [&](mongocxx::client_session& txn) {
        // In the transaction's snapshot, so the set compared is the set written.
        // One more than the capacity, so an over-full scope reads as a mismatch
        // rather than as a truncated match.
        Result<std::vector<Uuid>> held =
            entries_.scope_ids(client, txn, scope, static_cast<std::int64_t>(kind.capacity) + 1);
        if (!held) { throw AbortTransaction{held.error()}; }
        std::vector<Uuid> present = std::move(held).value();
        std::sort(present.begin(), present.end());
        if (present != wanted) { throw AbortTransaction{fail(ErrorCode::Conflict)}; }

        for (std::size_t i = 0; i < order.size(); ++i) {
            if (const Status placed = entries_.set_position(client, txn, kind, order[i],
                                                            static_cast<std::int64_t>(i));
                !placed) {
                throw AbortTransaction{placed.error()};
            }
        }
    });
    if (!written) { return written; }
    publish_invalidation(kind.shape.key);
    return ok();
}

Status EntryService::remove(mongocxx::client& client, const KindSpec& kind, const Uuid& id,
                            std::int64_t expected_version, const WriteGuard& guard) {
    const Result<std::optional<EntryDocument>> stored =
        entries_.find(client, kind, id, Stage::Working);
    if (!stored) { return stored.error(); }
    if (!stored.value().has_value()) { return fail(ErrorCode::NotFound); }
    const EntryDocument& before = *stored.value();
    if (const Status allowed = check_guard(before, guard); !allowed) { return allowed; }
    if (before.children > 0) { return fail(ErrorCode::Conflict, "children"); }

    const std::vector<Uuid> held = media_of(before.published, before.draft);
    const KindSpec* parent_kind = before.parent.has_value() ? find_kind(kinds_, kind.parent) : nullptr;

    const Status written = transact(client, "remove", [&](mongocxx::client_session& txn) {
        // The delete filters on the version AND on a zero child count, so a
        // reply landing between the read above and this is either seen here —
        // and the delete misses — or lands after it and finds no parent.
        const Result<bool> removed = entries_.remove(client, txn, kind, id, expected_version);
        if (!removed) { throw AbortTransaction{removed.error()}; }
        if (!removed.value()) { throw AbortTransaction{fail(ErrorCode::VersionMismatch)}; }
        move_references(media_, client, txn, config_.image_namespace, held, {});
        if (parent_kind != nullptr) {
            if (const Status counted =
                    entries_.adjust_children(client, txn, *parent_kind, *before.parent, -1, 0);
                !counted) {
                throw AbortTransaction{counted.error()};
            }
        }
    });
    if (!written) { return written; }
    if (before.live()) { publish_invalidation(kind.shape.key); }
    return ok();
}

// --- bootstrap ---------------------------------------------------------------------

Result<EntryBootstrapReport> EntryService::bootstrap(mongocxx::client& client,
                                                     std::span<const KindSeeds> seeds,
                                                     const sections::DefaultImageResolver& resolve,
                                                     const Uuid& actor) {
    EntryBootstrapReport report{};
    for (const KindSeeds& seeded : seeds) {
        const KindSpec* kind = find_kind(kinds_, seeded.kind);
        // seeds_match_kinds rules this out at compile time; a caller that skipped
        // the assert gets a refusal rather than a row under an undeclared kind.
        if (kind == nullptr || !kind->parent.empty()) { return fail(ErrorCode::Internal); }

        // Images first, OUTSIDE the transaction: registering one runs the media
        // pipeline, which is seconds of CPU and a file write that no rollback
        // would undo. A file registered for a kind some other boot has already
        // seeded is found again by its content hash next time, not stored twice.
        sections::BootstrapReport images{};
        std::vector<bsoncxx::document::value> rows;
        std::vector<std::vector<Uuid>> held;
        rows.reserve(seeded.entries.size());
        held.reserve(seeded.entries.size());
        const std::string scope = scope_of(*kind, std::nullopt);
        for (std::size_t i = 0; i < seeded.entries.size(); ++i) {
            const EntrySeed& entry = seeded.entries[i];
            const EntryContent stage = make_stage(
                *kind, sections::resolve_default_content(kind->shape, entry.content, resolve, images),
                actor);

            bsoncxx::builder::basic::document doc;
            db::codec::append_uuid(doc, ef::kId, uuid::generate_v7());
            doc.append(kvp(key(ef::kKind), bsoncxx::types::b_string{key(kind->shape.key)}));
            doc.append(kvp(key(ef::kScope), bsoncxx::types::b_string{key(scope)}));
            db::codec::append_optional_uuid(doc, ef::kParent, std::nullopt);
            if (!entry.slug.empty()) {
                doc.append(kvp(key(ef::kSlug), bsoncxx::types::b_string{key(entry.slug)}));
            }
            db::codec::append_int64(doc, ef::kPosition,
                                    kind->ordering == Ordering::Manual ? static_cast<std::int64_t>(i)
                                                                       : 0);
            doc.append(kvp(key(ef::kFlags),
                           bsoncxx::types::b_int32{static_cast<std::int32_t>(entry.flags)}));
            db::codec::append_int64(doc, ef::kChildren, 0);
            // A seed is what a fresh deployment PUBLISHES, exactly as a section
            // default is: an Editorial seed starts live with a draft equal to it.
            doc.append(kvp(key(ef::kLive), bsoncxx::types::b_bool{true}));
            append_stage(doc, ef::kPublished, *kind, std::optional<EntryContent>{stage});
            append_stage(doc, ef::kDraft, *kind,
                         kind->workflow == Workflow::Editorial ? std::optional<EntryContent>{stage}
                                                               : std::optional<EntryContent>{});
            db::codec::append_time(doc, ef::kCreatedAt, stage.updated_at);
            db::codec::append_uuid(doc, ef::kCreatedBy, actor);
            db::codec::append_time(doc, ef::kUpdatedAt, stage.updated_at);
            repo::append_initial_version(doc);
            rows.push_back(doc.extract());
            held.push_back(media_of(std::optional<EntryContent>{stage}, std::nullopt));
        }

        bool claimed = false;
        bool seeded_before = false;
        const Status written = transact(client, "seed", [&](mongocxx::client_session& txn) {
            claimed = false;
            const Result<bool> claim = entries_.claim_seed(client, txn, kind->shape.key);
            if (!claim) { throw AbortTransaction{claim.error()}; }
            if (!claim.value()) {
                // Somebody seeded this kind before. The duplicate-key error has
                // already aborted the transaction on the server, so it must
                // UNWIND here: returning would ask the driver to commit an
                // aborted transaction, which it retries for two minutes.
                seeded_before = true;
                throw AbortTransaction{fail(ErrorCode::Conflict)};
            }
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (const Status inserted = entries_.insert(client, txn, rows[i].view());
                    !inserted) {
                    throw AbortTransaction{inserted.error()};
                }
                move_references(media_, client, txn, config_.image_namespace, {}, held[i]);
            }
            claimed = true;
        });
        if (!written && !seeded_before) { return written.error(); }

        report.images_registered += images.images_registered;
        report.images_missing += images.images_missing;
        if (claimed) {
            ++report.kinds_seeded;
            report.entries_created += rows.size();
            publish_invalidation(kind->shape.key);
        } else {
            ++report.kinds_already_seeded;
        }
    }
    return report;
}

// --- invalidation ------------------------------------------------------------------

void EntryService::invalidate_local(std::string_view kind) noexcept {
    // The allow-list applies to the channel as it does to a request: a message
    // is data from another process, and one naming no declared kind is dropped.
    const KindSpec* spec = find_kind(kinds_, kind);
    if (spec == nullptr || !config_.on_invalidated) { return; }
    try {
        // The REGISTRY's spelling, which lives in .rodata, never the caller's —
        // on the subscriber's path that is a view into a message buffer that
        // dies with the callback (CLAUDE.md §2.2).
        config_.on_invalidated(spec->shape.key);
    } catch (const std::exception& e) {
        LOG_ERROR << "entry invalidation hook threw for kind " << std::string{spec->shape.key}
                  << ": " << e.what();
    } catch (...) {
        LOG_ERROR << "entry invalidation hook threw for kind " << std::string{spec->shape.key};
    }
}

void EntryService::publish_invalidation(std::string_view kind) noexcept {
    invalidate_local(kind);
    try {
        redis::RedisClient::instance().publish(channel_, std::string{kind});
    } catch (const std::exception& e) {
        LOG_WARN << "entry invalidation not published; other instances keep what they "
                    "derived until their next notice: "
                 << e.what();
    }
}

void EntryService::start_invalidation_listener() {
    if (listening_.exchange(true)) { return; }
    listener_ = std::thread{[this]() { listener_loop(); }};
}

void EntryService::stop_invalidation_listener() noexcept {
    if (!listening_.exchange(false)) { return; }
    if (listener_.joinable()) { listener_.join(); }
}

void EntryService::listener_loop() noexcept {
    while (listening_.load(std::memory_order_acquire)) {
        try {
            sw::redis::Subscriber subscriber = redis::RedisClient::instance().subscriber();
            subscriber.on_message([this](std::string /*channel*/, std::string message) {
                invalidate_local(message);
            });
            subscriber.subscribe(channel_);
            while (listening_.load(std::memory_order_acquire)) {
                try {
                    subscriber.consume();
                } catch (const sw::redis::TimeoutError&) {
                    // The socket timeout is what lets the loop see the stop flag.
                    continue;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN << "entry invalidation listener reconnecting: " << e.what();
            for (int i = 0; i < 20 && listening_.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
    }
}

}  // namespace anvil::entries
